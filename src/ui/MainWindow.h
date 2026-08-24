#pragma once
#include <QMainWindow>
#include <vector>
#include "core/GeometryWorker.h"
#include "core/Simulation.h"
#include "core/SimulationResult.h"
#include "core/Report.h"
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
class QGroupBox;
class QPushButton;
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
    void onGeometryReady(Simulation::SceneRef data, quint64 generation);
    void onGeometryFailed(const QString& message, quint64 generation);
    void onRun();
    void onCancel();
    void onProgress(int percent);
    void onResult(const SimulationResult& res);
    void onPartial(const SimulationResult& partial);
    void onSurfacePicked(int index);
    void onCutMoved(double x, double y);

    void onRunConvergence();
    void onConvergenceReady(const std::vector<studies::ConvergencePoint>& points);
    void onFocusSweep();
    void onRunSweep();
    void onSweepReady(const std::vector<studies::SweepPoint>& points);
    void onRunOptimisation();
    void onOptimisationReady(const studies::OptimisationResult& result);
    void onAdoptOptimum();
    void onRunTolerance();
    void onToleranceReady(const studies::ToleranceStudy& study);

    void onPinResult();
    void onClearPin();
    void onSurfaceEdited();
    void onResetSurface();

    void onSaveConfig();
    void onLoadConfig();
    void onExportIrradianceCsv();
    void onExportIntensityCsv();
    void onExportMetricsCsv();
    void onExportPhotometry();
    void onExportImage();
    void onExportReport();
    void onImportCad();

private:
    QWidget* buildViewerTab();
    QWidget* buildDiagramTab();
    QWidget* buildIrradianceTab();
    QWidget* buildIntensityTab();
    QWidget* buildStudiesTab();
    QWidget* buildDesignTab();
    QWidget* buildToleranceTab();
    void     buildMenus();

    void rebuildGeometryView();
    void refreshDerivedViews();
    void refreshDerivedQuantities();
    void updateSummary();
    void updateSurfacePanel();
    // The surfaces the 3D viewport is showing right now: a built-in scene's own
    // geometry, or (after an import) the CAD surfaces. Picking and surface
    // editing go through here so the panel never disagrees with what is drawn.
    const std::vector<OpticalSurface>& currentSurfaces() const;
    void refreshSweepAxes();
    void refreshImageQuality();
    QString formatMetrics() const;
    // Every view, rendered at report size with the caption it goes under.
    std::vector<report::Figure> figuresForReport() const;
    // The config the next run should use, including any per-surface edits.
    SimConfig currentConfig() const;

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

    // ---- design tab: sweeps, optimisation, and the surface being edited ----
    PlotWidget*     m_sweepPlot   = nullptr;
    PlotWidget*     m_optPlot     = nullptr;
    QComboBox*      m_sweepParam  = nullptr;
    QComboBox*      m_sweepMetric = nullptr;
    QSpinBox*       m_sweepSteps  = nullptr;
    QSpinBox*       m_sweepRepeats = nullptr;
    QComboBox*      m_optMetric   = nullptr;
    QComboBox*      m_optGoal     = nullptr;
    QDoubleSpinBox* m_optTarget   = nullptr;
    QComboBox*      m_optMethod   = nullptr;
    QSpinBox*       m_optEvals    = nullptr;
    QPushButton*    m_adoptButton = nullptr;
    QTextBrowser*   m_optSummary  = nullptr;

    // ---- tolerancing -------------------------------------------------------
    PlotWidget*     m_tolPlot     = nullptr;
    QComboBox*      m_tolMetric   = nullptr;
    QDoubleSpinBox* m_tolPercent  = nullptr;
    QDoubleSpinBox* m_tolCriterion = nullptr;
    QComboBox*      m_tolShape    = nullptr;
    QSpinBox*       m_tolSamples  = nullptr;
    QTextBrowser*   m_tolSummary  = nullptr;

    // ---- image quality -----------------------------------------------------
    PlotWidget*     m_mtfPlot     = nullptr;

    QLabel*         m_derived     = nullptr;
    QGroupBox*      m_surfaceBox  = nullptr;
    QLabel*         m_surfaceName = nullptr;
    QDoubleSpinBox* m_surfReflect = nullptr;
    QDoubleSpinBox* m_surfScatter = nullptr;
    QDoubleSpinBox* m_surfRough   = nullptr;
    QDoubleSpinBox* m_surfAbsorb  = nullptr;
    QPushButton*    m_surfReset   = nullptr;

    SimulationWorker* m_worker = nullptr;
    StudyWorker*      m_study  = nullptr;
    GeometryWorker*   m_geometry = nullptr;
    QTimer*           m_geometryTimer = nullptr;

    // The build the viewport is waiting for. A build that comes back stamped
    // with anything else has been overtaken -- by a later edit, or by an import
    // taking the viewport over -- and is dropped rather than drawn. Zero means
    // nothing is awaited.
    quint64 m_geometryGen = 0;
    // The geometry that was last asked for -- in flight or already drawn. An
    // edit that lands back on it costs nothing, and it is what the delivered
    // build is recorded as, rather than whatever the spin boxes read by the
    // time it arrives.
    GeometryProvider::Scene m_requestedScene   = GeometryProvider::Scene::Reflector;
    SceneParams             m_requestedParams;
    int                     m_requestedDetBins = -1;
    bool                    m_haveRequested    = false;

    SimulationResult                      m_last;
    bool                                  m_hasResult = false;
    std::vector<studies::ConvergencePoint> m_convergencePoints;
    studies::FocusStudy                    m_focusStudy;

    // A pinned run to compare against. Design is iterative, and the app used to
    // forget the previous iteration the moment a parameter moved.
    SimulationResult m_pinned;
    bool             m_hasPinned = false;
    QString          m_pinnedLabel;

    std::vector<studies::SweepPoint> m_sweepPoints;
    int                              m_sweepSlot = -1;
    studies::Metric                  m_sweepMetricUsed = studies::Metric::Efficiency;
    studies::ToleranceStudy          m_toleranceStudy;
    studies::OptimisationResult      m_optimisation;
    std::vector<int>                 m_optimisedSlots;
    studies::Objective               m_objective;

    // Per-surface optical edits, applied at trace time so changing one costs a
    // trace rather than a rebuild.
    std::vector<SurfaceOverride> m_overrides;
    int                          m_pickedSurface = -1;
    bool                         m_loadingSurface = false;
    // The surfaces currently displayed, so a pick can be named without asking
    // the cache for geometry that may have been rebuilt since.
    Simulation::SceneRef m_sceneData;

    // Geometry from an imported CAD file: the parts, the receiver placed around
    // them and the source placement that aims at them. Held here so an imported
    // part can be shown, picked and *traced* even though it has no slot in the
    // built-in scene table -- a run reads it out of the config, which is what
    // stops the trace falling back on whichever scene the controls still show.
    std::shared_ptr<const GeometryProvider::SceneSetup> m_imported;
    bool                                                m_showingImported = false;

    // Whether a study that varies the scene's own dimensions can run at all.
    // Imported geometry declares none, so it reports that rather than tracing
    // the identical solid a dozen times and drawing the flat line.
    bool requireParametricScene(const QString& what);
};
