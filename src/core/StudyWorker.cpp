// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#include "StudyWorker.h"

#include <algorithm>

StudyWorker::StudyWorker(QObject* parent) : QThread(parent) {
    // Results travel across queued connections, so they need to be known
    // metatypes.
    qRegisterMetaType<std::vector<studies::ConvergencePoint>>(
        "std::vector<studies::ConvergencePoint>");
    qRegisterMetaType<std::vector<studies::SweepPoint>>("std::vector<studies::SweepPoint>");
    qRegisterMetaType<studies::OptimisationResult>("studies::OptimisationResult");
    qRegisterMetaType<studies::ToleranceStudy>("studies::ToleranceStudy");
}

StudyWorker::~StudyWorker() {
    cancel();
    wait();
}

void StudyWorker::startConvergence(const SimConfig& cfg, int minRays, int maxRays, int points) {
    if (isRunning()) return;
    m_job    = Job::Convergence;
    m_cfg    = cfg;
    m_min    = minRays;
    m_max    = maxRays;
    m_points = points;
    m_cancel.store(false, std::memory_order_relaxed);
    start();
}

void StudyWorker::startSweep(const SimConfig& cfg, int slotIndex, double from, double to,
                             int steps, studies::Metric metric, int repeats) {
    if (isRunning()) return;
    m_job     = Job::Sweep;
    m_cfg     = cfg;
    m_slot    = slotIndex;
    m_from    = from;
    m_to      = to;
    m_steps   = steps;
    m_metric  = metric;
    m_repeats = repeats;
    m_cancel.store(false, std::memory_order_relaxed);
    start();
}

void StudyWorker::startOptimisation(const SimConfig& cfg, const std::vector<int>& paramSlots,
                                    const studies::Objective& objective,
                                    studies::Optimiser method, int maxEvaluations) {
    if (isRunning()) return;
    m_job            = Job::Optimisation;
    m_cfg            = cfg;
    m_optSlots       = paramSlots;
    m_objective      = objective;
    m_method         = method;
    m_maxEvaluations = maxEvaluations;
    m_cancel.store(false, std::memory_order_relaxed);
    start();
}

void StudyWorker::startTolerance(const SimConfig& cfg,
                                 const std::vector<studies::Tolerance>& tolerances,
                                 studies::Metric metric, double criterion, bool passIsAbove,
                                 int samples) {
    if (isRunning()) return;
    m_job         = Job::Tolerancing;
    m_cfg         = cfg;
    m_tolerances  = tolerances;
    m_metric      = metric;
    m_criterion   = criterion;
    m_passIsAbove = passIsAbove;
    m_samples     = samples;
    m_cancel.store(false, std::memory_order_relaxed);
    start();
}

void StudyWorker::cancel() {
    m_cancel.store(true, std::memory_order_relaxed);
}

void StudyWorker::run() {
    TraceControl ctl;
    ctl.cancel = &m_cancel;
    // Progress is per completed point rather than per ray: a convergence sweep
    // spaces its points geometrically, so the last one alone is most of the work
    // and a ray-level bar would sit at 90 % for the whole thing.
    ctl.progress = [this](std::size_t done, std::size_t total) {
        emit progress(total ? std::clamp(int(100.0 * double(done) / double(total)), 0, 100) : 100);
    };

    switch (m_job) {
    case Job::Convergence:
        emit convergenceReady(studies::convergence(m_cfg, m_min, m_max, m_points, ctl));
        break;
    case Job::Sweep:
        emit sweepReady(studies::parameterSweep(m_cfg, m_slot, m_from, m_to, m_steps,
                                                m_metric, m_repeats, ctl));
        break;
    case Job::Optimisation:
        emit optimisationReady(studies::optimise(m_cfg, m_optSlots, m_objective,
                                                 m_method, m_maxEvaluations, ctl));
        break;
    case Job::Tolerancing:
        emit toleranceReady(studies::tolerance(m_cfg, m_tolerances, m_metric, m_criterion,
                                               m_passIsAbove, m_samples, ctl));
        break;
    }
}
