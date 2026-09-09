#include "AiClient.h"
#include "Log.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <random>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

namespace
{
constexpr int ConnectTimeoutMs = 10000;
constexpr int IoTimeoutSeconds = 300;      // AI 生成可能较慢
constexpr std::size_t MaxHeadBytes = 64u * 1024;
constexpr std::size_t MaxBodyBytes = 32u * 1024 * 1024;
constexpr std::size_t MaxJobs = 64;

std::string jsonEscape(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (unsigned char ch : value)
    {
        switch (ch)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20)
            {
                const char hex[] = "0123456789abcdef";
                out += "\\u00";
                out += hex[ch >> 4];
                out += hex[ch & 0x0f];
            }
            else
            {
                out += static_cast<char>(ch);
            }
        }
    }
    return out;
}

// 从 JSON 文本中取字符串字段（足够解析 ai-config.json 与 SSE 增量）
std::string jsonString(const std::string& body, const std::string& key)
{
    const std::string pattern = "\"" + key + "\"";
    std::size_t keyPos = body.find(pattern);
    if (keyPos == std::string::npos)
    {
        return std::string();
    }
    std::size_t colon = body.find(':', keyPos + pattern.size());
    if (colon == std::string::npos)
    {
        return std::string();
    }
    std::size_t i = colon + 1;
    while (i < body.size() && (body[i] == ' ' || body[i] == '\t' ||
        body[i] == '\r' || body[i] == '\n'))
    {
        ++i;
    }
    if (i >= body.size() || body[i] != '"')
    {
        return std::string();
    }
    ++i;
    std::string value;
    while (i < body.size())
    {
        const char ch = body[i++];
        if (ch == '\\' && i < body.size())
        {
            const char esc = body[i++];
            switch (esc)
            {
            case 'n': value += '\n'; break;
            case 'r': value += '\r'; break;
            case 't': value += '\t'; break;
            case 'b': value += '\b'; break;
            case 'f': value += '\f'; break;
            case '"': value += '"'; break;
            case '\\': value += '\\'; break;
            case '/': value += '/'; break;
            case 'u':
            {
                if (i + 4 <= body.size())
                {
                    unsigned code = 0;
                    for (int k = 0; k < 4; ++k)
                    {
                        const char h = body[i + k];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                    }
                    i += 4;
                    if (code < 0x80)
                    {
                        value += static_cast<char>(code);
                    }
                    else if (code < 0x800)
                    {
                        value += static_cast<char>(0xC0 | (code >> 6));
                        value += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    else
                    {
                        value += static_cast<char>(0xE0 | (code >> 12));
                        value += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                        value += static_cast<char>(0x80 | (code & 0x3F));
                    }
                }
                break;
            }
            default: value += esc; break;
            }
        }
        else if (ch == '"')
        {
            break;
        }
        else
        {
            value += ch;
        }
    }
    return value;
}

bool jsonBool(const std::string& body, const std::string& key, bool fallback)
{
    const std::string pattern = "\"" + key + "\"";
    const std::size_t keyPos = body.find(pattern);
    if (keyPos == std::string::npos)
    {
        return fallback;
    }
    const std::size_t colon = body.find(':', keyPos + pattern.size());
    if (colon == std::string::npos)
    {
        return fallback;
    }
    std::size_t i = colon + 1;
    while (i < body.size() && (body[i] == ' ' || body[i] == '\t' ||
        body[i] == '\r' || body[i] == '\n'))
    {
        ++i;
    }
    if (body.compare(i, 4, "true") == 0) return true;
    if (body.compare(i, 5, "false") == 0) return false;
    return fallback;
}

struct ParsedUrl
{
    std::string scheme;
    std::string host;
    std::string path;
    int port = 443;
    std::string hostHeader;
};

bool parseUrl(const std::string& url, ParsedUrl& out, std::string& error)
{
    std::string rest;
    if (url.rfind("https://", 0) == 0)
    {
        out.scheme = "https";
        out.port = 443;
        rest = url.substr(8);
    }
    else if (url.rfind("http://", 0) == 0)
    {
        out.scheme = "http";
        out.port = 80;
        rest = url.substr(7);
    }
    else
    {
        error = "AI 地址必须以 http:// 或 https:// 开头";
        return false;
    }

    const std::size_t slash = rest.find('/');
    std::string hostPort = slash == std::string::npos ? rest : rest.substr(0, slash);
    out.path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (out.path.empty())
    {
        out.path = "/";
    }

    const std::size_t at = hostPort.rfind('@');
    if (at != std::string::npos)
    {
        hostPort = hostPort.substr(at + 1);
    }

    if (!hostPort.empty() && hostPort.front() == '[')
    {
        const std::size_t close = hostPort.find(']');
        if (close == std::string::npos)
        {
            error = "AI 地址的 IPv6 格式不正确";
            return false;
        }
        out.host = hostPort.substr(1, close - 1);
        if (close + 1 < hostPort.size() && hostPort[close + 1] == ':')
        {
            out.port = atoi(hostPort.c_str() + close + 2);
        }
        out.hostHeader = "[" + out.host + "]";
    }
    else
    {
        const std::size_t colon = hostPort.rfind(':');
        if (colon == std::string::npos)
        {
            out.host = hostPort;
        }
        else
        {
            out.host = hostPort.substr(0, colon);
            out.port = atoi(hostPort.c_str() + colon + 1);
        }
        out.hostHeader = out.host;
    }
    if (out.host.empty() || out.port <= 0 || out.port > 65535)
    {
        error = "AI 地址缺少有效的主机名或端口";
        return false;
    }
    return true;
}

bool connectTcp(const std::string& host, int port, int& fd, std::string& error)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* addresses = nullptr;
    const std::string portText = std::to_string(port);
    const int lookup = getaddrinfo(host.c_str(), portText.c_str(), &hints, &addresses);
    if (lookup != 0)
    {
        error = std::string("无法解析 AI 主机：") + gai_strerror(lookup);
        return false;
    }

    fd = -1;
    for (struct addrinfo* address = addresses; address != nullptr; address = address->ai_next)
    {
        int sock = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (sock < 0)
        {
            continue;
        }
        const int flags = fcntl(sock, F_GETFL, 0);
        if (flags == -1 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) == -1)
        {
            close(sock);
            continue;
        }
        int rc = connect(sock, address->ai_addr, address->ai_addrlen);
        if (rc == 0)
        {
            fd = sock;
            break;
        }
        if (rc < 0 && (errno == EINPROGRESS || errno == EWOULDBLOCK))
        {
            struct pollfd pfd;
            pfd.fd = sock;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            if (poll(&pfd, 1, ConnectTimeoutMs) > 0 && (pfd.revents & POLLOUT) != 0)
            {
                int socketError = 0;
                socklen_t length = sizeof(socketError);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &socketError, &length) == 0 &&
                    socketError == 0)
                {
                    fd = sock;
                    break;
                }
            }
        }
        close(sock);
    }
    freeaddrinfo(addresses);
    if (fd < 0)
    {
        error = "无法连接 AI 主机 " + host + ":" + portText;
        return false;
    }

    // 转回阻塞模式并设置读写超时，后台线程里阻塞等待最简单可靠
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1)
    {
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
    struct timeval timeout;
    timeout.tv_sec = IoTimeoutSeconds;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return true;
}

