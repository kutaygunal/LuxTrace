// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once
#include <QWidget>

#include "core/SceneDocument.h"

class QLabel;
class QTreeWidget;

// The catalogue of things that can go into a scene, as a tree to drag from.
//
// A palette is the honest shape for this: what a user is choosing is *what to
// make*, and a list of twenty types with no grouping is a list nobody reads. So
// the types file themselves under the category their TypeInfo declares, and the
// panel itself knows nothing about optics -- adding a type to the registry puts
// it in the library with no edit here, the same way a new scene has always
// appeared in the scene list.
//
// Dragging carries the type's key on scenedoc::dragMimeType(); the 3D viewport
// is what turns the drop point into a position. Double-clicking a row is the
// keyboard-and-mouse-shy path to the same thing, and drops the object at the
// origin.
class ObjectLibraryPanel : public QWidget {
    Q_OBJECT
public:
    explicit ObjectLibraryPanel(QWidget* parent = nullptr);

signals:
    // A row was double-clicked or activated: create one at the scene origin.
    void createRequested(scenedoc::ObjectType type);

private:
    QTreeWidget* m_tree = nullptr;
    QLabel*      m_hint = nullptr;
};
