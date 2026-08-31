#pragma once
#include <QMainWindow>
#include <vector>
#include "core/GeometryWorker.h"
#include "core/SceneDocument.h"
#include "core/Simulation.h"
#include "core/SimulationResult.h"
#include "core/Report.h"
#include "core/Studies.h"
#include "render/AppearanceExport.h"

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
class AppearanceView;
class QTextBrowser;
class QTimer;
class QToolButton;

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
    void onSourcePicked(int glyphIndex);
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
    // The whole tree selection changed. The primary (current) row drives the
    // inspector via onObjectSelected; this carries the full set so the 3D view
    // can highlight every object the user picked.
    void onSelectionSetChanged(const QList<int>& ids);
    void onObjectVisibility(int id, bool visible);
    void onObjectRenamed(int id, const QString& name);
    // Bulk operations: `ids` is the whole tree selection, so a Delete or
    // Duplicate acts on every row the user picked with Ctrl/Shift.
    void onObjectDelete(const QList<int>& ids);
    // The Delete key was pressed in the 3D viewport. Confirms with the user
    // before removing, because a Delete in the view is easy to hit by accident
    // and the removal is not undoable.
    void onViewportDelete(int id);
    void onObjectDuplicate(const QList<int>& ids);
    void onObjectReparent(int id, int newParent);
    void onAddGroup();
    void onIsolateObject(const QList<int>& ids);
    void onShowAllObjects();
    // The selected object was dragged by the viewport's gizmo. `delta` is the
    // whole drag, in world coordinates.
    void onObjectTransformed(const gp_Trsf& delta);
    void onTransformModeChanged(int mode);
    void onObjectPlacementEdited(int id);
    void onObjectParametersEdited(int id);
    void onObjectSourceEdited(int id);
    void onObjectOpticsEdited(int id);
    void onObjectOpticsReset(int id);

    // The same four edits, against every selected object rather than one.
    // `fields` names the multiedit:: flags the user actually answered, and
    // nothing outside that mask is touched -- which is what lets four objects
    // with four different positions keep them while their shared Z is set once.
    void onMultiPlacementEdited(quint32 fields);
    void onMultiParametersEdited(quint32 fields);
    void onMultiSourceEdited(quint32 fields);
    void onMultiOpticsEdited(quint32 fields);

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
    // Throws away everything the tracer produced and nothing else. The scene,
    // the objects in it and the camera looking at them are untouched -- this is
    // "clear the answer", not "start again".
    void onResetResults();
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
    // The Appearance preview -- what the part looks like switched on. Last in
    // the tab order and additive: it reads the scene, nothing reads it back,
    // and it is labelled a preview rather than a result everywhere it appears.
    QWidget* buildAppearanceTab();
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
    // The narrow version, for an edit that came *from* the property panel.
    //
    // Nothing structural has happened -- no object appeared, moved in the tree
    // or was renamed -- so the tree is not rebuilt and the panel is not written
    // back over. That matters twice: rebuilding the tree on every step of a
    // spin box collapses it and throws away its scroll position, and re-reading
    // the document into the panel fights the control the user is holding.
    void syncEditedObject(bool rebuildGeometry);
    void refreshInspector();
    // Just the viewport's half of the selection -- which bodies read as
    // selected and which emitter does. Safe to call while the panel is being
    // edited, because it does not touch the panel.
    void refreshSelectionHighlight();
    // What an edit from the property panel acts on: the primary first, then the
    // rest of the selection, and only objects the document still has.
    //
    // The primary is put in by hand rather than trusted to be in the set. It
    // always is, coming from the tree -- but a selection that silently dropped
    // the one object whose name is at the top of the panel would be the worst
    // possible way for that to stop being true.
    QList<int> editSelection() const;
    // Asks the user to confirm that an edit is about to be written to every
    // selected object, and answers whether to go ahead.
    //
    // Asked once per selection rather than once per keystroke: a spin box
    // reports every step it takes, and a dialog on each of them would be a
    // dialog nobody reads. "Do not ask again" silences it for the rest of the
    // session -- this instance only, deliberately: nothing about it is written
    // to disk, so a habit formed in one sitting cannot quietly disarm the
    // warning in the next one.
    //
    // Declining puts the panel back to what the document says, so a refused
    // edit leaves no number on screen that is not in the scene.
    bool confirmMultiEdit();
    // Asks the user to confirm removing `ids` and everything under them, then
    // removes them if they agree. Shared by the tree's Delete and the
    // viewport's Delete key, so both ask the same question and do the same
    // thing.
    void confirmAndRemove(const QList<int>& ids);
    // Applies each object's "show this" flag to the bodies it compiled into.
    void applySurfaceVisibility();
    // Why the tutorial's dimensions no longer describe what is traced, or an
    // empty string while they still do.
    QString detachReason() const;

    // Set while a slot is acting on an edit that came from the property panel.
    // The panel already holds the newest version of what it shows, so anything
    // that would write it back is skipped for the duration.
    // The three transform tools, in the toolbar under the viewport. Kept so the
    // buttons can be put back in step when the mode changes from the keyboard.
    QToolButton* m_moveTool   = nullptr;
    QToolButton* m_rotateTool = nullptr;
    QToolButton* m_scaleTool  = nullptr;

    bool m_inspectorEditing = false;
    // Whether this selection has already been confirmed for multi-editing, and
    // whether the question is still being asked at all this session.
    bool m_multiEditConfirmed = false;
    bool m_multiEditAsk       = true;
    // Sets that flag for the length of a scope, exception or early return
    // included.
    struct InspectorEditScope {
        bool& flag;
        explicit InspectorEditScope(bool& f) : flag(f) { flag = true; }
        ~InspectorEditScope() { flag = false; }
    };

    // The two directions of the one selection: a body in the viewport and a row
    // in the tree are the same object.
    int              objectForSurface(int surfaceIndex) const;
    std::vector<int> surfacesForObject(int id) const;
    // Emitters are compiled in document order -- the primary first, then the
    // extras -- which is the order the glyphs are drawn in, so one index maps
    // both ways.
    int              objectForSource(int glyphIndex) const;
    int              sourceIndexForObject(int id) const;

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
    // The emitters the current configuration traces, for the Appearance view to
    // light itself from. Asked of Simulation::sourcesFor rather than derived
    // here, so the render and the trace cannot disagree about where the light
    // is.
    std::vector<SourceConfig> appearanceSources() const;
    // The render, offscreen at a size the window never has to be. Asks for the
    // resolution and the sample count first, because both cost time and a
    // renderer that decides them silently is one that either wastes a minute or
    // hands back a picture too small for the document it was wanted for.
    void exportAppearanceImage();
    // The render as a report figure, or an empty image where the tab has never
    // been opened and there is no GL context to render with. `shot` comes back
    // describing what was actually produced -- size and frames accumulated --
    // because the caption has to state the render's own sample count and not
    // the window's, which the offscreen pass has just reset.
    QImage appearanceFigure(appearance::ExportRequest& shot) const;
    // The receivers the last run measured, offered as exit surfaces the render
    // can glow with, and the line under the render that says what it is.
    void refreshAppearanceSurfaces();
    void refreshAppearanceReceivers();
    void refreshAppearanceCaption();
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
    // Whether it has been positioned yet. It is placed the first time it opens
    // rather than at construction, when the main window's geometry is not final.
    bool                m_simWindowPlaced = false;
    ObjectLibraryPanel* m_library  = nullptr;
    SceneTreePanel*     m_sceneTree = nullptr;
    ObjectInspector*    m_object   = nullptr;
    PythonPanel*      m_python   = nullptr;
    OcctViewWidget*   m_view3d   = nullptr;
    AppearanceView*   m_appearance         = nullptr;
    QLabel*           m_appearanceState    = nullptr;
    QComboBox*        m_appearanceEmission = nullptr;
    int               m_appearanceSamples  = 0;
    int               m_appearanceBudget   = 1;
    // What the last export asked for, so the second one does not have to be
    // typed again. Normalised on the way in, so it is always a size that can
    // actually be rendered.
    appearance::ExportRequest m_appearanceExport;
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
    // The whole tree selection, not just the primary. What the 3D view
    // highlights when the user has picked several objects with Ctrl/Shift.
    QList<int>                        m_selectedSet;

    // The surfaces currently displayed, so a pick can be named without asking
    // the cache for geometry that may have been rebuilt since.
    Simulation::SceneRef m_sceneData;

    // Whether a study that varies the scene's own dimensions can run at all. An
    // assembled or imported scene declares none, so it reports that rather than
    // tracing the identical solid a dozen times and drawing the flat line.
    bool requireParametricScene(const QString& what);
    // Refuses a run the compile says has nothing to emit -- every source in the
    // scene switched off, or none in it at all. Without this the trace falls
    // back to the panel's own emitter and answers a question nobody asked.
    bool requireLightSource();
};