SSL_CTX* tlsContext()
{
    static SSL_CTX* context = [] {
        SSL_library_init();
        SSL_load_error_strings();
        SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
        if (ctx != nullptr)
        {
            SSL_CTX_set_default_verify_paths(ctx);
            SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
        }
        return ctx;
    }();
    return context;
}

struct Conn
{
    int fd = -1;
    SSL* ssl = nullptr;

    ssize_t sendBytes(const char* data, std::size_t length, std::string& error)
    {
        std::size_t sent = 0;
        while (sent < length)
        {
            ssize_t count = ssl != nullptr
                ? SSL_write(ssl, data + sent, static_cast<int>(length - sent))
                : send(fd, data + sent, length - sent, 0);
            if (count <= 0)
            {
                if (ssl != nullptr)
                {
                    const int sslError = SSL_get_error(ssl, static_cast<int>(count));
                    if (sslError == SSL_ERROR_WANT_READ || sslError == SSL_ERROR_WANT_WRITE)
                    {
                        continue;
                    }
                    error = "AI 连接写入失败（TLS）";
                }
                else if (errno == EINTR)
                {
                    continue;
                }
                else
                {
                    error = std::string("AI 连接写入失败：") + strerror(errno);
                }
                return -1;
            }
            sent += static_cast<std::size_t>(count);
        }
        return static_cast<ssize_t>(sent);
    }

