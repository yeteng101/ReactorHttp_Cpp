#include "ThreadPool.h"
#include "ServerMetrics.h"

#include <algorithm>
#include <assert.h>
#include <chrono>
#include <limits>
#include <stdlib.h>

namespace
{
constexpr size_t ScaleUpConnectionsPerWorker = 64;
constexpr size_t ScaleDownConnectionsPerWorker = 8;
constexpr int ScaleDownDelaySeconds = 30;
}

ThreadPool::ThreadPool(EventLoop* mainLoop, int count)
    : ThreadPool(mainLoop, count, count)
{
}

ThreadPool::ThreadPool(EventLoop* mainLoop, int minThreads, int maxThreads)
    : m_mainLoop(mainLoop),
      m_isStart(false),
      m_minThreads(minThreads),
      m_maxThreads(maxThreads),
      m_nextWorkerId(0),
      m_lowLoadSeconds(0),
      m_stopping(false)
{
    assert(minThreads > 0 && maxThreads >= minThreads);
}

ThreadPool::~ThreadPool()
{
    stop();
}

WorkerThread* ThreadPool::addWorkerLocked()
{
    WorkerThread* worker = new WorkerThread(m_nextWorkerId++);
    worker->run();
    m_workerThreads.push_back(worker);
    updateMetricsLocked();
    return worker;
}

void ThreadPool::run()
{
    assert(!m_isStart);
    if (m_mainLoop->getThreadID() != this_thread::get_id())
    {
        exit(EXIT_FAILURE);
    }

    {
        lock_guard<mutex> lock(m_mutex);
        m_isStart = true;
        m_stopping.store(false);
        ServerMetrics::instance().configureWorkerPool(m_minThreads, m_maxThreads);
        for (int i = 0; i < m_minThreads; ++i)
        {
            addWorkerLocked();
        }
    }

    if (m_maxThreads > m_minThreads)
    {
        m_managerThread = thread(&ThreadPool::managerLoop, this);
    }
}

void ThreadPool::stop()
{
    if (!m_isStart)
    {
        return;
    }

    m_stopping.store(true);
    m_managerCondition.notify_all();
    if (m_managerThread.joinable())
    {
        m_managerThread.join();
    }

    vector<WorkerThread*> workers;
    {
        lock_guard<mutex> lock(m_mutex);
        workers.swap(m_workerThreads);
        m_isStart = false;
        updateMetricsLocked();
    }
    for (WorkerThread* worker : workers)
    {
        delete worker;
    }
}

size_t ThreadPool::availableWorkerCountLocked() const
{
    return static_cast<size_t>(count_if(m_workerThreads.begin(), m_workerThreads.end(),
        [](const WorkerThread* worker) { return !worker->isDraining(); }));
}

EventLoop* ThreadPool::takeWorkerEventLoop()
{
    assert(m_isStart);
    if (m_mainLoop->getThreadID() != this_thread::get_id())
    {
        exit(EXIT_FAILURE);
    }

    lock_guard<mutex> lock(m_mutex);
    WorkerThread* selected = nullptr;
    size_t selectedConnections = numeric_limits<size_t>::max();
    for (WorkerThread* worker : m_workerThreads)
    {
        const size_t connections = worker->connectionCount();
        if (!worker->isDraining() && connections < selectedConnections)
        {
            selected = worker;
            selectedConnections = connections;
        }
    }

    const size_t availableWorkers = availableWorkerCountLocked();
    if (selected != nullptr && selectedConnections >= ScaleUpConnectionsPerWorker &&
        availableWorkers < static_cast<size_t>(m_maxThreads))
    {
        auto draining = find_if(m_workerThreads.begin(), m_workerThreads.end(),
            [](const WorkerThread* worker) { return worker->isDraining(); });
        if (draining != m_workerThreads.end())
        {
            (*draining)->setDraining(false);
            m_lowLoadSeconds = 0;
        }
        else if (m_workerThreads.size() < static_cast<size_t>(m_maxThreads))
        {
            addWorkerLocked();
        }

        selected = nullptr;
        selectedConnections = numeric_limits<size_t>::max();
        for (WorkerThread* worker : m_workerThreads)
        {
            const size_t connections = worker->connectionCount();
            if (!worker->isDraining() && connections < selectedConnections)
            {
                selected = worker;
                selectedConnections = connections;
            }
        }
        updateMetricsLocked();
    }

    return selected == nullptr ? m_mainLoop : selected->getEventLoop();
}

