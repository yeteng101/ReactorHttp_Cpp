#include "TcpServer.h"
#include "Log.h"
#include "ServerMetrics.h"
#include "TcpConnection.h"

#include <arpa/inet.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
const char* RejectedResponse =
    "HTTP/1.1 503 Service Unavailable\r\n"
    "Connection: close\r\n"
    "Content-Length: 0\r\n"
    "Server: ReactorHttp-Cpp\r\n\r\n";
}

int TcpServer::s_signalPipe[2] = {-1, -1};

TcpServer::TcpServer(const ServerConfig& config)
    : m_config(config),
      m_minThreads(config.minWorkers),
      m_maxThreads(config.maxWorkers)
{
    m_mainLoop = new EventLoop;
    m_threadPool = new ThreadPool(m_mainLoop, m_minThreads, m_maxThreads);
    installSignalHandlers();
    setListen();
}

TcpServer::~TcpServer()
{
    if (!m_finalized && m_threadPool != nullptr)
    {
        m_threadPool->stop();
    }
    delete m_threadPool;
    delete m_mainLoop;
    if (s_signalPipe[0] != -1)
    {
        close(s_signalPipe[0]); s_signalPipe[0] = -1;
    }
    if (s_signalPipe[1] != -1)
    {
        close(s_signalPipe[1]); s_signalPipe[1] = -1;
    }
}

void TcpServer::installSignalHandlers()
{
    if (pipe(s_signalPipe) != 0)
    {
        perror("pipe");
        exit(EXIT_FAILURE);
    }
    for (int i = 0; i < 2; ++i)
    {
        int flags = fcntl(s_signalPipe[i], F_GETFL, 0);
        fcntl(s_signalPipe[i], F_SETFL, flags | O_NONBLOCK);
        fcntl(s_signalPipe[i], F_SETFD, FD_CLOEXEC);
    }

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handleSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
}

void TcpServer::handleSignal(int signal)
{
    // 信号处理器里只做 async-signal-safe 操作：向管道写一个字节唤醒事件循环
    const char byte = static_cast<char>(signal);
    const int savedErrno = errno;
    (void)!write(s_signalPipe[1], &byte, 1);
    errno = savedErrno;
}

int TcpServer::handleSignalPipe(void* arg)
{
    TcpServer* server = static_cast<TcpServer*>(arg);
    char buffer[64];
    while (read(s_signalPipe[0], buffer, sizeof(buffer)) > 0)
    {
    }
    if (!server->m_shuttingDown.load())
    {
        server->beginShutdown();
    }
    return 0;
}

int TcpServer::acceptConnection(void* arg)
{
    TcpServer* server = static_cast<TcpServer*>(arg);
    while (true)
    {
        // Drain the nonblocking accept queue in one readiness callback.
        int cfd = accept(server->m_lfd, NULL, NULL);
        if (cfd == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                perror("accept");
            }
            break;
        }

        if (server->m_shuttingDown.load())
        {
            close(cfd);
            continue;
        }

        int flags = fcntl(cfd, F_GETFL, 0);
        if (flags == -1 || fcntl(cfd, F_SETFL, flags | O_NONBLOCK) == -1 ||
            fcntl(cfd, F_SETFD, FD_CLOEXEC) == -1)
        {
            close(cfd);
            continue;
        }

        // 全局连接上限：超过则回 503 并关闭
        if (ServerMetrics::instance().activeConnections() >= server->m_config.maxConnections)
        {
            ServerMetrics::instance().connectionRejected();
#ifdef MSG_NOSIGNAL
            const int sendFlags = MSG_NOSIGNAL;
#else
            const int sendFlags = 0;
#endif
            send(cfd, RejectedResponse, strlen(RejectedResponse), sendFlags);
            close(cfd);
            continue;
        }

        EventLoop* evLoop = server->m_threadPool->takeWorkerEventLoop();
        TcpConnection* connection = new TcpConnection(cfd, evLoop, server,
            server->m_config.idleTimeoutSeconds, server->m_config.maxRequestsPerConnection);
        server->registerConnection(connection);
    }
    return 0;
}