    // 返回 >0 读到的字节数，0 表示 EOF，-1 表示错误
    ssize_t recvBytes(char* buffer, std::size_t length, std::string& error)
    {
        for (;;)
        {
            ssize_t count = ssl != nullptr
                ? SSL_read(ssl, buffer, static_cast<int>(length))
                : recv(fd, buffer, length, 0);
            if (count > 0)
            {
                return count;
            }
            if (ssl != nullptr)
            {
                const int sslError = SSL_get_error(ssl, static_cast<int>(count));
                if (sslError == SSL_ERROR_ZERO_RETURN)
                {
                    return 0;
                }
                if (sslError == SSL_ERROR_WANT_READ || sslError == SSL_ERROR_WANT_WRITE)
                {
                    continue;
                }
                if (sslError == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK))
                {
                    error = "AI 响应超时";
                }
                else
                {
                    error = "AI 连接读取失败（TLS）";
                }
                return -1;
            }
            if (count == 0)
            {
                return 0;
            }
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                error = "AI 响应超时";
            }
            else
            {
                error = std::string("AI 连接读取失败：") + strerror(errno);
            }
            return -1;
        }
    }

    void closeAll()
    {
        if (ssl != nullptr)
        {
            SSL_shutdown(ssl);
            SSL_free(ssl);
            ssl = nullptr;
        }
        if (fd >= 0)
        {
            close(fd);
            fd = -1;
        }
    }
};

std::string lower(std::string value)
{
    for (char& ch : value)
    {
        ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::string trim(const std::string& value)
{
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && (value[begin] == ' ' || value[begin] == '\t' ||
        value[begin] == '\r' || value[begin] == '\n'))
    {
        ++begin;
    }
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' ||
        value[end - 1] == '\r' || value[end - 1] == '\n'))
    {
        --end;
    }
    return value.substr(begin, end - begin);
}

struct HttpResponseInfo
{
    int status = 0;
    bool chunked = false;
    std::size_t contentLength = std::string::npos;
    std::string contentType;
};

