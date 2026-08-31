#pragma once
#include <QWidget>
#include <vector>

#include "MultiEdit.h"
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
//
// It shows a selection rather than an object, because the tree can select
// several. With more than one row picked the panel narrows itself to what they
// have in common and says so at the top:
//
//   * placement is common to everything there is, so it is always offered --
//     which is what makes a lens and a mirror and a group editable together.
//   * geometry is per type: slot 2 is a radius on one type and a wall thickness
//     on another, so those rows appear only where the whole selection is one
//     type.
//   * emission needs every one of them to be a source, and optics needs every
//     one of them to have a surface. A source and a lens together share their
//     placement and nothing else, and the panel then shows exactly that.
//
// A field the selection disagrees about reads as a dash instead of as one of
// their values, and stays a dash until it is answered -- see MultiEdit.h. What
// the user answers is written to all of them; what they leave alone is left
// alone on all of them.
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

    // Shows what a whole selection has in common. `objects` is in tree order
    // with the primary first; one object is the same thing as setObject.
    void setObjects(const std::vector<scenedoc::SceneObject>& objects);

    void clearObject();
    bool hasObject() const { return m_haveObject; }
    // How many objects the form is editing. One is the ordinary case; more than
    // one means every edit signal is a multi-edit signal.
    int  objectCount() const { return m_count; }

    // What the form currently describes: the primary selection, carrying
    // whatever the user has just typed. The caller writes the fields it was
    // told changed back into the document -- into every selected object where
    // this is a multi-edit.
    const scenedoc::SceneObject& object() const { return m_object; }

signals:
    // None of these are emitted while setObject is writing the widgets.
    //
    // The single-object signals fire only while exactly one object is shown;
    // the multi ones only above that, and they carry the multiedit:: flags of
    // the fields the user actually answered rather than an id, because the
    // window already knows what is selected and the field mask is the part it
    // cannot work out for itself.
    void nameEdited(int id);
    void placementEdited(int id);
    void parametersEdited(int id);
    void sourceEdited(int id);
    void opticsEdited(int id);
    void opticsResetRequested(int id);

    void multiPlacementEdited(quint32 fields);
    void multiParametersEdited(quint32 fields);
    void multiSourceEdited(quint32 fields);
    void multiOpticsEdited(quint32 fields);

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
    // The one path that writes the form. Both setObject and setObjects come
    // through it, so a selection of one and a selection of nine differ only in
    // what it finds in `objects` -- there is no second version of the form
    // filling to keep in step with this one.
    void showObjects(const std::vector<scenedoc::SceneObject>& objects);
    // Marks every row the selection disagrees about, and clears the marks on
    // the rest. Called with the whole selection; a single object clears
    // everything, which is what a one-object form has always looked like.
    void markMixedRows(const std::vector<scenedoc::SceneObject>& objects);
    // The banner: how many, of what, and that an edit reaches all of them.
    void showSelectionSummary(const std::vector<scenedoc::SceneObject>& objects);
    // Reports an edit as either the single or the multi form of the signal, so
    // no handler has to ask which mode the panel is in.
    void emitPlacement(quint32 field);
    void emitParameters(quint32 field);
    void emitSource(quint32 field);

    scenedoc::SceneObject m_object;
    // What the primary's type declares, so the optical section has something to
    // reset to. Only meaningful with one object shown.
    SurfaceOptics         m_defaultOptics;
    bool                  m_haveObject = false;
    // How many objects the form describes. m_object is the first of them.
    int                   m_count = 0;
    bool isMulti() const { return m_count > 1; }
    // Set while setObject rewrites the widgets, so their valueChanged signals
    // do not read back as user edits.
    bool                  m_loading = false;

    QScrollArea* m_scroll = nullptr;
    QLabel*      m_title  = nullptr;
    QLabel*      m_kind   = nullptr;
    QLabel*      m_empty  = nullptr;
    QWidget*     m_body   = nullptr;
    // Shown only while several objects are selected. Loud on purpose: the one
    // thing a user must never do by accident is type a number into what looks
    // like one object's form and change nine.
    QLabel*      m_multiBanner = nullptr;

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
