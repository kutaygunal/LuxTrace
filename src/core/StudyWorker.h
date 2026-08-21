#pragma once
#include <QMetaType>
#include <QThread>
#include <atomic>
#include <vector>
#include "Studies.h"

Q_DECLARE_METATYPE(std::vector<studies::ConvergencePoint>)

// Runs a convergence sweep off the calling thread.
//
// A sweep is a dozen full traces back to back -- at the top of the range that is
// seconds of work -- so it needs the same off-thread treatment, progress and
// cancellation that a single run already has. It sits beside SimulationWorker
// rather than inside it because the two report different results, and folding
// them together would mean a mode flag on every signal.
class StudyWorker : public QThread {
    Q_OBJECT
public:
    explicit StudyWorker(QObject* parent = nullptr);
    ~StudyWorker() override;

    // Starts a sweep. Must not be called while one is already running.
    void startConvergence(const SimConfig& cfg, int minRays, int maxRays, int points);
    void cancel();

signals:
    void progress(int percent);
    void convergenceReady(const std::vector<studies::ConvergencePoint>& points);

protected:
    void run() override;

private:
    SimConfig         m_cfg;      // written before start(), read by run()
    int               m_min = 1000, m_max = 1000000, m_points = 10;
    std::atomic<bool> m_cancel{false};
};
