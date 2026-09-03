// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once
#include <functional>

// A pool of worker threads owned by the process.
//
// RayTracer::trace used to spawn `threads - 1` std::threads and join them, once
// per trace. A tolerance study of 200 samples at 8000 rays each pays that 200
// times, and an optimisation run pays it once per evaluation -- hundreds of
// times, against traces short enough for the spawn cost to be a real share of
// them. On Windows a thread creation is tens of microseconds; a 8000-ray trace
// of a simple scene is under a millisecond.
//
// Nothing about reproducibility changes. The tracer already reduces its scalar
// totals in chunk order rather than thread order, precisely so that which
// worker ran which chunk cannot reach the answer -- so a pooled worker and a
// freshly spawned one are interchangeable by construction, and the
// thread-count-invariance test says so.
class ThreadPool {
public:
    // The process-wide pool. Threads are created on first use, not at start-up:
    // a run of `--materials` should not spin up a dozen threads to print a
    // table.
    static ThreadPool& shared();

    // How many workers are standing by, not counting the caller.
    unsigned workers() const;

    // Runs `body(i)` for i in [0, count), with the *calling* thread taking
    // i == 0, and returns when every one of them has finished.
    //
    // The caller taking part is deliberate and predates the pool: a thread that
    // parks on a join is a core that is not tracing.
    //
    // A request for more workers than the pool holds spawns the remainder
    // itself, so an explicit thread count in the run settings is honoured
    // rather than silently clamped. A re-entrant or concurrent call -- one
    // parallel region asking for another while the pool is committed -- also
    // falls back to threads of its own rather than deadlocking on a pool that
    // cannot answer it.
    void runParallel(unsigned count, const std::function<void(unsigned)>& body);

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

private:
    ThreadPool();
    ~ThreadPool();
    struct Impl;
    Impl* m_impl;
};
