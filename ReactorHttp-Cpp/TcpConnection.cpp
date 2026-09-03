#include "TcpConnection.h"
#include "ServerMetrics.h"
#include "TcpServer.h"
#include "Log.h"

#include <arpa/inet.h>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/sendfile.h>
#endif

namespace
{
constexpr int MaxPipelinedRequests = 16;
#ifdef __linux__
constexpr uint64_t MaxFileChunk = 1u << 20;    // 每次 sendfile 最多 1MB
#endif
}

TcpConnection::TcpConnection(int fd, EventLoop* evloop, TcpServer* server,
    int idleTimeoutSeconds, int maxRequestsPerConnection, ServerContext* serverContext)
    : m_name("Connection-" + to_string(fd)),
      m_evLoop(evloop),
      m_readBuf(new Buffer(10240)),
      m_writeBuf(new Buffer(10240)),
      m_request(new HttpRequest),
      m_response(new HttpResponse),
      m_server(server),
      m_idleTimeoutSeconds(idleTimeoutSeconds),
      m_maxRequestsPerConnection(maxRequestsPerConnection),
      m_lastActivity(std::chrono::steady_clock::now())
{
    // 网盘模式等服务器级状态：每个连接的所有请求共享同一份上下文
    m_request->setContext(serverContext);

    // 记录远端地址，供访问日志使用
    struct sockaddr_storage address;
    socklen_t addressLength = sizeof(address);
    if (getpeername(fd, reinterpret_cast<struct sockaddr*>(&address), &addressLength) == 0)
    {
        if (address.ss_family == AF_INET)
        {
            auto* ipv4 = reinterpret_cast<struct sockaddr_in*>(&address);
            char buffer[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &ipv4->sin_addr, buffer, sizeof(buffer)) != nullptr)
            {
                m_peerIp = buffer;
            }
            m_peerPort = ntohs(ipv4->sin_port);
        }
        else if (address.ss_family == AF_INET6)
        {
            auto* ipv6 = reinterpret_cast<struct sockaddr_in6*>(&address);
            char buffer[INET6_ADDRSTRLEN];
            if (inet_ntop(AF_INET6, &ipv6->sin6_addr, buffer, sizeof(buffer)) != nullptr)
            {
                m_peerIp = buffer;
            }
            m_peerPort = ntohs(ipv6->sin6_port);
        }
    }
    if (m_peerIp.empty())
    {
        m_peerIp = "unknown";
    }

    m_response->setKeepAliveLimits(idleTimeoutSeconds, maxRequestsPerConnection);
    m_channel = new Channel(fd, FDEvent::ReadEvent, processRead, processWrite, destroy, this,
        checkTimeout);
    m_evLoop->connectionOpened();
    evloop->addTask(m_channel, ElemType::ADD);
    ServerMetrics::instance().connectionOpened();
}

TcpConnection::~TcpConnection()
{
    closePendingFile();
    if (m_server != nullptr)
    {
        m_server->unregisterConnection(this);
    }
    delete m_readBuf;
    delete m_writeBuf;
    delete m_request;
    delete m_response;
    ServerMetrics::instance().connectionClosed();
    m_evLoop->connectionClosed();
    m_evLoop->freeChannel(m_channel);
    Debug("connection closed, resources released, connName: %s", m_name.c_str());
}

