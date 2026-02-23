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
    m_denoiserBackend = DenoiserBackend::None;
#if defined(TRACE_WITH_OIDN)
    if (initializeOidn()) {
        m_denoiserBackend = DenoiserBackend::OidnCpu;
    } else if (m_denoiseProgram != 0u) {
        m_denoiserBackend = DenoiserBackend::BuiltinCompute;
    }
#else
    if (m_denoiseProgram != 0u) {
        m_denoiserBackend = DenoiserBackend::BuiltinCompute;
    }
#endif
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
#if defined(TRACE_WITH_OIDN)
    shutdownOidn();
#endif
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

    auto drawOutput = [this]() {
        glViewport(0, 0, m_width, m_height);
        glUseProgram(m_displayProgram);
        glBindVertexArray(m_fullscreenVao);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_outputTexture);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_denoisedTexture);
        const bool showDenoised = (m_params.denoiseEnabled != 0) &&
                                  (m_hasDenoisedFrame || m_denoiserBackend == DenoiserBackend::BuiltinCompute);
        const GLint rawLocation = glGetUniformLocation(m_displayProgram, "uRawTexture");
        const GLint denoisedLocation = glGetUniformLocation(m_displayProgram, "uDenoisedTexture");
        const GLint useDenoisedLocation = glGetUniformLocation(m_displayProgram, "uUseDenoised");
        const GLint denoiseBlendLocation = glGetUniformLocation(m_displayProgram, "uDenoiseBlend");
        glUniform1i(rawLocation, 0);
        glUniform1i(denoisedLocation, 1);
        glUniform1i(useDenoisedLocation, showDenoised ? 1 : 0);
        glUniform1f(denoiseBlendLocation, std::clamp(m_params.denoiseBlend, 0.0f, 1.0f));
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(0);
    };

    auto pollGpuTimeQuery = [this]() {
        const GLuint readQuery = m_timeQueries[static_cast<size_t>(m_queryReadIndex)];
        GLuint queryAvailable = 0;
        glGetQueryObjectuiv(readQuery, GL_QUERY_RESULT_AVAILABLE, &queryAvailable);
        if (queryAvailable != 0u) {
            GLuint64 elapsed = 0;
            glGetQueryObjectui64v(readQuery, GL_QUERY_RESULT, &elapsed);
            m_stats.gpuTimeMs = static_cast<double>(elapsed) / 1.0e6;
            m_queryReadIndex = (m_queryReadIndex + 1) % static_cast<int>(m_timeQueries.size());
        }
    };

    if (m_computeInFlight && m_computeFence != nullptr) {
        const GLenum waitResult = glClientWaitSync(m_computeFence, 0, 0);
        if (waitResult == GL_ALREADY_SIGNALED || waitResult == GL_CONDITION_SATISFIED) {
            glDeleteSync(m_computeFence);
            m_computeFence = nullptr;
            m_computeInFlight = false;
        }
    }

    pollGpuTimeQuery();
    if (m_computeInFlight) {
#if defined(TRACE_WITH_OIDN)
        if (m_params.denoiseEnabled != 0 && m_denoiserBackend == DenoiserBackend::OidnCpu) {
            uploadOidnResultIfReady();
        }
#endif
        m_stats.bvhRebuildPending = m_bvhRebuildPending;
        m_stats.accumulatedFrameCount = m_frameSinceReset;
        drawOutput();
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
    m_stats.denoiseEnabled = (m_params.denoiseEnabled != 0) && (m_denoiserBackend != DenoiserBackend::None);

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
    m_queryWriteIndex = (m_queryWriteIndex + 1) % static_cast<int>(m_timeQueries.size());
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    runDenoiserIfEnabled();
    if (m_computeFence != nullptr) {
        glDeleteSync(m_computeFence);
        m_computeFence = nullptr;
    }
    m_computeFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    m_computeInFlight = (m_computeFence != nullptr);

    drawOutput();

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
    m_hasDenoisedFrame = false;
    ++m_resetSerial;
#if defined(TRACE_WITH_OIDN)
    {
        std::scoped_lock<std::mutex> lock(m_oidnMutex);
        m_oidnJobPending = false;
        m_oidnResultReady = false;
    }
#endif
}

