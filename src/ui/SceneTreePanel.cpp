#include "SceneTreePanel.h"

#include <functional>

#include <QDragMoveEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QBrush>
#include <QMenu>
#include <QMimeData>
#include <QPalette>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

using scenedoc::ObjectType;
using scenedoc::SceneObject;

namespace {

constexpr int kIdRole = Qt::UserRole + 1;

int idOf(const QTreeWidgetItem* item) {
    return item ? item->data(0, kIdRole).toInt() : 0;
}

// A tree that reports drops instead of performing them.
//
// The rows are a projection of the document, not the document itself, so
// letting the view move one would put the two out of step for exactly as long
// as it took the next rebuild to snap it back. The drop is turned into a
// request, the document answers it, and the rows are rebuilt from what the
// document then says.
class DocumentTree : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;

    std::function<void(int, int)>        onReparent;   // (id, newParent)
    std::function<void(ObjectType, int)> onCreate;     // (type, parent)

protected:
    bool acceptsLibraryDrag(const QMimeData* m) const {
        return m && m->hasFormat(QLatin1String(scenedoc::dragMimeType()));
    }

    void dragEnterEvent(QDragEnterEvent* e) override {
        if (acceptsLibraryDrag(e->mimeData())) { e->acceptProposedAction(); return; }
        QTreeWidget::dragEnterEvent(e);
    }

    void dragMoveEvent(QDragMoveEvent* e) override {
        if (acceptsLibraryDrag(e->mimeData())) { e->acceptProposedAction(); return; }
        QTreeWidget::dragMoveEvent(e);
    }

    void dropEvent(QDropEvent* e) override {
        QTreeWidgetItem* target = itemAt(e->position().toPoint());
        const int        parent = idOf(target);

        if (acceptsLibraryDrag(e->mimeData())) {
            bool             ok   = false;
            const ObjectType type = scenedoc::typeFromKey(
                QString::fromUtf8(e->mimeData()->data(
                    QLatin1String(scenedoc::dragMimeType()))),
                &ok);
            e->acceptProposedAction();
            if (ok && onCreate) onCreate(type, parent);
            return;
        }

        const int moved = idOf(currentItem());
        // Accepted rather than ignored so the view stops dragging, but never
        // passed to the base class, which would move the row itself.
        e->accept();
        if (moved != 0 && onReparent) onReparent(moved, parent);
    }
};

} // namespace

SceneTreePanel::SceneTreePanel(QWidget* parent) : QWidget(parent) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    auto* tree = new DocumentTree(this);
    m_tree = tree;
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({QStringLiteral("Scene"), QStringLiteral("Type")});
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setDragEnabled(true);
    m_tree->setAcceptDrops(true);
    m_tree->setDropIndicatorShown(true);
    m_tree->setDragDropMode(QAbstractItemView::DragDrop);
    m_tree->setEditTriggers(QAbstractItemView::EditKeyPressed |
                            QAbstractItemView::SelectedClicked);

    tree->onReparent = [this](int id, int newParent) {
        emit reparentRequested(id, newParent);
    };
    tree->onCreate = [this](ObjectType type, int parent) {
        emit createRequested(type, parent);
    };

    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);
    m_group     = new QPushButton(QStringLiteral("Group"), this);
    m_duplicate = new QPushButton(QStringLiteral("Duplicate"), this);
    m_delete    = new QPushButton(QStringLiteral("Delete"), this);
    m_isolate   = new QPushButton(QStringLiteral("Isolate"), this);
    m_showAll   = new QPushButton(QStringLiteral("Show all"), this);
    m_group->setToolTip(QStringLiteral("Add an empty group. Drag objects onto it to "
                                       "move them together."));
    m_isolate->setToolTip(QStringLiteral("Switch everything except this object off. "
                                         "A run traces what is ticked, so this changes "
                                         "the trace too -- Show all puts it back."));
    for (QPushButton* b : {m_group, m_duplicate, m_delete, m_isolate, m_showAll}) {
        b->setAutoDefault(false);
        row->addWidget(b);
    }

    v->addWidget(m_tree, 1);
    v->addLayout(row, 0);

    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &SceneTreePanel::onCurrentChanged);
    connect(m_tree, &QTreeWidget::itemChanged, this, &SceneTreePanel::onItemChanged);
    connect(m_tree, &QWidget::customContextMenuRequested, this, &SceneTreePanel::onContextMenu);

    connect(m_group,     &QPushButton::clicked, this, [this] { emit groupRequested(); });
    connect(m_duplicate, &QPushButton::clicked, this, [this] {
        const QList<int> ids = selectedIds();
        if (!ids.isEmpty()) emit duplicateRequested(ids);
    });
    connect(m_delete,    &QPushButton::clicked, this, [this] {
        const QList<int> ids = selectedIds();
        if (!ids.isEmpty()) emit deleteRequested(ids);
    });
    connect(m_isolate,   &QPushButton::clicked, this, [this] {
        const QList<int> ids = selectedIds();
        if (!ids.isEmpty()) emit isolateRequested(ids);
    });
    connect(m_showAll,   &QPushButton::clicked, this, [this] { emit showAllRequested(); });

    syncButtons();
}

