#!/usr/bin/env python3
"""Regenerate the scene and test counts the README quotes.

The README's "26 scenes" and "283 tests in 58 suites" are derived facts, and a
derived fact should be derived, not retyped. This script regenerates them from
the two authorities that already print them:

  * `LuxTrace.exe --smoke <n>` enumerates every scene in the registry, one per
    line, so counting the lines is counting the scenes.
  * `optics_tests.exe` prints its own totals -- "N test(s), ... check(s)" -- and
    the suite count is the number of distinct suite names it runs.

Run it from the repository root:

    python tools/regenerate_counts.py

It prints the two numbers the README's Capabilities and Test sections quote.
Pass --build to point at a specific build directory (default: build/Release,
falling back to build/Debug). Pass --smoke-rays to change the ray count used to
enumerate scenes (default 1000 -- enough to name every scene, far less than a
real run).

The script never edits the README; it prints the numbers so a human (or a
pre-commit hook) can paste them in. Keeping the derivation and the paste
separate is deliberate: the script is the source of truth, the README is a
document, and a document that regenerates itself is a document that cannot be
trusted to have been read.
"""

import argparse
import os
import re
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def find_binary(build_dir, name):
    """Return the path to `name` under `build_dir`, or None."""
    for root, _dirs, files in os.walk(build_dir):
        if name in files:
            return os.path.join(root, name)
    return None


def count_scenes(exe, smoke_rays):
    """Run --smoke and count the scenes it enumerates."""
    env = dict(os.environ)
    env["QT_QPA_PLATFORM"] = "offscreen"
    proc = subprocess.run(
        [exe, "--smoke", str(smoke_rays)],
        capture_output=True, text=True, env=env, timeout=600,
    )
    out = proc.stdout + proc.stderr
    # Every scene prints one line carrying an "emitted=" field.
    return len([l for l in out.splitlines() if "emitted=" in l])


def count_tests(exe):
    """Run the test binary and return (tests, suites)."""
    env = dict(os.environ)
    env["QT_QPA_PLATFORM"] = "offscreen"
    proc = subprocess.run(
        [exe], capture_output=True, text=True, env=env, timeout=1800,
    )
    out = proc.stdout + proc.stderr
    m = re.search(r"(\d+) test\(s\), (\d+) check\(s\)", out)
    if not m:
        raise RuntimeError("test binary did not print its totals")
    tests = int(m.group(1))
    suites = len({l.split(".")[0].replace("[ RUN ]", "").strip()
                  for l in out.splitlines() if l.startswith("[ RUN ]")})
    return tests, suites


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build", default=None,
                    help="build directory (default: build/Release, then build/Debug)")
    ap.add_argument("--smoke-rays", type=int, default=1000,
                    help="ray count for the scene enumeration (default 1000)")
    args = ap.parse_args()

    build_dir = args.build
    if build_dir is None:
        for cand in ("build/Release", "build/Debug"):
            p = os.path.join(REPO_ROOT, cand)
            if os.path.isdir(p):
                build_dir = p
                break
    if build_dir is None:
        sys.exit("no build directory found; pass --build")

    app = find_binary(build_dir, "LuxTrace.exe")
    tests_exe = find_binary(build_dir, "optics_tests.exe")
    if app is None or tests_exe is None:
        sys.exit("could not find LuxTrace.exe and optics_tests.exe under " + build_dir)

    scenes = count_scenes(app, args.smoke_rays)
    tests, suites = count_tests(tests_exe)

    print(f"scenes: {scenes}")
    print(f"tests:  {tests} in {suites} suites")
    print()
    print("README cells to keep in sync:")
    print(f"  Capabilities / Geometry:  {scenes} parametric OCCT scenes")
    print(f"  Test section:             {tests} tests in {suites} suites")


if __name__ == "__main__":
    main()
