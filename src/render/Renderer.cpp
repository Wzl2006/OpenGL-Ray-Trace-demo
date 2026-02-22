#include "render/Renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <glm/ext/matrix_transform.hpp>

namespace trace {

namespace {

constexpr uint32_t kLocalSize = 16;
constexpr int kSphereCount = 3;
constexpr float kCauchyA = 1.5f;
constexpr float kCauchyB = 0.004f;
constexpr int kDispersionBands = 6;

} // namespace

Renderer::Renderer() {
    m_scene = SceneData::createDefault();
    m_params = RenderParams{};
}

Renderer::~Renderer() {
    shutdown();
}

void Renderer::initialize() {
    if (m_initialized) {
        return;
    }

    initializeOpenGLFunctions();

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    if (!createPrograms() || !createBuffers() || !createTextures()) {
        qWarning() << "Renderer initialization failed.";
        return;
    }

    glGenVertexArrays(1, &m_fullscreenVao);
    glGenQueries(static_cast<GLsizei>(m_timeQueries.size()), m_timeQueries.data());

    m_bvhRebuildPending = true;
    m_pendingDirty = SceneDirtyFlags::All;
    m_initialized = true;
    m_fpsTimer.start();
    m_sceneIdleTimer.start();
    m_lastChangeMs = 0;
}

void Renderer::resize(int width, int height) {
    m_width = std::max(width, 1);
    m_height = std::max(height, 1);
    if (!m_initialized) {
        return;
    }

    createTextures();
    m_forceResetThisFrame = true;
}

void Renderer::shutdown() {
    if (!m_initialized) {
        return;
    }
    releaseResources();
    m_initialized = false;
}

void Renderer::applyChanges(const SceneData& scene, const RenderParams& params, SceneDirtyFlags dirtyFlags) {
    m_scene = scene;
    m_params = params;
    m_pendingDirty |= dirtyFlags;

    if (hasFlag(dirtyFlags, SceneDirtyFlags::SphereTransformChanged)) {
        m_bvhRebuildPending = true;
    }
    if (dirtyFlags != SceneDirtyFlags::None) {
        m_forceResetThisFrame = true;
        if (m_sceneIdleTimer.isValid()) {
            m_lastChangeMs = m_sceneIdleTimer.elapsed();
        }
    }
}

void Renderer::renderFrame() {
    if (!m_initialized) {
        return;
    }

    const bool fullUpload = (m_pendingDirty == SceneDirtyFlags::All);
    if (fullUpload) {
        uploadFullScene();
        m_bvhRebuildPending = false;
    } else {
        if (hasFlag(m_pendingDirty, SceneDirtyFlags::SphereTransformChanged) || m_bvhRebuildPending) {
            rebuildBvhAndUpload();
            m_bvhRebuildPending = false;
        }

        if (hasFlag(m_pendingDirty, SceneDirtyFlags::MaterialChanged) || hasFlag(m_pendingDirty, SceneDirtyFlags::SphereTransformChanged) ||
            hasFlag(m_pendingDirty, SceneDirtyFlags::LightChanged)) {
            uploadMaterialsAndSpheres();
        }
    }

    qint64 idleMs = 0;
    if (m_sceneIdleTimer.isValid()) {
        idleMs = m_sceneIdleTimer.elapsed() - m_lastChangeMs;
    }
    const bool dynamicNow = idleMs < m_params.settleDelayMs;
    m_effectiveSppPerFrame = std::max(1, std::min(m_params.sppPerPixel, m_params.sppBudgetPerFrame));
    if (dynamicNow) {
        m_effectiveSppPerFrame = 1;
    }
    m_stats.effectiveSppPerFrame = m_effectiveSppPerFrame;
    m_stats.denoiseEnabled = (m_params.denoiseEnabled != 0);

    updateCameraBuffer();
    updateParamsBuffer();

    if (m_forceResetThisFrame) {
        clearAccumulationTextures();
        m_frameSinceReset = 0;
    }
    m_stats.isDynamic = dynamicNow;

    glUseProgram(m_computeProgram);
    glBindImageTexture(0, m_accumTexture, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
    glBindImageTexture(1, m_outputTexture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);
    glBindImageTexture(2, m_beautyTexture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);
    glBindImageTexture(3, m_normalTexture, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
    glBindImageTexture(4, m_albedoTexture, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_bvhSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_triangleSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_sphereSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_materialSsbo);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_cameraUbo);
    glBindBufferBase(GL_UNIFORM_BUFFER, 1, m_paramUbo);
    glBindBufferBase(GL_UNIFORM_BUFFER, 2, m_lightUbo);

    const GLuint query = m_timeQueries[static_cast<size_t>(m_queryWriteIndex)];
    glBeginQuery(GL_TIME_ELAPSED, query);
    glDispatchCompute((m_width + kLocalSize - 1) / kLocalSize, (m_height + kLocalSize - 1) / kLocalSize, 1);
    glEndQuery(GL_TIME_ELAPSED);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    runDenoiserIfEnabled();

    const GLuint readQuery = m_timeQueries[static_cast<size_t>(m_queryReadIndex)];
    GLuint queryAvailable = 0;
    glGetQueryObjectuiv(readQuery, GL_QUERY_RESULT_AVAILABLE, &queryAvailable);
    if (queryAvailable != 0u) {
        GLuint64 elapsed = 0;
        glGetQueryObjectui64v(readQuery, GL_QUERY_RESULT, &elapsed);
        m_stats.gpuTimeMs = static_cast<double>(elapsed) / 1.0e6;
        m_queryReadIndex = (m_queryReadIndex + 1) % static_cast<int>(m_timeQueries.size());
    }
    m_queryWriteIndex = (m_queryWriteIndex + 1) % static_cast<int>(m_timeQueries.size());

    glViewport(0, 0, m_width, m_height);
    glUseProgram(m_displayProgram);
    glBindVertexArray(m_fullscreenVao);
    glActiveTexture(GL_TEXTURE0);
    const bool showDenoised = (m_params.denoiseEnabled != 0) && (m_denoiseProgram != 0u);
    glBindTexture(GL_TEXTURE_2D, showDenoised ? m_denoisedTexture : m_outputTexture);
    const GLint outputLocation = glGetUniformLocation(m_displayProgram, "uOutputTexture");
    glUniform1i(outputLocation, 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    m_pendingDirty = SceneDirtyFlags::None;
    m_forceResetThisFrame = false;
    ++m_frameIndex;
    ++m_frameSinceReset;
    m_stats.accumulatedFrameCount = m_frameSinceReset;
    m_stats.bvhRebuildPending = m_bvhRebuildPending;

    ++m_fpsFrames;
    const qint64 elapsedMs = m_fpsTimer.elapsed();
    if (elapsedMs >= 250) {
        m_stats.fps = static_cast<double>(m_fpsFrames) * 1000.0 / static_cast<double>(elapsedMs);
        m_fpsFrames = 0;
        m_fpsTimer.restart();
    }
}

RenderStats Renderer::stats() const {
    return m_stats;
}

bool Renderer::createPrograms() {
    if (m_computeProgram != 0u) {
        glDeleteProgram(m_computeProgram);
        m_computeProgram = 0;
    }
    if (m_denoiseProgram != 0u) {
        glDeleteProgram(m_denoiseProgram);
        m_denoiseProgram = 0;
    }
    if (m_displayProgram != 0u) {
        glDeleteProgram(m_displayProgram);
        m_displayProgram = 0;
    }

    const QString shaderRoot = QDir(QCoreApplication::applicationDirPath()).filePath("glsl");
    const std::string computeSrc = loadTextFile(QDir(shaderRoot).filePath("raytrace.comp.glsl").toStdString());
    const std::string denoiseSrc = loadTextFile(QDir(shaderRoot).filePath("denoise.comp.glsl").toStdString());
    const std::string fullscreenVertSrc = loadTextFile(QDir(shaderRoot).filePath("fullscreen.vert.glsl").toStdString());
    const std::string fullscreenFragSrc = loadTextFile(QDir(shaderRoot).filePath("fullscreen.frag.glsl").toStdString());

    if (computeSrc.empty() || denoiseSrc.empty() || fullscreenVertSrc.empty() || fullscreenFragSrc.empty()) {
        return false;
    }

    GLuint computeShader = compileShader(GL_COMPUTE_SHADER, computeSrc, "raytrace.comp");
    if (computeShader == 0u) {
        return false;
    }
    m_computeProgram = linkProgram({computeShader}, "raytrace.comp");
    glDeleteShader(computeShader);
    if (m_computeProgram == 0u) {
        return false;
    }

    GLuint denoiseShader = compileShader(GL_COMPUTE_SHADER, denoiseSrc, "denoise.comp");
    if (denoiseShader == 0u) {
        return false;
    }
    m_denoiseProgram = linkProgram({denoiseShader}, "denoise.comp");
    glDeleteShader(denoiseShader);
    if (m_denoiseProgram == 0u) {
        return false;
    }

    GLuint vertShader = compileShader(GL_VERTEX_SHADER, fullscreenVertSrc, "fullscreen.vert");
    GLuint fragShader = compileShader(GL_FRAGMENT_SHADER, fullscreenFragSrc, "fullscreen.frag");
    if (vertShader == 0u || fragShader == 0u) {
        if (vertShader != 0u) {
            glDeleteShader(vertShader);
        }
        if (fragShader != 0u) {
            glDeleteShader(fragShader);
        }
        return false;
    }

    m_displayProgram = linkProgram({vertShader, fragShader}, "fullscreen");
    glDeleteShader(vertShader);
    glDeleteShader(fragShader);
    return m_displayProgram != 0u;
}

bool Renderer::createBuffers() {
    if (m_bvhSsbo == 0u) {
        glGenBuffers(1, &m_bvhSsbo);
    }
    if (m_triangleSsbo == 0u) {
        glGenBuffers(1, &m_triangleSsbo);
    }
    if (m_sphereSsbo == 0u) {
        glGenBuffers(1, &m_sphereSsbo);
    }
    if (m_materialSsbo == 0u) {
        glGenBuffers(1, &m_materialSsbo);
    }
    if (m_cameraUbo == 0u) {
        glGenBuffers(1, &m_cameraUbo);
    }
    if (m_paramUbo == 0u) {
        glGenBuffers(1, &m_paramUbo);
    }
    if (m_lightUbo == 0u) {
        glGenBuffers(1, &m_lightUbo);
    }

    uploadBufferData(m_cameraUbo, GL_UNIFORM_BUFFER, sizeof(GpuCamera), nullptr);
    uploadBufferData(m_paramUbo, GL_UNIFORM_BUFFER, sizeof(GpuRenderParams), nullptr);
    uploadBufferData(m_lightUbo, GL_UNIFORM_BUFFER, sizeof(GpuLight), nullptr);
    return true;
}

bool Renderer::createTextures() {
    if (m_accumTexture == 0u) {
        glGenTextures(1, &m_accumTexture);
    }
    if (m_outputTexture == 0u) {
        glGenTextures(1, &m_outputTexture);
    }
    if (m_beautyTexture == 0u) {
        glGenTextures(1, &m_beautyTexture);
    }
    if (m_normalTexture == 0u) {
        glGenTextures(1, &m_normalTexture);
    }
    if (m_albedoTexture == 0u) {
        glGenTextures(1, &m_albedoTexture);
    }
    if (m_denoisedTexture == 0u) {
        glGenTextures(1, &m_denoisedTexture);
    }

    glBindTexture(GL_TEXTURE_2D, m_accumTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, m_width, m_height, 0, GL_RGBA, GL_FLOAT, nullptr);

    glBindTexture(GL_TEXTURE_2D, m_outputTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, m_width, m_height, 0, GL_RGBA, GL_FLOAT, nullptr);

    glBindTexture(GL_TEXTURE_2D, m_beautyTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, m_width, m_height, 0, GL_RGBA, GL_FLOAT, nullptr);

    glBindTexture(GL_TEXTURE_2D, m_normalTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, m_width, m_height, 0, GL_RGBA, GL_FLOAT, nullptr);

    glBindTexture(GL_TEXTURE_2D, m_albedoTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, m_width, m_height, 0, GL_RGBA, GL_FLOAT, nullptr);

    glBindTexture(GL_TEXTURE_2D, m_denoisedTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, m_width, m_height, 0, GL_RGBA, GL_FLOAT, nullptr);

    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

bool Renderer::uploadFullScene() {
    if (!rebuildBvhAndUpload()) {
        return false;
    }
    return uploadMaterialsAndSpheres();
}

bool Renderer::uploadMaterialsAndSpheres() {
    std::vector<GpuMaterial> gpuMaterials;
    gpuMaterials.reserve(m_scene.materials.size());
    for (const MaterialDefinition& material : m_scene.materials) {
        GpuMaterial gpu{};
        gpu.albedoRoughness = glm::vec4(material.albedo, material.roughness);
        gpu.emissionTransmission = glm::vec4(material.emission, material.transmission);
        gpu.iorAbsorption = glm::vec4(material.ior, material.absorption.x, material.absorption.y, material.absorption.z);
        gpu.options = glm::vec4(static_cast<float>(material.enableDiffuse != 0 ? 1 : 0), 0.0f, 0.0f, 0.0f);
        gpuMaterials.push_back(gpu);
    }
    uploadBufferData(m_materialSsbo,
                     GL_SHADER_STORAGE_BUFFER,
                     static_cast<GLsizeiptr>(gpuMaterials.size() * sizeof(GpuMaterial)),
                     gpuMaterials.empty() ? nullptr : gpuMaterials.data());

    std::array<GpuSphere, kSphereCount> gpuSpheres{};
    for (int i = 0; i < kSphereCount; ++i) {
        const SphereDefinition& sphere = m_scene.spheres[static_cast<size_t>(i)];
        gpuSpheres[static_cast<size_t>(i)].centerRadius = glm::vec4(sphere.center, sphere.radius);
        gpuSpheres[static_cast<size_t>(i)].meta = glm::ivec4(sphere.materialIndex, 0, 0, 0);
    }
    uploadBufferData(m_sphereSsbo, GL_SHADER_STORAGE_BUFFER, sizeof(gpuSpheres), gpuSpheres.data());

    GpuLight light{};
    light.centerIntensity = glm::vec4(m_scene.light.center, m_scene.light.intensity);
    light.colorSizeX = glm::vec4(m_scene.light.color, m_scene.light.size.x);
    light.normalSizeY = glm::vec4(m_scene.light.normal, m_scene.light.size.y);
    uploadBufferData(m_lightUbo, GL_UNIFORM_BUFFER, sizeof(GpuLight), &light);

    return true;
}

bool Renderer::rebuildBvhAndUpload() {
    m_bvh = m_bvhBuilder.build(m_scene.triangles);

    uploadBufferData(m_bvhSsbo,
                     GL_SHADER_STORAGE_BUFFER,
                     static_cast<GLsizeiptr>(m_bvh.nodes.size() * sizeof(GpuBvhNode)),
                     m_bvh.nodes.empty() ? nullptr : m_bvh.nodes.data());
    uploadBufferData(m_triangleSsbo,
                     GL_SHADER_STORAGE_BUFFER,
                     static_cast<GLsizeiptr>(m_bvh.triangles.size() * sizeof(GpuTriangle)),
                     m_bvh.triangles.empty() ? nullptr : m_bvh.triangles.data());
    return true;
}

void Renderer::uploadBufferData(GLuint buffer, GLenum target, GLsizeiptr size, const void* data) {
    glBindBuffer(target, buffer);
    glBufferData(target, size, data, GL_DYNAMIC_DRAW);
    glBindBuffer(target, 0);
}

void Renderer::updateCameraBuffer() {
    const float aspect = static_cast<float>(m_width) / static_cast<float>(m_height);
    const glm::mat4 view = viewMatrix(m_scene.camera);
    const glm::mat4 proj = projectionMatrix(m_scene.camera, aspect);

    GpuCamera camera{};
    camera.invView = glm::inverse(view);
    camera.invProj = glm::inverse(proj);
    camera.cameraPosition = glm::vec4(cameraPosition(m_scene.camera), 1.0f);
    camera.viewportAndFrame = glm::vec4(static_cast<float>(m_width),
                                        static_cast<float>(m_height),
                                        static_cast<float>(m_frameIndex),
                                        0.0f);

    uploadBufferData(m_cameraUbo, GL_UNIFORM_BUFFER, sizeof(GpuCamera), &camera);
}

void Renderer::updateParamsBuffer() {
    GpuRenderParams params{};
    params.counts = glm::ivec4(static_cast<int>(m_bvh.triangles.size()),
                               static_cast<int>(m_bvh.nodes.size()),
                               kSphereCount,
                               static_cast<int>(m_scene.materials.size()));
    params.options = glm::ivec4(m_params.maxBounces,
                                m_params.areaLightSamples,
                                m_effectiveSppPerFrame,
                                m_forceResetThisFrame ? 1 : 0);
    params.alphaAndMode = glm::vec4(m_params.staticAlpha, m_forceResetThisFrame ? 1.0f : 0.0f, 0.0f, 0.0f);
    params.cauchy = glm::vec4(kCauchyA, kCauchyB, static_cast<float>(kDispersionBands), 0.0f);
    params.debug = glm::vec4(static_cast<float>(m_params.debugMonochromaticMode),
                             m_params.debugWavelengthNm,
                             0.0f,
                             0.0f);
    uploadBufferData(m_paramUbo, GL_UNIFORM_BUFFER, sizeof(GpuRenderParams), &params);
}

void Renderer::clearAccumulationTextures() {
    std::vector<float> zeros(static_cast<size_t>(m_width) * static_cast<size_t>(m_height) * 4u, 0.0f);
    glBindTexture(GL_TEXTURE_2D, m_accumTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_width, m_height, GL_RGBA, GL_FLOAT, zeros.data());
    glBindTexture(GL_TEXTURE_2D, m_outputTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_width, m_height, GL_RGBA, GL_FLOAT, zeros.data());
    glBindTexture(GL_TEXTURE_2D, m_beautyTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_width, m_height, GL_RGBA, GL_FLOAT, zeros.data());
    glBindTexture(GL_TEXTURE_2D, m_normalTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_width, m_height, GL_RGBA, GL_FLOAT, zeros.data());
    glBindTexture(GL_TEXTURE_2D, m_albedoTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_width, m_height, GL_RGBA, GL_FLOAT, zeros.data());
    glBindTexture(GL_TEXTURE_2D, m_denoisedTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_width, m_height, GL_RGBA, GL_FLOAT, zeros.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Renderer::runDenoiserIfEnabled() {
    if (m_params.denoiseEnabled == 0 || m_denoiseProgram == 0u) {
        return;
    }

    glUseProgram(m_denoiseProgram);
    glBindImageTexture(0, m_outputTexture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32F);
    glBindImageTexture(1, m_normalTexture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32F);
    glBindImageTexture(2, m_albedoTexture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32F);
    glBindImageTexture(3, m_denoisedTexture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);

    const GLint sizeLoc = glGetUniformLocation(m_denoiseProgram, "uImageSize");
    const GLint sigmaColorLoc = glGetUniformLocation(m_denoiseProgram, "uSigmaColor");
    const GLint sigmaNormalLoc = glGetUniformLocation(m_denoiseProgram, "uSigmaNormal");
    const GLint sigmaAlbedoLoc = glGetUniformLocation(m_denoiseProgram, "uSigmaAlbedo");
    const GLint radiusLoc = glGetUniformLocation(m_denoiseProgram, "uRadius");

    glUniform2i(sizeLoc, m_width, m_height);
    glUniform1f(sigmaColorLoc, std::max(0.01f, m_params.denoiseSigma));
    glUniform1f(sigmaNormalLoc, 0.15f);
    glUniform1f(sigmaAlbedoLoc, 0.20f);
    glUniform1i(radiusLoc, 2);

    glDispatchCompute((m_width + kLocalSize - 1) / kLocalSize, (m_height + kLocalSize - 1) / kLocalSize, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

std::string Renderer::loadTextFile(const std::string& path) const {
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "Failed to open shader file:" << QString::fromStdString(path);
        return {};
    }
    return file.readAll().toStdString();
}

GLuint Renderer::compileShader(GLenum stage, const std::string& source, const std::string& label) {
    const GLuint shader = glCreateShader(stage);
    const char* srcPtr = source.c_str();
    glShaderSource(shader, 1, &srcPtr, nullptr);
    glCompileShader(shader);

    GLint success = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (success == GL_FALSE) {
        GLint logLen = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLen);
        std::string log(static_cast<size_t>(std::max(logLen, 1)), '\0');
        glGetShaderInfoLog(shader, logLen, nullptr, log.data());
        qWarning() << "Failed to compile shader" << QString::fromStdString(label) << "\n" << QString::fromStdString(log);
        glDeleteShader(shader);
        return 0u;
    }
    return shader;
}

GLuint Renderer::linkProgram(const std::vector<GLuint>& shaders, const std::string& label) {
    const GLuint program = glCreateProgram();
    for (const GLuint shader : shaders) {
        glAttachShader(program, shader);
    }
    glLinkProgram(program);

    GLint success = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (success == GL_FALSE) {
        GLint logLen = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLen);
        std::string log(static_cast<size_t>(std::max(logLen, 1)), '\0');
        glGetProgramInfoLog(program, logLen, nullptr, log.data());
        qWarning() << "Failed to link program" << QString::fromStdString(label) << "\n" << QString::fromStdString(log);
        glDeleteProgram(program);
        return 0u;
    }
    return program;
}

void Renderer::releaseResources() {
    if (m_computeProgram != 0u) {
        glDeleteProgram(m_computeProgram);
        m_computeProgram = 0;
    }
    if (m_denoiseProgram != 0u) {
        glDeleteProgram(m_denoiseProgram);
        m_denoiseProgram = 0;
    }
    if (m_displayProgram != 0u) {
        glDeleteProgram(m_displayProgram);
        m_displayProgram = 0;
    }
    if (m_fullscreenVao != 0u) {
        glDeleteVertexArrays(1, &m_fullscreenVao);
        m_fullscreenVao = 0;
    }

    if (m_accumTexture != 0u) {
        glDeleteTextures(1, &m_accumTexture);
        m_accumTexture = 0;
    }
    if (m_outputTexture != 0u) {
        glDeleteTextures(1, &m_outputTexture);
        m_outputTexture = 0;
    }
    if (m_beautyTexture != 0u) {
        glDeleteTextures(1, &m_beautyTexture);
        m_beautyTexture = 0;
    }
    if (m_normalTexture != 0u) {
        glDeleteTextures(1, &m_normalTexture);
        m_normalTexture = 0;
    }
    if (m_albedoTexture != 0u) {
        glDeleteTextures(1, &m_albedoTexture);
        m_albedoTexture = 0;
    }
    if (m_denoisedTexture != 0u) {
        glDeleteTextures(1, &m_denoisedTexture);
        m_denoisedTexture = 0;
    }

    if (m_bvhSsbo != 0u) {
        glDeleteBuffers(1, &m_bvhSsbo);
        m_bvhSsbo = 0;
    }
    if (m_triangleSsbo != 0u) {
        glDeleteBuffers(1, &m_triangleSsbo);
        m_triangleSsbo = 0;
    }
    if (m_sphereSsbo != 0u) {
        glDeleteBuffers(1, &m_sphereSsbo);
        m_sphereSsbo = 0;
    }
    if (m_materialSsbo != 0u) {
        glDeleteBuffers(1, &m_materialSsbo);
        m_materialSsbo = 0;
    }
    if (m_cameraUbo != 0u) {
        glDeleteBuffers(1, &m_cameraUbo);
        m_cameraUbo = 0;
    }
    if (m_paramUbo != 0u) {
        glDeleteBuffers(1, &m_paramUbo);
        m_paramUbo = 0;
    }
    if (m_lightUbo != 0u) {
        glDeleteBuffers(1, &m_lightUbo);
        m_lightUbo = 0;
    }

    if (!m_timeQueries.empty() && m_timeQueries[0] != 0u) {
        glDeleteQueries(static_cast<GLsizei>(m_timeQueries.size()), m_timeQueries.data());
        m_timeQueries.fill(0u);
    }
}

} // namespace trace
