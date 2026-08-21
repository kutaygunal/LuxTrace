#pragma once
#include <QThread>
#include <atomic>
#include "Simulation.h"

// Runs Simulation::run off the calling thread so a GUI stays responsive while
// a trace is in flight, and exposes progress plus cooperative cancellation.
//
// It lives in the core library rather than next to the widgets because it has
// no widget dependency, which lets the tests exercise the threading and
// cancellation path the window relies on.
//
// The job is a single self-contained computation with no event loop of its own,
// so overriding QThread::run is the simplest correct shape. Signals emitted
// from run() reach GUI-thread receivers through a queued connection.
class SimulationWorker : public QThread {
    Q_OBJECT
public:
    explicit SimulationWorker(QObject* parent = nullptr);
    ~SimulationWorker() override;

    // Starts a run. Must not be called while one is already running.
    void startRun(const SimConfig& cfg);
    // Asks the trace to stop at the next chunk boundary (a few milliseconds).
    void cancel();
    bool cancelRequested() const { return m_cancel.load(std::memory_order_relaxed); }

signals:
    void progress(int percent);
    void resultReady(const SimulationResult& result);

protected:
    void run() override;

private:
    SimConfig         m_cfg;      // written before start(), read by run()
    std::atomic<bool> m_cancel{false};
};
