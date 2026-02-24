#pragma once

#include <array>

#include <QMainWindow>

#include "render/RenderWidget.h"

QT_BEGIN_NAMESPACE
class QLabel;
class QDoubleSpinBox;
class QSlider;
class QPushButton;
class QTimer;
class QCheckBox;
QT_END_NAMESPACE

namespace trace {

struct SphereControlSet {
    QDoubleSpinBox* posX = nullptr;
    QDoubleSpinBox* posY = nullptr;
    QDoubleSpinBox* posZ = nullptr;
    QSlider* radius = nullptr;
    QSlider* roughness = nullptr;
    QSlider* ior = nullptr;
    QSlider* transmission = nullptr;
    QCheckBox* diffuseEnabled = nullptr;
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override = default;

private:
    QWidget* createControlPanel();
    QWidget* createRenderSettingsPage();
    QWidget* createSpherePage(int sphereIndex, const QString& title, bool showTransmission);
    QWidget* createLightPage();
    void setupInfoPanel(QWidget* parent);

    void bindSphereControls(int sphereIndex, const SphereControlSet& controls, bool showTransmission);
    void refreshInfoPanel();

    static int toSlider(float value, float min, float max);
    static float fromSlider(int sliderValue, float min, float max);

private:
    RenderWidget* m_renderWidget = nullptr;

    QLabel* m_gpuTimeLabel = nullptr;
    QLabel* m_fpsLabel = nullptr;
    QLabel* m_frameCountLabel = nullptr;
    QLabel* m_stateLabel = nullptr;
    QLabel* m_bvhLabel = nullptr;
    QLabel* m_effectiveSppLabel = nullptr;
    QLabel* m_denoiseStateLabel = nullptr;

    QSlider* m_maxBouncesSlider = nullptr;
    QSlider* m_alphaSlider = nullptr;
    QSlider* m_lightSamplesSlider = nullptr;
    QSlider* m_sppSlider = nullptr;
    QSlider* m_sppBudgetSlider = nullptr;
    QSlider* m_settleDelaySlider = nullptr;
    QSlider* m_lightIntensitySlider = nullptr;
    QLabel* m_sppValueLabel = nullptr;
    QLabel* m_sppBudgetValueLabel = nullptr;
    QLabel* m_settleDelayValueLabel = nullptr;
    QCheckBox* m_denoiseEnabledCheck = nullptr;
    QCheckBox* m_debugMonochromaticCheck = nullptr;

    QPushButton* m_lightColorButton = nullptr;

    std::array<SphereControlSet, 3> m_sphereControls;
    QTimer* m_infoTimer = nullptr;
};

} // namespace trace
