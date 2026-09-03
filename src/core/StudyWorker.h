// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once
#include <QMetaType>
#include <QThread>
#include <atomic>
#include <vector>
#include "Studies.h"

Q_DECLARE_METATYPE(std::vector<studies::ConvergencePoint>)
Q_DECLARE_METATYPE(std::vector<studies::SweepPoint>)
Q_DECLARE_METATYPE(studies::OptimisationResult)
Q_DECLARE_METATYPE(studies::ToleranceStudy)

// Runs the studies off the calling thread.
//
// A sweep is a dozen full traces back to back and an optimisation is a hundred,
// so both need the same off-thread treatment, progress and cancellation that a
// single run already has. They sit beside SimulationWorker rather than inside it
// because the three report different results, and folding them together would
// mean a mode flag on every signal.
class StudyWorker : public QThread {
    Q_OBJECT
public:
    explicit StudyWorker(QObject* parent = nullptr);
    ~StudyWorker() override;

    // Each starts a study. None may be called while one is already running.
    void startConvergence(const SimConfig& cfg, int minRays, int maxRays, int points);
    // A metric against one of the scene's own dimensions, with error bars.
    void startSweep(const SimConfig& cfg, int slotIndex, double from, double to,
                    int steps, studies::Metric metric, int repeats);
    // A search for the design that makes a metric best.
    void startOptimisation(const SimConfig& cfg, const std::vector<int>& paramSlots,
                           const studies::Objective& objective,
                           studies::Optimiser method, int maxEvaluations);
    // Perturbs the design many times over and reports the yield.
    void startTolerance(const SimConfig& cfg, const std::vector<studies::Tolerance>& tolerances,
                        studies::Metric metric, double criterion, bool passIsAbove,
                        int samples);
    void cancel();

    // What the last sweep was of, so the receiving end can label its axes
    // without having to remember what it asked for.
    int             sweepSlot() const { return m_slot; }
    studies::Metric sweepMetric() const { return m_metric; }
    const std::vector<int>&    optimisedSlots() const { return m_optSlots; }
    const studies::Objective&  objective() const { return m_objective; }

signals:
    void progress(int percent);
    void convergenceReady(const std::vector<studies::ConvergencePoint>& points);
    void sweepReady(const std::vector<studies::SweepPoint>& points);
    void optimisationReady(const studies::OptimisationResult& result);
    void toleranceReady(const studies::ToleranceStudy& study);

protected:
    void run() override;

private:
    enum class Job { Convergence, Sweep, Optimisation, Tolerancing };

    // All written before start(), read by run().
    Job               m_job = Job::Convergence;
    SimConfig         m_cfg;
    int               m_min = 1000, m_max = 1000000, m_points = 10;

    int               m_slot = 0;
    double            m_from = 0.0, m_to = 1.0;
    int               m_steps = 11, m_repeats = 2;
    studies::Metric   m_metric = studies::Metric::Efficiency;

    std::vector<int>     m_optSlots;
    studies::Objective   m_objective;
    studies::Optimiser   m_method = studies::Optimiser::NelderMead;
    int                  m_maxEvaluations = 100;

    std::vector<studies::Tolerance> m_tolerances;
    double m_criterion = 0.0;
    bool   m_passIsAbove = true;
    int    m_samples = 100;

    std::atomic<bool> m_cancel{false};
};