void SceneTreePanel::setDocument(const scenedoc::SceneDocument* doc) {
    m_doc = doc;
    rebuild();
}

void SceneTreePanel::addRows(int parentId, QTreeWidgetItem* parentItem) {
    if (!m_doc) return;
    for (int id : m_doc->childrenOf(parentId)) {
        const SceneObject* o = m_doc->find(id);
        if (!o) continue;

        auto* item = parentItem ? new QTreeWidgetItem(parentItem)
                                : new QTreeWidgetItem(m_tree);
        item->setText(0, o->name);
        // The optic, not the machinery that built it: a tutorial's parts are
        // all TutorialPart, and a column reading "tutorial part" down every row
        // answers a question nobody asked. Each builder names its surfaces, so
        // this says "Parabolic Mirror" and "Detector" in every scene.
        item->setText(1, scenedoc::typeLabel(*o));
        item->setToolTip(0, scenedoc::typeInfo(o->type).description);
        item->setData(0, kIdRole, id);
        item->setCheckState(0, o->visible ? Qt::Checked : Qt::Unchecked);
        // A row switched off by the group above it is ticked but not in the
        // scene, so it is drawn the way a disabled control is: the tick says
        // what this row was set to, the grey says what the scene actually has.
        if (!m_doc->effectiveVisible(id)) {
            const QBrush dim = palette().brush(QPalette::Disabled, QPalette::Text);
            item->setForeground(0, dim);
            item->setForeground(1, dim);
        }

        Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable |
                              Qt::ItemIsUserCheckable | Qt::ItemIsEditable |
                              Qt::ItemIsDragEnabled;
        // Only a group can take children; dropping onto a lens would be asking
        // for a hierarchy the compiler has no meaning for.
        if (o->isGroup()) flags |= Qt::ItemIsDropEnabled;
        item->setFlags(flags);

        addRows(id, item);
    }
}

void SceneTreePanel::rebuild() {
    m_loading = true;
    m_tree->clear();
    addRows(0, nullptr);
    m_tree->expandAll();

    // Put the selection back where it was, if what it pointed at survived. The
    // whole set is restored, not just the current row, so a multi-selection
    // survives a visibility toggle or a rename that rebuilds the tree.
    QList<int> toSelect;
    for (int id : m_selectedSet)
        if (m_doc && m_doc->find(id)) toSelect.append(id);
    if (toSelect.isEmpty() && m_doc && m_doc->find(m_selected))
        toSelect.append(m_selected);

    m_tree->clearSelection();
    QTreeWidgetItem* current = nullptr;
    for (int id : toSelect) {
        QTreeWidgetItem* it = findItem(id);
        if (!it) continue;
        it->setSelected(true);
        if (id == m_selected) current = it;
    }
    if (!current && !toSelect.isEmpty())
        current = findItem(toSelect.first());
    if (current) {
        m_tree->setCurrentItem(current);
        m_tree->scrollToItem(current);
    }
    m_selectedSet = QSet<int>(toSelect.begin(), toSelect.end());
    m_loading = false;
    syncButtons();
    emit selectionSetChanged(selectedIds());
}

