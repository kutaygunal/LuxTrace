// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#include "gpu/GpuTrace.h"

// The backend when the build found no CUDA.
//
// Present rather than absent so every caller links either way and the question
// "is there a preview backend" is answered at run time by available(), in one
// place, instead of by #ifdef in each of them.
namespace gputrace {

bool    available()         { return false; }
QString deviceName()        { return QString(); }
QString unavailableReason() {
    return QStringLiteral("this build has no GPU backend: CUDA was not found when it was configured");
}

Support supports(const TraceScene&, const std::vector<SceneSurface>&,
                 const std::vector<SourceConfig>&, const TraceOptions&) {
    Support s;
    s.reasons << unavailableReason();
    return s;
}

bool trace(const TraceScene&, const std::vector<SceneSurface>&,
           const std::vector<SourceConfig>&, const TraceOptions&,
           double, FluxUnit, SimulationResult&, QString* error) {
    if (error) *error = unavailableReason();
    return false;
}

} // namespace gputrace
