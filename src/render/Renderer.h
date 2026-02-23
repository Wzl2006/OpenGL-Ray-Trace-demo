#pragma once

#include <array>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <QElapsedTimer>
#include <QOpenGLFunctions_4_3_Core>

#if defined(TRACE_WITH_OIDN)
#include <OpenImageDenoise/oidn.hpp>
#endif

#include "scene/BvhBuilder.h"
#include "scene/SceneData.h"

namespace trace {

struct RenderStats {
    double gpuTimeMs = 0.0;
    double fps = 0.0;
    uint64_t accumulatedFrameCount = 0;
    bool isDynamic = false;
    bool bvhRebuildPending = false;
    int effectiveSppPerFrame = 1;
    bool denoiseEnabled = false;
};

class Renderer : protected QOpenGLFunctions_4_3_Core {
public:
    enum class DenoiserBackend {
        None = 0,
        BuiltinCompute = 1,
        OidnCpu = 2
    };

    Renderer();
    ~Renderer();

    void initialize();
    void shutdown();
    void resize(int width, int height);
    void applyChanges(const SceneData& scene, const RenderParams& params, SceneDirtyFlags dirtyFlags);
    void renderFrame();

    [[nodiscard]] RenderStats stats() const;

private:
    bool createPrograms();
    bool createBuffers();
    bool createTextures();
    bool uploadFullScene();
    bool uploadMaterialsAndSpheres();
    bool rebuildBvhAndUpload();
    void uploadBufferData(GLuint buffer, GLenum target, GLsizeiptr size, const void* data);
    void updateCameraBuffer();
    void updateParamsBuffer();
    void clearAccumulationTextures();
    void runDenoiserIfEnabled();
    void runBuiltinDenoiser();
    std::string loadTextFile(const std::string& path) const;
    GLuint compileShader(GLenum stage, const std::string& source, const std::string& label);
    GLuint linkProgram(const std::vector<GLuint>& shaders, const std::string& label);
    void releaseResources();
#if defined(TRACE_WITH_OIDN)
    bool initializeOidn();
    void shutdownOidn();
    bool captureOidnInput();
    void uploadOidnResultIfReady();
    void enqueueOidnJob();
    void oidnWorkerMain();
#endif

private:
    int m_width = 1280;
    int m_height = 720;
    bool m_initialized = false;

    SceneData m_scene;
    RenderParams m_params;
    SceneDirtyFlags m_pendingDirty = SceneDirtyFlags::All;

    bool m_bvhRebuildPending = true;
    uint64_t m_frameIndex = 0;
    uint64_t m_frameSinceReset = 0;
    bool m_forceResetThisFrame = true;

    GLuint m_computeProgram = 0;
    GLuint m_denoiseProgram = 0;
    GLuint m_displayProgram = 0;
    GLuint m_fullscreenVao = 0;

    GLuint m_accumTexture = 0;
    GLuint m_outputTexture = 0;
    GLuint m_beautyTexture = 0;
    GLuint m_normalTexture = 0;
    GLuint m_albedoTexture = 0;
    GLuint m_denoisedTexture = 0;

    GLuint m_bvhSsbo = 0;
    GLuint m_triangleSsbo = 0;
    GLuint m_sphereSsbo = 0;
    GLuint m_materialSsbo = 0;
    GLuint m_cameraUbo = 0;
    GLuint m_paramUbo = 0;
    GLuint m_lightUbo = 0;

    std::array<GLuint, 3> m_timeQueries = {};
    int m_queryWriteIndex = 0;
    int m_queryReadIndex = 0;

    BvhBuilder m_bvhBuilder;
    BvhBuildOutput m_bvh;

    RenderStats m_stats;
    QElapsedTimer m_fpsTimer;
    uint64_t m_fpsFrames = 0;
    DenoiserBackend m_denoiserBackend = DenoiserBackend::None;
    QElapsedTimer m_sceneIdleTimer;
    qint64 m_lastChangeMs = 0;
    int m_effectiveSppPerFrame = 1;
    bool m_hasDenoisedFrame = false;
    uint64_t m_resetSerial = 0;
#if defined(TRACE_WITH_OIDN)
    struct OidnJob {
        int width = 0;
        int height = 0;
        uint64_t resetSerial = 0;
        std::vector<float> color;
        std::vector<float> normal;
        std::vector<float> albedo;
    };

    struct OidnResult {
        int width = 0;
        int height = 0;
        uint64_t resetSerial = 0;
        std::vector<float> color;
    };

    oidn::DeviceRef m_oidnDevice;
    std::thread m_oidnWorker;
    std::mutex m_oidnMutex;
    std::condition_variable m_oidnCv;
    bool m_oidnExitRequested = false;
    bool m_oidnJobPending = false;
    bool m_oidnJobInFlight = false;
    bool m_oidnResultReady = false;
    OidnJob m_oidnPendingJob;
    OidnResult m_oidnResult;
    std::vector<float> m_oidnColorReadback;
    std::vector<float> m_oidnNormalReadback;
    std::vector<float> m_oidnAlbedoReadback;
    std::vector<float> m_oidnUploadRgba;
    qint64 m_lastOidnKickMs = 0;
#endif
};

} // namespace trace