// 发起一次 HTTP(S) 请求；onChunk 不为空时对每个正文片段回调
bool httpRequest(const std::string& url, const std::string& method,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& body, bool insecureTls,
    const std::function<void(const char*, std::size_t)>& onChunk,
    HttpResponseInfo& info, std::string& error)
{
    ParsedUrl parsed;
    if (!parseUrl(url, parsed, error))
    {
        return false;
    }

    int fd = -1;
    if (!connectTcp(parsed.host, parsed.port, fd, error))
    {
        return false;
    }

    Conn conn;
    conn.fd = fd;
    if (parsed.scheme == "https")
    {
        SSL_CTX* context = tlsContext();
        if (context == nullptr)
        {
            error = "无法初始化 TLS";
            conn.closeAll();
            return false;
        }
        conn.ssl = SSL_new(context);
        if (conn.ssl == nullptr)
        {
            error = "无法创建 TLS 连接";
            conn.closeAll();
            return false;
        }
        SSL_set_fd(conn.ssl, fd);
        SSL_set_tlsext_host_name(conn.ssl, parsed.host.c_str());
        if (!insecureTls)
        {
            SSL_set_verify(conn.ssl, SSL_VERIFY_PEER, nullptr);
            X509_VERIFY_PARAM* param = SSL_get0_param(conn.ssl);
            X509_VERIFY_PARAM_set1_host(param, parsed.host.c_str(), 0);
        }
        else
        {
            SSL_set_verify(conn.ssl, SSL_VERIFY_NONE, nullptr);
        }
        if (SSL_connect(conn.ssl) != 1)
        {
            const long verify = SSL_get_verify_result(conn.ssl);
            error = verify == X509_V_OK
                ? "TLS 握手失败（无法连接 AI 服务）"
                : std::string("TLS 证书校验失败：") + X509_verify_cert_error_string(verify);
            conn.closeAll();
            return false;
        }
    }

    std::string request = method + " " + parsed.path + " HTTP/1.1\r\n";
    request += "Host: " + parsed.hostHeader +
        ((parsed.scheme == "https" && parsed.port == 443) ||
         (parsed.scheme == "http" && parsed.port == 80)
            ? "" : ":" + std::to_string(parsed.port)) + "\r\n";
    request += "User-Agent: FujiNetdisk/3.0\r\n";
    request += "Accept: */*\r\n";
    request += "Connection: close\r\n";
    for (const auto& header : headers)
    {
        request += header.first + ": " + header.second + "\r\n";
    }
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    request += body;

    if (conn.sendBytes(request.data(), request.size(), error) < 0)
    {
        conn.closeAll();
        return false;
    }

    std::string buffer;
    char chunk[16384];
    auto readMore = [&]() -> int {
        std::string readError;
        const ssize_t count = conn.recvBytes(chunk, sizeof(chunk), readError);
        if (count > 0)
        {
            buffer.append(chunk, static_cast<std::size_t>(count));
        }
        else if (count < 0)
        {
            error = readError;
        }
        return static_cast<int>(count);
    };

    std::size_t headerEnd = std::string::npos;
    while ((headerEnd = buffer.find("\r\n\r\n")) == std::string::npos)
    {
        if (buffer.size() > MaxHeadBytes)
        {
            error = "AI 响应头过大";
            conn.closeAll();
            return false;
        }
        if (readMore() <= 0)
        {
            if (error.empty())
            {
                error = "AI 服务没有返回完整响应";
            }
            conn.closeAll();
            return false;
        }
    }

    const std::string head = buffer.substr(0, headerEnd);
    buffer.erase(0, headerEnd + 4);

    std::istringstream headStream(head);
    std::string statusLine;
    std::getline(headStream, statusLine);
    {
        const std::size_t firstSpace = statusLine.find(' ');
        const std::size_t codeEnd = firstSpace == std::string::npos
            ? std::string::npos : statusLine.find(' ', firstSpace + 1);
        const std::string code = statusLine.substr(firstSpace + 1,
            codeEnd == std::string::npos ? std::string::npos : codeEnd - firstSpace - 1);
        info.status = atoi(code.c_str());
        if (info.status < 100 || info.status > 599)
        {
            error = "AI 服务返回了无效状态码";
            conn.closeAll();
            return false;
        }
    }
    std::string line;
    while (std::getline(headStream, line))
    {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos)
        {
            continue;
        }
        const std::string name = lower(trim(line.substr(0, colon)));
        const std::string value = trim(line.substr(colon + 1));
        if (name == "content-type")
        {
            info.contentType = value;
        }
        else if (name == "transfer-encoding" && lower(value).find("chunked") != std::string::npos)
        {
            info.chunked = true;
        }
        else if (name == "content-length")
        {
            info.contentLength = static_cast<std::size_t>(strtoull(value.c_str(), nullptr, 10));
        }
    }

    auto emit = [&](const char* data, std::size_t length) {
        if (onChunk)
        {
            onChunk(data, length);
        }
    };

    if (info.chunked)
    {
        while (true)
        {
            std::size_t lineEnd = std::string::npos;
            while ((lineEnd = buffer.find("\r\n")) == std::string::npos)
            {
                if (readMore() <= 0)
                {
                    error = "AI 流式响应中断";
                    conn.closeAll();
                    return false;
                }
            }
            std::string sizeLine = buffer.substr(0, lineEnd);
            buffer.erase(0, lineEnd + 2);
            const std::size_t semi = sizeLine.find(';');
            if (semi != std::string::npos)
            {
                sizeLine = sizeLine.substr(0, semi);
            }
            const std::size_t chunkSize = static_cast<std::size_t>(
                strtoull(trim(sizeLine).c_str(), nullptr, 16));
            if (chunkSize == 0)
            {
                break;
            }
            if (chunkSize > MaxBodyBytes)
            {
                error = "AI 响应过大";
                conn.closeAll();
                return false;
            }
            while (buffer.size() < chunkSize + 2)
            {
                if (readMore() <= 0)
                {
                    error = "AI 流式响应中断";
                    conn.closeAll();
                    return false;
                }
            }
            emit(buffer.data(), chunkSize);
            buffer.erase(0, chunkSize + 2);
        }
    }
    else if (info.contentLength != std::string::npos)
    {
        std::size_t remaining = info.contentLength;
        while (remaining > 0)
        {
            if (!buffer.empty())
            {
                const std::size_t take = std::min(buffer.size(), remaining);
                emit(buffer.data(), take);
                buffer.erase(0, take);
                remaining -= take;
            }
            else if (readMore() <= 0)
            {
                error = "AI 响应被截断";
                conn.closeAll();
                return false;
            }
        }
    }
    else
    {
        while (true)
        {
            if (!buffer.empty())
            {
                emit(buffer.data(), buffer.size());
                buffer.clear();
            }
            if (readMore() <= 0)
            {
                break;
            }
        }
    }

    conn.closeAll();
    return true;
}
}

