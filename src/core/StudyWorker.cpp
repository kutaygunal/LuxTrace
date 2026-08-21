#include "StudyWorker.h"

#include <algorithm>

StudyWorker::StudyWorker(QObject* parent) : QThread(parent) {
    // The sweep travels across a queued connection, so it needs to be a known
    // metatype.
    qRegisterMetaType<std::vector<studies::ConvergencePoint>>(
        "std::vector<studies::ConvergencePoint>");
}

StudyWorker::~StudyWorker() {
    cancel();
    wait();
}

void StudyWorker::startConvergence(const SimConfig& cfg, int minRays, int maxRays, int points) {
    if (isRunning()) return;
    m_cfg    = cfg;
    m_min    = minRays;
    m_max    = maxRays;
    m_points = points;
    m_cancel.store(false, std::memory_order_relaxed);
    start();
}

void StudyWorker::cancel() {
    m_cancel.store(true, std::memory_order_relaxed);
}

void StudyWorker::run() {
    TraceControl ctl;
    ctl.cancel = &m_cancel;
    // Progress is per completed point rather than per ray: the points are spaced
    // geometrically, so the last one alone is most of the work and a ray-level
    // bar would sit at 90 % for the whole sweep.
    ctl.progress = [this](std::size_t done, std::size_t total) {
        emit progress(total ? std::clamp(int(100.0 * double(done) / double(total)), 0, 100) : 100);
    };

    emit convergenceReady(studies::convergence(m_cfg, m_min, m_max, m_points, ctl));
}
