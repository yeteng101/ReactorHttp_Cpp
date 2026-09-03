#include "SidecarClient.h"
#include "ServerContext.h"
#include "Log.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace
{
constexpr int ConnectTimeoutMs = 5000;
constexpr int SendTimeoutMs = 15000;
constexpr int ReceiveIdleTimeoutMs = 180000;   // AI 生成可能较慢
constexpr std::size_t MaxResponseBody = 64u * 1024 * 1024;

struct ParsedSidecar
{
    std::string host;
    std::string port;
    std::string basePath;
};

bool parseUrl(const std::string& url, ParsedSidecar& parsed, std::string& error)
{
    const std::string prefix = "http://";
    if (url.compare(0, prefix.size(), prefix) != 0)
    {
        error = "sidecar URL must start with http:// (TLS is terminated by sidecar itself)";
        return false;
    }
    const std::string rest = url.substr(prefix.size());
    const std::size_t slash = rest.find('/');
    const std::string hostPort = slash == std::string::npos ? rest : rest.substr(0, slash);
    parsed.basePath = slash == std::string::npos ? std::string() : rest.substr(slash);
    while (!parsed.basePath.empty() && parsed.basePath.back() == '/')
    {
        parsed.basePath.pop_back();
    }
    if (hostPort.empty())
    {
        error = "empty sidecar host";
        return false;
    }

    if (hostPort.front() == '[')
    {
        const std::size_t close = hostPort.find(']');
        if (close == std::string::npos)
        {
            error = "invalid IPv6 sidecar URL";
            return false;
        }
        parsed.host = hostPort.substr(1, close - 1);
        parsed.port = close + 1 < hostPort.size() && hostPort[close + 1] == ':'
            ? hostPort.substr(close + 2) : "80";
    }
    else
    {
        const std::size_t colon = hostPort.rfind(':');
        if (colon == std::string::npos)
        {
            parsed.host = hostPort;
            parsed.port = "80";
        }
        else
        {
            parsed.host = hostPort.substr(0, colon);
            parsed.port = hostPort.substr(colon + 1);
        }
    }
    if (parsed.host.empty() || parsed.port.empty() || parsed.port.size() > 5)
    {
        error = "invalid sidecar host or port";
        return false;
    }
    for (char ch : parsed.port)
    {
        if (ch < '0' || ch > '9')
        {
            error = "invalid sidecar port";
            return false;
        }
    }
    return true;
}

bool connectTcp(const ParsedSidecar& target, int& fd, std::string& error)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* addresses = nullptr;
    const int lookup = getaddrinfo(target.host.c_str(), target.port.c_str(), &hints, &addresses);
    if (lookup != 0)
    {
        error = "cannot resolve sidecar host: ";
        error += gai_strerror(lookup);
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
            const int ready = poll(&pfd, 1, ConnectTimeoutMs);
            if (ready > 0 && (pfd.revents & POLLOUT) != 0)
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
        error = "cannot connect to sidecar (" + target.host + ":" + target.port + ")";
        return false;
    }
    return true;
}

bool sendAll(int fd, const std::string& data, std::string& error)
{
    std::size_t sent = 0;
    while (sent < data.size())
    {
        ssize_t count;
        do
        {
            count = ::send(fd, data.data() + sent, data.size() - sent, 0);
        } while (count < 0 && errno == EINTR);
        if (count < 0)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                error = "sidecar send failed";
                return false;
            }
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            const int ready = poll(&pfd, 1, SendTimeoutMs);
            if (ready <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            {
                error = "sidecar send timeout";
                return false;
            }
            continue;
        }
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

bool recvSome(int fd, std::string& buffer, std::string& error)
{
    char chunk[16384];
    ssize_t count;
    do
    {
        count = recv(fd, chunk, sizeof(chunk), 0);
    } while (count < 0 && errno == EINTR);
    if (count > 0)
    {
        buffer.append(chunk, static_cast<std::size_t>(count));
        return true;
    }
    if (count == 0)
    {
        return false;   // EOF
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK)
    {
        error = "sidecar recv failed";
        return false;
    }
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    const int ready = poll(&pfd, 1, ReceiveIdleTimeoutMs);
    // 本机 sidecar 经常“发完数据立刻关连接”，poll 会同时返回 POLLIN|POLLHUP。
    // 不能把 POLLHUP 当错误，必须先让上面的 recv 把缓冲区读完；EOF 由 recv 返回 0。
    if (ready <= 0 || (pfd.revents & POLLNVAL) != 0)
    {
        error = "sidecar recv timeout";
        return false;
    }
    return true;
}

std::string lowerHeader(const std::string& name)
{
    std::string lower;
    lower.reserve(name.size());
    for (char ch : name)
    {
        lower += static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    }
    return lower;
}

bool parseStatusLine(const std::string& line, int& code)
{
    // HTTP/1.1 200 OK
    const std::size_t firstSpace = line.find(' ');
    if (firstSpace == std::string::npos || line.compare(0, 5, "HTTP/") != 0)
    {
        return false;
    }
    const std::size_t codeBegin = firstSpace + 1;
    const std::size_t codeEnd = line.find(' ', codeBegin);
    const std::string codeText = line.substr(codeBegin,
        codeEnd == std::string::npos ? std::string::npos : codeEnd - codeBegin);
    if (codeText.empty() || codeText.size() != 3)
    {
        return false;
    }
    code = 0;
    for (char ch : codeText)
    {
        if (ch < '0' || ch > '9')
        {
            return false;
        }
        code = code * 10 + (ch - '0');
    }
    return code >= 100 && code <= 599;
}
}