bool aiChatStream(const AiConfig& config, const std::string& requestJson,
    const std::function<void(const std::string&)>& onDelta,
    std::string& fullText, std::string& model, std::string& error)
{
    if (config.apiKey.empty())
    {
        error = "AI 未配置：请在「AI 设置」里填写 API Key";
        return false;
    }
    std::string base = config.baseUrl;
    while (!base.empty() && base.back() == '/')
    {
        base.pop_back();
    }
    const std::string url = base + "/chat/completions";

    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("Authorization", "Bearer " + config.apiKey);
    headers.emplace_back("Content-Type", "application/json");
    headers.emplace_back("Accept", "text/event-stream");

    HttpResponseInfo info;
    std::string sseLine;
    bool sawDelta = false;
    bool streamDone = false;
    std::string nonStreamBody;

    auto handleEvent = [&](const std::string& payload) {
        if (payload == "[DONE]")
        {
            streamDone = true;
            return;
        }
        const std::string delta = jsonString(payload, "content");
        if (!delta.empty())
        {
            sawDelta = true;
            fullText += delta;
            if (onDelta)
            {
                onDelta(delta);
            }
        }
        const std::string eventModel = jsonString(payload, "model");
        if (!eventModel.empty())
        {
            model = eventModel;
        }
        const std::string message = jsonString(payload, "message");
        if (delta.empty() && !message.empty())
        {
            // 兼容部分服务在流里返回完整 message 的情况
            sawDelta = true;
            fullText += message;
            if (onDelta)
            {
                onDelta(message);
            }
        }
    };

    auto onChunk = [&](const char* data, std::size_t length) {
        sseLine.append(data, length);
        std::size_t newline = std::string::npos;
        while ((newline = sseLine.find('\n')) != std::string::npos)
        {
            std::string line = sseLine.substr(0, newline);
            sseLine.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            if (line.rfind("data:", 0) == 0)
            {
                handleEvent(trim(line.substr(5)));
            }
        }
        if (!info.contentType.empty() &&
            info.contentType.find("text/event-stream") == std::string::npos &&
            info.contentType.find("json") != std::string::npos)
        {
            nonStreamBody.append(data, length);
        }
    };

    if (!httpRequest(url, "POST", headers, requestJson, config.insecureTls, onChunk, info, error))
    {
        return false;
    }
    if (info.status < 200 || info.status >= 300)
    {
        std::string detail = nonStreamBody.empty() ? fullText : nonStreamBody;
        if (detail.empty())
        {
            detail = "HTTP " + std::to_string(info.status);
        }
        error = "AI 接口返回 " + std::to_string(info.status) + "：" + detail.substr(0, 600);
        return false;
    }

    if (!nonStreamBody.empty())
    {
        const std::string content = jsonString(nonStreamBody, "content");
        if (!content.empty())
        {
            fullText = content;
            if (onDelta)
            {
                onDelta(content);
            }
        }
        const std::string responseModel = jsonString(nonStreamBody, "model");
        if (!responseModel.empty())
        {
            model = responseModel;
        }
        return true;
    }
    if (!sseLine.empty() && sseLine.rfind("data:", 0) == 0)
    {
        handleEvent(trim(sseLine.substr(5)));
    }
    if (!sawDelta && !streamDone && fullText.empty())
    {
        error = "AI 没有返回任何内容";
        return false;
    }
    return true;
}

