// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

// The scene document: creating, selecting, editing and deleting objects.
//
// This is the editor half of the window -- the object library, the scene tree
// and the two inspectors all arrive here -- and it is the half that writes to
// the document. The multi-selection handlers at the end are the reason the
// field-flag copies live in core/MultiEditFields.h: what they do is asserted
// headlessly by the "multiedit" suite, and what is left here is the document
// bookkeeping around it.
#include "MainWindowInternal.h"

void MainWindow::loadTutorial(GeometryProvider::Scene scene) {
    m_document.loadTutorial(scene, GeometryProvider::defaultParams(scene));
    m_controls->setScene(scene);
    m_controls->setParams(m_document.tutorialParams());
    m_selectedObject = 0;
    m_sceneTree->setSelectedId(0);
    // A different optic is a different subject, so the camera is allowed to
    // reframe and the previous optic's rays do not belong to it.
    m_view3d->setRays({});
    syncDocument(true);
    refreshSweepAxes();
    statusBar()->showMessage(QStringLiteral("%1 — %2")
                                 .arg(GeometryProvider::info(scene).name,
                                      GeometryProvider::info(scene).description),
                             8000);
}

QString MainWindow::detachReason() const {
    if (m_document.linkedToTutorial()) return {};
    return QStringLiteral(
        "The scene is assembled: %1 object(s) are traced exactly as the tree "
        "lists them. These dimensions describe the tutorial they started from, "
        "which is no longer what a run traces.")
        .arg(m_document.count());
}

void MainWindow::syncDocument(bool rebuildGeometry) {
    m_compiled = m_document.compile();
    m_sceneTree->rebuild();
    m_controls->setGeometryDetached(detachReason());
    refreshInspector();
    refreshDerivedQuantities();

    if (rebuildGeometry) {
        onGeometryChanged();
    } else {
        applySurfaceVisibility();
        refreshSourceGlyphs();
    }
}

void MainWindow::syncEditedObject(bool rebuildGeometry) {
    m_compiled = m_document.compile();
    m_controls->setGeometryDetached(detachReason());
    refreshDerivedQuantities();

    if (rebuildGeometry) {
        onGeometryChanged();
    } else {
        applySurfaceVisibility();
        refreshSourceGlyphs();
    }
}

void MainWindow::refreshInspector() {
    const scenedoc::SceneObject* o = m_document.find(m_selectedObject);
    if (!o) {
        m_object->clearObject();
        m_view3d->setHighlightedSurfaces({});
        m_view3d->setHighlightedSource(-1);
        return;
    }
    if (!m_inspectorEditing) {
        // The primary first, then the rest of the selection in tree order: the
        // panel starts every field from the primary's value and marks the ones
        // the others disagree about.
        std::vector<scenedoc::SceneObject> selection;
        for (int id : editSelection())
            if (const scenedoc::SceneObject* each = m_document.find(id))
                selection.push_back(*each);
        m_object->setObjects(selection);
    }
    refreshSelectionHighlight();
}

void MainWindow::refreshSelectionHighlight() {
    // The gizmo sits on the primary (current) selection only -- it is a tool
    // for moving one object, and a gizmo on every row of a multi-selection
    // would be a tangle of handles.
    const scenedoc::SceneObject* o = m_document.find(m_selectedObject);
    if (o)
        m_view3d->setSelectionFrame(o->id, m_document.worldPlacement(o->id));

    // The highlight covers the whole multi-selection, so Ctrl/Shift-picking
    // several rows lights up every one of them in the 3D view.
    std::vector<int> surfaces;
    int              primarySource = -1;
    for (int id : m_selectedSet) {
        const scenedoc::SceneObject* s = m_document.find(id);
        if (!s) continue;
        for (int idx : surfacesForObject(id)) surfaces.push_back(idx);
        if (s->isSource()) {
            const int si = sourceIndexForObject(id);
            if (si >= 0) primarySource = si;
        }
    }
    m_view3d->setHighlightedSurfaces(surfaces);
    m_view3d->setHighlightedSource(primarySource);
}