bool sidecarCall(const ServerContext& context, const std::string& method,
    const std::string& pathAndQuery, const std::string& contentType,
    const std::string& body, SidecarResult& result)
{
    result = SidecarResult{};
    ParsedSidecar target;
    std::string error;
    if (context.sidecarUrl.empty() || !parseUrl(context.sidecarUrl, target, error))
    {
        result.error = error.empty() ? "sidecar not configured" : error;
        Log::warn("sidecar %s", result.error.c_str());
        return false;
    }

    std::string requestPath = target.basePath + pathAndQuery;
    if (requestPath.empty())
    {
        requestPath = "/";
    }

    std::string request = method + " " + requestPath + " HTTP/1.1\r\n";
    request += "Host: " + target.host + ":" + target.port + "\r\n";
    request += "User-Agent: FujiNetdisk/1.0\r\n";
    request += "Accept: application/json\r\n";
    request += "Connection: close\r\n";
    if (!contentType.empty())
    {
        request += "Content-Type: " + contentType + "\r\n";
    }
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    request += "\r\n";
    request += body;

    int fd = -1;
    if (!connectTcp(target, fd, error))
    {
        result.error = error;
        Log::warn("sidecar %s", error.c_str());
        return false;
    }
    if (!sendAll(fd, request, error))
    {
        close(fd);
        result.error = error;
        Log::warn("sidecar %s", error.c_str());
        return false;
    }

    std::string wire;
    while (true)
    {
        const std::size_t headerEnd = wire.find("\r\n\r\n");
        if (headerEnd != std::string::npos || wire.size() > 65536)
        {
            break;
        }
        const bool gotData = recvSome(fd, wire, error);
        if (!gotData)
        {
            if (error.empty())
            {
                break;   // EOF（头部不完整时下面会报错）
            }
            break;
        }
    }
    if (!error.empty())
    {
        close(fd);
        result.error = error;
        Log::warn("sidecar %s", error.c_str());
        return false;
    }

    const std::size_t headerEnd = wire.find("\r\n\r\n");
    if (headerEnd == std::string::npos || headerEnd > 65536)
    {
        close(fd);
        result.error = "sidecar returned malformed response";
        return false;
    }
    const std::string head = wire.substr(0, headerEnd);
    std::string payload = wire.substr(headerEnd + 4);

    std::vector<std::string> lines;
    std::size_t begin = 0;
    while (begin <= head.size())
    {
        const std::size_t end = head.find("\r\n", begin);
        lines.push_back(head.substr(begin, end == std::string::npos ? std::string::npos
            : end - begin));
        if (end == std::string::npos)
        {
            break;
        }
        begin = end + 2;
    }
    if (lines.empty() || !parseStatusLine(lines[0], result.statusCode))
    {
        close(fd);
        result.error = "sidecar returned malformed status line";
        return false;
    }

    std::size_t contentLength = std::string::npos;
    for (std::size_t i = 1; i < lines.size(); ++i)
    {
        const std::size_t colon = lines[i].find(':');
        if (colon == std::string::npos)
        {
            continue;
        }
        std::string name = lowerHeader(lines[i].substr(0, colon));
        std::string value = lines[i].substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        {
            value.erase(value.begin());
        }
        if (name == "content-type")
        {
            result.contentType = value;
        }
        else if (name == "content-length")
        {
            std::size_t length = 0;
            bool digits = !value.empty();
            for (char ch : value)
            {
                if (ch < '0' || ch > '9')
                {
                    digits = false;
                    break;
                }
                length = length * 10 + static_cast<std::size_t>(ch - '0');
            }
            contentLength = digits ? length : std::string::npos;
        }
    }

    result.body = payload;
    if (contentLength == std::string::npos)
    {
        while (true)
        {
            const bool gotData = recvSome(fd, result.body, error);
            if (!gotData)
            {
                break;   // EOF 正常收尾
            }
            if (result.body.size() > MaxResponseBody)
            {
                error = "sidecar response too large";
                break;
            }
        }
    }
    else
    {
        while (result.body.size() < contentLength)
        {
            const bool gotData = recvSome(fd, result.body, error);
            if (!gotData)
            {
                if (error.empty())
                {
                    error = "sidecar response truncated";
                }
                break;
            }
            if (result.body.size() > MaxResponseBody)
            {
                error = "sidecar response too large";
                break;
            }
        }
    }
    close(fd);
    if (!error.empty())
    {
        result.error = "sidecar response too large";
        if (error.find("too large") == std::string::npos)
        {
            result.error = error;
        }
        result.body.clear();
        return false;
    }
    if (result.body.size() > MaxResponseBody)
    {
        result.error = "sidecar response too large";
        result.body.clear();
        return false;
    }
    result.ok = result.statusCode >= 200 && result.statusCode < 300;
    return true;
}
