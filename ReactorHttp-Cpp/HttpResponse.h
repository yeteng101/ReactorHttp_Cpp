#pragma once
#include "Buffer.h"
#include <cstdint>
#include <map>
#include <string>
using namespace std;

// 定义状态码枚举
enum class StatusCode
{
    Unknown,
    OK = 200,
    NoContent = 204,
    PartialContent = 206,
    MovedPermanently = 301,
    MovedTemporarily = 302,
    BadRequest = 400,
    Unauthorized = 401,
    Forbidden = 403,
    NotFound = 404,
    MethodNotAllowed = 405,
    Conflict = 409,
    PayloadTooLarge = 413,
    RangeNotSatisfiable = 416,
    ServiceUnavailable = 503,
    InternalServerError = 500
};

// 定义结构体
class HttpResponse
{
public:
    HttpResponse();
    ~HttpResponse();
    void reset();
    // 添加响应头
    void addHeader(const string key, const string value);
    // 组织http响应数据
    void prepareMsg(Buffer* sendBuf, int socket);
    inline void setFileName(string name)
    {
        m_fileName = name;
    }
    inline void setStatusCode(StatusCode code)
    {
        m_statusCode = code;
    }
    inline StatusCode getStatusCode() const
    {
        return m_statusCode;
    }
    inline void setBody(const string& body)
    {
        m_body = body;
    }
    inline void setKeepAlive(bool keepAlive)
    {
        m_keepAlive = keepAlive;
    }
    inline bool isKeepAlive() const
    {
        return m_keepAlive;
    }
    // HEAD 请求：只回响应头，不回正文
    inline void setHeadOnly(bool headOnly)
    {
        m_headOnly = headOnly;
    }
    inline bool isHeadOnly() const
    {
        return m_headOnly;
    }
    // 让响应携带请求信息，供访问日志使用
    void setRequestInfo(const string& method, const string& url, const string& version);
    inline const string& requestMethod() const
    {
        return m_requestMethod;
    }
    inline const string& requestUrl() const
    {
        return m_requestUrl;
    }
    inline const string& requestVersion() const
    {
        return m_requestVersion;
    }
    // 本次响应生成的字节数（不含文件正文）
    inline uint64_t responseBytes() const
    {
        return m_responseBytes;
    }
    // Keep-Alive 参数
    inline void setKeepAliveLimits(int timeoutSeconds, int maxRequests)
    {
        m_keepAliveTimeout = timeoutSeconds;
        m_keepAliveMax = maxRequests;
    }
    // 静态文件：由连接层在写事件里从 offset 开始流式发送 size 字节
    inline void setFileToSend(const string& path, uint64_t offset, uint64_t size)
    {
        m_fileToSend = path;
        m_fileOffset = offset;
        m_fileSize = size;
    }
    inline bool hasFile() const
    {
        return !m_fileToSend.empty();
    }
    inline const string& fileName() const
    {
        return m_fileToSend;
    }
    inline uint64_t fileSize() const
    {
        return m_fileSize;
    }
    inline uint64_t fileOffset() const
    {
        return m_fileOffset;
    }
private:
    // 状态行: 状态码, 状态描述
    StatusCode m_statusCode;
    string m_fileName;
    string m_body;
    string m_fileToSend;
    uint64_t m_fileOffset = 0;
    uint64_t m_fileSize = 0;
    bool m_keepAlive = false;
    bool m_headOnly = false;
    string m_requestMethod;
    string m_requestUrl;
    string m_requestVersion;
    uint64_t m_responseBytes = 0;
    int m_keepAliveTimeout = 30;
    int m_keepAliveMax = 100;
    // 响应头 - 键值对
    map<string, string> m_headers;
    // 定义状态码和描述的对应关系
    const map<int, string> m_info = {
        {200, "OK"},
        {204, "No Content"},
        {206, "Partial Content"},
        {301, "MovedPermanently"},
        {302, "MovedTemporarily"},
        {400, "Bad Request"},
        {401, "Unauthorized"},
        {403, "Forbidden"},
        {404, "Not Found"},
        {405, "Method Not Allowed"},
        {409, "Conflict"},
        {413, "Payload Too Large"},
        {416, "Range Not Satisfiable"},
        {500, "Internal Server Error"},
        {503, "Service Unavailable"},
    };
};