void Renderer::runDenoiserIfEnabled() {
    if (m_params.denoiseEnabled == 0) {
        return;
    }

    if (m_denoiserBackend == DenoiserBackend::OidnCpu) {
#if defined(TRACE_WITH_OIDN)
        uploadOidnResultIfReady();

        const uint64_t minAccumFrames = static_cast<uint64_t>(std::max(1, m_params.denoiseMinAccumFrames));
        if (m_stats.isDynamic || m_frameSinceReset < minAccumFrames || !m_sceneIdleTimer.isValid()) {
            return;
        }

        const qint64 nowMs = m_sceneIdleTimer.elapsed();
        const qint64 minIntervalMs = static_cast<qint64>(std::max(50, m_params.denoiseIntervalMs));
        if ((nowMs - m_lastOidnKickMs) < minIntervalMs) {
            return;
        }

        {
            std::scoped_lock<std::mutex> lock(m_oidnMutex);
            if (m_oidnJobPending || m_oidnJobInFlight) {
                return;
            }
        }

        if (!captureOidnInput()) {
            return;
        }
        enqueueOidnJob();
        m_lastOidnKickMs = nowMs;
#endif
        return;
    }

    if (m_denoiserBackend == DenoiserBackend::BuiltinCompute) {
        runBuiltinDenoiser();
    }
}

