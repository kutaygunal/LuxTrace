#!/usr/bin/env python3
"""runner.py -- drive LuxTrace from a Python script.

What it does
------------
Starts the LuxTrace executable in server mode (``LuxTrace.exe --serve``), sends
one operation per call over its stdin, and reads one JSON envelope per reply
from its stdout. Nothing else exists on that channel: stdout carries JSON and
nothing else, so ``runner.py`` can trust every line it parses.

Usage
-----
  python runner.py                  pick a .py script from the Desktop and run it
  python runner.py myscript.py      run one script
  python runner.py --demo           write a demo script onto the Desktop and run it
  python runner.py --job job.json   batch mode: job JSON in, JSON lines out
  python runner.py --exe <path>     point at a different LuxTrace.exe (or set
                                    the LUXTRACE_EXE environment variable)

Inside a script a client object is already in scope as ``luxtrace``:

    info  = luxtrace.features()
    mats  = luxtrace.materials()
    res   = luxtrace.run({"scene": {"name": "Parabolic Reflector"},
                          "run":   {"rays": 50000, "seed": 42}},
                         export_csv="out/irradiance.csv")
    print(res["metrics"]["efficiency"])

    sweep = luxtrace.sweep({"scene": {"name": "Parabolic Reflector"}},
                           slot=0, metric="rms_radius_mm", steps=8)
    best  = luxtrace.optimise(config, slots=[0, 2], goal="maximise",
                              metric="efficiency", evaluations=60)

Every call returns the envelope's ``data`` object (a plain dict) or raises
``LuxTraceError`` carrying the machine-readable ``code`` and ``message``.
"""

from __future__ import annotations

import argparse
import atexit
import json
import os
import queue
import subprocess
import sys
import threading
import time

DESKTOP = os.path.join(os.path.expanduser("~"), "Desktop")


class LuxTraceError(Exception):
    """An operation LuxTrace refused. .code is machine-readable."""

    def __init__(self, code: str, message: str, op: str = ""):
        super().__init__(message)
        self.code = code
        self.message = message
        self.op = op


class LuxTraceDown(Exception):
    """The LuxTrace process is gone; .stderr_tail says what it printed last."""

    def __init__(self, message: str, stderr_tail: str = ""):
        super().__init__(message)
        self.stderr_tail = stderr_tail


def _default_exe() -> str | None:
    """Where the executable usually is, checked in order."""
    candidates = []
    env = os.environ.get("LUXTRACE_EXE")
    if env:
        candidates.append(env)
    here = os.path.dirname(os.path.abspath(__file__))
    for rel in (os.path.join("..", "build", "Release", "LuxTrace.exe"),
                os.path.join("..", "build", "Debug", "LuxTrace.exe"),
                os.path.join("build", "Release", "LuxTrace.exe"),
                os.path.join("build", "Debug", "LuxTrace.exe")):
        candidates.append(os.path.normpath(os.path.join(here, rel)))
    candidates.append(os.path.join(
        r"C:\Users\kutay\Desktop\Projects\LuxTrace\build\Release\LuxTrace.exe"))
    for c in candidates:
        if c and os.path.isfile(c):
            return c
    return None