int TcpConnection::processRead(void* arg)
{
    TcpConnection* conn = static_cast<TcpConnection*>(arg);
    const int socket = conn->m_channel->getSocket();
    const int count = conn->m_readBuf->socketRead(socket);

    if (count > 0)
    {
        conn->m_lastActivity = std::chrono::steady_clock::now();
        int parsedRequests = 0;
        while (conn->m_readBuf->readableSize() > 0 &&
            parsedRequests++ < MaxPipelinedRequests && !conn->m_closeAfterWrite &&
            conn->m_fileFd < 0)   // 前一个文件还没发完时不解析新请求
        {
            // 请求数超限或服务器停机排空时，本次响应后强制关闭连接
            const bool forceClose = conn->m_draining.load() ||
                conn->m_requestsServed + 1 >= conn->m_maxRequestsPerConnection;
            conn->m_request->setForceClose(forceClose);
            conn->m_lastRequestStart = std::chrono::steady_clock::now();

            const bool responseReady = conn->m_request->parseHttpRequest(
                conn->m_readBuf, conn->m_response, conn->m_writeBuf, socket);
            if (!responseReady)
            {
                break;
            }

            ++conn->m_requestsServed;
            conn->m_closeAfterWrite = !conn->m_response->isKeepAlive() ||
                conn->m_draining.load();

            // 静态文件：打开 fd，之后在写事件里用 sendfile 流式发送，避免整文件读进内存
            if (conn->m_response->hasFile() && !conn->m_response->isHeadOnly())
            {
                conn->closePendingFile();
                const std::string& path = conn->m_response->fileName();
                int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
                if (fd >= 0)
                {
                    conn->m_fileFd = fd;
                    conn->m_fileOffset = conn->m_response->fileOffset();
                    conn->m_fileRemaining = conn->m_response->fileSize();
                }
                else
                {
                    Log::warn("open file failed: %s (%s)", path.c_str(), strerror(errno));
                    conn->m_closeAfterWrite = true;
                }
            }

            // 访问日志
            const auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - conn->m_lastRequestStart).count();
            uint64_t bytes = conn->m_response->responseBytes();
            if (conn->m_response->hasFile() && !conn->m_response->isHeadOnly())
            {
                bytes += conn->m_response->fileSize();
            }
            Log::access("%s - - \"%s %s %s\" %d %llu %lldms",
                conn->m_peerIp.c_str(),
                conn->m_response->requestMethod().c_str(),
                conn->m_response->requestUrl().c_str(),
                conn->m_response->requestVersion().c_str(),
                static_cast<int>(conn->m_response->getStatusCode()),
                static_cast<unsigned long long>(bytes),
                static_cast<long long>(durationMs));

            conn->m_channel->writeEventEnable(true);
            conn->m_evLoop->addTask(conn->m_channel, ElemType::MODIFY);
        }
    }
    else if (count == 0 || count == -1)
    {
        conn->m_evLoop->addTask(conn->m_channel, ElemType::DELETE);
    }
    return 0;
}

int TcpConnection::processWrite(void* arg)
{
    TcpConnection* conn = static_cast<TcpConnection*>(arg);
    const int socket = conn->m_channel->getSocket();

    const int count = conn->m_writeBuf->sendData(socket);
    if (count > 0)
    {
        conn->m_lastActivity = std::chrono::steady_clock::now();
    }
    if (count < 0)
    {
        conn->m_evLoop->addTask(conn->m_channel, ElemType::DELETE);
        return 0;
    }

    // 响应头已发完，且还有静态文件正文要发 → 用 sendfile 流式发送
    if (conn->m_writeBuf->readableSize() == 0 && conn->m_fileFd >= 0)
    {
        conn->sendPendingFile(socket);
    }

    if (conn->m_fileFd < 0 && conn->m_writeBuf->readableSize() == 0)
    {
        if (conn->m_closeAfterWrite)
        {
            conn->m_evLoop->addTask(conn->m_channel, ElemType::DELETE);
        }
        else
        {
            conn->m_channel->writeEventEnable(false);
            conn->m_evLoop->addTask(conn->m_channel, ElemType::MODIFY);
            // 文件发完、还有管道化的请求已在读缓冲里：继续解析，避免一直等新数据
            if (conn->m_readBuf->readableSize() > 0)
            {
                processRead(arg);
            }
        }
    }
    // 否则缓冲区或文件还有数据，保持写事件，等下次可写继续发送
    return 0;
}

