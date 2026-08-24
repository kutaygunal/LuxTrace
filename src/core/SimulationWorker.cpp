#include "SimulationWorker.h"

#include <algorithm>

SimulationWorker::SimulationWorker(QObject* parent) : QThread(parent) {
    // SimulationResult travels across a queued connection, so it needs to be a
    // known metatype.
    qRegisterMetaType<SimulationResult>("SimulationResult");
}

SimulationWorker::~SimulationWorker() {
    cancel();
    wait();
}

void SimulationWorker::startRun(const SimConfig& cfg) {
    if (isRunning()) return;
    m_cfg = cfg;
    m_cancel.store(false, std::memory_order_relaxed);
    start();
}

void SimulationWorker::cancel() {
    m_cancel.store(true, std::memory_order_relaxed);
}

void SimulationWorker::run() {
    TraceControl ctl;
    ctl.cancel = &m_cancel;

    int lastPercent = -1;
    ctl.progress = [this, &lastPercent](std::size_t done, std::size_t total) {
        const int pct = total ? int(100.0 * double(done) / double(total)) : 100;
        if (pct != lastPercent) {
            lastPercent = pct;
            emit progress(std::clamp(pct, 0, 100));
        }
    };

    if (m_partialMs > 0) {
        ctl.partialIntervalMs = m_partialMs;
        ctl.partial = [this](const SimulationResult& snap) { emit partialReady(snap); };
    }

    emit resultReady(Simulation::run(m_cfg, ctl));
}
