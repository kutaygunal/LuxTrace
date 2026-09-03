// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once
#include <QString>
#include <QStringList>
#include <vector>

#include "core/RayTracer.h"
#include "core/SimulationResult.h"
#include "core/TraceScene.h"

// The GPU preview backend.
//
// A second, faster tracer over the same geometry, and deliberately not a second
// implementation of the same physics. It models a subset -- specular reflection,
// Fresnel refraction with medium tracking, Beer-Lambert absorption -- in single
// precision, samples one branch at each interface rather than following both,
// and carries none of the variance reduction the reference does.
//
// So it is a *preview*, and the two rules that keep that honest are here rather
// than in the caller:
//
//   1. `supports()` refuses a scene the kernel cannot model, by name. A preview
//      that quietly skips a coating and returns a plausible efficiency is worse
//      than no preview, because the number it returns looks exactly like the
//      right one. Nothing is approximated silently.
//
//   2. Its result is meant to be handed to backendcheck::compare() against a
//      reference run. Being fast is not evidence of being right, and the only
//      claim a sampled result can make is that it is indistinguishable from
//      another draw of the reference.
//
// Built only where CUDA was found. Without it every entry point below still
// links and reports itself unavailable, so nothing above has to be compiled
// twice.
namespace gputrace {

// Whether this build has a GPU backend at all, and whether a device answered.
bool    available();
QString deviceName();
// Why not, when available() is false. Empty when it is true.
QString unavailableReason();

// What the preview can and cannot take on.
struct Support {
    bool    ok = false;
    // Every reason it was refused, so one run tells the whole story rather than
    // one refusal at a time.
    QStringList reasons;
};

Support supports(const TraceScene& scene, const std::vector<SceneSurface>& surfs,
                 const std::vector<SourceConfig>& srcs, const TraceOptions& opt);

// Traces `srcs` through `scene` and fills `out`. Returns false and sets `error`
// if the device failed or the scene was not supported; `out` is untouched then,
// because a half-filled result reads as a whole one.
//
// `out` carries the receiver grid, the energy budget, an efficiency with a
// standard error, and -- when opt.noiseMap is set -- the per-bin variance the
// comparison needs. It does not carry ray paths, arrivals, spectra,
// polarisation or a far field: those belong to the reference.
bool trace(const TraceScene& scene, const std::vector<SceneSurface>& surfs,
           const std::vector<SourceConfig>& srcs, const TraceOptions& opt,
           double totalPower, FluxUnit unit, SimulationResult& out, QString* error);

} // namespace gputrace
