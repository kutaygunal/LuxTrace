// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#include "core/Diagnostics.h"

#include "core/ConfigIO.h"
#include "core/Logging.h"
#include "core/ZipWriter.h"
#include "gpu/GpuTrace.h"
#include "build_info.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QTextStream>

namespace diagnostics {

QString buildFingerprint() {
    QString out;
    QTextStream ts(&out);
    ts << "LuxTrace build fingerprint\n";
    ts << "=========================\n";
    ts << "version      " << LUXTRACE_BUILD_VERSION << "\n";
    ts << "git commit   " << LUXTRACE_BUILD_GIT_COMMIT << "\n";
    ts << "git branch   " << LUXTRACE_BUILD_GIT_BRANCH << "\n";
#ifndef LUXTRACE_BUILD_TYPE_STR
#define LUXTRACE_BUILD_TYPE_STR "unknown"
#endif
    ts << "build type   " << LUXTRACE_BUILD_TYPE_STR << "\n";
    ts << "built at     " << LUXTRACE_BUILD_DATE << "\n";
    ts << "compiler     " << LUXTRACE_BUILD_COMPILER << "\n";
    ts << "Qt           " << LUXTRACE_BUILD_QT << "\n";
    ts << "CUDA         " << LUXTRACE_BUILD_CUDA << "\n";
    return out;
}

QString gpuReport() {
    QString out;
    QTextStream ts(&out);
    ts << "GPU / driver report\n";
    ts << "===================\n";
    if (gputrace::available()) {
        ts << "backend   " << gputrace::deviceName() << "\n";
    } else {
        ts << "backend   none\n";
        ts << "reason    " << gputrace::unavailableReason() << "\n";
    }
    return out;
}

bool saveBundle(const QString& path, const SimConfig& cfg,
                const scenedoc::SceneDocument* doc, QString* errorOut) {
    // The log must be on disk before it is read back into the bundle.
    logging::flush();

    QVector<zip::Entry> entries;
    entries.reserve(5);

    entries.push_back({QStringLiteral("log.txt"), logging::currentLogText().toUtf8()});
    entries.push_back({QStringLiteral("config.json"), configio::toJson(cfg, doc).toUtf8()});
    entries.push_back({QStringLiteral("build.txt"), buildFingerprint().toUtf8()});
    entries.push_back({QStringLiteral("gpu.txt"), gpuReport().toUtf8()});

    QString manifest;
    QTextStream ms(&manifest);
    ms << "LuxTrace diagnostics bundle\n";
    ms << "===========================\n";
    ms << "generated " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
    ms << "version   "
       << (QCoreApplication::applicationVersion().isEmpty()
               ? QStringLiteral("1.0.0")
               : QCoreApplication::applicationVersion())
       << "\n";
    ms << "contents  log.txt, config.json, build.txt, gpu.txt\n";
    entries.push_back({QStringLiteral("manifest.txt"), manifest.toUtf8()});

    // Every failure this can have is zip::write's, and it sets `errorOut`
    // itself, so its answer is the answer.
    return zip::write(path, entries, errorOut);
}

} // namespace diagnostics