void SceneTreePanel::setSelectedId(int id) {
    m_selected = id;
    m_selectedSet.clear();
    if (id != 0) m_selectedSet.insert(id);
    m_loading  = true;
    m_tree->clearSelection();
    if (QTreeWidgetItem* hit = findItem(id)) {
        hit->setSelected(true);
        m_tree->setCurrentItem(hit);
        m_tree->scrollToItem(hit);
    }
    m_loading = false;
    syncButtons();
    emit selectionSetChanged(selectedIds());
}

void SceneTreePanel::onCurrentChanged() {
    if (m_loading) return;
    const int id = idOf(m_tree->currentItem());
    m_selectedSet.clear();
    for (int i : selectedIds()) m_selectedSet.insert(i);
    if (id != m_selected) {
        m_selected = id;
        syncButtons();
        emit selectionChanged(id);
    }
    emit selectionSetChanged(selectedIds());
}

// A row changed: it was ticked, unticked, or renamed in place.
//
// Everything is read off the row first and reported afterwards, from a queued
// call. Both of those edits rebuild the tree, which deletes this very item --
// while Qt is still inside the model's setData for it, and while this function
// still holds a pointer to it. Reporting the tick and then reading the name off
// the freed row is what renamed every object in the scene to whatever text
// happened to be lying in that memory.
void SceneTreePanel::onItemChanged(QTreeWidgetItem* item, int column) {
    if (m_loading || column != 0 || !item || !m_doc) return;
    const int          id = idOf(item);
    const SceneObject* o  = m_doc->find(id);
    if (!o) return;

    const bool    wantVisible = item->checkState(0) == Qt::Checked;
    const QString text        = item->text(0);
    const bool    visChanged  = wantVisible != o->visible;
    const bool    nameChanged = !text.isEmpty() && text != o->name;
    if (!visChanged && !nameChanged) return;

    QMetaObject::invokeMethod(
        this,
        [this, id, wantVisible, text, visChanged, nameChanged] {
            if (visChanged)  emit visibilityChanged(id, wantVisible);
            if (nameChanged) emit renamed(id, text);
        },
        Qt::QueuedConnection);
}

void SceneTreePanel::onContextMenu(const QPoint& pos) {
    QTreeWidgetItem* item = m_tree->itemAt(pos);
    const int        id   = idOf(item);

    // Right-clicking a row that is part of a multi-selection acts on the whole
    // selection; right-clicking an unselected row acts on just that row.
    QList<int> target;
    if (item && item->isSelected())
        target = selectedIds();
    else if (id != 0)
        target = {id};

    QMenu menu(this);
    if (!target.isEmpty()) {
        menu.addAction(QStringLiteral("Rename"), this,
                       [this, item] { m_tree->editItem(item, 0); });
        menu.addAction(QStringLiteral("Duplicate"), this,
                       [this, target] { emit duplicateRequested(target); });
        menu.addAction(QStringLiteral("Delete"), this,
                       [this, target] { emit deleteRequested(target); });
        menu.addSeparator();
        menu.addAction(QStringLiteral("Isolate"), this,
                       [this, target] { emit isolateRequested(target); });
        menu.addAction(QStringLiteral("Move to top level"), this,
                       [this, id] { emit reparentRequested(id, 0); });
    }
    menu.addAction(QStringLiteral("Show all"), this, [this] { emit showAllRequested(); });
    menu.addAction(QStringLiteral("Add group"), this, [this] { emit groupRequested(); });
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

void SceneTreePanel::syncButtons() {
    bool have = false;
    for (int id : m_selectedSet)
        if (m_doc && m_doc->find(id)) { have = true; break; }
    m_duplicate->setEnabled(have);
    m_delete->setEnabled(have);
    m_isolate->setEnabled(have);
}

QList<int> SceneTreePanel::selectedIds() const {
    QList<int> ids;
    for (const QTreeWidgetItem* it : m_tree->selectedItems()) {
        const int i = idOf(it);
        if (i != 0) ids.append(i);
    }
    return ids;
}

QTreeWidgetItem* SceneTreePanel::findItem(int id) const {
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> walk =
        [&](QTreeWidgetItem* item) -> QTreeWidgetItem* {
        if (idOf(item) == id) return item;
        for (int i = 0; i < item->childCount(); ++i)
            if (QTreeWidgetItem* hit = walk(item->child(i))) return hit;
        return nullptr;
    };
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
        if (QTreeWidgetItem* hit = walk(m_tree->topLevelItem(i))) return hit;
    return nullptr;
}