void TcpServer::setListen()
{
    m_lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_lfd == -1)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    int flags = fcntl(m_lfd, F_GETFL, 0);
    if (flags == -1 || fcntl(m_lfd, F_SETFL, flags | O_NONBLOCK) == -1 ||
        fcntl(m_lfd, F_SETFD, FD_CLOEXEC) == -1)
    {
        perror("fcntl");
        close(m_lfd);
        exit(EXIT_FAILURE);
    }
    int opt = 1;
    int ret = setsockopt(m_lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    if (ret == -1)
    {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_config.port);
    addr.sin_addr.s_addr = INADDR_ANY;
    ret = ::bind(m_lfd, (struct sockaddr*)&addr, sizeof addr);
    if (ret == -1)
    {
        perror("bind");
        exit(EXIT_FAILURE);
    }
    ret = listen(m_lfd, 1024);
    if (ret == -1)
    {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    Log::info("listening on 0.0.0.0:%hu, web root: %s, workers: %d-%d, max connections: %zu",
        m_config.port, m_config.webRoot.c_str(), m_minThreads, m_maxThreads,
        m_config.maxConnections);
}

void TcpServer::run()
{
    Log::info("server starting...");
    m_threadPool->run();

    // 监听信号管道，收到 SIGTERM/SIGINT 后触发优雅停机
    Channel* signalChannel = new Channel(s_signalPipe[0], FDEvent::ReadEvent,
        handleSignalPipe, nullptr, nullptr, this);
    m_mainLoop->addTask(signalChannel, ElemType::ADD);

    // 监听套接字
    m_listenChannel = new Channel(m_lfd, FDEvent::ReadEvent, acceptConnection,
        nullptr, nullptr, this);
    m_mainLoop->addTask(m_listenChannel, ElemType::ADD);

    // 每轮事件循环检查一次停机进度
    m_mainLoop->setIdleCallback([this] { shutdownTick(); });

    m_mainLoop->run();

    // 主循环退出：确保线程池停掉、连接全部释放
    m_finalized = true;
    m_threadPool->stop();
    Log::info("server stopped");
}

void TcpServer::beginShutdown()
{
    Log::warn("shutdown signal received, draining connections (max %d seconds)",
        m_config.gracefulShutdownSeconds);
    m_shuttingDown.store(true);
    m_shutdownStart = std::chrono::steady_clock::now();

    // 停止 accept，关闭监听套接字
    if (m_listenChannel != nullptr)
    {
        m_mainLoop->addTask(m_listenChannel, ElemType::DELETE);
        m_listenChannel = nullptr;
    }
    if (m_lfd != -1)
    {
        close(m_lfd);
        m_lfd = -1;
    }

    // 让现有连接尽快收尾：空闲的直接关闭，正在传输的响应发完再关。
    // 持锁遍历：连接析构也要拿同一把锁，防止遍历期间对象被工作线程销毁。
    // beginShutdown 只做非阻塞投递，不会等待工作线程，因此不会死锁。
    std::lock_guard<std::mutex> lock(m_connMutex);
    for (TcpConnection* connection : m_connections)
    {
        connection->beginShutdown();
    }
}

void TcpServer::shutdownTick()
{
    if (!m_shuttingDown.load() || m_finalized)
    {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - m_shutdownStart).count();

    if (!m_forceCloseInitiated && elapsed >= m_config.gracefulShutdownSeconds)
    {
        Log::warn("drain timeout (%d seconds), force closing remaining connections",
            m_config.gracefulShutdownSeconds);
        m_forceCloseInitiated = true;
        forceCloseAllConnections();
        return;    // 再给事件循环一轮时间处理关闭任务
    }

    bool drained;
    {
        std::lock_guard<std::mutex> lock(m_connMutex);
        drained = m_connections.empty();
    }
    if (drained || (m_forceCloseInitiated && elapsed >= m_config.gracefulShutdownSeconds + 2))
    {
        Log::info("all connections closed, stopping event loops");
        m_finalized = true;
        m_threadPool->stop();
        m_mainLoop->requestStop();
    }
}

void TcpServer::forceCloseAllConnections()
{
    std::lock_guard<std::mutex> lock(m_connMutex);
    for (TcpConnection* connection : m_connections)
    {
        connection->forceClose();
    }
}

void TcpServer::registerConnection(TcpConnection* connection)
{
    std::lock_guard<std::mutex> lock(m_connMutex);
    m_connections.insert(connection);
}

void TcpServer::unregisterConnection(TcpConnection* connection)
{
    std::lock_guard<std::mutex> lock(m_connMutex);
    m_connections.erase(connection);
}
