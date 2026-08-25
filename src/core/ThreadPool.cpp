#include "ThreadPool.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// One less than the hardware allows, because the caller takes part in the work
// rather than parking on a join.
unsigned defaultWorkers() {
    const unsigned hw = std::thread::hardware_concurrency();
    return hw > 1 ? hw - 1 : 0;
}

} // namespace

struct ThreadPool::Impl {
    std::vector<std::thread> threads;

    std::mutex              mutex;
    std::condition_variable wake;      // a job is available, or the pool is closing
    std::condition_variable done;      // a worker finished its share

    // The job currently in flight. A worker claims an index off `nextIndex`
    // until it runs past `activeCount`, which is what lets one job be spread
    // over however many workers the caller asked for.
    const std::function<void(unsigned)>* body = nullptr;
    unsigned activeCount = 0;          // total participants, the caller included
    std::atomic<unsigned> nextIndex{1};
    unsigned outstanding = 0;          // workers still inside the job
    std::uint64_t generation = 0;      // bumped per job, so a worker knows it is new
    bool closing = false;

    // Only one job at a time. A second caller -- a nested parallel region, or a
    // study thread overlapping the UI's -- runs on threads of its own rather
    // than waiting for a pool that is already committed.
    std::atomic<bool> busy{false};

    void workerLoop() {
        std::uint64_t seen = 0;
        for (;;) {
            const std::function<void(unsigned)>* job = nullptr;
            unsigned count = 0;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [&] { return closing || generation != seen; });
                if (closing) return;
                seen  = generation;
                job   = body;
                count = activeCount;
            }
            if (job) {
                for (;;) {
                    const unsigned i = nextIndex.fetch_add(1, std::memory_order_relaxed);
                    if (i >= count) break;
                    (*job)(i);
                }
            }

            // Unconditionally, and exactly once per generation. A worker that
            // claimed no index still has to report back, because the tally it
            // is decrementing counts wakeups rather than useful work -- see
            // runParallel.
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (outstanding > 0 && --outstanding == 0) done.notify_all();
            }
        }
    }
};

ThreadPool::ThreadPool() : m_impl(new Impl) {
    const unsigned n = defaultWorkers();
    m_impl->threads.reserve(n);
    for (unsigned i = 0; i < n; ++i)
        m_impl->threads.emplace_back([this] { m_impl->workerLoop(); });
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->closing = true;
    }
    m_impl->wake.notify_all();
    for (std::thread& t : m_impl->threads)
        if (t.joinable()) t.join();
    delete m_impl;
}

ThreadPool& ThreadPool::shared() {
    // Created on first use and never destroyed: a static with a destructor
    // would be torn down during exit while a detached study could still be
    // asking it for workers, and joining threads from a static destructor is
    // the classic way to hang on shutdown.
    static ThreadPool* pool = new ThreadPool();
    return *pool;
}

unsigned ThreadPool::workers() const { return unsigned(m_impl->threads.size()); }

void ThreadPool::runParallel(unsigned count, const std::function<void(unsigned)>& body) {
    if (count <= 1) { body(0); return; }

    bool expected = false;
    const bool mine = m_impl->busy.compare_exchange_strong(expected, true);
    const unsigned poolSize = mine ? unsigned(m_impl->threads.size()) : 0;

    // Whatever the pool cannot cover -- because it is smaller than the request,
    // or because it is already committed to another job -- is covered by
    // threads spawned here, exactly as the tracer used to do for all of it.
    const unsigned poolShare = std::min<unsigned>(count - 1, poolSize);
    const unsigned extra     = count - 1 - poolShare;

    std::vector<std::thread> spawned;
    spawned.reserve(extra);

    if (poolSize > 0) {
        {
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            m_impl->body        = &body;
            // Pool workers claim indices 1 .. poolShare; the spawned ones take
            // the rest, so every participant gets a distinct index and the
            // per-thread accumulators stay one per worker.
            m_impl->activeCount = poolShare + 1;
            m_impl->nextIndex.store(1, std::memory_order_relaxed);
            // Every worker in the pool, not just the ones with an index to
            // claim: the wake is a broadcast, so all of them run the loop and
            // all of them report back. Counting only the ones expected to do
            // work lets a worker that claimed nothing decrement the *next*
            // job's tally instead, and then the caller returns while real
            // workers are still inside the body -- reading captures that have
            // been destroyed and writing accumulators that no longer exist.
            m_impl->outstanding = poolSize;
            ++m_impl->generation;
        }
        m_impl->wake.notify_all();
    }

    for (unsigned i = 0; i < extra; ++i)
        spawned.emplace_back(body, poolShare + 1 + i);

    body(0);

    if (poolSize > 0) {
        std::unique_lock<std::mutex> lock(m_impl->mutex);
        m_impl->done.wait(lock, [&] { return m_impl->outstanding == 0; });
        m_impl->body = nullptr;
    }
    for (std::thread& t : spawned) t.join();

    if (mine) m_impl->busy.store(false);
}
