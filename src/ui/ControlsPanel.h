#pragma once
#include <QWidget>
#include <vector>
#include "core/Simulation.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;

// Left-side controls: scene and its geometry parameters, source, physics model,
// ray budget, run/cancel, progress.
//
// The parameter rows are built from GeometryProvider::paramInfo rather than
// hardcoded, so a scene that declares a new number gets an editor for it with
// no edit here -- the same way the scene list itself is driven by the registry.
class ControlsPanel : public QWidget {
    Q_OBJECT
public:
    explicit ControlsPanel(QWidget* parent = nullptr);

    SimConfig config() const;
    void      setConfig(const SimConfig& cfg);

    GeometryProvider::Scene scene() const;

    // Says that what will be traced came from a file rather than from the scene
    // list; an empty label hands the scene list back the geometry.
    //
    // The parameter block below describes the selected scene, and while an
    // import is loaded that scene is not what a run traces. Leaving those spin
    // boxes live was half of why an imported part looked like it was being
    // traced with the previous shape's dimensions, so they are named as
    // inactive rather than silently ignored.
    void setImportedGeometry(const QString& label);

    // Freezes the inputs for the duration of a run and swaps Run for Cancel.
    // The window itself stays responsive -- the trace runs on a worker thread.
    void setRunning(bool running);
    void setProgress(int percent);
    void setStatus(const QString& text);

signals:
    void runRequested();
    void cancelRequested();
    // The scene selection changed: the whole parameter block has been rebuilt.
    void sceneChanged();
    // Geometry parameters changed; the 3D view needs rebuilding. Coalesced by
    // the window, because dragging a spin box fires this per step.
    void geometryChanged();
    // Anything that only affects the next trace (source, physics, ray count).
    void settingsChanged();

private:
    void rebuildParamRows();
    void syncEnabledState();
    SceneParams currentParams() const;

    QComboBox*    m_scene    = nullptr;
    QLabel*       m_importNote = nullptr;
    // Set while geometry from a file stands in for the selected scene, so a run
    // finishing does not quietly re-enable the parameters it does not drive.
    bool          m_importedGeometry = false;
    QGroupBox*    m_paramBox = nullptr;
    QFormLayout*  m_paramForm = nullptr;
    std::vector<QDoubleSpinBox*> m_params;
    QPushButton*  m_resetParams = nullptr;

    QComboBox*      m_source     = nullptr;
    QComboBox*      m_shape      = nullptr;
    QDoubleSpinBox* m_halfAngle  = nullptr;
    QDoubleSpinBox* m_sizeA      = nullptr;
    QDoubleSpinBox* m_sizeB      = nullptr;
    QDoubleSpinBox* m_beamRadius = nullptr;
    QComboBox*      m_spectrum   = nullptr;
    QDoubleSpinBox* m_cct        = nullptr;
    QDoubleSpinBox* m_power      = nullptr;
    QComboBox*      m_powerUnit  = nullptr;
    QComboBox*      m_detBins    = nullptr;
    QCheckBox*      m_coatings   = nullptr;
    QCheckBox*      m_volume     = nullptr;
    QCheckBox*      m_polarised  = nullptr;
    QComboBox*      m_polState   = nullptr;
    QDoubleSpinBox* m_wavelength = nullptr;

    QCheckBox*      m_fresnel    = nullptr;
    QCheckBox*      m_absorption = nullptr;
    QCheckBox*      m_scattering = nullptr;
    QCheckBox*      m_roughness  = nullptr;
    QCheckBox*      m_dispersion = nullptr;
    QDoubleSpinBox* m_roughOverride = nullptr;
    QDoubleSpinBox* m_scatterOverride = nullptr;
    QDoubleSpinBox* m_absorptionScale = nullptr;

    QSpinBox*     m_rays     = nullptr;
    QSpinBox*     m_seed     = nullptr;
    QSpinBox*     m_threads  = nullptr;
    QPushButton*  m_run      = nullptr;
    QPushButton*  m_cancel   = nullptr;
    QProgressBar* m_progress = nullptr;
    QLabel*       m_status   = nullptr;

    // Set while setConfig / rebuildParamRows rewrite the widgets, so their
    // valueChanged signals do not read back as user edits.
    bool m_loading = false;
};
