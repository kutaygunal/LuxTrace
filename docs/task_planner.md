# Planner — Tier3 Engine Ceiling

Project: C:/Users/kutay/Desktop/Projects/LuxTrace
Cycle: tier3-engine-ceiling

You are the PLANNER in the AI Software Development Cycle.
Read the high-level task brief at C:/Users/kutay/Desktop/Projects/LuxTrace/docs/tier3-brief.md
and inspect the codebase (src/, test/, CMakeLists.txt) to confirm the evidence.

Produce an execution plan that maximizes SAFE PARALLELISM:
- Which tasks are independent? dependent?
- Which tasks touch the same files/modules/classes? (flag real conflict — two engineers on the
  same header is a hazard.)
- Should any shared interface be created first to unblock parallel work?
- Recommend an execution order and which tasks fan out in parallel vs run sequentially.

Consider especially:
- T3-001 (theta binning: SimulationResult.h + export) and T3-002 (sample caps: Coating/Material/
  Bsdf/MaterialFile) are both flagged "move first" — do they actually conflict on files?
- T3-003 (SceneParams in GeometryProvider) — is that header included by many others (risk)?
- T3-004 (RayTracer.cpp), T3-005 (MeshBuilder.cpp) — separate translation units?
- T3-006 (README) is docs-only and independent.
- NOTE: the project has uncommitted prior-session changes (new src/core/PythonEnv.cpp, modified
  src/ui/PythonPanel.*, python/runner.py, tests, README, HANDOFF). These are NOT part of this
  cycle. Flag any file overlap so engineers do not clobber them.

Output: write plan.md (with a text dependency graph and a recommended parallel fan-out) to
C:/Users/kutay/Desktop/Projects/LuxTrace/.pi/devcycle/tier3-engine-ceiling/artifacts/plan.md
Then write C:/Users/kutay/Desktop/Projects/LuxTrace/.pi/devcycle/tier3-engine-ceiling/artifacts/Planner.done
