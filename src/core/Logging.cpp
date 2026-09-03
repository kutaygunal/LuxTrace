// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#include "core/Logging.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QStandardPaths>

#include <cstdio>

namespace logging {
namespace {

QMutex g_mutex;
QFile* g_file = nullptr;
QtMessageHandler g_previous = nullptr;
QString g_logDirOverride;
qint64 g_maxSize = 2 * 1024 * 1024;   // 2 MiB before a file rotates
int    g_maxRotated = 3;
bool   g_installed = false;

QString levelName(QtMsgType t) {
    switch (t) {
    case QtDebugMsg:    return QStringLiteral("debug");
    case QtInfoMsg:     return QStringLiteral("info");
    case QtWarningMsg:  return QStringLiteral("warning");
    case QtCriticalMsg: return QStringLiteral("critical");
    case QtFatalMsg:    return QStringLiteral("fatal");
    }
    return QStringLiteral("info");
}

QString baseName() { return QStringLiteral("luxtrace.log"); }
QString rotatedName(int i) { return QStringLiteral("luxtrace.log.%1").arg(i); }

// Opens the active log file. A failure leaves `g_file` null rather than a
// closed QFile: every later write on a closed device is rejected by Qt with a
// qWarning of its own, which would come straight back through the handler
// below. Null means "log nowhere", which is the survivable outcome.
void openCurrent() {
    const QString dir = logDir();
    QDir().mkpath(dir);
    auto* f = new QFile(dir + QLatin1Char('/') + baseName());
    if (!f->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        delete f;
        g_file = nullptr;
        return;
    }
    g_file = f;
}

// Rotates the log: the current file becomes .1, each older file shifts up by
// one, and the oldest is dropped. Caller holds the mutex.
void rotateLocked() {
    if (!g_file) return;
    g_file->close();
    delete g_file;
    g_file = nullptr;

    const QString dir = logDir();
    QFile::remove(dir + QLatin1Char('/') + rotatedName(g_maxRotated));
    for (int i = g_maxRotated - 1; i >= 1; --i)
        QFile::rename(dir + QLatin1Char('/') + rotatedName(i),
                      dir + QLatin1Char('/') + rotatedName(i + 1));
    QFile::rename(dir + QLatin1Char('/') + baseName(),
                  dir + QLatin1Char('/') + rotatedName(1));
    openCurrent();
}

// Whatever Qt did with a message before we took over. `qInstallMessageHandler`
// returns null when the *default* handler was in place -- which is the ordinary
// case at startup -- so forwarding only when `g_previous` is set would silently
// drop every console message. stderr is what the default handler writes to.
void toConsole(QtMsgType type, const QMessageLogContext& context, const QString& msg) {
    if (g_previous) { g_previous(type, context, msg); return; }
    const QByteArray line = msg.toLocal8Bit();
    std::fputs(line.constData(), stderr);
    std::fputc('\n', stderr);
}

void handler(QtMsgType type, const QMessageLogContext& context, const QString& msg) {
    // Re-entry guard. Everything inside the locked region below -- QFile,
    // QDir, QStandardPaths -- can itself emit a Qt message, and that message
    // arrives back here on this same thread while the mutex is held. A
    // non-recursive mutex would deadlock; a recursive one would let a failing
    // write recurse without bound. Dropping the nested record is the only
    // outcome that terminates, so the nested one goes to the console only.
    static thread_local bool inHandler = false;
    if (inHandler) { toConsole(type, context, msg); return; }
    struct Guard {
        bool& f;
        explicit Guard(bool& b) : f(b) { f = true; }
        ~Guard() { f = false; }
    } guard(inHandler);

    Record rec;
    rec.level     = type;
    rec.category  = context.category ? QString::fromLatin1(context.category) : QString();
    rec.message   = msg;
    rec.file      = context.file ? QString::fromLatin1(context.file) : QString();
    rec.line      = context.line;
    rec.function  = context.function ? QString::fromLatin1(context.function) : QString();
    rec.timestamp = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);

    const QByteArray line = rec.toJsonLine().toUtf8() + '\n';

    {
        QMutexLocker lock(&g_mutex);
        if (g_file) {
            g_file->write(line);
            g_file->flush();
            if (g_file->size() > g_maxSize) rotateLocked();
        }
    }

    // Keep the console behaviour Qt had before we took over, so a run from a
    // terminal still shows its messages as well as logging them.
    toConsole(type, context, msg);
}

} // namespace

QString Record::toJsonLine() const {
    QJsonObject o;
    o[QStringLiteral("ts")]      = timestamp;
    o[QStringLiteral("level")]   = levelName(level);
    if (!category.isEmpty()) o[QStringLiteral("category")] = category;
    o[QStringLiteral("message")] = message;
    if (!file.isEmpty()) o[QStringLiteral("file")] = file;
    if (line > 0) o[QStringLiteral("line")] = line;
    if (!function.isEmpty()) o[QStringLiteral("function")] = function;
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

void install() {
    QMutexLocker lock(&g_mutex);
    if (g_installed) return;
    g_installed = true;
    openCurrent();
    g_previous = qInstallMessageHandler(handler);
}

QString logDir() {
    if (!g_logDirOverride.isEmpty()) return g_logDirOverride;
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QLatin1String("/logs");
}

QString currentLogPath() {
    QMutexLocker lock(&g_mutex);
    return logDir() + QLatin1Char('/') + baseName();
}

QStringList logFiles() {
    QMutexLocker lock(&g_mutex);
    QStringList out;
    const QString dir = logDir();
    // Only what is on disk. Synthesising the whole ladder would hand a caller
    // paths to files that were never created, and would make "how many log
    // files are there" -- the question a rotation test asks -- answerable
    // without any rotation having happened.
    const QString active = dir + QLatin1Char('/') + baseName();
    if (QFile::exists(active)) out << active;
    for (int i = 1; i <= g_maxRotated; ++i) {
        const QString p = dir + QLatin1Char('/') + rotatedName(i);
        if (QFile::exists(p)) out << p;
    }
    return out;
}

QString currentLogText() {
    QMutexLocker lock(&g_mutex);
    if (!g_file) return QString();
    g_file->flush();
    // Computed directly rather than via currentLogPath(), which locks the same
    // mutex again and would deadlock while it is already held.
    const QString p = logDir() + QLatin1Char('/') + baseName();
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

void flush() {
    QMutexLocker lock(&g_mutex);
    if (g_file) g_file->flush();
}

// Both limits are read by handler() and rotateLocked() under the mutex, so
// they are read and written under it here too rather than raced against a
// worker thread that is logging.
qint64 maxFileSize() { QMutexLocker lock(&g_mutex); return g_maxSize; }
void setMaxFileSize(qint64 bytes) {
    QMutexLocker lock(&g_mutex);
    if (bytes > 0) g_maxSize = bytes;
}
int maxRotatedFiles() { QMutexLocker lock(&g_mutex); return g_maxRotated; }
void setMaxRotatedFiles(int n) {
    QMutexLocker lock(&g_mutex);
    if (n >= 0) g_maxRotated = n;
}
void setLogDir(const QString& dir) {
    QMutexLocker lock(&g_mutex);
    g_logDirOverride = dir;
    // Reopen in the new directory so a test (or a caller) that changes the
    // directory after install() actually writes there. Harmless in production,
    // where the directory is set once before install().
    if (g_installed) {
        if (g_file) { g_file->close(); delete g_file; g_file = nullptr; }
        openCurrent();
    }
}

} // namespace logging
