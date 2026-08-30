#pragma once
#include <QList>
#include <QSet>
#include <QWidget>

#include "core/SceneDocument.h"

class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// What is actually in the scene, as a tree.
//
// The 3D view can only reach a surface it can see, which leaves the far wall of
// a light guide and the second face of a lens unselectable; and once a scene is
// something a user assembles, "what is in it" stops being a fact about the
// registry and becomes a thing they need to read back. The tree is that answer,
// and it is the same selection as the viewport in both directions.
//
// It does not own the document. The window does, because the window is what
// recompiles and retraces when the document changes; this panel reads it and
// asks for edits by signal.
class SceneTreePanel : public QWidget {
    Q_OBJECT
public:
    explicit SceneTreePanel(QWidget* parent = nullptr);

    // The document to display. Not owned, and expected to outlive the panel.
    void setDocument(const scenedoc::SceneDocument* doc);
    // Re-reads the document. Cheap enough to call on every structural change.
    void rebuild();

    int  selectedId() const { return m_selected; }
    void setSelectedId(int id);

signals:
    // The primary (current) selection -- the row the inspector reads. With a
    // multi-selection this is the last row clicked, and the rest of the set is
    // carried by the operation signals below.
    void selectionChanged(int id);
    // The whole selection, primary and all. Emitted on every selection change
    // so the 3D view can highlight every object the user picked, not just the
    // one the inspector shows.
    void selectionSetChanged(const QList<int>& ids);
    void visibilityChanged(int id, bool visible);
    void renamed(int id, const QString& name);
    // Bulk operations. Each carries the whole selection, so a Delete or
    // Duplicate acts on every row the user picked with Ctrl/Shift, not just the
    // one the inspector happens to show.
    void deleteRequested(const QList<int>& ids);
    void duplicateRequested(const QList<int>& ids);
    void reparentRequested(int id, int newParent);
    // A library item was dropped onto a row rather than onto the 3D view: make
    // one at the origin, parented to that row.
    void createRequested(scenedoc::ObjectType type, int parent);
    void groupRequested();
    // Show only these objects, or put everything back.
    void isolateRequested(const QList<int>& ids);
    void showAllRequested();

private slots:
    void onCurrentChanged();
    void onItemChanged(QTreeWidgetItem* item, int column);
    void onContextMenu(const QPoint& pos);

private:
    void addRows(int parentId, QTreeWidgetItem* parentItem);
    void syncButtons();
    // The ids of every row currently selected, in tree order.
    QList<int> selectedIds() const;
    // The row holding `id`, or null.
    QTreeWidgetItem* findItem(int id) const;

    const scenedoc::SceneDocument* m_doc = nullptr;
    QTreeWidget* m_tree      = nullptr;
    QPushButton* m_group     = nullptr;
    QPushButton* m_duplicate = nullptr;
    QPushButton* m_delete    = nullptr;
    QPushButton* m_isolate   = nullptr;
    QPushButton* m_showAll   = nullptr;

    int  m_selected = 0;
    // Every row selected, not just the current one. What the bulk buttons and
    // the context menu act on when the user has picked several rows.
    QSet<int> m_selectedSet;
    // Set while the tree is being rebuilt, so its own selection and check
    // signals do not read back as user actions.
    bool m_loading = false;
};
