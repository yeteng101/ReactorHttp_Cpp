#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

class ServerMetrics
{
public:
    static ServerMetrics& instance();

    void connectionOpened();
    void connectionClosed();
    void connectionRejected();
    void requestCompleted(int statusCode);
    void idleTimeout();
    std::uint64_t activeConnections() const;
    void configureWorkerPool(int minWorkers, int maxWorkers);
    void setWorkerPoolState(int currentWorkers, int drainingWorkers);
    std::string toJson() const;

private:
    ServerMetrics();

    std::chrono::steady_clock::time_point m_startedAt;
    std::atomic<std::uint64_t> m_totalRequests{0};
    std::atomic<std::uint64_t> m_activeConnections{0};
    std::atomic<std::uint64_t> m_clientErrors{0};
    std::atomic<std::uint64_t> m_serverErrors{0};
    std::atomic<std::uint64_t> m_idleTimeouts{0};
    std::atomic<std::uint64_t> m_rejectedConnections{0};
    std::atomic<int> m_minWorkers{0};
    std::atomic<int> m_maxWorkers{0};
    std::atomic<int> m_currentWorkers{0};
    std::atomic<int> m_drainingWorkers{0};
};