void Renderer::runBuiltinDenoiser() {
    if (m_denoiseProgram == 0u) {
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
    m_hasDenoisedFrame = true;
}

#if defined(TRACE_WITH_OIDN)
bool Renderer::initializeOidn() {
    m_oidnDevice = oidn::newDevice(oidn::DeviceType::CPU);
    if (!m_oidnDevice) {
        qWarning() << "OIDN device creation failed.";
        return false;
    }

    m_oidnDevice.commit();
    const char* errorMessage = nullptr;
    if (m_oidnDevice.getError(errorMessage) != oidn::Error::None) {
        qWarning() << "OIDN device commit failed:" << (errorMessage != nullptr ? errorMessage : "unknown error");
        m_oidnDevice.release();
        return false;
    }

    m_oidnExitRequested = false;
    m_oidnWorker = std::thread(&Renderer::oidnWorkerMain, this);
    qInfo() << "OIDN CPU backend initialized.";
    return true;
}

void Renderer::shutdownOidn() {
    {
        std::scoped_lock<std::mutex> lock(m_oidnMutex);
        m_oidnExitRequested = true;
        m_oidnJobPending = false;
    }
    m_oidnCv.notify_all();
    if (m_oidnWorker.joinable()) {
        m_oidnWorker.join();
    }
    m_oidnDevice.release();
    m_oidnColorReadback.clear();
    m_oidnNormalReadback.clear();
    m_oidnAlbedoReadback.clear();
    m_oidnUploadRgba.clear();
}

bool Renderer::captureOidnInput() {
    const size_t pixelCount = static_cast<size_t>(m_width) * static_cast<size_t>(m_height);
    if (pixelCount == 0u) {
        return false;
    }

    const size_t rgbaCount = pixelCount * 4u;
    m_oidnColorReadback.resize(rgbaCount);
    m_oidnNormalReadback.resize(rgbaCount);
    m_oidnAlbedoReadback.resize(rgbaCount);

    glBindTexture(GL_TEXTURE_2D, m_outputTexture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, m_oidnColorReadback.data());
    glBindTexture(GL_TEXTURE_2D, m_normalTexture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, m_oidnNormalReadback.data());
    glBindTexture(GL_TEXTURE_2D, m_albedoTexture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, m_oidnAlbedoReadback.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    OidnJob job;
    job.width = m_width;
    job.height = m_height;
    job.resetSerial = m_resetSerial;
    job.color.resize(pixelCount * 3u);
    job.normal.resize(pixelCount * 3u);
    job.albedo.resize(pixelCount * 3u);

    for (size_t i = 0; i < pixelCount; ++i) {
        const size_t rgbaBase = i * 4u;
        const size_t rgbBase = i * 3u;

        job.color[rgbBase + 0u] = m_oidnColorReadback[rgbaBase + 0u];
        job.color[rgbBase + 1u] = m_oidnColorReadback[rgbaBase + 1u];
        job.color[rgbBase + 2u] = m_oidnColorReadback[rgbaBase + 2u];

        float nx = m_oidnNormalReadback[rgbaBase + 0u] * 2.0f - 1.0f;
        float ny = m_oidnNormalReadback[rgbaBase + 1u] * 2.0f - 1.0f;
        float nz = m_oidnNormalReadback[rgbaBase + 2u] * 2.0f - 1.0f;
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1e-6f) {
            nx /= len;
            ny /= len;
            nz /= len;
        } else {
            nx = 0.0f;
            ny = 1.0f;
            nz = 0.0f;
        }
        job.normal[rgbBase + 0u] = nx;
        job.normal[rgbBase + 1u] = ny;
        job.normal[rgbBase + 2u] = nz;

        job.albedo[rgbBase + 0u] = m_oidnAlbedoReadback[rgbaBase + 0u];
        job.albedo[rgbBase + 1u] = m_oidnAlbedoReadback[rgbaBase + 1u];
        job.albedo[rgbBase + 2u] = m_oidnAlbedoReadback[rgbaBase + 2u];
    }

    {
        std::scoped_lock<std::mutex> lock(m_oidnMutex);
        m_oidnPendingJob = std::move(job);
        m_oidnJobPending = true;
    }
    return true;
}

void Renderer::enqueueOidnJob() {
    m_oidnCv.notify_one();
}

void Renderer::uploadOidnResultIfReady() {
    OidnResult result;
    {
        std::scoped_lock<std::mutex> lock(m_oidnMutex);
        if (!m_oidnResultReady) {
            return;
        }
        result = std::move(m_oidnResult);
        m_oidnResultReady = false;
    }

    if (result.resetSerial != m_resetSerial || result.width != m_width || result.height != m_height || result.color.empty()) {
        return;
    }

    const size_t pixelCount = static_cast<size_t>(result.width) * static_cast<size_t>(result.height);
    m_oidnUploadRgba.resize(pixelCount * 4u);
    for (size_t i = 0; i < pixelCount; ++i) {
        const size_t rgbBase = i * 3u;
        const size_t rgbaBase = i * 4u;
        m_oidnUploadRgba[rgbaBase + 0u] = result.color[rgbBase + 0u];
        m_oidnUploadRgba[rgbaBase + 1u] = result.color[rgbBase + 1u];
        m_oidnUploadRgba[rgbaBase + 2u] = result.color[rgbBase + 2u];
        m_oidnUploadRgba[rgbaBase + 3u] = 1.0f;
    }

    glBindTexture(GL_TEXTURE_2D, m_denoisedTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, result.width, result.height, GL_RGBA, GL_FLOAT, m_oidnUploadRgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    m_hasDenoisedFrame = true;
}

void Renderer::oidnWorkerMain() {
    while (true) {
        OidnJob job;
        {
            std::unique_lock<std::mutex> lock(m_oidnMutex);
            m_oidnCv.wait(lock, [this]() { return m_oidnExitRequested || m_oidnJobPending; });
            if (m_oidnExitRequested) {
                break;
            }
            job = std::move(m_oidnPendingJob);
            m_oidnJobPending = false;
            m_oidnJobInFlight = true;
        }

        std::vector<float> denoised;
        denoised.resize(job.color.size(), 0.0f);

        bool success = false;
        if (job.width > 0 && job.height > 0 && !job.color.empty() && !job.normal.empty() && !job.albedo.empty() && m_oidnDevice) {
            oidn::FilterRef filter = m_oidnDevice.newFilter("RT");
            filter.setImage("color", job.color.data(), oidn::Format::Float3, static_cast<size_t>(job.width), static_cast<size_t>(job.height));
            filter.setImage("normal", job.normal.data(), oidn::Format::Float3, static_cast<size_t>(job.width), static_cast<size_t>(job.height));
            filter.setImage("albedo", job.albedo.data(), oidn::Format::Float3, static_cast<size_t>(job.width), static_cast<size_t>(job.height));
            filter.setImage("output", denoised.data(), oidn::Format::Float3, static_cast<size_t>(job.width), static_cast<size_t>(job.height));
            filter.set("hdr", true);
            filter.set("cleanAux", false);
            filter.commit();
            filter.execute();

            const char* errorMessage = nullptr;
            if (m_oidnDevice.getError(errorMessage) == oidn::Error::None) {
                success = true;
            } else {
                qWarning() << "OIDN execute failed:" << (errorMessage != nullptr ? errorMessage : "unknown error");
            }
        }

        {
            std::scoped_lock<std::mutex> lock(m_oidnMutex);
            if (success) {
                m_oidnResult.width = job.width;
                m_oidnResult.height = job.height;
                m_oidnResult.resetSerial = job.resetSerial;
                m_oidnResult.color = std::move(denoised);
                m_oidnResultReady = true;
            }
            m_oidnJobInFlight = false;
        }
    }
}
#endif

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
    if (m_computeFence != nullptr) {
        glDeleteSync(m_computeFence);
        m_computeFence = nullptr;
    }
    m_computeInFlight = false;
}

} // namespace trace