class LuxTrace:
    """One LuxTrace process, one JSON-request/JSON-reply pipe pair."""

    def __init__(self, exe: str | None = None, verbose: bool = True):
        self.exe = exe or _default_exe()
        if not self.exe or not os.path.isfile(self.exe):
            raise LuxTraceDown(
                "LuxTrace.exe not found. Pass --exe <path> or set LUXTRACE_EXE.")
        self.verbose = verbose
        self._id = 0
        self._log("starting %s --serve" % self.exe)
        self.proc = subprocess.Popen(
            [self.exe, "--serve"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, encoding="utf-8", bufsize=1)
        self._stderr_tail: list[str] = []
        self._stderr_lines: queue.SimpleQueue[str] = queue.SimpleQueue()
        threading.Thread(target=self._pump_stderr, daemon=True).start()
        threading.Thread(target=self._drain_human_stderr, daemon=True).start()
        self._replies: queue.SimpleQueue[dict] = queue.SimpleQueue()
        threading.Thread(target=self._pump_stdout, daemon=True).start()
        atexit.register(self.close)

    # -- plumbing ----------------------------------------------------------

    def _log(self, msg: str) -> None:
        if self.verbose:
            print("[luxtrace] " + msg, file=sys.stderr)

    def _pump_stderr(self) -> None:
        for line in self.proc.stderr:            # type: ignore[union-attr]
            self._stderr_lines.put(line.rstrip("\n"))

    def _drain_human_stderr(self) -> None:
        """Echo human text (progress, warnings) through, never into JSON."""
        while True:
            line = self._stderr_lines.get()
            self._stderr_tail.append(line)
            del self._stderr_tail[:-40]          # keep the last 40 lines only
            if self.verbose:
                print("[luxtrace] " + line, file=sys.stderr)

    def _pump_stdout(self) -> None:
        assert self.proc.stdout is not None
        for line in self.proc.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                self._replies.put(json.loads(line))
            except json.JSONDecodeError:
                # A process that cannot speak protocol is dead weight.
                self._log("unparsable stdout line: %r" % line[:200])
                continue
        self._replies.put({"__eof__": True})

    # -- protocol ----------------------------------------------------------

    def call(self, op: str, params: dict | None = None, timeout: float | None = None) -> dict:
        """One operation, one reply. Returns its 'data' dict."""
        self._id += 1
        req_id = self._id
        request = {"id": req_id, "op": op, "params": params or {}}
        try:
            self.proc.stdin.write(json.dumps(request) + "\n")   # type: ignore[union-attr]
            self.proc.stdin.flush()                             # type: ignore[union-attr]
        except (BrokenPipeError, OSError):
            raise LuxTraceDown("LuxTrace closed its pipe",
                               "\n".join(self._stderr_tail)) from None
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            try:
                reply = (self._replies.get() if deadline is None
                         else self._replies.get(timeout=max(0.05, deadline - time.monotonic())))
            except queue.Empty:
                self.close()
                raise LuxTraceDown("timed out after %.1fs waiting for '%s'"
                                   % (timeout, op), "\n".join(self._stderr_tail)) from None
            if reply.get("__eof__"):
                raise LuxTraceDown("LuxTrace exited unexpectedly",
                                   "\n".join(self._stderr_tail))
            if reply.get("id") != req_id:
                continue                                        # not ours; drop
            kind = reply.get("type")
            if kind == "result":
                return reply.get("data", {})
            if kind == "error":
                err = reply.get("error", {})
                raise LuxTraceError(err.get("code", "?"),
                                    err.get("message", ""), op)
            if kind == "fatal":
                self.close()
                err = reply.get("error", {})
                raise LuxTraceDown("fatal: " + err.get("message", ""),
                                   "\n".join(self._stderr_tail))

    # -- the operations, one wrapper each -----------------------------------

    def features(self) -> dict:
        return self.call("features")

    def scenes(self) -> dict:
        return self.call("scenes")

    def materials(self) -> dict:
        return self.call("materials")

    def run(self, config: dict, **params) -> dict:
        return self.call("run", {"config": config, **params})

    def focus(self, config: dict, **params) -> dict:
        return self.call("focus", {"config": config, **params})

    def convergence(self, config: dict, **params) -> dict:
        return self.call("convergence", {"config": config, **params})

    def sweep(self, config: dict, **params) -> dict:
        return self.call("sweep", {"config": config, **params})

    def sweep2d(self, config: dict, **params) -> dict:
        return self.call("sweep2d", {"config": config, **params})

    def optimise(self, config: dict, **params) -> dict:
        return self.call("optimise", {"config": config, **params})

    def tolerance(self, config: dict, **params) -> dict:
        return self.call("tolerance", {"config": config, **params})

    def validate(self, **params) -> dict:
        return self.call("validate", params)

    def cad(self, path: str, **params) -> dict:
        return self.call("cad", {"path": path, **params})

    def rayfile(self, path: str, **params) -> dict:
        return self.call("rayfile", {"path": path, **params})

    def close(self) -> None:
        p = getattr(self, "proc", None)
        if p and p.poll() is None:
            try:
                if p.stdin:
                    p.stdin.close()
                p.wait(timeout=5)
            except Exception:
                p.terminate()


# ---------------------------------------------------------------------------
# script execution
# ---------------------------------------------------------------------------

def run_script(path: str, exe: str | None) -> int:
    path = os.path.abspath(path)
    if not os.path.isfile(path):
        print("runner.py: no such script: %s" % path, file=sys.stderr)
        return 2
    with open(path, "r", encoding="utf-8") as f:
        source = f.read()
    client = LuxTrace(exe=exe)
    code = compile(source, path, "exec")
    scope = {
        "__name__": "__main__", "__file__": path,
        "luxtrace": client,                       # the API: one call, one op
        "math": __import__("math"), "json": json, "os": os,
    }
    started = time.time()
    print("[runner] running %s with LuxTrace %s"
          % (os.path.basename(path), client.features()["version"]), file=sys.stderr)
    try:
        exec(code, scope)                          # noqa: S102 - user's own script
    except LuxTraceError as e:
        print("[runner] LuxTrace refused '%s': [%s] %s" % (e.op, e.code, e.message),
              file=sys.stderr)
        client.close()
        return 1
    except LuxTraceDown as e:
        print("[runner] LuxTrace is gone: %s" % e, file=sys.stderr)
        if e.stderr_tail:
            print(e.stderr_tail, file=sys.stderr)
        return 1
    finally:
        client.close()
    print("[runner] finished in %.1fs" % (time.time() - started), file=sys.stderr)
    return 0


def pick_from_desktop(exe: str | None) -> int:
    """No argument given: browse for a script on the Desktop."""
    try:
        import tkinter as tk
        from tkinter import filedialog
    except ImportError:
        print("runner.py: pass a script: python runner.py <script.py>", file=sys.stderr)
        return 2
    root = tk.Tk()
    root.withdraw()
    root.attributes("-topmost", True)
    chosen = filedialog.askopenfilename(
        title="Choose a Python script to run",
        initialdir=DESKTOP,
        filetypes=[("Python scripts", "*.py"), ("All files", "*.*")])
    root.destroy()
    if not chosen:
        print("[runner] nothing chosen", file=sys.stderr)
        return 2
    return run_script(chosen, exe)


def run_demo(exe: str | None) -> int:
    """Write a small demo script onto the Desktop (once) and run it."""
    demo = os.path.join(DESKTOP, "luxtrace_demo.py")
    if not os.path.isfile(demo):
        example = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "examples", "luxtrace_demo.py")
        with open(example, "r", encoding="utf-8") as src, open(demo, "w", encoding="utf-8") as dst:
            dst.write(src.read())
        print("[runner] demo written to %s" % demo, file=sys.stderr)
    return run_script(demo, exe)


