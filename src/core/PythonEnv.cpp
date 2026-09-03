// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#include "PythonEnv.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>

namespace pythonenv {
namespace {

// Rank by where an interpreter came from. A user who set LUXTRACE_PYTHON meant
// it; after that, the interpreter on PATH is the one every other tool on the
// machine would also pick, which is the least surprising default.
enum Source { SourceEnv = 0, SourcePath = 1, SourceRegistry = 2, SourceWellKnown = 3, SourceLauncher = 4 };

struct Candidate {
    Interpreter entry;
    int         source = SourceWellKnown;
};

// "3.13" -> 313, "3.9" -> 309, so that 3.13 sorts above 3.9 rather than below
// it the way a string compare would have it.
int versionRank(const QString& v) {
    static const QRegularExpression re(QStringLiteral("^(\\d+)\\.(\\d+)"));
    const auto m = re.match(v);
    if (!m.hasMatch()) return -1;
    return m.captured(1).toInt() * 100 + m.captured(2).toInt();
}

// Python3.13, Python313, python3.13 -> "3.13". The install directories and the
// executable names both carry the version this way, and it is the only version
// available without launching the interpreter.
QString versionFromPath(const QString& path) {
    static const QRegularExpression re(QStringLiteral("[Pp]ython[\\\\/]?(\\d)\\.?(\\d+)"));
    const auto m = re.match(QDir::fromNativeSeparators(path));
    if (!m.hasMatch()) return QString();
    return m.captured(1) + QLatin1Char('.') + m.captured(2);
}

void add(std::vector<Candidate>& out, const QString& path, int source,
         const QString& label, const QString& version = QString()) {
    if (path.isEmpty() || !isUsable(path)) return;
    const QString canonical = QFileInfo(path).canonicalFilePath();
    if (canonical.isEmpty()) return;
    for (const Candidate& c : out)
        if (QFileInfo(c.entry.path).canonicalFilePath().compare(
                canonical, Qt::CaseInsensitive) == 0)
            return;                       // already known, by a better source
    Candidate c;
    c.entry.path    = QDir::toNativeSeparators(canonical);
    c.entry.label   = label;
    c.entry.version = version.isEmpty() ? versionFromPath(canonical) : version;
    c.source        = source;
    out.push_back(std::move(c));
}

#ifdef Q_OS_WIN
// Every python.org installer writes itself here, whether or not it also agreed
// to go on PATH -- which by default it does not. Both hives, because a per-user
// install and a machine-wide one land in different ones.
void addFromRegistry(std::vector<Candidate>& out) {
    const QStringList hives{
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Python\\PythonCore"),
        QStringLiteral("HKEY_LOCAL_MACHINE\\Software\\Python\\PythonCore")};

    for (const QString& hive : hives) {
        QSettings reg(hive, QSettings::NativeFormat);
        for (const QString& version : reg.childGroups()) {
            // A key's default value is "Default" or "." to QSettings; which one
            // answers depends on the version of the key that wrote it.
            QString dir = reg.value(version + QStringLiteral("/InstallPath/Default")).toString();
            if (dir.isEmpty())
                dir = reg.value(version + QStringLiteral("/InstallPath/.")).toString();
            if (dir.isEmpty()) continue;
            add(out, QDir(dir).filePath(QStringLiteral("python.exe")), SourceRegistry,
                QStringLiteral("registry"), version);
        }
    }
}

// The directories the installer offers by default, for an install that never
// reached the registry -- an unpacked embeddable distribution, or a copy moved
// into place by hand.
void addWellKnown(std::vector<Candidate>& out) {
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QStringList roots;
    const QString localApp = env.value(QStringLiteral("LOCALAPPDATA"));
    if (!localApp.isEmpty())
        roots << QDir(localApp).filePath(QStringLiteral("Programs/Python"));
    for (const char* var : {"ProgramFiles", "ProgramFiles(x86)"}) {
        const QString p = env.value(QString::fromLatin1(var));
        if (!p.isEmpty()) roots << p;
    }
    roots << QStringLiteral("C:/");

    for (const QString& root : roots) {
        QDir dir(root);
        if (!dir.exists()) continue;
        QStringList versions = dir.entryList({QStringLiteral("Python3*")},
                                             QDir::Dirs | QDir::NoDotAndDotDot,
                                             QDir::Name | QDir::Reversed);
        for (const QString& v : versions)
            add(out, dir.filePath(v + QStringLiteral("/python.exe")), SourceWellKnown,
                QStringLiteral("installed"));
    }
}
#endif // Q_OS_WIN

} // namespace

bool isUsable(const QString& path) {
    if (path.isEmpty()) return false;
    const QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile()) return false;
    // The Microsoft Store's app-execution alias: a reparse point that every
    // existence check accepts, that has no bytes, and that opens a storefront
    // rather than running the script it was handed.
    if (fi.size() <= 0) return false;
    return fi.isExecutable();
}

std::vector<Interpreter> discover() {
    std::vector<Candidate> found;

    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    add(found, env.value(QStringLiteral("LUXTRACE_PYTHON")), SourceEnv,
        QStringLiteral("LUXTRACE_PYTHON"));

    // PATH, under both names it goes by. python3 first: where a machine has
    // both, python3 is the one that is certainly not Python 2.
    for (const char* exe : {"python3", "python"})
        add(found, QStandardPaths::findExecutable(QString::fromLatin1(exe)),
            SourcePath, QStringLiteral("on PATH"));

#ifdef Q_OS_WIN
    addFromRegistry(found);
    addWellKnown(found);
    // The launcher is last: it resolves to an interpreter rather than being
    // one, so naming it hides which version actually ran.
    add(found, QStandardPaths::findExecutable(QStringLiteral("py")), SourceLauncher,
        QStringLiteral("py launcher"));
#endif

    std::stable_sort(found.begin(), found.end(),
                     [](const Candidate& a, const Candidate& b) {
                         if (a.source != b.source) return a.source < b.source;
                         return versionRank(a.entry.version) > versionRank(b.entry.version);
                     });

    std::vector<Interpreter> out;
    out.reserve(found.size());
    for (Candidate& c : found) out.push_back(std::move(c.entry));
    return out;
}

QString choose(const QString& configured, const std::vector<Interpreter>& found) {
    if (isUsable(configured)) return configured;
    for (const Interpreter& i : found)
        if (isUsable(i.path)) return i.path;
    return QString();
}

QString versionOf(const QString& path, int timeoutMs) {
    if (!isUsable(path)) return QString();

    QProcess p;
    // -I keeps the user's site directory and PYTHON* variables out of it, so
    // this answers about the interpreter rather than about its environment.
    p.start(path, {QStringLiteral("-I"), QStringLiteral("-c"),
                   QStringLiteral("import sys;print('.'.join(map(str,sys.version_info[:3])))")});
    if (!p.waitForStarted(timeoutMs) || !p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(500);
        return QString();
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) return QString();
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

} // namespace pythonenv
