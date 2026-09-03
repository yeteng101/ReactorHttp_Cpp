#include "HttpResponse.h"
#include <cstring>
#include <ctime>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

HttpResponse::HttpResponse()
{
    reset();
}

void HttpResponse::reset()
{
    m_statusCode = StatusCode::Unknown;
    m_headers.clear();
    m_fileName.clear();
    m_body.clear();
    m_fileToSend.clear();
    m_fileOffset = 0;
    m_fileSize = 0;
    m_keepAlive = false;
    m_headOnly = false;
    m_requestMethod.clear();
    m_requestUrl.clear();
    m_requestVersion.clear();
    m_responseBytes = 0;
}

HttpResponse::~HttpResponse()
{
}

void HttpResponse::addHeader(const string key, const string value)
{
    if (key.empty() || value.empty())
    {
        return;
    }
    m_headers[key] = value;
}

void HttpResponse::setRequestInfo(const string& method, const string& url, const string& version)
{
    m_requestMethod = method;
    m_requestUrl = url;
    m_requestVersion = version;
}

void HttpResponse::prepareMsg(Buffer* sendBuf, int socket)
{
    (void)socket;
    int code = static_cast<int>(m_statusCode);
    auto info = m_info.find(code);
    if (info == m_info.end())
    {
        m_statusCode = StatusCode::InternalServerError;
        code = static_cast<int>(m_statusCode);
        info = m_info.find(code);
        m_body = "Internal Server Error\n";
    }

    // Content-Length：文件响应由 HttpRequest 提前设置，其余按正文长度计算
    if (!m_body.empty())
    {
        addHeader("Content-Length", to_string(m_body.size()));
    }
    else if (!hasFile())
    {
        addHeader("Content-Length", "0");
    }
    if (m_keepAlive)
    {
        addHeader("Connection", "keep-alive");
        addHeader("Keep-Alive", "timeout=" + to_string(m_keepAliveTimeout) +
            ", max=" + to_string(m_keepAliveMax));
    }
    else
    {
        addHeader("Connection", "close");
    }
    addHeader("Server", "ReactorHttp-Cpp");
    addHeader("Access-Control-Allow-Origin", "*");

    // Date 头（RFC 7231 要求，格式: Sun, 06 Nov 1994 08:49:37 GMT）
    char dateBuffer[64];
    const std::time_t now = std::time(nullptr);
    struct tm tmUtc;
    if (gmtime_r(&now, &tmUtc) != nullptr &&
        strftime(dateBuffer, sizeof(dateBuffer), "%a, %d %b %Y %H:%M:%S GMT", &tmUtc) != 0)
    {
        addHeader("Date", dateBuffer);
    }

    const int startPos = sendBuf->readableSize();
    sendBuf->appendString("HTTP/1.1 " + to_string(code) + " " + info->second + "\r\n");
    for (const auto& header : m_headers)
    {
        sendBuf->appendString(header.first + ": " + header.second + "\r\n");
    }
    sendBuf->appendString("\r\n");

    // HEAD 请求不回正文；文件正文由连接层在写事件里流式发送
    if (!m_headOnly && !m_body.empty())
    {
        sendBuf->appendString(m_body.data(), static_cast<int>(m_body.size()));
    }
    m_responseBytes = static_cast<uint64_t>(sendBuf->readableSize() - startPos);
}