void MainWindow::applySurfaceVisibility() {
    // A switched-off object is left out of the compile entirely, so in an
    // assembled scene there is usually nothing here to hide. This still runs
    // for the case the compile cannot cover: a document restored from a file
    // whose parts are drawn from the registry rather than from the compile.
    for (const scenedoc::SceneObject& o : m_document.objects()) {
        if (o.isGroup() || o.isSource()) continue;
        const bool on = m_document.effectiveVisible(o.id);
        for (int idx : surfacesForObject(o.id)) m_view3d->setSurfaceVisible(idx, on);
    }
}

int MainWindow::objectForSurface(int surfaceIndex) const {
    if (surfaceIndex < 0) return 0;
    if (m_document.linkedToTutorial()) {
        for (const scenedoc::SceneObject& o : m_document.objects())
            if (o.tutorialIndex == surfaceIndex) return o.id;
        return 0;
    }
    if (surfaceIndex < int(m_compiled.surfaceObject.size()))
        return m_compiled.surfaceObject[std::size_t(surfaceIndex)];
    return 0;
}

std::vector<int> MainWindow::surfacesForObject(int id) const {
    std::vector<int> out;
    if (id == 0) return out;
    // A group covers everything under it, which is what makes selecting one
    // light up the whole assembly rather than nothing at all.
    if (m_document.linkedToTutorial()) {
        for (const scenedoc::SceneObject& o : m_document.objects())
            if (o.tutorialIndex >= 0 && m_document.isAncestorOf(id, o.id))
                out.push_back(o.tutorialIndex);
    } else {
        for (std::size_t i = 0; i < m_compiled.surfaceObject.size(); ++i)
            if (m_document.isAncestorOf(id, m_compiled.surfaceObject[i]))
                out.push_back(int(i));
    }
    return out;
}

// A glyph per emitter the compile kept, in the order it kept them -- so a
// switched-off source, which is not compiled and so has no marker, does not
// shift the markers after it onto the wrong objects.
int MainWindow::objectForSource(int glyphIndex) const {
    if (glyphIndex < 0) return 0;
    int seen = 0;
    for (const scenedoc::SceneObject& o : m_document.objects())
        if (o.isSource() && m_document.effectiveVisible(o.id) && seen++ == glyphIndex)
            return o.id;
    return 0;
}

int MainWindow::sourceIndexForObject(int id) const {
    int seen = 0;
    for (const scenedoc::SceneObject& o : m_document.objects()) {
        if (!o.isSource() || !m_document.effectiveVisible(o.id)) continue;
        if (o.id == id) return seen;
        ++seen;
    }
    return -1;
}

std::vector<SurfaceOverride> MainWindow::overridesFromDocument() const {
    std::vector<SurfaceOverride> out;
    for (const scenedoc::SceneObject& o : m_document.objects()) {
        if (!o.opticsEdited || o.tutorialIndex < 0) continue;
        SurfaceOverride ov;
        // Keyed by what the surface *is*, not by where it sat, so a saved
        // config reopened against changed geometry cannot land the edit on
        // whatever now occupies the slot.
        ov.label   = o.name;
        ov.surface = o.tutorialIndex;
        if (m_sceneData && o.tutorialIndex < int(m_sceneData->surfaces.size()))
            ov.identity = m_sceneData->scene.surfaceIdentity(o.tutorialIndex);
        ov.optics = o.optics;
        out.push_back(std::move(ov));
    }
    return out;
}

