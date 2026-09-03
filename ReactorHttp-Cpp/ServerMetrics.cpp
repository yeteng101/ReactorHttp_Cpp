#include "ServerMetrics.h"

ServerMetrics& ServerMetrics::instance()
{
    static ServerMetrics metrics;
    return metrics;
}

ServerMetrics::ServerMetrics() : m_startedAt(std::chrono::steady_clock::now())
{
}

void ServerMetrics::connectionOpened()
{
    m_activeConnections.fetch_add(1, std::memory_order_relaxed);
}

void ServerMetrics::connectionClosed()
{
    m_activeConnections.fetch_sub(1, std::memory_order_relaxed);
}

void ServerMetrics::connectionRejected()
{
    m_rejectedConnections.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t ServerMetrics::activeConnections() const
{
    return m_activeConnections.load(std::memory_order_relaxed);
}

void ServerMetrics::requestCompleted(int statusCode)
{
    m_totalRequests.fetch_add(1, std::memory_order_relaxed);
    if (statusCode >= 500)
    {
        m_serverErrors.fetch_add(1, std::memory_order_relaxed);
    }
    else if (statusCode >= 400)
    {
        m_clientErrors.fetch_add(1, std::memory_order_relaxed);
    }
}

void ServerMetrics::idleTimeout()
{
    m_idleTimeouts.fetch_add(1, std::memory_order_relaxed);
}

void ServerMetrics::configureWorkerPool(int minWorkers, int maxWorkers)
{
    m_minWorkers.store(minWorkers, std::memory_order_relaxed);
    m_maxWorkers.store(maxWorkers, std::memory_order_relaxed);
}

void ServerMetrics::setWorkerPoolState(int currentWorkers, int drainingWorkers)
{
    m_currentWorkers.store(currentWorkers, std::memory_order_relaxed);
    m_drainingWorkers.store(drainingWorkers, std::memory_order_relaxed);
}

std::string ServerMetrics::toJson() const
{
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - m_startedAt).count();
    return "{\"status\":\"ok\",\"uptime_seconds\":" + std::to_string(uptime) +
        ",\"total_requests\":" + std::to_string(m_totalRequests.load(std::memory_order_relaxed)) +
        ",\"active_connections\":" + std::to_string(m_activeConnections.load(std::memory_order_relaxed)) +
        ",\"client_errors\":" + std::to_string(m_clientErrors.load(std::memory_order_relaxed)) +
        ",\"server_errors\":" + std::to_string(m_serverErrors.load(std::memory_order_relaxed)) +
        ",\"idle_timeouts\":" + std::to_string(m_idleTimeouts.load(std::memory_order_relaxed)) +
        ",\"rejected_connections\":" + std::to_string(m_rejectedConnections.load(std::memory_order_relaxed)) +
        ",\"worker_threads\":" + std::to_string(m_currentWorkers.load(std::memory_order_relaxed)) +
        ",\"min_worker_threads\":" + std::to_string(m_minWorkers.load(std::memory_order_relaxed)) +
        ",\"max_worker_threads\":" + std::to_string(m_maxWorkers.load(std::memory_order_relaxed)) +
        ",\"draining_worker_threads\":" + std::to_string(m_drainingWorkers.load(std::memory_order_relaxed)) + "}";
}
