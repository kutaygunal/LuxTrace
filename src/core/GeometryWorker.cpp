#include "GeometryWorker.h"

#include <Standard_Failure.hxx>

GeometryWorker::GeometryWorker(QObject* parent) : QThread(parent) {
    // The built geometry crosses to the GUI thread through a queued connection,
    // so both parameter types have to be known metatypes.
    qRegisterMetaType<Simulation::SceneRef>("Simulation::SceneRef");
    qRegisterMetaType<quint64>("quint64");
}

GeometryWorker::~GeometryWorker() {
    {
        QMutexLocker lock(&m_mutex);
        m_quit = true;
    }
    m_wake.wakeAll();
    wait();
}

quint64 GeometryWorker::request(const SimConfig& cfg) {
    quint64 gen = 0;
    {
        QMutexLocker lock(&m_mutex);
        m_pending     = cfg;
        m_havePending = true;
        gen = ++m_generation;
    }
    m_wake.wakeAll();
    // Started lazily so a session that never edits a dimension never spins the
    // thread up at all.
    if (!isRunning()) start();
    return gen;
}

void GeometryWorker::run() {
    for (;;) {
        SimConfig cfg;
        quint64   gen = 0;
        {
            QMutexLocker lock(&m_mutex);
            while (!m_havePending && !m_quit) m_wake.wait(&m_mutex);
            if (m_quit) return;
            cfg           = m_pending;
            gen           = m_generation;
            m_havePending = false;
        }

        Simulation::SceneRef data;
        QString              failure;
        try {
            data = Simulation::dataFor(cfg);
        } catch (const Standard_Failure& e) {
            // A parameter combination the kernel cannot build reports itself
            // rather than taking the window down with it.
            failure = QString::fromUtf8(e.GetMessageString());
        }

        {
            // Newer parameters arrived while this was building: drawing this
            // result would be drawing the state the user has already left, and
            // the build that replaces it is about to start anyway.
            QMutexLocker lock(&m_mutex);
            if (m_havePending || m_quit) continue;
        }

        if (failure.isEmpty()) emit geometryReady(data, gen);
        else                   emit geometryFailed(failure, gen);
    }
}
