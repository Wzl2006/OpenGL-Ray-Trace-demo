#include "app/MainWindow.h"

#include <algorithm>

#include <QColorDialog>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QToolBox>
#include <QVBoxLayout>

#include <glm/glm.hpp>

namespace trace {

namespace {

constexpr int kRenderViewportSize = 720;
constexpr int kControlPanelWidth = 300;
constexpr int kMaxBouncesSliderMax = 20;
constexpr int kAreaLightSamplesSliderMax = 32;
constexpr int kSppSliderMax = 2048;
constexpr int kSppBudgetSliderMax = 256;
constexpr int kSettleDelaySliderMax = 2000;

QSlider* createSlider(int min, int max, int value) {
    QSlider* slider = new QSlider(Qt::Horizontal);
    slider->setRange(min, max);
    slider->setValue(value);
    return slider;
}

QDoubleSpinBox* createSpinBox(double min, double max, double step, double value) {
    QDoubleSpinBox* box = new QDoubleSpinBox();
    box->setDecimals(3);
    box->setRange(min, max);
    box->setSingleStep(step);
    box->setValue(value);
    return box;
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle("OpenGL Compute Ray Tracing Demo");

    QWidget* central = new QWidget(this);
    setCentralWidget(central);

    auto* rootLayout = new QHBoxLayout(central);
    rootLayout->setContentsMargins(8, 8, 8, 8);
    rootLayout->setSpacing(8);

    QWidget* renderHost = new QWidget(central);
    auto* renderLayout = new QVBoxLayout(renderHost);
    renderLayout->setContentsMargins(0, 0, 0, 0);
    renderLayout->addStretch(1);
    m_renderWidget = new RenderWidget(renderHost);
    renderLayout->addWidget(m_renderWidget, 0, Qt::AlignCenter);
    renderLayout->addStretch(1);
    rootLayout->addWidget(renderHost, 1);

    QWidget* panel = createControlPanel();
    panel->setFixedWidth(kControlPanelWidth);
    rootLayout->addWidget(panel, 0, Qt::AlignTop);

    setFixedSize(kRenderViewportSize + kControlPanelWidth + 24, kRenderViewportSize + 16);
}

QWidget* MainWindow::createControlPanel() {
    QWidget* panel = new QWidget(this);
    auto* panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(6);

    auto* toolbox = new QToolBox(panel);
    toolbox->addItem(createRenderSettingsPage(), tr("渲染设置"));
    toolbox->addItem(createSpherePage(0, tr("球体 1(镜面)"), false), tr("球体 1"));
    toolbox->addItem(createSpherePage(1, tr("球体 2(透明)"), false), tr("球体 2"));
    toolbox->addItem(createSpherePage(2, tr("球体 3(半透明)"), true), tr("球体 3"));
    toolbox->addItem(createLightPage(), tr("光源"));
    panelLayout->addWidget(toolbox, 1);

    QWidget* infoBox = new QWidget(panel);
    infoBox->setMinimumHeight(180);
    setupInfoPanel(infoBox);
    panelLayout->addWidget(infoBox, 0);

    m_infoTimer = new QTimer(this);
    m_infoTimer->setInterval(250);
    connect(m_infoTimer, &QTimer::timeout, this, &MainWindow::refreshInfoPanel);
    m_infoTimer->start();
    refreshInfoPanel();

    return panel;
}

QWidget* MainWindow::createRenderSettingsPage() {
    QWidget* page = new QWidget(this);
    auto* layout = new QFormLayout(page);

    m_maxBouncesSlider = createSlider(1, kMaxBouncesSliderMax, m_renderWidget->renderParams().maxBounces);
    connect(m_maxBouncesSlider, &QSlider::valueChanged, m_renderWidget, &RenderWidget::setMaxBounces);
    layout->addRow(tr("最大弹射次数"), m_maxBouncesSlider);

    m_alphaSlider = createSlider(5, 100, toSlider(m_renderWidget->renderParams().staticAlpha, 0.05f, 1.0f));
    connect(m_alphaSlider, &QSlider::valueChanged, this, [this](int value) {
        m_renderWidget->setStaticAlpha(fromSlider(value, 0.05f, 1.0f));
    });
    layout->addRow(tr("静态 alpha"), m_alphaSlider);

    m_lightSamplesSlider = createSlider(1, kAreaLightSamplesSliderMax, m_renderWidget->renderParams().areaLightSamples);
    connect(m_lightSamplesSlider, &QSlider::valueChanged, m_renderWidget, &RenderWidget::setLightSampleCount);
    layout->addRow(tr("面光源采样次数"), m_lightSamplesSlider);

    QWidget* sppRowWidget = new QWidget(page);
    auto* sppRowLayout = new QHBoxLayout(sppRowWidget);
    sppRowLayout->setContentsMargins(0, 0, 0, 0);
    m_sppSlider = createSlider(1, kSppSliderMax, m_renderWidget->renderParams().sppPerPixel);
    m_sppValueLabel = new QLabel(QString::number(m_renderWidget->renderParams().sppPerPixel), sppRowWidget);
    m_sppValueLabel->setMinimumWidth(56);
    sppRowLayout->addWidget(m_sppSlider, 1);
    sppRowLayout->addWidget(m_sppValueLabel, 0);
    connect(m_sppSlider, &QSlider::valueChanged, this, [this](int value) {
        m_renderWidget->setSppPerPixel(value);
        if (m_sppValueLabel != nullptr) {
            m_sppValueLabel->setText(QString::number(value));
        }
    });
    layout->addRow(tr("SPP / Frame"), sppRowWidget);

    QWidget* budgetRowWidget = new QWidget(page);
    auto* budgetRowLayout = new QHBoxLayout(budgetRowWidget);
    budgetRowLayout->setContentsMargins(0, 0, 0, 0);
    m_sppBudgetSlider = createSlider(1, kSppBudgetSliderMax, m_renderWidget->renderParams().sppBudgetPerFrame);
    m_sppBudgetValueLabel = new QLabel(QString::number(m_renderWidget->renderParams().sppBudgetPerFrame), budgetRowWidget);
    m_sppBudgetValueLabel->setMinimumWidth(56);
    budgetRowLayout->addWidget(m_sppBudgetSlider, 1);
    budgetRowLayout->addWidget(m_sppBudgetValueLabel, 0);
    connect(m_sppBudgetSlider, &QSlider::valueChanged, this, [this](int value) {
        m_renderWidget->setSppBudgetPerFrame(value);
        if (m_sppBudgetValueLabel != nullptr) {
            m_sppBudgetValueLabel->setText(QString::number(value));
        }
    });
    layout->addRow(tr("SPP Budget"), budgetRowWidget);

    QWidget* delayRowWidget = new QWidget(page);
    auto* delayRowLayout = new QHBoxLayout(delayRowWidget);
    delayRowLayout->setContentsMargins(0, 0, 0, 0);
    m_settleDelaySlider = createSlider(0, kSettleDelaySliderMax, m_renderWidget->renderParams().settleDelayMs);
    m_settleDelayValueLabel = new QLabel(QString("%1 ms").arg(m_renderWidget->renderParams().settleDelayMs), delayRowWidget);
    m_settleDelayValueLabel->setMinimumWidth(56);
    delayRowLayout->addWidget(m_settleDelaySlider, 1);
    delayRowLayout->addWidget(m_settleDelayValueLabel, 0);
    connect(m_settleDelaySlider, &QSlider::valueChanged, this, [this](int value) {
        m_renderWidget->setSettleDelayMs(value);
        if (m_settleDelayValueLabel != nullptr) {
            m_settleDelayValueLabel->setText(QString("%1 ms").arg(value));
        }
    });
    layout->addRow(tr("Settle Delay"), delayRowWidget);

    m_denoiseEnabledCheck = new QCheckBox(tr("Enable Denoise"), page);
    m_denoiseEnabledCheck->setChecked(m_renderWidget->renderParams().denoiseEnabled != 0);
    connect(m_denoiseEnabledCheck, &QCheckBox::toggled, m_renderWidget, &RenderWidget::setDenoiseEnabled);
    layout->addRow(m_denoiseEnabledCheck);

    m_debugMonochromaticCheck = new QCheckBox(tr("Debug Monochromatic (550nm)"), page);
    m_debugMonochromaticCheck->setChecked(m_renderWidget->renderParams().debugMonochromaticMode != 0);
    connect(m_debugMonochromaticCheck, &QCheckBox::toggled, m_renderWidget, &RenderWidget::setDebugMonochromaticMode);
    layout->addRow(m_debugMonochromaticCheck);

    QLabel* hint = new QLabel(tr("注：渲染时间统计使用 GPU Timer Query。"), page);
    hint->setWordWrap(true);
    layout->addRow(hint);

    return page;
}

QWidget* MainWindow::createSpherePage(int sphereIndex, const QString& title, bool showTransmission) {
    QWidget* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    QGroupBox* group = new QGroupBox(title, page);
    auto* form = new QFormLayout(group);

    const SceneData& scene = m_renderWidget->scene();
    const SphereDefinition& sphere = scene.spheres[static_cast<size_t>(sphereIndex)];
    const MaterialDefinition& material = scene.materials[static_cast<size_t>(sphere.materialIndex)];

    SphereControlSet controls{};
    controls.posX = createSpinBox(-1.4, 1.4, 0.01, sphere.center.x);
    controls.posY = createSpinBox(-1.4, 1.4, 0.01, sphere.center.y);
    controls.posZ = createSpinBox(-1.4, 1.4, 0.01, sphere.center.z);
    controls.radius = createSlider(20, 90, toSlider(sphere.radius, 0.2f, 0.9f));
    controls.roughness = createSlider(0, 80, toSlider(material.roughness, 0.0f, 0.8f));
    controls.ior = createSlider(100, 250, static_cast<int>(material.ior * 100.0f));
    controls.diffuseEnabled = new QCheckBox(tr("Enable Diffuse Branch"), group);
    controls.diffuseEnabled->setChecked(material.enableDiffuse != 0);
    if (showTransmission) {
        controls.transmission = createSlider(30, 80, toSlider(material.transmission, 0.3f, 0.8f));
    }

    form->addRow(tr("位置 X"), controls.posX);
    form->addRow(tr("位置 Y"), controls.posY);
    form->addRow(tr("位置 Z"), controls.posZ);
    form->addRow(tr("半径"), controls.radius);
    form->addRow(tr("粗糙度"), controls.roughness);
    form->addRow(tr("折射率"), controls.ior);
    form->addRow(controls.diffuseEnabled);
    if (showTransmission) {
        form->addRow(tr("透明度"), controls.transmission);
    }

    layout->addWidget(group);
    layout->addStretch(1);
    m_sphereControls[static_cast<size_t>(sphereIndex)] = controls;
    bindSphereControls(sphereIndex, controls, showTransmission);
    return page;
}

QWidget* MainWindow::createLightPage() {
    QWidget* page = new QWidget(this);
    auto* layout = new QFormLayout(page);

    m_lightColorButton = new QPushButton(tr("选择颜色"), page);
    connect(m_lightColorButton, &QPushButton::clicked, this, [this]() {
        const glm::vec3 color = m_renderWidget->scene().light.color;
        QColor initial = QColor::fromRgbF(color.r, color.g, color.b);
        QColor picked = QColorDialog::getColor(initial, this, tr("选择光源颜色"));
        if (picked.isValid()) {
            m_renderWidget->setLightColor(picked);
            const QString style = QString("background-color: rgb(%1,%2,%3);")
                                      .arg(picked.red())
                                      .arg(picked.green())
                                      .arg(picked.blue());
            m_lightColorButton->setStyleSheet(style);
        }
    });
    layout->addRow(tr("颜色"), m_lightColorButton);

    m_lightIntensitySlider = createSlider(0, 100, toSlider(m_renderWidget->scene().light.intensity, 0.0f, 10.0f));
    connect(m_lightIntensitySlider, &QSlider::valueChanged, this, [this](int value) {
        m_renderWidget->setLightIntensity(fromSlider(value, 0.0f, 10.0f));
    });
    layout->addRow(tr("强度"), m_lightIntensitySlider);
    return page;
}

void MainWindow::setupInfoPanel(QWidget* parent) {
    auto* layout = new QFormLayout(parent);
    m_gpuTimeLabel = new QLabel("-", parent);
    m_fpsLabel = new QLabel("-", parent);
    m_frameCountLabel = new QLabel("-", parent);
    m_stateLabel = new QLabel("-", parent);
    m_bvhLabel = new QLabel("-", parent);
    m_effectiveSppLabel = new QLabel("-", parent);
    m_denoiseStateLabel = new QLabel("-", parent);

    layout->addRow(tr("GPU 渲染时间 (ms)"), m_gpuTimeLabel);
    layout->addRow(tr("FPS"), m_fpsLabel);
    layout->addRow(tr("累计帧数"), m_frameCountLabel);
    layout->addRow(tr("Effective SPP"), m_effectiveSppLabel);
    layout->addRow(tr("渲染状态"), m_stateLabel);
    layout->addRow(tr("BVH 重建"), m_bvhLabel);
    layout->addRow(tr("Denoise"), m_denoiseStateLabel);
}

void MainWindow::bindSphereControls(int sphereIndex, const SphereControlSet& controls, bool showTransmission) {
    auto applyPosition = [this, sphereIndex, controls]() {
        glm::vec3 p(static_cast<float>(controls.posX->value()),
                    static_cast<float>(controls.posY->value()),
                    static_cast<float>(controls.posZ->value()));
        m_renderWidget->setSpherePosition(sphereIndex, p);
    };
    connect(controls.posX, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [applyPosition](double) { applyPosition(); });
    connect(controls.posY, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [applyPosition](double) { applyPosition(); });
    connect(controls.posZ, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [applyPosition](double) { applyPosition(); });

    connect(controls.radius, &QSlider::valueChanged, this, [this, sphereIndex](int value) {
        m_renderWidget->setSphereRadius(sphereIndex, fromSlider(value, 0.2f, 0.9f));
    });
    connect(controls.roughness, &QSlider::valueChanged, this, [this, sphereIndex](int value) {
        m_renderWidget->setSphereRoughness(sphereIndex, fromSlider(value, 0.0f, 0.8f));
    });
    connect(controls.ior, &QSlider::valueChanged, this, [this, sphereIndex](int value) {
        m_renderWidget->setSphereIor(sphereIndex, static_cast<float>(value) / 100.0f);
    });
    if (controls.diffuseEnabled != nullptr) {
        connect(controls.diffuseEnabled, &QCheckBox::toggled, this, [this, sphereIndex](bool checked) {
            m_renderWidget->setSphereDiffuseEnabled(sphereIndex, checked);
        });
    }
    if (showTransmission && controls.transmission != nullptr) {
        connect(controls.transmission, &QSlider::valueChanged, this, [this, sphereIndex](int value) {
            m_renderWidget->setSphereTransmission(sphereIndex, fromSlider(value, 0.3f, 0.8f));
        });
    }
}

void MainWindow::refreshInfoPanel() {
    const RenderStats stats = m_renderWidget->stats();
    m_gpuTimeLabel->setText(QString::number(stats.gpuTimeMs, 'f', 3));
    m_fpsLabel->setText(QString::number(stats.fps, 'f', 2));
    m_frameCountLabel->setText(QString::number(stats.accumulatedFrameCount));
    m_effectiveSppLabel->setText(QString::number(stats.effectiveSppPerFrame));
    m_stateLabel->setText(stats.isDynamic ? tr("动态") : tr("静态"));
    m_bvhLabel->setText(stats.bvhRebuildPending ? tr("待重建") : tr("就绪"));
    m_denoiseStateLabel->setText(stats.denoiseEnabled ? tr("开启") : tr("关闭"));
}

int MainWindow::toSlider(float value, float min, float max) {
    const float n = std::clamp((value - min) / (max - min), 0.0f, 1.0f);
    return static_cast<int>(n * 100.0f);
}

float MainWindow::fromSlider(int sliderValue, float min, float max) {
    const float n = static_cast<float>(sliderValue) / 100.0f;
    return min + (max - min) * n;
}

} // namespace trace