void MainWindow::onObjectDropped(const QString& typeKey, const gp_Pnt& where) {
    bool ok = false;
    const scenedoc::ObjectType type = scenedoc::typeFromKey(typeKey, &ok);
    if (!ok) return;

    const int id = m_document.add(type, where, 0);
    m_selectedObject = id;
    syncDocument(true);
    m_sceneTree->setSelectedId(id);
    statusBar()->showMessage(
        QStringLiteral("%1 placed at %2, %3, %4 mm")
            .arg(scenedoc::typeInfo(type).name)
            .arg(where.X(), 0, 'f', 1).arg(where.Y(), 0, 'f', 1).arg(where.Z(), 0, 'f', 1),
        5000);
}

void MainWindow::onCreateObject(scenedoc::ObjectType type, int parent) {
    const int id = m_document.add(type, gp_Pnt(0, 0, 0), parent);
    m_selectedObject = id;
    syncDocument(true);
    m_sceneTree->setSelectedId(id);
}

void MainWindow::onAddGroup() {
    const int id = m_document.add(scenedoc::ObjectType::Group, gp_Pnt(0, 0, 0), 0);
    m_selectedObject = id;
    syncDocument(false);
    m_sceneTree->setSelectedId(id);
}

void MainWindow::onObjectSelected(int id) {
    // The panel is not written here. The tree emits this immediately before
    // selectionSetChanged, and the panel needs the whole set -- filling it from
    // a set that is one signal out of date would show a multi-edit of the
    // previous selection for the length of a click.
    m_selectedObject = id;
}

void MainWindow::onSelectionSetChanged(const QList<int>& ids) {
    // Every document sync rebuilds the tree, which re-emits this with the same
    // ids. Only a genuine change of selection re-arms the multi-edit warning:
    // re-asking after every rebuild would put a dialog between a spin box and
    // its own next step.
    if (ids != m_selectedSet) {
        m_selectedSet        = ids;
        m_multiEditConfirmed = false;
    }
    refreshInspector();
}

QList<int> MainWindow::editSelection() const {
    QList<int> ids;
    if (m_document.find(m_selectedObject)) ids.append(m_selectedObject);
    for (int id : m_selectedSet)
        if (id != m_selectedObject && m_document.find(id)) ids.append(id);
    return ids;
}

bool MainWindow::confirmMultiEdit() {
    const int n = int(editSelection().size());
    if (n < 2) return false;
    if (m_multiEditConfirmed || !m_multiEditAsk) {
        m_multiEditConfirmed = true;
        return true;
    }

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("Apply to multiple objects"));
    box.setText(QStringLiteral("Apply this value to all %1 selected objects?").arg(n));
    box.setInformativeText(QStringLiteral(
        "The property you changed will be written to every selected object that "
        "carries it. Properties you have not changed are left as they are on "
        "each object.\n\nThis cannot be undone."));
    auto* again = new QCheckBox(
        QStringLiteral("Do not ask again for the rest of this session"), &box);
    box.setCheckBox(again);
    box.setStandardButtons(QMessageBox::Apply | QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Apply);
    if (QAbstractButton* apply = box.button(QMessageBox::Apply))
        apply->setText(QStringLiteral("Apply to All"));

    if (box.exec() != QMessageBox::Apply) {
        // The form is showing a number that is not in the scene. Put the
        // document back on screen rather than leaving the two disagreeing.
        refreshInspector();
        statusBar()->showMessage(QStringLiteral("Nothing was changed"), 3000);
        return false;
    }
    m_multiEditConfirmed = true;
    if (again->isChecked()) m_multiEditAsk = false;
    return true;
}

