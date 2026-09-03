// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once
#include <QString>
#include <vector>

// Finding a Python interpreter on the machine the app is running on.
//
// The Python panel drives `python runner.py <script>`, so it needs an
// interpreter before it can do anything at all. Asking PATH for `python` is
// where that search starts and not where it can end: on Windows the usual
// python.org installer does not put itself on PATH, the Microsoft Store ships
// an alias that is not an interpreter, and a machine can carry three versions
// at once. So the search reads the places an interpreter actually lives, ranks
// what it finds, and lets the user overrule the ranking permanently.
//
// The choice policy is a pure function of (what the user picked, what was
// found), so it can be tested without a registry or a settings store: reading
// and writing the stored preference belongs to the caller.
namespace pythonenv {

// One interpreter this machine appears to have.
struct Interpreter {
    QString path;      // absolute path to the executable
    QString label;     // where it was found, so the picker can explain itself
    QString version;   // "3.13" when the source knew it, else empty
};

// Every interpreter found, best first. The order is by *source* -- an explicit
// LUXTRACE_PYTHON, then PATH, then the registry, then the well-known install
// directories, then the `py` launcher -- and within one source by version,
// newest first. Paths that are not usable are not returned at all, so the first
// entry is always a candidate worth running.
std::vector<Interpreter> discover();

// Whether a path is an interpreter that can actually be executed.
//
// The zero-byte case is the one that matters on Windows: an App Execution Alias
// under WindowsApps looks like python.exe to every existence check and opens the
// Microsoft Store instead of running a script. It has no bytes in it, which is
// what separates it from an interpreter.
bool isUsable(const QString& path);

// Which interpreter to use, given what the user configured and what was found.
// A configured path wins whenever it still exists -- that is the whole point of
// configuring one -- and a configured path that has since been uninstalled
// falls back to discovery rather than failing, because a stale setting should
// not break the panel. Empty when the machine has no interpreter at all.
QString choose(const QString& configured, const std::vector<Interpreter>& found);

// The interpreter's own version string, by asking it. Costs a process launch,
// so it is called when a person is choosing between interpreters rather than on
// every run. Empty if it did not answer within the timeout.
QString versionOf(const QString& path, int timeoutMs = 3000);

} // namespace pythonenv