bool AiService::load(const std::string& path, std::string& error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_path = path;
    m_config = AiConfig{};
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr)
    {
        if (errno == ENOENT)
        {
            return true;    // 尚未配置
        }
        error = std::string("无法读取 AI 配置：") + strerror(errno);
        return false;
    }
    std::string content;
    char buffer[4096];
    std::size_t count = 0;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0)
    {
        content.append(buffer, count);
        if (content.size() > 1024 * 1024)
        {
            break;
        }
    }
    fclose(file);

    const std::string baseUrl = jsonString(content, "baseUrl");
    if (!baseUrl.empty()) m_config.baseUrl = baseUrl;
    const std::string model = jsonString(content, "model");
    if (!model.empty()) m_config.model = model;
    m_config.apiKey = jsonString(content, "apiKey");
    m_config.insecureTls = jsonBool(content, "insecureTls", false);
    return true;
}

bool AiService::save(std::string& error) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_path.empty())
    {
        error = "AI 配置路径为空";
        return false;
    }
    const std::string content =
        "{\n  \"baseUrl\": \"" + jsonEscape(m_config.baseUrl) + "\",\n" +
        "  \"model\": \"" + jsonEscape(m_config.model) + "\",\n" +
        "  \"apiKey\": \"" + jsonEscape(m_config.apiKey) + "\",\n" +
        "  \"insecureTls\": " + (m_config.insecureTls ? "true" : "false") + "\n}\n";
    const std::string tmpPath = m_path + ".tmp";
    FILE* file = fopen(tmpPath.c_str(), "wb");
    if (file == nullptr)
    {
        error = std::string("无法写入 AI 配置：") + strerror(errno);
        return false;
    }
    const std::size_t written = fwrite(content.data(), 1, content.size(), file);
    fclose(file);
    if (written != content.size())
    {
        error = "写入 AI 配置失败";
        return false;
    }
    chmod(tmpPath.c_str(), 0600);
    if (rename(tmpPath.c_str(), m_path.c_str()) != 0)
    {
        error = std::string("保存 AI 配置失败：") + strerror(errno);
        return false;
    }
    return true;
}

bool AiService::update(const std::string& baseUrl, const std::string& model,
    const std::string& apiKey, bool keyProvided, bool insecureTls,
    bool insecureProvided, std::string& error)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!baseUrl.empty())
        {
            m_config.baseUrl = baseUrl;
        }
        if (!model.empty())
        {
            m_config.model = model;
        }
        if (keyProvided)
        {
            m_config.apiKey = apiKey;
        }
        if (insecureProvided)
        {
            m_config.insecureTls = insecureTls;
        }
    }
    return save(error);
}

AiConfig AiService::snapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config;
}

std::string AiService::configPath() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_path;
}

void AiService::pruneLocked()
{
    const auto now = std::chrono::steady_clock::now();
    for (auto it = m_jobs.begin(); it != m_jobs.end();)
    {
        const auto age = std::chrono::duration_cast<std::chrono::seconds>(
            now - it->second->createdAt).count();
        if (age > 1800)
        {
            it = m_jobs.erase(it);
        }
        else
        {
            ++it;
        }
    }
    while (m_jobs.size() > MaxJobs)
    {
        m_jobs.erase(m_jobs.begin());
    }
}

std::string AiService::startJob(const std::string& requestJson, std::string& error)
{
    AiConfig config;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_config.apiKey.empty())
        {
            error = "AI 未配置：请在「AI 设置」里填写 API Key";
            return std::string();
        }
        config = m_config;
    }

    static std::mt19937_64 generator(std::random_device{}());
    std::ostringstream idStream;
    idStream << std::hex << generator() << generator();
    const std::string jobId = idStream.str();

    auto job = std::make_shared<AiJob>();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        pruneLocked();
        m_jobs[jobId] = job;
    }

    std::thread([job, config, requestJson]() {
        std::string fullText;
        std::string model;
        std::string chatError;
        const bool ok = aiChatStream(config, requestJson,
            [&job](const std::string& delta) {
                std::lock_guard<std::mutex> lock(job->mutex);
                job->text += delta;
            },
            fullText, model, chatError);
        std::lock_guard<std::mutex> lock(job->mutex);
        job->model = model;
        job->ok = ok;
        if (!ok)
        {
            job->error = chatError;
        }
        job->done = true;
    }).detach();

    return jobId;
}

std::shared_ptr<AiJob> AiService::findJob(const std::string& id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    pruneLocked();
    const auto it = m_jobs.find(id);
    return it == m_jobs.end() ? nullptr : it->second;
}