void MainWindow::onMultiPlacementEdited(quint32 fields) {
    if (!confirmMultiEdit()) return;
    InspectorEditScope editing(m_inspectorEditing);

    const scenedoc::SceneObject& ref = m_object->object();
    const QList<int> ids = editSelection();
    bool any = false;
    for (int id : ids) {
        const scenedoc::SceneObject* o = m_document.find(id);
        if (!o) continue;
        // Each object keeps every number the user did not answer, which is what
        // makes this an edit to one axis rather than a stamp of the primary's
        // whole placement onto the rest. The copy itself is in the core, where
        // the multiedit suite asserts exactly that.
        gp_Pnt p = o->position;
        double r[3] = {o->rotationDeg[0], o->rotationDeg[1], o->rotationDeg[2]};
        double sc   = o->scale;
        multiedit::applyPlacementFields(p, r, sc, ref.position, ref.rotationDeg,
                                        ref.scale, fields);
        any = m_document.setTransform(id, p, r, sc) || any;
    }
    if (!any) return;
    syncEditedObject(true);
    statusBar()->showMessage(
        QStringLiteral("Placement written to %1 objects").arg(ids.size()), 4000);
}

void MainWindow::onMultiParametersEdited(quint32 fields) {
    if (!confirmMultiEdit()) return;
    InspectorEditScope editing(m_inspectorEditing);

    const scenedoc::SceneObject& ref = m_object->object();
    const QList<int> ids = editSelection();
    bool any = false;
    for (int id : ids) {
        const scenedoc::SceneObject* o = m_document.find(id);
        // Only ever offered for a selection that is all one type, so a slot
        // means the same dimension on every one of them. Checked here as well,
        // because a signal is not a promise.
        if (!o || o->type != ref.type) continue;
        double p[scenedoc::SceneObject::kMaxParams];
        for (int k = 0; k < scenedoc::SceneObject::kMaxParams; ++k) p[k] = o->p[k];
        multiedit::applyParamFields(p, ref.p, scenedoc::SceneObject::kMaxParams, fields);
        any = m_document.setParams(id, p, scenedoc::SceneObject::kMaxParams) || any;
    }
    if (!any) return;
    syncEditedObject(true);
    statusBar()->showMessage(
        QStringLiteral("Dimension written to %1 objects").arg(ids.size()), 4000);
}

void MainWindow::onMultiSourceEdited(quint32 fields) {
    if (!confirmMultiEdit()) return;
    InspectorEditScope editing(m_inspectorEditing);

    const SourceSpec& ref = m_object->object().source;
    const QList<int> ids = editSelection();
    bool any = false;
    for (int id : ids) {
        const scenedoc::SceneObject* o = m_document.find(id);
        if (!o || !o->isSource()) continue;
        SourceSpec spec = o->source;
        multiedit::applySourceFields(spec, ref, fields);
        any = m_document.setSourceSpec(id, spec) || any;
    }
    if (!any) return;
    m_compiled = m_document.compile();
    refreshSourceGlyphs();
    refreshDerivedQuantities();
    statusBar()->showMessage(
        QStringLiteral("Emission written to %1 sources").arg(ids.size()), 4000);
}

void MainWindow::onMultiOpticsEdited(quint32 fields) {
    if (!confirmMultiEdit()) return;
    InspectorEditScope editing(m_inspectorEditing);

    const SurfaceOptics& ref = m_object->object().optics;
    const QList<int> ids = editSelection();
    bool any  = false;
    bool bins = false;
    for (int id : ids) {
        const scenedoc::SceneObject* o = m_document.find(id);
        if (!o || o->isGroup() || o->isSource()) continue;
        SurfaceOptics optics = o->optics;
        const int oldNX = optics.detNX, oldNY = optics.detNY;
        multiedit::applyOpticsFields(optics, ref, fields);
        // A receiver's bin grid is baked into the geometry; everything else
        // here is a ray-time property the tessellation does not care about.
        bins = bins || (optics.isDetector &&
                        (optics.detNX != oldNX || optics.detNY != oldNY));
        any = m_document.setOptics(id, optics) || any;
    }
    if (!any) return;

    if (bins) m_haveRequested = false;
    m_compiled = m_document.compile();
    if (bins) onGeometryChanged();
    refreshAppearanceSurfaces();
    statusBar()->showMessage(
        bins ? QStringLiteral("Optics written to %1 objects — the bin grid is "
                              "geometry, so this rebuilds").arg(ids.size())
             : QStringLiteral("Optics written to %1 objects — run to trace it "
                              "(no rebuild needed)").arg(ids.size()),
        4000);
}

