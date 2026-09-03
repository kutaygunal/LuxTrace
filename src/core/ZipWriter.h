// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>

// A minimal ZIP writer.
//
// Qt's own QZipWriter is a private header, so it cannot be linked against from
// outside Qt. The diagnostics bundle needs a single portable file a support
// engineer can open with any unzip tool, and the ZIP format is small enough to
// write by hand: this writes the "store" method (no compression), which is all
// a bundle of text files needs and keeps the writer free of a deflate
// dependency. The output is a standards-conforming ZIP that Windows Explorer,
// 7-Zip and `unzip` all open.
namespace zip {

struct Entry {
    QString   name;
    QByteArray data;
};

// Writes `entries` to `path` as a ZIP archive. Returns false and sets `error`
// when the file cannot be opened for writing.
bool write(const QString& path, const QVector<Entry>& entries, QString* error = nullptr);

} // namespace zip
