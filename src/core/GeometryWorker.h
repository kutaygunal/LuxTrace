#pragma once
#include <QMutex>
#include <QString>
#include <QThread>
#include <QWaitCondition>
#include <QtGlobal>

#include "Simulation.h"

// Builds ray-trace-ready geometry off the GUI thread.
//
// Editing a dimension used to run OCCT's modelling kernel, its tessellator and
// the BVH build inside the spin box's valueChanged, which is why dragging one
// felt like the window had died: every step blocked the event loop for as long
// as the whole build took. The work is unchanged -- it is the same
// Simulation::dataFor every trace goes through, so the cache it fills is the
// one the run reads -- it simply happens here.
//
// Requests coalesce. A drag across twenty values is only ever worth the
// geometry it ends on, so a request arriving while a build is in flight
// replaces whatever else was queued, and a finished build whose parameters have
// already been superseded is dropped rather than drawn.
//
// It lives in the core library rather than beside the widgets for the same
// reason SimulationWorker does: no widget dependency, so the tests can exercise
// the coalescing the window relies on.
class GeometryWorker : public QThread {
    Q_OBJECT
public:
    explicit GeometryWorker(QObject* parent = nullptr);
    ~GeometryWorker() override;

    // Queues a build and returns the generation stamp it will report back with.
    // The caller compares that against the newest stamp it issued to know
    // whether the geometry that arrived is still the geometry it wants.
    quint64 request(const SimConfig& cfg);

signals:
    void geometryReady(Simulation::SceneRef data, quint64 generation);
    void geometryFailed(const QString& message, quint64 generation);

protected:
    void run() override;

private:
    QMutex         m_mutex;
    QWaitCondition m_wake;
    SimConfig      m_pending;
    bool           m_havePending = false;
    bool           m_quit        = false;
    quint64        m_generation  = 0;
};