void MainWindow::onObjectVisibility(int id, bool visible) {
    // Erased first, off the surface list the viewport is currently showing: the
    // recompile below drops the object's geometry altogether, but that reaches
    // the screen a debounce interval later, and a box that stays ticked-off
    // over a body still on screen reads as a click that did nothing.
    if (!visible)
        for (int idx : surfacesForObject(id)) m_view3d->setSurfaceVisible(idx, false);

    if (!m_document.setVisibleTree(id, visible)) return;
    syncDocument(true);
    // The emitter markers come from the compile, which has already run: this is
    // what makes a source's cone go with the tick rather than with the rebuild.
    refreshSourceGlyphs();
    m_sceneTree->setSelectedId(m_selectedObject);

    const scenedoc::SceneObject* o = m_document.find(id);
    statusBar()->showMessage(
        QStringLiteral("%1 %2 the scene -- a run traces what is ticked")
            .arg(o ? o->name : QStringLiteral("Object"),
                 visible ? QStringLiteral("is back in") : QStringLiteral("is out of")),
        5000);
}

void MainWindow::onObjectRenamed(int id, const QString& name) {
    if (!m_document.setName(id, name)) return;
    syncDocument(false);
    m_sceneTree->setSelectedId(m_selectedObject);
}

void MainWindow::onObjectDelete(const QList<int>& ids) {
    confirmAndRemove(ids);
}

void MainWindow::onViewportDelete(int id) {
    confirmAndRemove({id});
}

