// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#include "ObjectLibraryPanel.h"

#include <QHeaderView>
#include <QLabel>
#include <QMimeData>
#include <QTreeWidget>
#include <QVBoxLayout>

using scenedoc::Category;
using scenedoc::ObjectType;

namespace {

constexpr int kTypeRole = Qt::UserRole + 1;

// A tree whose drags carry a type rather than a row. QTreeWidget's own encoding
// describes the *item* -- text, icons, the model index it came from -- none of
// which means anything to a 3D viewport; what the viewport needs is the one
// string that says which builder to call.
class LibraryTree : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;

protected:
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override {
        auto* mime = new QMimeData;
        for (const QTreeWidgetItem* item : items) {
            const QVariant v = item->data(0, kTypeRole);
            if (!v.isValid()) continue;   // a category row carries nothing
            const ObjectType type = ObjectType(v.toInt());
            mime->setData(QLatin1String(scenedoc::dragMimeType()),
                          scenedoc::typeInfo(type).key.toUtf8());
            mime->setText(scenedoc::typeInfo(type).name);
            break;
        }
        return mime;
    }
};

} // namespace

ObjectLibraryPanel::ObjectLibraryPanel(QWidget* parent) : QWidget(parent) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    m_tree = new LibraryTree(this);
    m_tree->setHeaderHidden(true);
    m_tree->setColumnCount(1);
    m_tree->setRootIsDecorated(true);
    m_tree->setDragEnabled(true);
    m_tree->setDragDropMode(QAbstractItemView::DragOnly);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setUniformRowHeights(true);

    // One branch per category, in registry order, and a category only appears
    // once something files under it.
    QTreeWidgetItem* branches[int(Category::Count)] = {};
    for (ObjectType type : scenedoc::creatableTypes()) {
        const scenedoc::TypeInfo& info = scenedoc::typeInfo(type);
        const int                 cat  = int(info.category);
        if (!branches[cat]) {
            branches[cat] = new QTreeWidgetItem(m_tree);
            branches[cat]->setText(0, scenedoc::categoryName(info.category));
            branches[cat]->setFlags(Qt::ItemIsEnabled);   // a heading, not a payload
            QFont f = branches[cat]->font(0);
            f.setBold(true);
            branches[cat]->setFont(0, f);
        }
        auto* item = new QTreeWidgetItem(branches[cat]);
        item->setText(0, info.name);
        item->setToolTip(0, info.description);
        item->setData(0, kTypeRole, int(type));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
    }
    m_tree->expandAll();

    connect(m_tree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int) {
                const QVariant v = item ? item->data(0, kTypeRole) : QVariant();
                if (v.isValid()) emit createRequested(ObjectType(v.toInt()));
            });

    m_hint = new QLabel(QStringLiteral("Drag onto the 3D view to place, or "
                                       "double-click to drop at the origin."),
                        this);
    m_hint->setWordWrap(true);
    m_hint->setStyleSheet(QStringLiteral("color:#9aa0b0; font-size:11px;"));

    v->addWidget(m_tree, 1);
    v->addWidget(m_hint, 0);
}
