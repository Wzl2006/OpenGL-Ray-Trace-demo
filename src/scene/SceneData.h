#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace trace {

struct MaterialDefinition {
    glm::vec3 albedo = glm::vec3(1.0f);
    float roughness = 0.0f;
    glm::vec3 emission = glm::vec3(0.0f);
    float transmission = 0.0f;
    float ior = 1.0f;
    glm::vec3 absorption = glm::vec3(0.0f);
    int enableDiffuse = 1;
};

struct SphereDefinition {
    glm::vec3 center = glm::vec3(0.0f);
    float radius = 0.5f;
    int materialIndex = 0;
};

struct TriangleDefinition {
    glm::vec3 v0 = glm::vec3(0.0f);
    glm::vec3 v1 = glm::vec3(0.0f);
    glm::vec3 v2 = glm::vec3(0.0f);
    int materialIndex = 0;
};

struct LightDefinition {
    glm::vec3 color = glm::vec3(1.0f);
    float intensity = 5.0f;
    glm::vec3 center = glm::vec3(0.0f, 1.5f, 0.0f);
    glm::vec2 size = glm::vec2(1.0f, 1.0f);
    glm::vec3 normal = glm::vec3(0.0f, -1.0f, 0.0f);
    int materialIndex = 0;
};

struct CameraState {
    glm::vec3 target = glm::vec3(0.0f);
    float yawDeg = 0.0f;
    float pitchDeg = 0.0f;
    float distance = 5.0f;
    float fovYDeg = 45.0f;
};

struct RenderParams {
    int maxBounces = 6;
    float staticAlpha = 0.05f;
    int areaLightSamples = 3;
    int sppPerPixel = 1;
    int sppBudgetPerFrame = 32;
    int settleDelayMs = 250;
    int denoiseEnabled = 1;
    int denoiseMinAccumFrames = 16;
    int denoiseIntervalMs = 200;
    float denoiseBlend = 0.85f;
    float internalScale = 1.0f;
    int debugMonochromaticMode = 0;
    float debugWavelengthNm = 550.0f;
};

struct SceneData {
    std::vector<TriangleDefinition> triangles;
    std::vector<MaterialDefinition> materials;
    std::array<SphereDefinition, 3> spheres;
    LightDefinition light;
    CameraState camera;

    static SceneData createDefault();
};

enum class SceneDirtyFlags : uint32_t {
    None = 0,
    CameraChanged = 1u << 0u,
    MaterialChanged = 1u << 1u,
    LightChanged = 1u << 2u,
    SphereTransformChanged = 1u << 3u,
    RenderParamChanged = 1u << 4u,
    All = 0xFFFFFFFFu
};

constexpr SceneDirtyFlags operator|(SceneDirtyFlags lhs, SceneDirtyFlags rhs) {
    return static_cast<SceneDirtyFlags>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

constexpr SceneDirtyFlags operator&(SceneDirtyFlags lhs, SceneDirtyFlags rhs) {
    return static_cast<SceneDirtyFlags>(static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs));
}

inline SceneDirtyFlags& operator|=(SceneDirtyFlags& lhs, SceneDirtyFlags rhs) {
    lhs = lhs | rhs;
    return lhs;
}

inline bool hasFlag(SceneDirtyFlags value, SceneDirtyFlags flag) {
    return (value & flag) != SceneDirtyFlags::None;
}

struct alignas(16) GpuTriangle {
    glm::vec4 v0;
    glm::vec4 v1;
    glm::vec4 v2;
    glm::vec4 normalMaterial;
};

struct alignas(16) GpuBvhNode {
    glm::vec4 bboxMin;
    glm::vec4 bboxMax;
    glm::ivec4 meta;
};

struct alignas(16) GpuSphere {
    glm::vec4 centerRadius;
    glm::ivec4 meta;
};

struct alignas(16) GpuMaterial {
    glm::vec4 albedoRoughness;
    glm::vec4 emissionTransmission;
    glm::vec4 iorAbsorption;
    glm::vec4 options; // x diffuseEnabled(0/1)
};

struct alignas(16) GpuLight {
    glm::vec4 centerIntensity;
    glm::vec4 colorSizeX;
    glm::vec4 normalSizeY;
};

struct alignas(16) GpuCamera {
    glm::mat4 invView;
    glm::mat4 invProj;
    glm::vec4 cameraPosition;
    glm::vec4 viewportAndFrame;
};

struct alignas(16) GpuRenderParams {
    glm::ivec4 counts;
    glm::ivec4 options;
    glm::vec4 alphaAndMode;
    glm::vec4 cauchy;
    glm::vec4 debug;
};

struct BvhBuildOutput {
    std::vector<GpuBvhNode> nodes;
    std::vector<GpuTriangle> triangles;
};

glm::vec3 cameraPosition(const CameraState& camera);
glm::mat4 viewMatrix(const CameraState& camera);
glm::mat4 projectionMatrix(const CameraState& camera, float aspect);

} // namespace trace
