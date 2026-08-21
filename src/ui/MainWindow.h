#pragma once
#include <QMainWindow>
#include <vector>
#include "core/Simulation.h"
#include "core/SimulationResult.h"
#include "core/Studies.h"

class ControlsPanel;
class RayDiagramWidget;
class HeatmapWidget;
class OcctViewWidget;
class PlotWidget;
class PolarPlotWidget;
class SimulationWorker;
class StudyWorker;

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSlider;
class QSpinBox;
class QTabWidget;
class QTextBrowser;
class QTimer;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onSceneChanged();
    void onGeometryChanged();
    void onRun();
    void onCancel();
    void onProgress(int percent);
    void onResult(const SimulationResult& res);
    void onSurfacePicked(int index);
    void onCutMoved(double x, double y);

    void onRunConvergence();
    void onConvergenceReady(const std::vector<studies::ConvergencePoint>& points);
    void onFocusSweep();

    void onSaveConfig();
    void onLoadConfig();
    void onExportIrradianceCsv();
    void onExportIntensityCsv();
    void onExportMetricsCsv();
    void onExportImage();

private:
    QWidget* buildViewerTab();
    QWidget* buildDiagramTab();
    QWidget* buildIrradianceTab();
    QWidget* buildIntensityTab();
    QWidget* buildStudiesTab();
    void     buildMenus();

    void rebuildGeometryView();
    void refreshDerivedViews();
    void updateSummary();
    QString formatMetrics() const;

    ControlsPanel*    m_controls = nullptr;
    OcctViewWidget*   m_view3d   = nullptr;
    RayDiagramWidget* m_diagram  = nullptr;
    HeatmapWidget*    m_heatmap  = nullptr;
    PlotWidget*       m_profile  = nullptr;
    PlotWidget*       m_encircled = nullptr;
    PolarPlotWidget*  m_polar    = nullptr;
    PlotWidget*       m_convergence = nullptr;
    PlotWidget*       m_focus    = nullptr;
    QTextBrowser*     m_metrics  = nullptr;
    QLabel*           m_result   = nullptr;
    QTabWidget*       m_tabs     = nullptr;

    QComboBox* m_rayColor    = nullptr;
    QCheckBox* m_detectorOnly = nullptr;
    QCheckBox* m_clipOn      = nullptr;
    QComboBox* m_clipAxis    = nullptr;
    QSlider*   m_clipSlider  = nullptr;
    QCheckBox* m_clipFlip    = nullptr;
    QComboBox* m_colormap    = nullptr;
    QComboBox* m_scale       = nullptr;
    QCheckBox* m_rgbMode     = nullptr;
    QComboBox* m_polarMode   = nullptr;
    QCheckBox* m_polarForward = nullptr;
    QSpinBox*  m_convMin     = nullptr;
    QSpinBox*  m_convMax     = nullptr;
    QSpinBox*  m_convPoints  = nullptr;
    QDoubleSpinBox* m_focusSpan = nullptr;

    SimulationWorker* m_worker = nullptr;
    StudyWorker*      m_study  = nullptr;
    QTimer*           m_geometryTimer = nullptr;

    SimulationResult                      m_last;
    bool                                  m_hasResult = false;
    std::vector<studies::ConvergencePoint> m_convergencePoints;
    studies::FocusStudy                    m_focusStudy;
    // The surfaces currently displayed, so a pick can be named without asking
    // the cache for geometry that may have been rebuilt since.
    Simulation::SceneRef m_sceneData;
};
