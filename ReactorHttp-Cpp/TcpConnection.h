#pragma once
#include "EventLoop.h"
#include "Buffer.h"
#include "Channel.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

class TcpServer;
struct ServerContext;

class TcpConnection
{
public:
    TcpConnection(int fd, EventLoop* evloop, TcpServer* server, int idleTimeoutSeconds,
        int maxRequestsPerConnection, ServerContext* serverContext);
    ~TcpConnection();

    static int processRead(void* arg);
    static int processWrite(void* arg);
    static int destroy(void* arg);
    static int checkTimeout(void* arg);

    // 服务器停机时调用（可从主线程调用）：标记排空，让连接尽快收尾
    void beginShutdown();
    // 强制立刻关闭（停机超时后调用）
    void forceClose();

    inline const std::string& peerIp() const
    {
        return m_peerIp;
    }

private:
    bool sendPendingFile(int socket);
    void closePendingFile();
    void closeNow();

    string m_name;
    EventLoop* m_evLoop;
    Channel* m_channel;
    Buffer* m_readBuf;
    Buffer* m_writeBuf;
    // http 协议
    HttpRequest* m_request;
    HttpResponse* m_response;
    TcpServer* m_server;
    // 配置
    int m_idleTimeoutSeconds;
    int m_maxRequestsPerConnection;
    int m_requestsServed = 0;
    // 远端地址
    std::string m_peerIp;
    unsigned short m_peerPort = 0;
    // 流式发送文件的状态
    int m_fileFd = -1;
    uint64_t m_fileOffset = 0;
    uint64_t m_fileRemaining = 0;
    // 请求开始时间（访问日志用）
    std::chrono::steady_clock::time_point m_lastRequestStart;
    // 状态
    std::chrono::steady_clock::time_point m_lastActivity;
    bool m_closeAfterWrite = false;
    std::atomic<bool> m_draining{false};
};