bool TcpConnection::sendPendingFile(int socket)
{
    bool progress = false;
#ifdef __linux__
    while (m_fileRemaining > 0 && m_fileFd >= 0)
    {
        const size_t chunk = m_fileRemaining < MaxFileChunk
            ? static_cast<size_t>(m_fileRemaining) : MaxFileChunk;
        ssize_t sent = sendfile(socket, m_fileFd, reinterpret_cast<off_t*>(&m_fileOffset), chunk);
        if (sent < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;    // 内核发送缓冲满，等下一次写事件
            }
            Log::warn("sendfile error on %s: %s", m_peerIp.c_str(), strerror(errno));
            closePendingFile();
            m_closeAfterWrite = true;
            break;
        }
        if (sent == 0)
        {
            break;
        }
        progress = true;
        m_fileRemaining -= static_cast<uint64_t>(sent);
        m_lastActivity = std::chrono::steady_clock::now();
    }
#else
    // macOS/其他平台没有 Linux 版 sendfile，用 pread + send 分块发送
    char buffer[65536];
    while (m_fileRemaining > 0 && m_fileFd >= 0)
    {
        const size_t want = m_fileRemaining < sizeof(buffer)
            ? static_cast<size_t>(m_fileRemaining) : sizeof(buffer);
        ssize_t readCount = pread(m_fileFd, buffer, want, static_cast<off_t>(m_fileOffset));
        if (readCount < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            Log::warn("pread error on %s: %s", m_peerIp.c_str(), strerror(errno));
            closePendingFile();
            m_closeAfterWrite = true;
            break;
        }
        if (readCount == 0)
        {
            m_fileRemaining = 0;
            break;
        }
        ssize_t sent;
#ifdef MSG_NOSIGNAL
        const int sendFlags = MSG_NOSIGNAL;
#else
        const int sendFlags = 0;
#endif
        do
        {
            sent = send(socket, buffer, static_cast<size_t>(readCount), sendFlags);
        } while (sent == -1 && errno == EINTR);
        if (sent < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            Log::warn("send error on %s: %s", m_peerIp.c_str(), strerror(errno));
            closePendingFile();
            m_closeAfterWrite = true;
            break;
        }
        progress = true;
        m_fileOffset += static_cast<uint64_t>(sent);
        m_fileRemaining -= static_cast<uint64_t>(sent);
        m_lastActivity = std::chrono::steady_clock::now();
    }
#endif
    if (m_fileRemaining == 0)
    {
        closePendingFile();
    }
    return progress;
}

void TcpConnection::closePendingFile()
{
    if (m_fileFd >= 0)
    {
        close(m_fileFd);
        m_fileFd = -1;
        m_fileRemaining = 0;
        m_fileOffset = 0;
    }
}

void TcpConnection::closeNow()
{
    m_evLoop->addTask(m_channel, ElemType::DELETE);
}

int TcpConnection::destroy(void* arg)
{
    TcpConnection* conn = static_cast<TcpConnection*>(arg);
    if (conn != nullptr)
    {
        delete conn;
    }
    return 0;
}

int TcpConnection::checkTimeout(void* arg)
{
    TcpConnection* conn = static_cast<TcpConnection*>(arg);
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - conn->m_lastActivity).count();
    if (elapsed >= conn->m_idleTimeoutSeconds)
    {
        ServerMetrics::instance().idleTimeout();
        return 1;
    }
    return 0;
}

void TcpConnection::beginShutdown()
{
    m_draining.store(true);
    // 必须在连接所属的事件循环线程里访问缓冲区和通道
    m_evLoop->queueInLoop([this] {
        if (m_writeBuf->readableSize() == 0 && m_fileFd < 0 && m_readBuf->readableSize() == 0)
        {
            m_evLoop->addTask(m_channel, ElemType::DELETE);
        }
        else
        {
            m_closeAfterWrite = true;
        }
    });
}

void TcpConnection::forceClose()
{
    m_draining.store(true);
    m_evLoop->queueInLoop([this] {
        m_evLoop->addTask(m_channel, ElemType::DELETE);
    });
}
