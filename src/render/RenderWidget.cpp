#include "render/RenderWidget.h"

#include <algorithm>
#include <cmath>

#include <QColor>
#include <QTimer>

namespace trace {

namespace {

constexpr float kYawLimit = 60.0f;
constexpr float kPitchLimit = 20.0f;
constexpr float kDistanceMin = 2.0f;
constexpr float kDistanceMax = 10.0f;
constexpr int kMaxBouncesLimit = 20;
constexpr int kMaxLightSamplesLimit = 32;
constexpr int kMaxSppPerPixelLimit = 2048;
constexpr int kMaxSppBudgetPerFrameLimit = 256;
constexpr int kMaxSettleDelayMs = 2000;

} // namespace

RenderWidget::RenderWidget(QWidget* parent)
    : QOpenGLWidget(parent)
    , m_scene(SceneData::createDefault()) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setFixedSize(720, 720);
}

RenderWidget::~RenderWidget() {
    makeCurrent();
    m_renderer.shutdown();
    doneCurrent();
}

void RenderWidget::setMaxBounces(int bounces) {
    m_renderParams.maxBounces = std::max(1, std::min(kMaxBouncesLimit, bounces));
    markDirty(SceneDirtyFlags::RenderParamChanged);
}

void RenderWidget::setStaticAlpha(float alpha) {
    m_renderParams.staticAlpha = std::clamp(alpha, 0.05f, 1.0f);
    markDirty(SceneDirtyFlags::RenderParamChanged);
}

void RenderWidget::setLightSampleCount(int samples) {
    m_renderParams.areaLightSamples = std::max(1, std::min(kMaxLightSamplesLimit, samples));
    markDirty(SceneDirtyFlags::RenderParamChanged);
}

void RenderWidget::setSppPerPixel(int sppPerPixel) {
    m_renderParams.sppPerPixel = std::max(1, std::min(kMaxSppPerPixelLimit, sppPerPixel));
    markDirty(SceneDirtyFlags::RenderParamChanged);
}

void RenderWidget::setSppBudgetPerFrame(int sppBudget) {
    m_renderParams.sppBudgetPerFrame = std::max(1, std::min(kMaxSppBudgetPerFrameLimit, sppBudget));
    markDirty(SceneDirtyFlags::RenderParamChanged);
}

void RenderWidget::setSettleDelayMs(int delayMs) {
    m_renderParams.settleDelayMs = std::max(0, std::min(kMaxSettleDelayMs, delayMs));
    markDirty(SceneDirtyFlags::RenderParamChanged);
}

void RenderWidget::setDenoiseEnabled(bool enabled) {
    m_renderParams.denoiseEnabled = enabled ? 1 : 0;
    markDirty(SceneDirtyFlags::RenderParamChanged);
}

void RenderWidget::setDenoiseSigma(float sigma) {
    m_renderParams.denoiseSigma = std::clamp(sigma, 0.01f, 0.5f);
    markDirty(SceneDirtyFlags::RenderParamChanged);
}

void RenderWidget::setDebugMonochromaticMode(bool enabled) {
    m_renderParams.debugMonochromaticMode = enabled ? 1 : 0;
    markDirty(SceneDirtyFlags::RenderParamChanged);
}

void RenderWidget::setLightColor(const QColor& color) {
    m_scene.light.color = glm::vec3(color.redF(), color.greenF(), color.blueF());
    const int materialIndex = m_scene.light.materialIndex;
    if (materialIndex >= 0 && materialIndex < static_cast<int>(m_scene.materials.size())) {
        m_scene.materials[static_cast<size_t>(materialIndex)].emission = m_scene.light.color;
    }
    markDirty(SceneDirtyFlags::LightChanged | SceneDirtyFlags::MaterialChanged);
}

void RenderWidget::setLightIntensity(float intensity) {
    m_scene.light.intensity = std::clamp(intensity, 0.0f, 10.0f);
    markDirty(SceneDirtyFlags::LightChanged);
}

void RenderWidget::setSpherePosition(int sphereIndex, const glm::vec3& position) {
    if (sphereIndex < 0 || sphereIndex >= static_cast<int>(m_scene.spheres.size())) {
        return;
    }
    m_scene.spheres[static_cast<size_t>(sphereIndex)].center = position;
    markDirty(SceneDirtyFlags::SphereTransformChanged);
}

