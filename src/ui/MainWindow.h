#pragma once
#include <QMainWindow>
#include <vector>
#include "core/GeometryWorker.h"
#include "core/SceneDocument.h"
#include "core/Simulation.h"
#include "core/SimulationResult.h"
#include "core/Report.h"
#include "core/Studies.h"

class ControlsPanel;
class EnergyBarWidget;
class ObjectInspector;
class ObjectLibraryPanel;
class RayDiagramWidget;
class SceneTreePanel;
class HeatmapWidget;
class OcctViewWidget;
class PlotWidget;
class PolarPlotWidget;
class PythonPanel; // the in-app script editor: Run drives python/runner.py over the job pipe
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
class QDialog;
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
    void onGeometryChanged();
    void onGeometryReady(Simulation::SceneRef data, quint64 generation);
    void onGeometryFailed(const QString& message, quint64 generation);
    // Redraws every emitter the configuration traces, at the placement the
    // trace will use. Called whenever the geometry or the source list changes.
    void refreshSourceGlyphs();
    void onRun();
    void onCancel();
    void onProgress(int percent);
    void onResult(const SimulationResult& res);
    void onPartial(const SimulationResult& partial);
    void onSurfacePicked(int index);
    void onCutMoved(double x, double y);

    // ---- the scene document ------------------------------------------------
    //
    // One object, one place it is edited. Each of these is a request from a
    // panel; the document answers it and the views are rebuilt from what the
    // document then says, so a tree row, a highlighted body and a property
    // sheet can never be describing three different things.
    void onObjectDropped(const QString& typeKey, const gp_Pnt& where);
    void onCreateObject(scenedoc::ObjectType type, int parent);
    void onObjectSelected(int id);
    void onObjectVisibility(int id, bool visible);
    void onObjectRenamed(int id, const QString& name);
    void onObjectDelete(int id);
    void onObjectDuplicate(int id);
    void onObjectReparent(int id, int newParent);
    void onAddGroup();
    void onIsolateObject(int id);
    void onShowAllObjects();
    void onObjectPlacementEdited(int id);
    void onObjectParametersEdited(int id);
    void onObjectSourceEdited(int id);
    void onObjectOpticsEdited(int id);
    void onObjectOpticsReset(int id);

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
    // A single ray through the optic, with every interaction it had.
    void onInspectRay();

    // Brings the simulation window up, or back to the front if it is already
    // open behind the main one.
    void onShowSimulationWindow();

    void onSaveConfig();
    void onLoadConfig();
    // Reads a Zemax .agf glass catalogue or a refractiveindex.info entry into
    // the material registry. Ten built-in materials is a demonstration; a
    // catalogue is a tool, and the first thing a lens designer does is type a
    // glass name.
    void onLoadMaterialCatalogue();
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
    QWidget* buildProbeTab();
    // The window that holds everything about the run rather than about a part:
    // the tutorial's dimensions, the physics switches, the ray budget, and the
    // button that starts it.
    void     buildSimulationWindow();
    void     buildMenus();

    // ---- the document, and everything that reads it ------------------------
    //
    // Loads one of the registry's scenes as a document of its own parts.
    void loadTutorial(GeometryProvider::Scene scene);
    // Recompiles the document and puts every view back in step with it.
    // `rebuildGeometry` is false for an edit the tessellation does not care
    // about -- renaming a part, hiding one, changing what its surface does to
    // light -- which is the difference between a redraw and a rebuild.
    void syncDocument(bool rebuildGeometry);
    void refreshInspector();
    // Applies each object's "show this" flag to the bodies it compiled into.
    void applySurfaceVisibility();
    // Why the tutorial's dimensions no longer describe what is traced, or an
    // empty string while they still do.
    QString detachReason() const;

    // The two directions of the one selection: a body in the viewport and a row
    // in the tree are the same object.
    int              objectForSurface(int surfaceIndex) const;
    std::vector<int> surfacesForObject(int id) const;

    // The optical edits a linked tutorial carries, as trace-time overrides. In
    // that mode the geometry still comes from the registry, so an edited
    // reflectivity has to reach the trace as an override rather than baked into
    // a surface -- which is also what keeps it costing a trace and not a
    // rebuild.
    std::vector<SurfaceOverride> overridesFromDocument() const;

    void rebuildGeometryView();
    void refreshDerivedViews();
    void refreshDerivedQuantities();
    void updateSummary();
    // The surfaces the 3D viewport is showing right now.
    const std::vector<OpticalSurface>& currentSurfaces() const;
    void refreshSweepAxes();
    void refreshImageQuality();
    QString formatMetrics() const;
    // Every view, rendered at report size with the caption it goes under.
    std::vector<report::Figure> figuresForReport() const;
    // The config the next run should use: the panel's settings, the document's
    // sources, and either the registry's geometry or the document's own.
    SimConfig currentConfig() const;

    ControlsPanel*      m_controls = nullptr;
    QDialog*            m_simWindow = nullptr;
    ObjectLibraryPanel* m_library  = nullptr;
    SceneTreePanel*     m_sceneTree = nullptr;
    ObjectInspector*    m_object   = nullptr;
    PythonPanel*      m_python   = nullptr;
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

    // ---- the ray probe -----------------------------------------------------
    QPushButton*    m_inspectRay  = nullptr;
    QTextBrowser*   m_rayLog      = nullptr;
    QDoubleSpinBox* m_probeX      = nullptr;
    QDoubleSpinBox* m_probeY      = nullptr;

    // Where the light went, drawn rather than printed.
    EnergyBarWidget*  m_energyBar   = nullptr;

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
    // edit that lands back on it costs nothing.
    GeometryProvider::Scene m_requestedScene   = GeometryProvider::Scene::Reflector;
    SceneParams             m_requestedParams;
    int                     m_requestedDetBins = -1;
    bool                    m_requestedAssembled = false;
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

    // What is in the scene, and what the last compile of it produced.
    scenedoc::SceneDocument           m_document;
    scenedoc::SceneDocument::Compiled m_compiled;
    int                               m_selectedObject = 0;

    // The surfaces currently displayed, so a pick can be named without asking
    // the cache for geometry that may have been rebuilt since.
    Simulation::SceneRef m_sceneData;

    // Whether a study that varies the scene's own dimensions can run at all. An
    // assembled or imported scene declares none, so it reports that rather than
    // tracing the identical solid a dozen times and drawing the flat line.
    bool requireParametricScene(const QString& what);
};