def run_job_file(path: str, exe: str | None) -> int:
    """Batch passthrough: job JSON in, one JSON envelope per operation out."""
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    exe = exe or _default_exe()
    if not exe:
        print("LuxTrace.exe not found; pass --exe", file=sys.stderr)
        return 2
    proc = subprocess.run([exe, "--job", "-"], input=text, capture_output=True,
                          text=True, encoding="utf-8")
    sys.stdout.write(proc.stdout)
    if proc.stderr:
        sys.stderr.write(proc.stderr)
    return proc.returncode


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Run a Python script against LuxTrace over JSON stdio.")
    ap.add_argument("script", nargs="?", help=".py script to run (omit to browse the Desktop)")
    ap.add_argument("--exe", help="path to LuxTrace.exe (default: build/Release)")
    ap.add_argument("--demo", action="store_true", help="run the bundled demo from the Desktop")
    ap.add_argument("--job", metavar="JOB.json", help="batch mode: run a JSON job document")
    ap.add_argument("--quiet", action="store_true", help="silence the launcher's own chatter")
    args = ap.parse_args()

    if args.job:
        return run_job_file(args.job, args.exe)
    if args.demo:
        return run_demo(args.exe)
    if args.script:
        return run_script(args.script, args.exe)
    return pick_from_desktop(args.exe)


if __name__ == "__main__":
    sys.exit(main())