void ThreadPool::managerLoop()
{
    while (!m_stopping.load())
    {
        vector<WorkerThread*> stoppedWorkers;
        {
            unique_lock<mutex> lock(m_mutex);
            m_managerCondition.wait_for(lock, chrono::seconds(1),
                [this] { return m_stopping.load(); });
            if (m_stopping.load())
            {
                break;
            }

            for (auto it = m_workerThreads.begin(); it != m_workerThreads.end();)
            {
                WorkerThread* worker = *it;
                if (worker->isDraining() && worker->connectionCount() == 0 &&
                    m_workerThreads.size() > static_cast<size_t>(m_minThreads))
                {
                    stoppedWorkers.push_back(worker);
                    it = m_workerThreads.erase(it);
                }
                else
                {
                    ++it;
                }
            }

            const size_t availableWorkers = availableWorkerCountLocked();
            const bool alreadyDraining = any_of(m_workerThreads.begin(), m_workerThreads.end(),
                [](const WorkerThread* worker) { return worker->isDraining(); });
            size_t totalConnections = 0;
            for (const WorkerThread* worker : m_workerThreads)
            {
                if (!worker->isDraining())
                {
                    totalConnections += worker->connectionCount();
                }
            }

            if (!alreadyDraining && availableWorkers > static_cast<size_t>(m_minThreads) &&
                totalConnections <= (availableWorkers - 1) * ScaleDownConnectionsPerWorker)
            {
                ++m_lowLoadSeconds;
            }
            else
            {
                m_lowLoadSeconds = 0;
            }

            if (m_lowLoadSeconds >= ScaleDownDelaySeconds)
            {
                WorkerThread* candidate = nullptr;
                size_t fewestConnections = numeric_limits<size_t>::max();
                for (WorkerThread* worker : m_workerThreads)
                {
                    const size_t connections = worker->connectionCount();
                    if (!worker->isDraining() && connections < fewestConnections)
                    {
                        candidate = worker;
                        fewestConnections = connections;
                    }
                }
                if (candidate != nullptr)
                {
                    candidate->setDraining(true);
                }
                m_lowLoadSeconds = 0;
            }
            updateMetricsLocked();
        }

        for (WorkerThread* worker : stoppedWorkers)
        {
            delete worker;
        }
    }
}

void ThreadPool::updateMetricsLocked() const
{
    int draining = 0;
    for (const WorkerThread* worker : m_workerThreads)
    {
        if (worker->isDraining())
        {
            ++draining;
        }
    }
    ServerMetrics::instance().setWorkerPoolState(
        static_cast<int>(m_workerThreads.size()), draining);
}

size_t ThreadPool::workerCount() const
{
    lock_guard<mutex> lock(m_mutex);
    return m_workerThreads.size();
}

void ThreadPool::forEachWorkerEventLoop(const function<void(EventLoop*)>& fn)
{
    vector<EventLoop*> loops;
    {
        lock_guard<mutex> lock(m_mutex);
        loops.reserve(m_workerThreads.size());
        for (WorkerThread* worker : m_workerThreads)
        {
            EventLoop* loop = worker->getEventLoop();
            if (loop != nullptr)
            {
                loops.push_back(loop);
            }
        }
    }
    for (EventLoop* loop : loops)
    {
        fn(loop);
    }
}

size_t ThreadPool::totalConnections() const
{
    size_t total = 0;
    lock_guard<mutex> lock(m_mutex);
    for (const WorkerThread* worker : m_workerThreads)
    {
        total += worker->connectionCount();
    }
    return total;
}
