// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once
#include <QString>
#include <QStringList>
#include <QtGlobal>

// Structured, rotating application logging.
//
// Qt's own message machinery (qDebug / qInfo / qWarning / qCritical) is the
// natural place to hang a log on: every subsystem already reports through it,
// and a handler installed once at startup captures all of them without each
// call site learning about the log. What this adds is that the records are
// *structured* -- one JSON object per line, with a timestamp, a level, a
// category and the source location -- and that the file *rotates*, so a
// long-lived session cannot grow a log without bound.
//
// The log is what a support engineer reads first. The diagnostics bundle
// (core/Diagnostics.h) packages it together with the configuration, the build
// fingerprint and the GPU report, which is the whole support story in one file.
namespace logging {

// One structured log record, as written to the log file.
struct Record {
    QtMsgType level = QtInfoMsg;
    QString   category;
    QString   message;
    QString   file;
    int       line = 0;
    QString   function;
    QString   timestamp;   // ISO 8601 with milliseconds

    // One JSON object per line.
    QString toJsonLine() const;
};

// Installs the rotating message handler. Idempotent: the second call is a
// no-op, so it is safe to call from more than one place.
void install();

// The directory the log files live in. Created on demand by install().
QString logDir();

// The active (current) log file path.
QString currentLogPath();

// Every log file, newest first: the active file, then the rotated ones.
QStringList logFiles();

// The current log content, for the diagnostics bundle. Flushes first.
QString currentLogText();

// Flushes the current file to disk.
void flush();

// Rotation limits. A file is rotated when it grows past `maxFileSize`; the
// `maxRotatedFiles` oldest files are kept, newest first.
qint64 maxFileSize();
void   setMaxFileSize(qint64 bytes);
int    maxRotatedFiles();
void   setMaxRotatedFiles(int n);

// Overrides the log directory. Intended for tests, which must not write into
// the user's real AppData. Takes effect on the next install().
void setLogDir(const QString& dir);

} // namespace logging
