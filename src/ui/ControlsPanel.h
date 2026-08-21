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