void RenderWidget::setSphereRadius(int sphereIndex, float radius) {
    if (sphereIndex < 0 || sphereIndex >= static_cast<int>(m_scene.spheres.size())) {
        return;
    }
    m_scene.spheres[static_cast<size_t>(sphereIndex)].radius = std::max(radius, 0.1f);
    markDirty(SceneDirtyFlags::SphereTransformChanged);
}

void RenderWidget::setSphereRoughness(int sphereIndex, float roughness) {
    sphereMaterial(sphereIndex).roughness = std::clamp(roughness, 0.0f, 0.8f);
    markDirty(SceneDirtyFlags::MaterialChanged);
}

void RenderWidget::setSphereIor(int sphereIndex, float ior) {
    sphereMaterial(sphereIndex).ior = std::clamp(ior, 1.0f, 2.5f);
    markDirty(SceneDirtyFlags::MaterialChanged);
}

void RenderWidget::setSphereTransmission(int sphereIndex, float transmission) {
    sphereMaterial(sphereIndex).transmission = std::clamp(transmission, 0.0f, 1.0f);
    markDirty(SceneDirtyFlags::MaterialChanged);
}

void RenderWidget::setSphereDiffuseEnabled(int sphereIndex, bool enabled) {
    sphereMaterial(sphereIndex).enableDiffuse = enabled ? 1 : 0;
    markDirty(SceneDirtyFlags::MaterialChanged);
}

void RenderWidget::initializeGL() {
    m_renderer.initialize();
    m_renderer.applyChanges(m_scene, m_renderParams, SceneDirtyFlags::All);
}

void RenderWidget::resizeGL(int w, int h) {
    const qreal dpr = devicePixelRatioF();
    const int fbWidth = std::max(1, static_cast<int>(std::lround(static_cast<double>(w) * dpr)));
    const int fbHeight = std::max(1, static_cast<int>(std::lround(static_cast<double>(h) * dpr)));
    m_renderer.resize(fbWidth, fbHeight);
    markDirty(SceneDirtyFlags::CameraChanged);
}

void RenderWidget::paintGL() {
    m_renderer.applyChanges(m_scene, m_renderParams, m_dirtyFlags);
    m_dirtyFlags = SceneDirtyFlags::None;
    m_renderer.renderFrame();
    update();
}

void RenderWidget::mousePressEvent(QMouseEvent* event) {
    m_lastMousePos = event->pos();
    QOpenGLWidget::mousePressEvent(event);
}

void RenderWidget::mouseMoveEvent(QMouseEvent* event) {
    if (!(event->buttons() & Qt::LeftButton)) {
        QOpenGLWidget::mouseMoveEvent(event);
        return;
    }

    const QPoint delta = event->pos() - m_lastMousePos;
    m_lastMousePos = event->pos();

    m_scene.camera.yawDeg += static_cast<float>(delta.x()) * 0.35f;
    m_scene.camera.pitchDeg -= static_cast<float>(delta.y()) * 0.25f;
    m_scene.camera.yawDeg = std::clamp(m_scene.camera.yawDeg, -kYawLimit, kYawLimit);
    m_scene.camera.pitchDeg = std::clamp(m_scene.camera.pitchDeg, -kPitchLimit, kPitchLimit);
    markDirty(SceneDirtyFlags::CameraChanged);

    QOpenGLWidget::mouseMoveEvent(event);
}

void RenderWidget::wheelEvent(QWheelEvent* event) {
    const float scroll = static_cast<float>(event->angleDelta().y()) / 120.0f;
    const float scale = 1.0f - scroll * 0.1f;
    m_scene.camera.distance = std::clamp(m_scene.camera.distance * scale, kDistanceMin, kDistanceMax);
    markDirty(SceneDirtyFlags::CameraChanged);
    QOpenGLWidget::wheelEvent(event);
}

void RenderWidget::markDirty(SceneDirtyFlags flags) {
    m_dirtyFlags |= flags;
    emit sceneChanged();
    update();
}

MaterialDefinition& RenderWidget::sphereMaterial(int sphereIndex) {
    const int materialIndex = m_scene.spheres[static_cast<size_t>(sphereIndex)].materialIndex;
    return m_scene.materials[static_cast<size_t>(materialIndex)];
}

} // namespace trace
