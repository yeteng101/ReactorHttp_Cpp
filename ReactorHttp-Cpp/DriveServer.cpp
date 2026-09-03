#include "DriveServer.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "Log.h"
#include "ServerContext.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <sstream>
#include <strings.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace
{
constexpr uint64_t MaxRequestedChunk = 64u * 1024 * 1024;   // 单次上传分片上限（与解析器一致）

void setResponse(HttpResponse* response, StatusCode status, const std::string& contentType,
    const std::string& body)
{
    response->reset();
    response->setStatusCode(status);
    response->addHeader("Content-Type", contentType);
    response->setBody(body);
}

void setJsonError(HttpResponse* response, StatusCode status, const std::string& message)
{
    std::string escaped = message;
    std::string out;
    out.reserve(message.size() + 16);
    for (unsigned char ch : escaped)
    {
        if (ch == '"' || ch == '\\')
        {
            out += '\\';
        }
        out += static_cast<char>(ch);
    }
    setResponse(response, status, "application/json; charset=utf-8",
        "{\"error\":\"" + out + "\"}");
}

std::string jsonEscape(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (unsigned char ch : value)
    {
        switch (ch)
        {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (ch < 0x20)
            {
                const char hex[] = "0123456789abcdef";
                escaped += "\\u00";
                escaped += hex[ch >> 4];
                escaped += hex[ch & 0x0f];
            }
            else
            {
                escaped += static_cast<char>(ch);
            }
        }
    }
    return escaped;
}

bool isHexDigit(char ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

int hexValue(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return ch - 'A' + 10;
}

bool urlDecode(const std::string& in, std::string& out)
{
    out.clear();
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i)
    {
        if (in[i] == '%')
        {
            if (i + 2 >= in.size() || !isHexDigit(in[i + 1]) || !isHexDigit(in[i + 2]))
            {
                return false;
            }
            out += static_cast<char>(hexValue(in[i + 1]) * 16 + hexValue(in[i + 2]));
            i += 2;
        }
        else
        {
            out += in[i];
        }
    }
    return true;
}

// url 查询串取值：url 形如 /api/x?path=a%2Fb&token=xx
std::string queryValue(const std::string& url, const std::string& key)
{
    const std::size_t question = url.find('?');
    if (question == std::string::npos)
    {
        return std::string();
    }
    const std::string query = url.substr(question + 1);
    std::size_t begin = 0;
    while (begin <= query.size())
    {
        const std::size_t amp = query.find('&', begin);
        const std::size_t end = amp == std::string::npos ? query.size() : amp;
        const std::string pair = query.substr(begin, end - begin);
        const std::size_t eq = pair.find('=');
        std::string pairKey = pair.substr(0, eq);
        std::string pairValue = eq == std::string::npos ? std::string() : pair.substr(eq + 1);
        std::string decodedKey;
        if (urlDecode(pairKey, decodedKey) && decodedKey == key)
        {
            std::string decodedValue;
            if (urlDecode(pairValue, decodedValue))
            {
                return decodedValue;
            }
            return pairValue;
        }
        if (amp == std::string::npos)
        {
            break;
        }
        begin = amp + 1;
    }
    return std::string();
}

// application/x-www-form-urlencoded 表单解析
std::map<std::string, std::string> parseForm(const std::string& body)
{
    std::map<std::string, std::string> pairs;
    std::size_t begin = 0;
    while (begin <= body.size())
    {
        const std::size_t amp = body.find('&', begin);
        const std::size_t end = amp == std::string::npos ? body.size() : amp;
        const std::string pair = body.substr(begin, end - begin);
        const std::size_t eq = pair.find('=');
        std::string key = pair.substr(0, eq);
        std::string value = eq == std::string::npos ? std::string() : pair.substr(eq + 1);
        for (char& ch : key)
        {
            if (ch == '+') ch = ' ';
        }
        for (char& ch : value)
        {
            if (ch == '+') ch = ' ';
        }
        std::string decodedKey;
        std::string decodedValue;
        if (urlDecode(key, decodedKey) && urlDecode(value, decodedValue))
        {
            pairs[decodedKey] = decodedValue;
        }
        if (amp == std::string::npos)
        {
            break;
        }
        begin = amp + 1;
    }
    return pairs;
}

// 极简 JSON 字符串取值：仅处理 {"username":"a","password":"b"} 形式
std::string jsonStringValue(const std::string& body, const std::string& key)
{
    const std::string pattern = "\"" + key + "\"";
    const std::size_t keyPos = body.find(pattern);
    if (keyPos == std::string::npos)
    {
        return std::string();
    }
    const std::size_t colon = body.find(':', keyPos + pattern.size());
    if (colon == std::string::npos)
    {
        return std::string();
    }
    const std::size_t quote = body.find('"', colon + 1);
    if (quote == std::string::npos)
    {
        return std::string();
    }
    std::string value;
    bool escaped = false;
    for (std::size_t i = quote + 1; i < body.size(); ++i)
    {
        const char ch = body[i];
        if (escaped)
        {
            if (ch == 'n') value += '\n';
            else if (ch == 'r') value += '\r';
            else if (ch == 't') value += '\t';
            else value += ch;
            escaped = false;
        }
        else if (ch == '\\')
        {
            escaped = true;
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

std::string bearerFromAuthHeader(const std::string& header)
{
    if (header.size() > 7 && strncasecmp(header.c_str(), "Bearer ", 7) == 0)
    {
        std::string token = header.substr(7);
        const std::size_t firstSpace = token.find_first_of(" \t");
        if (firstSpace != std::string::npos)
        {
            token = token.substr(0, firstSpace);
        }
        return token;
    }
    return std::string();
}

bool headerIsTrue(const std::string& value)
{
    if (value.empty())
    {
        return false;
    }
    return value == "1" || strcasecmp(value.c_str(), "true") == 0 ||
        strcasecmp(value.c_str(), "yes") == 0;
}

std::string cookieValue(const std::string& cookieHeader, const std::string& name)
{
    std::size_t begin = 0;
    while (begin <= cookieHeader.size())
    {
        const std::size_t semi = cookieHeader.find(';', begin);
        const std::size_t end = semi == std::string::npos ? cookieHeader.size() : semi;
        const std::string pair = cookieHeader.substr(begin, end - begin);
        const std::size_t eq = pair.find('=');
        if (eq != std::string::npos)
        {
            std::string key = pair.substr(0, eq);
            std::string value = pair.substr(eq + 1);
            while (!key.empty() && (key.front() == ' ' || key.front() == '\t'))
            {
                key.erase(key.begin());
            }
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
            {
                value.pop_back();
            }
            if (key == name)
            {
                return value;
            }
        }
        if (semi == std::string::npos)
        {
            break;
        }
        begin = semi + 1;
    }
    return std::string();
}

std::string sessionToken(HttpRequest& request)
{
    const std::string cookie = request.getHeader("Cookie");
    if (!cookie.empty())
    {
        const std::string token = cookieValue(cookie, "sid");
        if (!token.empty())
        {
            return token;
        }
    }
    const std::string auth = request.getHeader("Authorization");
    if (!auth.empty())
    {
        const std::string token = bearerFromAuthHeader(auth);
        if (!token.empty())
        {
            return token;
        }
    }
    return queryValue(request.url(), "token");
}

std::string currentUser(ServerContext& context, HttpRequest& request)
{
    return context.sessions.check(sessionToken(request));
}

bool isWithinRoot(const fs::path& root, const fs::path& candidate)
{
    auto candidateIt = candidate.begin();
    for (auto rootIt = root.begin(); rootIt != root.end(); ++rootIt, ++candidateIt)
    {
        if (candidateIt == candidate.end() || *rootIt != *candidateIt)
        {
            return false;
        }
    }
    return true;
}

// 每个登录用户拥有独立家目录 <driveRoot>/<username>，互不可见
std::string homeRootFor(const ServerContext& context, const std::string& username)
{
    return (fs::path(context.driveRoot) / username).lexically_normal().string();
}

// rel 必须是非空相对路径（可用 / 分隔子目录），禁止绝对路径与 .. 穿越
bool resolvePath(const std::string& driveRoot, const std::string& rel, fs::path& result,
    std::string& error)
{
    if (driveRoot.empty())
    {
        error = "netdisk is not enabled";
        return false;
    }
    if (rel.empty() || rel == "." || rel.front() == '/' ||
        rel.find("..") != std::string::npos)
    {
        error = "invalid path";
        return false;
    }
    for (const auto& component : fs::path(rel).lexically_normal())
    {
        if (component == "..")
        {
            error = "invalid path";
            return false;
        }
    }
    fs::path root = fs::weakly_canonical(fs::path(driveRoot));
    fs::path candidate = fs::weakly_canonical(root / rel);
    if (!isWithinRoot(root, candidate))
    {
        error = "invalid path";
        return false;
    }
    result = candidate;
    return true;
}

std::int64_t toEpochSeconds(const fs::file_time_type& time)
{
    using namespace std::chrono;
    const auto systemNow = system_clock::now();
    const auto fileNow = fs::file_time_type::clock::now();
    const auto systemTime = time_point_cast<system_clock::duration>(systemNow + (time - fileNow));
    return duration_cast<seconds>(systemTime.time_since_epoch()).count();
}

std::string isoTime(std::int64_t epoch)
{
    std::time_t raw = static_cast<std::time_t>(epoch);
    struct tm utc;
    char buffer[32];
    if (gmtime_r(&raw, &utc) == nullptr ||
        strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0)
    {
        return "";
    }
    return buffer;
}

std::string fileEntriesJson(const fs::path& directory)
{
    std::ostringstream output;
    output << "{\"entries\":[";
    struct Entry
    {
        std::string name;
        bool directory;
        std::uint64_t size;
        std::int64_t mtime;
    };
    std::vector<Entry> entries;
    std::error_code error;
    for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
    {
        const auto& entry = *it;
        std::error_code entryError;
        const bool isDirectory = entry.is_directory(entryError);
        const auto size = isDirectory ? 0 : entry.file_size(entryError);
        Entry item;
        item.name = entry.path().filename().string();
        item.directory = isDirectory;
        item.size = entryError ? 0 : static_cast<std::uint64_t>(size);
        item.mtime = toEpochSeconds(entry.last_write_time(entryError));
        entries.push_back(std::move(item));
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) {
        if (left.directory != right.directory)
        {
            return left.directory;
        }
        return left.name < right.name;
    });
    bool first = true;
    for (const Entry& item : entries)
    {
        if (!first)
        {
            output << ',';
        }
        first = false;
        output << "{\"name\":\"" << jsonEscape(item.name) << "\",\"type\":\""
               << (item.directory ? "directory" : "file") << "\",\"size\":" << item.size
               << ",\"mtime\":\"" << isoTime(item.mtime) << "\"}";
    }
    output << "]}";
    return output.str();
}

// 单段 bytes Range 解析
enum class RangeKind
{
    Ignore,
    Partial,
    Unsatisfiable
};

RangeKind parseRangeHeader(const std::string& header, std::uint64_t fileSize,
    std::uint64_t& start, std::uint64_t& end)
{
    const std::string prefix = "bytes=";
    if (header.size() < prefix.size() ||
        strncasecmp(header.c_str(), prefix.c_str(), prefix.size()) != 0)
    {
        return RangeKind::Ignore;
    }
    const std::string spec = header.substr(prefix.size());
    if (spec.empty() || spec.find(',') != std::string::npos)
    {
        return RangeKind::Ignore;
    }
    const std::size_t dash = spec.find('-');
    if (dash == std::string::npos || spec.find('-', dash + 1) != std::string::npos)
    {
        return RangeKind::Ignore;
    }
    const std::string left = spec.substr(0, dash);
    const std::string right = spec.substr(dash + 1);

    auto parseDecimal = [](const std::string& text, std::uint64_t& value) {
        if (text.empty())
        {
            return false;
        }
        std::uint64_t result = 0;
        for (char ch : text)
        {
            if (ch < '0' || ch > '9')
            {
                return false;
            }
            result = result * 10 + static_cast<std::uint64_t>(ch - '0');
        }
        value = result;
        return true;
    };

    if (left.empty())
    {
        if (right.empty())
        {
            return RangeKind::Ignore;
        }
        std::uint64_t suffix = 0;
        if (!parseDecimal(right, suffix))
        {
            return RangeKind::Ignore;
        }
        if (suffix == 0 || fileSize == 0)
        {
            return RangeKind::Unsatisfiable;
        }
        start = suffix >= fileSize ? 0 : fileSize - suffix;
        end = fileSize - 1;
        return RangeKind::Partial;
    }

    std::uint64_t rangeStart = 0;
    if (!parseDecimal(left, rangeStart))
    {
        return RangeKind::Ignore;
    }
    if (fileSize == 0 || rangeStart >= fileSize)
    {
        return RangeKind::Unsatisfiable;
    }
    start = rangeStart;
    end = fileSize - 1;
    if (!right.empty())
    {
        std::uint64_t rangeEnd = 0;
        if (!parseDecimal(right, rangeEnd))
        {
            return RangeKind::Ignore;
        }
        if (rangeEnd < start)
        {
            return RangeKind::Ignore;
        }
        if (rangeEnd < end)
        {
            end = rangeEnd;
        }
    }
    return RangeKind::Partial;
}

std::string percentEncode(const std::string& value)
{
    const char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char ch : value)
    {
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~')
        {
            encoded += static_cast<char>(ch);
        }
        else
        {
            encoded += '%';
            encoded += hex[ch >> 4];
            encoded += hex[ch & 0x0f];
        }
    }
    return encoded;
}

void handleLogin(ServerContext& context, HttpRequest& request, HttpResponse& response)
{
    if (strcasecmp(request.method().c_str(), "POST") != 0)
    {
        setJsonError(&response, StatusCode::MethodNotAllowed, "method not allowed");
        return;
    }
    const std::string& body = request.getBody();
    std::string username;
    std::string password;
    const std::string contentType = request.getHeader("Content-Type");
    if (!body.empty() && body.front() == '{' &&
        contentType.find("json") != std::string::npos)
    {
        username = jsonStringValue(body, "username");
        password = jsonStringValue(body, "password");
    }
    else
    {
        const auto pairs = parseForm(body);
        const auto userIt = pairs.find("username");
        const auto passIt = pairs.find("password");
        if (userIt != pairs.end())
        {
            username = userIt->second;
        }
        if (passIt != pairs.end())
        {
            password = passIt->second;
        }
    }
    if (username.empty() || password.empty() || !context.users.verify(username, password))
    {
        setJsonError(&response, StatusCode::Unauthorized, "invalid username or password");
        return;
    }
    // 首次登录时创建该用户的私有目录
    std::error_code homeError;
    const std::string home = homeRootFor(context, username);
    if (!fs::create_directories(home, homeError) && homeError)
    {
        Log::error("cannot create user home %s: %s", home.c_str(),
            homeError.message().c_str());
        setJsonError(&response, StatusCode::InternalServerError, "cannot create user space");
        return;
    }

    const std::string token = context.sessions.create(username);
    setResponse(&response, StatusCode::OK, "application/json; charset=utf-8",
        "{\"ok\":true,\"username\":\"" + jsonEscape(username) + "\",\"token\":\"" + token + "\"}");
    const int hours = context.sessions.sessionHours();
    std::string cookie =
        "sid=" + token + "; Path=/; HttpOnly; SameSite=Lax; Max-Age=" +
        std::to_string(static_cast<long long>(hours) * 3600);
    // 位于 Caddy/Nginx HTTPS 反代之后时，允许 cookie 带 Secure 标记
    const std::string forwardedProto = request.getHeader("X-Forwarded-Proto");
    if (strncasecmp(forwardedProto.c_str(), "https", 5) == 0)
    {
        cookie += "; Secure";
    }
    response.addHeader("Set-Cookie", cookie);
}

void handleLogout(ServerContext& context, HttpRequest& request, HttpResponse& response)
{
    context.sessions.remove(sessionToken(request));
    setResponse(&response, StatusCode::OK, "application/json; charset=utf-8",
        "{\"ok\":true}");
}

void handleList(ServerContext& context, HttpRequest& request, HttpResponse& response,
    const std::string& driveRoot)
{
    (void)context;
    const std::string rel = queryValue(request.url(), "path");
    fs::path directory = fs::weakly_canonical(fs::path(driveRoot));
    if (!rel.empty())
    {
        fs::path resolved;
        std::string error;
        if (!resolvePath(driveRoot, rel, resolved, error))
        {
            setJsonError(&response, StatusCode::Forbidden, error);
            return;
        }
        directory = resolved;
    }
    std::error_code error;
    if (!fs::is_directory(directory, error))
    {
        setJsonError(&response, StatusCode::NotFound, "directory not found");
        return;
    }
    std::ostringstream output;
    output << "{\"path\":\"" << jsonEscape(rel) << "\",";
    const fs::space_info space = fs::space(directory, error);
    if (!error)
    {
        output << "\"total_bytes\":" << static_cast<std::uint64_t>(space.capacity)
               << ",\"free_bytes\":" << static_cast<std::uint64_t>(space.available) << ',';
    }
    output << fileEntriesJson(directory).substr(1);
    setResponse(&response, StatusCode::OK, "application/json; charset=utf-8", output.str());
}

void handleStat(ServerContext& context, HttpRequest& request, HttpResponse& response,
    const std::string& driveRoot)
{
    (void)context;
    const std::string rel = queryValue(request.url(), "path");
    fs::path resolved;
    std::string error;
    if (!resolvePath(driveRoot, rel, resolved, error))
    {
        setJsonError(&response, StatusCode::Forbidden, error);
        return;
    }
    std::error_code fileError;
    const bool exists = fs::exists(resolved, fileError);
    std::ostringstream output;
    output << "{\"exists\":" << (exists && !fileError ? "true" : "false");
    if (exists && !fileError)
    {
        const bool isDir = fs::is_directory(resolved, fileError);
        output << ",\"type\":\"" << (isDir ? "directory" : "file") << "\"";
        if (!isDir)
        {
            output << ",\"size\":" << static_cast<std::uint64_t>(fs::file_size(resolved, fileError));
        }
        output << ",\"mtime\":\"" << isoTime(toEpochSeconds(fs::last_write_time(resolved, fileError)))
               << "\"";
    }
    output << '}';
    setResponse(&response, StatusCode::OK, "application/json; charset=utf-8", output.str());
}

void handleMkdir(ServerContext& context, HttpRequest& request, HttpResponse& response,
    const std::string& driveRoot)
{
    (void)context;
    if (strcasecmp(request.method().c_str(), "POST") != 0)
    {
        setJsonError(&response, StatusCode::MethodNotAllowed, "method not allowed");
        return;
    }
    const std::string rel = queryValue(request.url(), "path");
    fs::path resolved;
    std::string error;
    if (!resolvePath(driveRoot, rel, resolved, error))
    {
        setJsonError(&response, StatusCode::Forbidden, error);
        return;
    }
    std::error_code fsError;
    if (fs::exists(resolved, fsError))
    {
        if (fs::is_directory(resolved, fsError))
        {
            setResponse(&response, StatusCode::OK, "application/json; charset=utf-8",
                "{\"ok\":true}");
            return;
        }
        setJsonError(&response, StatusCode::Conflict, "a file with this name exists");
        return;
    }
    if (!fs::create_directories(resolved, fsError) || fsError)
    {
        setJsonError(&response, StatusCode::InternalServerError, "mkdir failed");
        return;
    }
    setResponse(&response, StatusCode::OK, "application/json; charset=utf-8", "{\"ok\":true}");
}

void handleUpload(ServerContext& context, HttpRequest& request, HttpResponse& response,
    const std::string& requestPath, const std::string& driveRoot)
{
    (void)context;
    if (strcasecmp(request.method().c_str(), "PATCH") != 0)
    {
        setJsonError(&response, StatusCode::MethodNotAllowed, "method not allowed");
        return;
    }
    const std::string prefix = "/api/drive/file/";
    const std::string rel = requestPath.substr(prefix.size());
    fs::path candidate;
    std::string error;
    if (!resolvePath(driveRoot, rel, candidate, error))
    {
        setJsonError(&response, StatusCode::Forbidden, error);
        return;
    }

    const std::string offsetHeader = request.getHeader("Upload-Offset");
    std::uint64_t offset = 0;
    if (offsetHeader.empty())
    {
        setJsonError(&response, StatusCode::BadRequest, "missing Upload-Offset header");
        return;
    }
    try
    {
        offset = std::stoull(offsetHeader);
    }
    catch (...)
    {
        setJsonError(&response, StatusCode::BadRequest, "invalid Upload-Offset");
        return;
    }
    const bool overwrite = headerIsTrue(request.getHeader("X-Overwrite"));

    const std::string& body = request.getBody();
    if (body.size() > MaxRequestedChunk)
    {
        setJsonError(&response, StatusCode::PayloadTooLarge, "chunk too large");
        return;
    }
    const std::uint64_t bodySize = body.size();

    std::error_code statError;
    std::uint64_t currentSize = 0;
    bool exists = fs::exists(candidate, statError);
    if (statError)
    {
        setJsonError(&response, StatusCode::InternalServerError, "stat failed");
        return;
    }
    if (exists)
    {
        if (fs::is_directory(candidate, statError))
        {
            setJsonError(&response, StatusCode::Forbidden, "cannot upload over a directory");
            return;
        }
        if (overwrite)
        {
            // 覆盖更新：必须从 0 开始整文件重传，先删除旧文件
            if (offset != 0)
            {
                setJsonError(&response, StatusCode::Conflict,
                    "overwrite must restart from offset 0");
                return;
            }
            std::error_code removeError;
            if (!fs::remove(candidate, removeError) || removeError)
            {
                setJsonError(&response, StatusCode::InternalServerError,
                    "cannot remove existing file");
                return;
            }
            exists = false;
        }
        else
        {
            currentSize = static_cast<std::uint64_t>(fs::file_size(candidate, statError));
            if (statError)
            {
                setJsonError(&response, StatusCode::InternalServerError, "stat failed");
                return;
            }
        }
    }
    else if (offset != 0)
    {
        setJsonError(&response, StatusCode::NotFound, "upload does not exist at this offset");
        return;
    }

    if (offset > currentSize || offset + bodySize < currentSize)
    {
        setJsonError(&response, StatusCode::Conflict, "offset mismatch");
        return;
    }
    if (bodySize > 0 && offset < currentSize && offset + bodySize == currentSize)
    {
        // 该分片数据已经存在（断点续传重复发送边界分片）
        setResponse(&response, StatusCode::OK, "application/json; charset=utf-8",
            "{\"size\":" + std::to_string(currentSize) + "}");
        response.addHeader("Upload-Offset", std::to_string(currentSize));
        return;
    }

    std::error_code parentError;
    fs::create_directories(candidate.parent_path(), parentError);
    const int fd = open(candidate.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        Log::error("upload open failed: %s (%s)", candidate.c_str(), strerror(errno));
        setJsonError(&response, StatusCode::InternalServerError, "cannot create file");
        return;
    }
    std::size_t written = 0;
    while (written < bodySize)
    {
        ssize_t count;
        do
        {
            count = pwrite(fd, body.data() + written, bodySize - written,
                static_cast<off_t>(offset + written));
        } while (count == -1 && errno == EINTR);
        if (count <= 0)
        {
            close(fd);
            setJsonError(&response, StatusCode::InternalServerError, "write failed");
            return;
        }
        written += static_cast<std::size_t>(count);
    }
    if (close(fd) != 0)
    {
        setJsonError(&response, StatusCode::InternalServerError, "close failed");
        return;
    }

    const std::uint64_t newSize = std::max(currentSize, offset + bodySize);
    setResponse(&response, StatusCode::OK, "application/json; charset=utf-8",
        "{\"size\":" + std::to_string(newSize) + "}");
    response.addHeader("Upload-Offset", std::to_string(newSize));
}

void handleRemove(ServerContext& context, HttpRequest& request, HttpResponse& response,
    const std::string& driveRoot)
{
    (void)context;
    if (strcasecmp(request.method().c_str(), "DELETE") != 0)
    {
        setJsonError(&response, StatusCode::MethodNotAllowed, "method not allowed");
        return;
    }
    const std::string rel = queryValue(request.url(), "path");
    fs::path resolved;
    std::string error;
    if (!resolvePath(driveRoot, rel, resolved, error))
    {
        setJsonError(&response, StatusCode::Forbidden, error);
        return;
    }
    std::error_code fsError;
    if (!fs::exists(resolved, fsError))
    {
        setJsonError(&response, StatusCode::NotFound, "path not found");
        return;
    }
    std::error_code removeError;
    const bool isDirectory = fs::is_directory(resolved, fsError);
    // 目录允许整棵删除；单文件与空目录等价
    if (isDirectory && !fs::remove_all(resolved, removeError))
    {
        setJsonError(&response, StatusCode::Conflict, "cannot remove (directory not empty?)");
        return;
    }
    if (!isDirectory && (!fs::remove(resolved, removeError) || removeError))
    {
        setJsonError(&response, StatusCode::InternalServerError, "cannot remove file");
        return;
    }
    setResponse(&response, StatusCode::OK, "application/json; charset=utf-8", "{\"ok\":true}");
}

void handleRename(ServerContext& context, HttpRequest& request, HttpResponse& response,
    const std::string& driveRoot)
{
    (void)context;
    if (strcasecmp(request.method().c_str(), "POST") != 0)
    {
        setJsonError(&response, StatusCode::MethodNotAllowed, "method not allowed");
        return;
    }
    const std::string fromRel = queryValue(request.url(), "from");
    const std::string toRel = queryValue(request.url(), "to");
    fs::path fromPath;
    fs::path toPath;
    std::string error;
    if (!resolvePath(driveRoot, fromRel, fromPath, error) ||
        !resolvePath(driveRoot, toRel, toPath, error))
    {
        setJsonError(&response, StatusCode::Forbidden, "invalid path");
        return;
    }
    std::error_code fsError;
    if (!fs::exists(fromPath, fsError))
    {
        setJsonError(&response, StatusCode::NotFound, "source not found");
        return;
    }
    if (fs::exists(toPath, fsError))
    {
        setJsonError(&response, StatusCode::Conflict, "target already exists");
        return;
    }
    std::error_code parentError;
    fs::create_directories(toPath.parent_path(), parentError);
    std::error_code renameError;
    fs::rename(fromPath, toPath, renameError);
    if (renameError)
    {
        setJsonError(&response, StatusCode::InternalServerError, "rename failed");
        return;
    }
    setResponse(&response, StatusCode::OK, "application/json; charset=utf-8", "{\"ok\":true}");
}

void handleFileRequest(ServerContext& context, HttpRequest& request, HttpResponse& response,
    const std::string& driveRoot, bool attachment)
{
    (void)context;
    const std::string rel = queryValue(request.url(), "path");
    fs::path resolved;
    std::string error;
    if (!resolvePath(driveRoot, rel, resolved, error))
    {
        setJsonError(&response, StatusCode::Forbidden, error);
        return;
    }
    std::error_code fsError;
    if (!fs::is_regular_file(resolved, fsError))
    {
        setJsonError(&response, StatusCode::NotFound, "file not found");
        return;
    }

    const std::uint64_t fileSize = static_cast<std::uint64_t>(fs::file_size(resolved, fsError));
    const std::string contentType = request.getFileType(resolved.string());

    std::uint64_t start = 0;
    std::uint64_t end = 0;
    const RangeKind range = parseRangeHeader(request.getHeader("Range"), fileSize, start, end);
    if (range == RangeKind::Unsatisfiable)
    {
        setJsonError(&response, StatusCode::RangeNotSatisfiable, "range not satisfiable");
        response.addHeader("Content-Range", "bytes */" + std::to_string(fileSize));
        response.addHeader("Accept-Ranges", "bytes");
        return;
    }

    response.reset();
    response.setStatusCode(range == RangeKind::Partial ? StatusCode::PartialContent
                                                       : StatusCode::OK);
    response.addHeader("Content-Type", contentType);
    response.addHeader("Accept-Ranges", "bytes");
    if (attachment)
    {
        const std::string filename = resolved.filename().string();
        response.addHeader("Content-Disposition",
            "attachment; filename*=UTF-8''" + percentEncode(filename) + "; filename=\"download\"");
    }
    if (range == RangeKind::Partial)
    {
        const std::uint64_t rangeSize = end - start + 1;
        response.addHeader("Content-Range", "bytes " + std::to_string(start) + "-" +
            std::to_string(end) + "/" + std::to_string(fileSize));
        response.addHeader("Content-Length", std::to_string(rangeSize));
        response.setFileToSend(resolved.string(), start, rangeSize);
    }
    else
    {
        response.addHeader("Content-Length", std::to_string(fileSize));
        response.setFileToSend(resolved.string(), 0, fileSize);
    }
}
}

namespace Drive
{
bool handle(ServerContext& context, const std::string& requestPath, HttpRequest& request,
    HttpResponse& response)
{
    // 公开路由
    if (requestPath == "/api/login")
    {
        handleLogin(context, request, response);
        return true;
    }

    // 以下全部需要登录
    const std::string username = currentUser(context, request);
    if (username.empty())
    {
        setJsonError(&response, StatusCode::Unauthorized, "login required");
        return true;
    }

    if (requestPath == "/api/logout")
    {
        handleLogout(context, request, response);
        return true;
    }
    if (requestPath == "/api/me")
    {
        setResponse(&response, StatusCode::OK, "application/json; charset=utf-8",
            "{\"username\":\"" + jsonEscape(username) + "\"}");
        return true;
    }
    // 所有文件操作都限制在登录用户自己的家目录下
    const std::string driveRoot = homeRootFor(context, username);
    std::error_code homeError;
    if (!fs::is_directory(driveRoot, homeError))
    {
        // 家目录在登录时应已创建；这里兜底一次，避免并发竞态导致 403
        if (!fs::create_directories(driveRoot, homeError) && homeError)
        {
            setJsonError(&response, StatusCode::InternalServerError, "user space unavailable");
            return true;
        }
    }
    if (requestPath == "/api/drive/list")
    {
        handleList(context, request, response, driveRoot);
    }
    else if (requestPath == "/api/drive/stat")
    {
        handleStat(context, request, response, driveRoot);
    }
    else if (requestPath == "/api/drive/mkdir")
    {
        handleMkdir(context, request, response, driveRoot);
    }
    else if (requestPath.compare(0, 16, "/api/drive/file/") == 0)
    {
        handleUpload(context, request, response, requestPath, driveRoot);
    }
    else if (requestPath == "/api/drive/remove")
    {
        handleRemove(context, request, response, driveRoot);
    }
    else if (requestPath == "/api/drive/rename")
    {
        handleRename(context, request, response, driveRoot);
    }
    else if (requestPath == "/api/drive/download")
    {
        handleFileRequest(context, request, response, driveRoot, true);
    }
    else if (requestPath == "/api/drive/stream")
    {
        handleFileRequest(context, request, response, driveRoot, false);
    }
    else
    {
        setJsonError(&response, StatusCode::NotFound, "api not found");
    }
    return true;
}
}
