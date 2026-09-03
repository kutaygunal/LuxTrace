# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Kutay Gunal
#
# This file is part of LuxTrace, distributed under the GNU Affero General
# Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
# A commercial licence is available; see LICENSING.md.

# luxtrace_demo.py -- a first conversation with LuxTrace.
#
# Run it either way:
#   python runner.py                          (browse for a script on the Desktop)
#   python runner.py C:\Users\kutay\Desktop\luxtrace_demo.py
#
# A client object called `luxtrace` is already in scope: every method call is
# one operation sent to LuxTrace, one JSON reply back.

import json

# 1. What is running? ------------------------------------------------------
info = luxtrace.features()
print("LuxTrace %s, protocol %s, %d operations"
      % (info["version"], info["protocol"], len(info["operations"])))

# 2. The catalogues ----------------------------------------------------------
scenes = luxtrace.scenes()
names = [s["name"] for s in scenes["scenes"]]
print("scenes: %d (first three: %s)" % (len(names), names[:3]))
mats = luxtrace.materials()
print("materials: %s" % ", ".join(m["name"] for m in mats["materials"]))

# 3. One trace of one scene ---------------------------------------------------
config = {
    "scene": {"name": "Parabolic Reflector"},
    "run": {"rays": 60000, "seed": 42},
}
res = luxtrace.run(config)
m = res["metrics"]
print()
print("trace of %s, %d rays, %.2fs" % (res["scene"], res["rays"], m["trace_seconds"]))
print("  efficiency  %.2f +- %.2f %%" % (100 * m["efficiency"], 100 * m["efficiency_std_err"]))
print("  rms spot    %.2f mm" % m["spot"]["rms_radius_mm"])
print("  d86 radius  %.2f mm" % m["spot"]["d86_radius_mm"])
print("  far-field   beam fwhm %.2f deg, peak %.1f %s/sr"
      % (m["far_field"]["beam_fwhm_deg"], m["far_field"]["peak_intensity"], m["unit"]))
print("  energy      %.2f%% on receiver, %.2f%% absorbed, %.2f%% escaped"
      % (100 * m["energy_budget"]["detector"], 100 * m["energy_budget"]["absorbed"],
         100 * m["energy_budget"]["escaped"]))

# 4. Sweep the focal length, find where the spot is tightest -----------------
sweep = luxtrace.sweep(config, slot=0, metric="rms_radius_mm",
                       steps=6, repeats=1)
best = min(sweep["points"], key=lambda p: p["value"])
print()
print("sweep %s: tightest rms spot %.2f mm at %s = %.0f %s"
      % (sweep["parameter"], best["value"], sweep["parameter"],
         best["parameter"], sweep["unit"]))

# 5. Optimise two parameters against efficiency -------------------------------
optimise = luxtrace.optimise(config, slots=[0, 1], metric="efficiency",
                             goal="maximise", optimiser="neldermead",
                             evaluations=25)
best_params = {b["name"]: round(b["value"], 1) for b in optimise["best_params"]}
print()
print("optimiser (%s): efficiency %.3f -> %.3f in %d evaluations"
      % (optimise["method"], optimise["start_value"], optimise["best_value"],
         optimise["evaluations"]))
print("  best %s" % json.dumps(best_params))

# 6. The tracer against closed forms -----------------------------------------
val = luxtrace.validate(rays=30000)
print()
print("validation: %d/%d closed-form checks passed"
      % (val["passed_count"], len(val["cases"])))
for c in val["cases"]:
    mark = "ok " if c["passed"] else "FAIL"
    print("  [%s] %-28s expected %9.4f, measured %9.4f %s"
          % (mark, c["name"], c["expected"], c["measured"], c["unit"]))

print()
print("done.")