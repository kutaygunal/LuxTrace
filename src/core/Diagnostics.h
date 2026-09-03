// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once
#include <QString>
#include "Simulation.h"

namespace scenedoc { class SceneDocument; }

// The diagnostics bundle: everything a support engineer needs to reproduce a
// problem, in one file.
//
// A bug report that says "it does not work" is not actionable. This packages
// the four things that make it one: the structured log (what happened), the
// configuration (what was being traced), the build fingerprint (which build it
// was) and the GPU/driver report (what the preview backend saw). Together they
// turn a support ticket into a reproduction.
namespace diagnostics {

// The build fingerprint: version, git commit, build type, compiler, Qt and
// CUDA. Generated into the binary at configure time, so it always describes the
// build that is actually running.
QString buildFingerprint();

// The GPU / driver report: whether the preview backend is available, the
// device name, and why not when it is not.
QString gpuReport();

// Writes the diagnostics bundle (a ZIP) to `path`. The bundle contains:
//   log.txt       the current structured log
//   config.json   the configuration (and the scene document, when present)
//   build.txt     the build fingerprint
//   gpu.txt       the GPU / driver report
//   manifest.txt  what the bundle is and when it was made
//
// Returns false and sets `errorOut` when the file cannot be written.
bool saveBundle(const QString& path, const SimConfig& cfg,
                const scenedoc::SceneDocument* doc, QString* errorOut = nullptr);

} // namespace diagnostics
