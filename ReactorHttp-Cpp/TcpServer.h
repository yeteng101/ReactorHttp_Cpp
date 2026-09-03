#pragma once
#include "Config.h"
#include "EventLoop.h"
#include "ThreadPool.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_set>
#include <vector>

class TcpConnection;

class TcpServer
{
public:
    explicit TcpServer(const ServerConfig& config);
    ~TcpServer();
    // 启动服务器（阻塞运行直到收到 SIGTERM/SIGINT 或优雅停机完成）
    void run();
    static int acceptConnection(void* arg);
    void registerConnection(TcpConnection* connection);
    void unregisterConnection(TcpConnection* connection);

private:
    static void handleSignal(int signal);
    static int handleSignalPipe(void* arg);
    void beginShutdown();
    void shutdownTick();
    void forceCloseAllConnections();
    void installSignalHandlers();
    void setListen();

    ServerConfig m_config;
    int m_minThreads;
    int m_maxThreads;
    EventLoop* m_mainLoop;
    ThreadPool* m_threadPool;
    Channel* m_listenChannel = nullptr;
    int m_lfd = -1;
    static int s_signalPipe[2];
    std::atomic<bool> m_shuttingDown{false};
    std::chrono::steady_clock::time_point m_shutdownStart;
    bool m_forceCloseInitiated = false;
    bool m_finalized = false;
    std::mutex m_connMutex;
    std::unordered_set<TcpConnection*> m_connections;
};
