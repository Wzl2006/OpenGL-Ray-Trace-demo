#include "scene/SceneData.h"

#include <algorithm>
#include <cmath>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

namespace trace {

namespace {

void addTriangle(std::vector<TriangleDefinition>& dst,
                 const glm::vec3& a,
                 const glm::vec3& b,
                 const glm::vec3& c,
                 int materialIndex,
                 const glm::vec3& inwardHint) {
    TriangleDefinition tri;
    tri.v0 = a;
    tri.v1 = b;
    tri.v2 = c;
    tri.materialIndex = materialIndex;

    const glm::vec3 normal = glm::normalize(glm::cross(tri.v1 - tri.v0, tri.v2 - tri.v0));
    if (glm::dot(normal, inwardHint) < 0.0f) {
        std::swap(tri.v1, tri.v2);
    }
    dst.push_back(tri);
}

void addQuad(std::vector<TriangleDefinition>& dst,
             const glm::vec3& p0,
             const glm::vec3& p1,
             const glm::vec3& p2,
             const glm::vec3& p3,
             int materialIndex,
             const glm::vec3& inwardHint) {
    addTriangle(dst, p0, p1, p2, materialIndex, inwardHint);
    addTriangle(dst, p0, p2, p3, materialIndex, inwardHint);
}

} // namespace

SceneData SceneData::createDefault() {
    SceneData scene;
    scene.materials.reserve(7);

    MaterialDefinition redWall;
    redWall.albedo = glm::vec3(0.8039f, 0.0f, 0.0f); // #CD0000
    redWall.roughness = 1.0f;
    redWall.enableDiffuse = 1;
    scene.materials.push_back(redWall); // 0

    MaterialDefinition greenWall;
    greenWall.albedo = glm::vec3(0.0f, 0.8039f, 0.0f); // #00CD00
    greenWall.roughness = 1.0f;
    greenWall.enableDiffuse = 1;
    scene.materials.push_back(greenWall); // 1

    MaterialDefinition whiteWall;
    whiteWall.albedo = glm::vec3(0.9608f); // #F5F5F5
    whiteWall.roughness = 1.0f;
    whiteWall.enableDiffuse = 1;
    scene.materials.push_back(whiteWall); // 2

    MaterialDefinition emissive;
    emissive.emission = glm::vec3(1.0f);
    emissive.albedo = glm::vec3(0.0f);
    emissive.transmission = 0.0f;
    emissive.enableDiffuse = 0;
    scene.materials.push_back(emissive); // 3

    MaterialDefinition mirrorLike;
    mirrorLike.albedo = glm::vec3(0.98f, 0.98f, 1.0f);
    mirrorLike.roughness = 0.02f;
    mirrorLike.transmission = 0.05f;
    mirrorLike.ior = 1.5f;
    mirrorLike.absorption = glm::vec3(0.01f, 0.01f, 0.02f);
    mirrorLike.enableDiffuse = 0;
    scene.materials.push_back(mirrorLike); // 4

    MaterialDefinition clearGlass;
    clearGlass.albedo = glm::vec3(0.96f, 0.98f, 1.0f);
    clearGlass.roughness = 0.04f;
    clearGlass.transmission = 0.95f;
    clearGlass.ior = 1.5f;
    clearGlass.absorption = glm::vec3(0.01f, 0.01f, 0.01f);
    clearGlass.enableDiffuse = 0;
    scene.materials.push_back(clearGlass); // 5

    MaterialDefinition frostedGlass;
    frostedGlass.albedo = glm::vec3(0.92f, 0.95f, 1.0f);
    frostedGlass.roughness = 0.5f;
    frostedGlass.transmission = 0.65f;
    frostedGlass.ior = 1.5f;
    frostedGlass.absorption = glm::vec3(0.08f, 0.08f, 0.05f);
    frostedGlass.enableDiffuse = 1;
    scene.materials.push_back(frostedGlass); // 6

    const float boxHalf = 1.5f;
    const float lightHalf = 0.5f;

    const glm::vec3 p000(-boxHalf, -boxHalf, -boxHalf);
    const glm::vec3 p001(-boxHalf, -boxHalf, boxHalf);
    const glm::vec3 p010(-boxHalf, boxHalf, -boxHalf);
    const glm::vec3 p011(-boxHalf, boxHalf, boxHalf);
    const glm::vec3 p100(boxHalf, -boxHalf, -boxHalf);
    const glm::vec3 p101(boxHalf, -boxHalf, boxHalf);
    const glm::vec3 p110(boxHalf, boxHalf, -boxHalf);
    const glm::vec3 p111(boxHalf, boxHalf, boxHalf);

    // Left wall (x = -1.5), inward +X.
    addQuad(scene.triangles, p000, p001, p011, p010, 0, glm::vec3(1.0f, 0.0f, 0.0f));
    // Right wall (x = +1.5), inward -X.
    addQuad(scene.triangles, p101, p100, p110, p111, 1, glm::vec3(-1.0f, 0.0f, 0.0f));
    // Back wall (z = -1.5), inward +Z.
    addQuad(scene.triangles, p100, p000, p010, p110, 2, glm::vec3(0.0f, 0.0f, 1.0f));
    // Floor (y = -1.5), inward +Y.
    addQuad(scene.triangles, p001, p101, p100, p000, 2, glm::vec3(0.0f, 1.0f, 0.0f));

    // Ceiling split into four white quads around a 1x1 embedded light.
    const float y = boxHalf;
    const float zNear = boxHalf;
    const float zFar = -boxHalf;
    const float xLeft = -boxHalf;
    const float xRight = boxHalf;
    const float l = -lightHalf;
    const float r = lightHalf;
    const float n = lightHalf;
    const float f = -lightHalf;
    const glm::vec3 inwardTop(0.0f, -1.0f, 0.0f);

    // Front strip.
    addQuad(scene.triangles,
            glm::vec3(xLeft, y, zNear),
            glm::vec3(xRight, y, zNear),
            glm::vec3(xRight, y, n),
            glm::vec3(xLeft, y, n),
            2,
            inwardTop);
    // Back strip.
    addQuad(scene.triangles,
            glm::vec3(xLeft, y, f),
            glm::vec3(xRight, y, f),
            glm::vec3(xRight, y, zFar),
            glm::vec3(xLeft, y, zFar),
            2,
            inwardTop);
    // Left strip.
    addQuad(scene.triangles,
            glm::vec3(xLeft, y, n),
            glm::vec3(l, y, n),
            glm::vec3(l, y, f),
            glm::vec3(xLeft, y, f),
            2,
            inwardTop);
    // Right strip.
    addQuad(scene.triangles,
            glm::vec3(r, y, n),
            glm::vec3(xRight, y, n),
            glm::vec3(xRight, y, f),
            glm::vec3(r, y, f),
            2,
            inwardTop);

    // Embedded area light.
    addQuad(scene.triangles,
            glm::vec3(l, y, n),
            glm::vec3(r, y, n),
            glm::vec3(r, y, f),
            glm::vec3(l, y, f),
            3,
            inwardTop);

    scene.light.color = glm::vec3(1.0f);
    scene.light.intensity = 5.0f;
    scene.light.center = glm::vec3(0.0f, boxHalf, 0.0f);
    scene.light.size = glm::vec2(1.0f, 1.0f);
    scene.light.normal = inwardTop;
    scene.light.materialIndex = 3;

    scene.spheres[0] = SphereDefinition{glm::vec3(-0.75f, -0.95f, -0.20f), 0.55f, 4};
    scene.spheres[1] = SphereDefinition{glm::vec3(0.15f, -0.95f, -0.65f), 0.55f, 5};
    scene.spheres[2] = SphereDefinition{glm::vec3(0.80f, -0.95f, 0.15f), 0.45f, 6};

    scene.camera.target = glm::vec3(0.0f);
    scene.camera.yawDeg = 0.0f;
    scene.camera.pitchDeg = 0.0f;
    scene.camera.distance = 5.0f;
    scene.camera.fovYDeg = 45.0f;

    return scene;
}

glm::vec3 cameraPosition(const CameraState& camera) {
    const float yaw = glm::radians(camera.yawDeg);
    const float pitch = glm::radians(camera.pitchDeg);
    const glm::vec3 offset(
        camera.distance * std::cos(pitch) * std::sin(yaw),
        camera.distance * std::sin(pitch),
        camera.distance * std::cos(pitch) * std::cos(yaw));
    return camera.target + offset;
}

glm::mat4 viewMatrix(const CameraState& camera) {
    return glm::lookAt(cameraPosition(camera), camera.target, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 projectionMatrix(const CameraState& camera, float aspect) {
    return glm::perspective(glm::radians(camera.fovYDeg), aspect, 0.1f, 100.0f);
}

} // namespace trace
