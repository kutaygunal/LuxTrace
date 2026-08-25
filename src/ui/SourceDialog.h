#pragma once
#include <QDialog>
#include "core/Simulation.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;

// Editor for one source beyond the one the scene places.
//
// A luminaire with more than one LED, and any system needing a stray-light
// source alongside the signal source, could not be built at all: the
// configuration held exactly one emitter and the scene decided where it went.
// This is the form for the rest of them.
//
// A source is described *relative* to the scene emitter by default -- offset in
// millimetres, aimed the same way -- which is what makes a four-LED array four
// copies of one description at four offsets rather than four hand-placed
// sources that all have to be moved again when the optic changes.
class SourceDialog : public QDialog {
    Q_OBJECT
public:
    explicit SourceDialog(QWidget* parent = nullptr);

    void       setSpec(const SourceSpec& spec);
    SourceSpec spec() const;

private:
    void syncEnabledState();
    void chooseRayFile();
    void clearRayFile();
    void refreshRayFileLabel();

    QLineEdit*      m_label      = nullptr;
    QComboBox*      m_type       = nullptr;
    QComboBox*      m_shape      = nullptr;
    QDoubleSpinBox* m_halfAngle  = nullptr;
    QDoubleSpinBox* m_sizeA      = nullptr;
    QDoubleSpinBox* m_sizeB      = nullptr;
    QDoubleSpinBox* m_beamRadius = nullptr;
    QComboBox*      m_spectrum   = nullptr;
    QDoubleSpinBox* m_wavelength = nullptr;
    QDoubleSpinBox* m_cct        = nullptr;
    QDoubleSpinBox* m_power      = nullptr;
    QComboBox*      m_polState   = nullptr;

    QCheckBox*      m_absolute   = nullptr;
    QDoubleSpinBox* m_ox         = nullptr;
    QDoubleSpinBox* m_oy         = nullptr;
    QDoubleSpinBox* m_oz         = nullptr;
    QCheckBox*      m_sceneAxis  = nullptr;
    QDoubleSpinBox* m_ax         = nullptr;
    QDoubleSpinBox* m_ay         = nullptr;
    QDoubleSpinBox* m_az         = nullptr;

    QLabel*         m_rayNote    = nullptr;
    QDoubleSpinBox* m_rayScale   = nullptr;
    QCheckBox*      m_rayLambda  = nullptr;

    std::shared_ptr<const RayFileData> m_rayFile;
};
