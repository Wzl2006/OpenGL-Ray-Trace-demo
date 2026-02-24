#pragma once

#include <QColor>
#include <QMouseEvent>
#include <QOpenGLWidget>
#include <QWheelEvent>

#include "render/Renderer.h"
#include "scene/SceneData.h"

namespace trace {

class RenderWidget : public QOpenGLWidget {
    Q_OBJECT

public:
    explicit RenderWidget(QWidget* parent = nullptr);
    ~RenderWidget() override;

    [[nodiscard]] const SceneData& scene() const { return m_scene; }
    [[nodiscard]] const RenderParams& renderParams() const { return m_renderParams; }
    [[nodiscard]] RenderStats stats() const { return m_renderer.stats(); }

    void setMaxBounces(int bounces);
    void setStaticAlpha(float alpha);
    void setLightSampleCount(int samples);
    void setSppPerPixel(int sppPerPixel);
    void setSppBudgetPerFrame(int sppBudget);
    void setSettleDelayMs(int delayMs);
    void setDenoiseEnabled(bool enabled);
    void setDebugMonochromaticMode(bool enabled);

    void setLightColor(const QColor& color);
    void setLightIntensity(float intensity);

    void setSpherePosition(int sphereIndex, const glm::vec3& position);
    void setSphereRadius(int sphereIndex, float radius);
    void setSphereRoughness(int sphereIndex, float roughness);
    void setSphereIor(int sphereIndex, float ior);
    void setSphereTransmission(int sphereIndex, float transmission);
    void setSphereDiffuseEnabled(int sphereIndex, bool enabled);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void markDirty(SceneDirtyFlags flags);
    MaterialDefinition& sphereMaterial(int sphereIndex);

private:
    Renderer m_renderer;
    SceneData m_scene;
    RenderParams m_renderParams;
    SceneDirtyFlags m_dirtyFlags = SceneDirtyFlags::All;

    QPoint m_lastMousePos;
};

} // namespace trace