void MainWindow::confirmAndRemove(const QList<int>& ids) {
    // Only the objects that still exist, and only the topmost of a parent and
    // its child: removing the parent already removes the child, so a selection
    // that contains both must not count the child twice.
    QList<int> valid;
    for (int id : ids)
        if (m_document.find(id)) valid.append(id);
    if (valid.isEmpty()) return;

    QStringList names;
    int totalChildren = 0;
    for (int id : valid) {
        const scenedoc::SceneObject* o = m_document.find(id);
        names << o->name;
        totalChildren += int(m_document.childrenOf(id).size());
    }

    // A Delete is easy to hit by accident and the removal is not undoable, so
    // the user is asked to confirm before anything is taken out of the scene.
    QString detail;
    if (valid.size() == 1) {
        detail = QStringLiteral("Remove \"%1\" from the scene?").arg(names.first());
        if (totalChildren > 0)
            detail += QStringLiteral("\n\nIt contains %1 child object%2, which will be removed with it.")
                          .arg(totalChildren).arg(totalChildren == 1 ? QString() : QStringLiteral("s"));
    } else {
        detail = QStringLiteral("Remove %1 objects from the scene?\n\n%2")
                     .arg(valid.size()).arg(names.join(QStringLiteral("\n")));
        if (totalChildren > 0)
            detail += QStringLiteral("\n\n%1 child object%2 will be removed with them.")
                          .arg(totalChildren).arg(totalChildren == 1 ? QString() : QStringLiteral("s"));
    }

    const QMessageBox::StandardButton choice = QMessageBox::question(
        this, QStringLiteral("Remove objects"), detail,
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (choice != QMessageBox::Yes) {
        statusBar()->showMessage(QStringLiteral("Nothing was removed"), 3000);
        return;
    }

    int removed = 0;
    for (int id : valid)
        if (m_document.remove(id) > 0) ++removed;
    if (!m_document.find(m_selectedObject)) m_selectedObject = 0;
    syncDocument(true);
    statusBar()->showMessage(QStringLiteral("%1 object%2 removed")
                                 .arg(removed).arg(removed == 1 ? QString() : QStringLiteral("s")), 4000);
}

void MainWindow::onObjectDuplicate(const QList<int>& ids) {
    int lastCopy = 0;
    for (int id : ids) {
        const int copy = m_document.duplicate(id);
        if (copy != 0) lastCopy = copy;
    }
    if (lastCopy == 0) return;
    m_selectedObject = lastCopy;
    syncDocument(true);
    m_sceneTree->setSelectedId(lastCopy);
}

void MainWindow::onObjectReparent(int id, int newParent) {
    if (!m_document.reparent(id, newParent)) return;
    syncDocument(true);
    m_sceneTree->setSelectedId(m_selectedObject);
}

void MainWindow::onIsolateObject(const QList<int>& ids) {
    for (const scenedoc::SceneObject& o : m_document.objects()) {
        // The objects and everything under them, and the groups above them:
        // hiding an isolated object's own parent would switch the object off
        // with it.
        bool keep = false;
        for (int id : ids)
            if (m_document.isAncestorOf(id, o.id) || m_document.isAncestorOf(o.id, id)) {
                keep = true;
                break;
            }
        m_document.setVisible(o.id, keep);
    }
    syncDocument(true);
    refreshSourceGlyphs();
    m_sceneTree->setSelectedId(m_selectedObject);
    statusBar()->showMessage(
        QStringLiteral("Isolated -- everything else is switched off, and a run traces "
                       "only what is left. Show all puts it back."),
        6000);
}

void MainWindow::onShowAllObjects() {
    for (const scenedoc::SceneObject& o : m_document.objects())
        m_document.setVisible(o.id, true);
    syncDocument(true);
    refreshSourceGlyphs();
    m_sceneTree->setSelectedId(m_selectedObject);
}

void MainWindow::onTransformModeChanged(int mode) {
    const auto m = OcctViewWidget::TransformMode(mode);
    using TM = OcctViewWidget::TransformMode;
    m_moveTool->setChecked(m == TM::Translate);
    m_rotateTool->setChecked(m == TM::Rotate);
    m_scaleTool->setChecked(m == TM::Scale);
}

void MainWindow::onObjectTransformed(const gp_Trsf& delta) {
    const scenedoc::SceneObject* o = m_document.find(m_selectedObject);
    if (!o) return;

    // The gizmo speaks in world coordinates and the document stores an object's
    // placement relative to its parent, so the drag is composed onto the world
    // placement and then read back through the parent's. Doing it the other way
    // -- adding the drag to the local numbers -- would be wrong for anything
    // inside a rotated or scaled group, which is exactly where it matters.
    const gp_Trsf world  = delta * m_document.worldPlacement(o->id);
    const gp_Trsf parent = m_document.worldPlacement(o->parent);
    const gp_Trsf local  = parent.Inverted() * world;

    // A gp_Trsf is a uniform scale, a rotation and an offset, which is exactly
    // the three things the object stores -- so this is a read, not a fit.
    const double scale = local.ScaleFactor();
    gp_Trsf rigid = local;
    rigid.SetScaleFactor(1.0);
    const gp_XYZ offset = rigid.TranslationPart();

    // localPlacement() composes Rz * Ry * Rx about the fixed axes, which is
    // what OCCT calls extrinsic XYZ. Reading the angles back the same way it
    // wrote them is what keeps the three spin boxes and the gizmo agreeing.
    double rx = 0.0, ry = 0.0, rz = 0.0;
    rigid.GetRotation().GetEulerAngles(gp_Extrinsic_XYZ, rx, ry, rz);
    const double toDeg = 180.0 / std::acos(-1.0);
    const double rotation[3] = {rx * toDeg, ry * toDeg, rz * toDeg};

    if (!m_document.setTransform(o->id, gp_Pnt(offset.X(), offset.Y(), offset.Z()),
                                 rotation, scale))
        return;

    syncEditedObject(true);
    // The numbers came from the gizmo rather than from the panel, so the panel
    // is the thing that has to catch up.
    refreshInspector();

    const scenedoc::SceneObject* after = m_document.find(m_selectedObject);
    if (!after) return;
    statusBar()->showMessage(
        QStringLiteral("%1 — at %2, %3, %4 mm · %5, %6, %7 deg · scale %8")
            .arg(after->name)
            .arg(after->position.X(), 0, 'f', 1)
            .arg(after->position.Y(), 0, 'f', 1)
            .arg(after->position.Z(), 0, 'f', 1)
            .arg(after->rotationDeg[0], 0, 'f', 1)
            .arg(after->rotationDeg[1], 0, 'f', 1)
            .arg(after->rotationDeg[2], 0, 'f', 1)
            .arg(after->scale, 0, 'f', 3),
        6000);
}

void MainWindow::onObjectPlacementEdited(int id) {
    InspectorEditScope editing(m_inspectorEditing);
    const scenedoc::SceneObject& edited = m_object->object();
    if (!m_document.setTransform(id, edited.position, edited.rotationDeg, edited.scale))
        return;
    syncEditedObject(true);
}

void MainWindow::onObjectParametersEdited(int id) {
    InspectorEditScope editing(m_inspectorEditing);
    const scenedoc::SceneObject& edited = m_object->object();
    if (!m_document.setParams(id, edited.p, scenedoc::SceneObject::kMaxParams)) return;
    syncEditedObject(true);
}

void MainWindow::onObjectSourceEdited(int id) {
    InspectorEditScope editing(m_inspectorEditing);
    if (!m_document.setSourceSpec(id, m_object->object().source)) return;
    // Nothing about an emitter is geometry, so this costs a redraw of the
    // markers and a trace -- not a tessellation.
    m_compiled = m_document.compile();
    refreshSourceGlyphs();
    refreshDerivedQuantities();
}

void MainWindow::onObjectOpticsEdited(int id) {
    InspectorEditScope editing(m_inspectorEditing);
    const scenedoc::SceneObject* before = m_document.find(id);
    if (!before) return;
    const bool wasDetector = before->optics.isDetector;
    const int  oldNX = before->optics.detNX, oldNY = before->optics.detNY;

    if (!m_document.setOptics(id, m_object->object().optics)) return;

    // Whether this costs a trace or a rebuild depends on what was touched: a
    // receiver's bin grid is baked into the geometry, and everything else is a
    // ray-time property the cached tessellation and hierarchy do not care about.
    const scenedoc::SceneObject* after = m_document.find(id);
    const bool binsChanged = wasDetector && after &&
                             (after->optics.detNX != oldNX || after->optics.detNY != oldNY);
    if (binsChanged) m_haveRequested = false;

    // No tree rebuild: what a surface does to light is not part of what the
    // tree lists, and repopulating it under every keystroke would collapse it.
    m_compiled = m_document.compile();
    statusBar()->showMessage(
        binsChanged
            ? QStringLiteral("%1 edited — the bin grid is geometry, so this rebuilds")
                  .arg(after ? after->name : QString())
            : QStringLiteral("%1 edited — run to trace it (no rebuild needed)")
                  .arg(after ? after->name : QString()),
        4000);
    if (binsChanged) onGeometryChanged();

    // The render is a picture *of the optics*, so an optics edit is exactly
    // what it has to redraw for. It used to be told only on a geometry rebuild
    // or a tab switch -- which an optics edit deliberately avoids, since a
    // reflectance is a ray-time property the tessellation does not care about
    // -- so changing what a surface is made of left the preview showing what it
    // used to be until something unrelated happened to rebuild the scene.
    refreshAppearanceSurfaces();
}

void MainWindow::onObjectOpticsReset(int id) {
    if (!m_document.resetOptics(id)) return;
    m_compiled = m_document.compile();
    refreshInspector();
    refreshAppearanceSurfaces();
    statusBar()->showMessage(QStringLiteral("Surface restored to the optics its type "
                                            "declares"), 3000);
}
