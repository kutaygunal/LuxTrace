// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

// Tests for the structured rotating log, the minimal ZIP writer, and the
// diagnostics bundle that packages the log, the configuration, the build
// fingerprint and the GPU report into one file.

#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>

#include "core/Logging.h"
#include "core/ZipWriter.h"
#include "core/Diagnostics.h"
#include "core/ConfigIO.h"
#include "core/GeometryProvider.h"

// ================================================================ logging ===

TEST(logging, writes_structured_records_to_the_log) {
    QTemporaryDir dir;
    CHECK(dir.isValid());
    logging::setLogDir(dir.path());
    logging::install();

    const QString marker =
        QStringLiteral("logging-marker-%1").arg(QDateTime::currentMSecsSinceEpoch());
    qInfo("%s", qPrintable(marker));

    logging::flush();
    const QString text = logging::currentLogText();
    CHECK(text.contains(marker));
    // One JSON object per line, with a level and the message.
    CHECK(text.trimmed().startsWith('{'));
    CHECK(text.contains(QStringLiteral("\"level\":\"info\"")));
    CHECK(text.contains(QStringLiteral("\"message\":\"") + marker + QStringLiteral("\"")));
}

TEST(logging, rotates_when_the_file_grows_past_the_limit) {
    QTemporaryDir dir;
    CHECK(dir.isValid());
    logging::setLogDir(dir.path());
    logging::setMaxFileSize(512);
    logging::setMaxRotatedFiles(2);
    logging::install();

    // Enough lines to force at least one rotation.
    for (int i = 0; i < 200; ++i)
        qInfo("rotation-test line %d with some padding to grow the file", i);
    logging::flush();

    const QStringList files = logging::logFiles();
    CHECK(files.size() >= 2);
    CHECK(QFile::exists(dir.path() + QStringLiteral("/luxtrace.log.1")));
    CHECK(QFile::exists(dir.path() + QStringLiteral("/luxtrace.log")));
    // The messages went somewhere in the chain: the active file can be empty if
    // the very last line triggered a rotation, so measure the whole chain.
    qint64 total = 0;
    for (const QString& p : files)
        if (QFileInfo(p).exists()) total += QFileInfo(p).size();
    CHECK(total > 0);
}

TEST(logging, current_log_text_returns_the_active_file) {
    QTemporaryDir dir;
    CHECK(dir.isValid());
    logging::setLogDir(dir.path());
    logging::install();

    const QString marker =
        QStringLiteral("text-marker-%1").arg(QDateTime::currentMSecsSinceEpoch());
    qInfo("%s", qPrintable(marker));
    logging::flush();

    const QString text = logging::currentLogText();
    CHECK(text.contains(marker));
    CHECK(logging::currentLogPath().endsWith(QStringLiteral("luxtrace.log")));
}

// ==================================================================== zip ===

TEST(zip, writes_a_parseable_archive) {
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString path = dir.path() + QStringLiteral("/bundle.zip");

    QVector<zip::Entry> entries;
    entries.push_back({QStringLiteral("a.txt"), QByteArray("hello")});
    entries.push_back({QStringLiteral("sub/b.txt"), QByteArray("world")});
    QString err;
    CHECK(zip::write(path, entries, &err));

    QFile f(path);
    CHECK(f.open(QIODevice::ReadOnly));
    const QByteArray bytes = f.readAll();
    // Local file header and end-of-central-directory signatures, entry names
    // and data. The EOCD signature sits 18 bytes before the end (after the two
    // zero comment-length bytes), so it is `contains`, not `endsWith`.
    CHECK(bytes.startsWith("PK\x03\x04"));
    CHECK(bytes.contains("PK\x05\x06"));
    CHECK(bytes.contains("a.txt"));
    CHECK(bytes.contains("sub/b.txt"));
    CHECK(bytes.contains("hello"));
    CHECK(bytes.contains("world"));
}

// ============================================================ diagnostics ===

TEST(diagbundle, build_fingerprint_identifies_the_build) {
    const QString fp = diagnostics::buildFingerprint();
    CHECK(fp.contains(QStringLiteral("LuxTrace build fingerprint")));
    CHECK(fp.contains(QStringLiteral("version")));
    CHECK(fp.contains(QStringLiteral("git commit")));
    CHECK(fp.contains(QStringLiteral("Qt")));
}

TEST(diagbundle, bundle_packages_log_config_build_and_gpu) {
    QTemporaryDir dir;
    CHECK(dir.isValid());
    logging::setLogDir(dir.path());
    logging::install();

    const QString marker =
        QStringLiteral("diag-marker-%1").arg(QDateTime::currentMSecsSinceEpoch());
    qInfo("%s", qPrintable(marker));

    const QString path = dir.path() + QStringLiteral("/diag.zip");
    SimConfig cfg;
    cfg.scene = GeometryProvider::Scene::Reflector;
    QString err;
    CHECK(diagnostics::saveBundle(path, cfg, nullptr, &err));

    QFile f(path);
    CHECK(f.open(QIODevice::ReadOnly));
    const QByteArray bytes = f.readAll();
    CHECK(bytes.startsWith("PK\x03\x04"));
    CHECK(bytes.contains("log.txt"));
    CHECK(bytes.contains("config.json"));
    CHECK(bytes.contains("build.txt"));
    CHECK(bytes.contains("gpu.txt"));
    CHECK(bytes.contains("manifest.txt"));
    // The log content and the build fingerprint made it in.
    CHECK(bytes.contains(marker.toUtf8()));
    CHECK(bytes.contains("LuxTrace build fingerprint"));
}
