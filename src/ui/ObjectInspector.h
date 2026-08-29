#pragma once
#include <QWidget>
#include <vector>

#include "core/SceneDocument.h"

class SurfaceInspector;

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;

// The property editor for whichever object is selected.
//
// One panel, not one per kind of thing. What a lens, a mirror, a light guide
// and a receiver have in common is a name, a place to sit and a surface that
// does something to light; what a source has instead of that surface is an
// angular law, a spectrum and a flux. So the panel is those sections, shown or
// hidden by what is selected, and the geometry rows are built from the type's
// own SceneParamInfo -- a type that declares a new number gets an editor for it
// with no edit here.
//
// The optical section is the existing SurfaceInspector, embedded rather than
// reimplemented: it already carries the material picker, the coating solver and
// the two live plots, and a second form over the same struct would be a second
// form to keep in step with the engine.
class ObjectInspector : public QWidget {
    Q_OBJECT
public:
    explicit ObjectInspector(QWidget* parent = nullptr);

    // Never narrower than its content.
    //
    // The panel scrolls vertically only, so a column narrower than its widest
    // row does not grow a scrollbar -- it silently clips, and the right-hand
    // half of every material readout and spin box goes off the edge. Refusing
    // the width is what stops that; the splitter can still be dragged wider.
    QSize minimumSizeHint() const override;
    // As wide as the content, no wider. This is only the width the column opens
    // at; the user sets it after that.
    QSize sizeHint() const override;

    // Shows `object`. `defaultOptics` is what its type declares, so the optical
    // section has something to reset to.
    void setObject(const scenedoc::SceneObject& object, const SurfaceOptics& defaultOptics);
    void clearObject();
    bool hasObject() const { return m_haveObject; }

    // What the form currently describes. The caller writes the parts it was
    // told changed back into the document.
    const scenedoc::SceneObject& object() const { return m_object; }

signals:
    // None of these are emitted while setObject is writing the widgets.
    void nameEdited(int id);
    void placementEdited(int id);
    void parametersEdited(int id);
    void sourceEdited(int id);
    void opticsEdited(int id);
    void opticsResetRequested(int id);

private:
    // `sameObject` says the panel is re-reading what it already shows, which
    // is the only case in which a control the user has hold of may be left
    // alone -- for a different object the stale value would be wrong.
    void rebuildParamRows(bool sameObject);
    void syncSourceEnabledState();
    void refreshRayFileLabel();
    void chooseRayFile();
    void clearRayFile();
    void readSourceForm();

    scenedoc::SceneObject m_object;
    bool                  m_haveObject = false;
    // Set while setObject rewrites the widgets, so their valueChanged signals
    // do not read back as user edits.
    bool                  m_loading = false;

    QScrollArea* m_scroll = nullptr;
    QLabel*      m_title  = nullptr;
    QLabel*      m_kind   = nullptr;
    QLabel*      m_empty  = nullptr;
    QWidget*     m_body   = nullptr;

    QLineEdit* m_name = nullptr;

    QGroupBox*      m_placeBox = nullptr;
    QDoubleSpinBox* m_px = nullptr;
    QDoubleSpinBox* m_py = nullptr;
    QDoubleSpinBox* m_pz = nullptr;
    QDoubleSpinBox* m_rx = nullptr;
    QDoubleSpinBox* m_ry = nullptr;
    QDoubleSpinBox* m_rz = nullptr;
    QDoubleSpinBox* m_scale = nullptr;

    QGroupBox*                   m_geomBox  = nullptr;
    QFormLayout*                 m_geomForm = nullptr;
    std::vector<QDoubleSpinBox*> m_params;
    // Which type the geometry rows were built for. The rows belong to the type,
    // not to the object, so selecting another lens of the same kind -- or
    // re-reading the one being edited -- reuses them instead of tearing down
    // the spin box the user is holding.
    int                          m_paramType = -1;
    QLabel*                      m_bakedNote = nullptr;

    QGroupBox*      m_sourceBox  = nullptr;
    QComboBox*      m_srcType    = nullptr;
    QComboBox*      m_srcShape   = nullptr;
    QDoubleSpinBox* m_halfAngle  = nullptr;
    QDoubleSpinBox* m_sizeA      = nullptr;
    QDoubleSpinBox* m_sizeB      = nullptr;
    QDoubleSpinBox* m_beamRadius = nullptr;
    QComboBox*      m_spectrum   = nullptr;
    QDoubleSpinBox* m_wavelength = nullptr;
    QDoubleSpinBox* m_cct        = nullptr;
    QDoubleSpinBox* m_power      = nullptr;
    QComboBox*      m_polState   = nullptr;
    QLabel*         m_rayNote    = nullptr;
    QPushButton*    m_rayChoose  = nullptr;
    QPushButton*    m_rayClear   = nullptr;
    QDoubleSpinBox* m_rayScale   = nullptr;
    QCheckBox*      m_rayLambda  = nullptr;

    SurfaceInspector* m_optics = nullptr;
};
