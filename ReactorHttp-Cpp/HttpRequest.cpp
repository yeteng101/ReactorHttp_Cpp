#include "HttpRequest.h"
#include "ServerMetrics.h"

#include <algorithm>
#include <cerrno>
#include <ctype.h>
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <fcntl.h>
#include <sstream>
#include <strings.h>
#include <string.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace
{
constexpr int MaxRequestLineSize = 8192;
constexpr int MaxHeaderLineSize = 8192;
constexpr int MaxHeaderCount = 100;
constexpr uint64_t MaxRequestBodySize = 64u * 1024 * 1024;

bool hasConnectionToken(const string& value, const char* token)
{
    size_t begin = 0;
    while (begin <= value.size())
    {
        const size_t end = value.find(',', begin);
        size_t tokenBegin = begin;
        size_t tokenEnd = end == string::npos ? value.size() : end;
        while (tokenBegin < tokenEnd && isspace(static_cast<unsigned char>(value[tokenBegin])))
        {
            ++tokenBegin;
        }
        while (tokenEnd > tokenBegin && isspace(static_cast<unsigned char>(value[tokenEnd - 1])))
        {
            --tokenEnd;
        }
        if (tokenEnd - tokenBegin == strlen(token) &&
            strncasecmp(value.c_str() + tokenBegin, token, tokenEnd - tokenBegin) == 0)
        {
            return true;
        }
        if (end == string::npos)
        {
            break;
        }
        begin = end + 1;
    }
    return false;
}

void setResponse(HttpResponse* response, StatusCode status, const string& contentType,
    const string& body)
{
    response->reset();
    response->setStatusCode(status);
    response->addHeader("Content-Type", contentType);
    response->setBody(body);
}

string jsonEscape(const string& value)
{
    string escaped;
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

string htmlEscape(const string& value)
{
    string escaped;
    for (char ch : value)
    {
        switch (ch)
        {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '"': escaped += "&quot;"; break;
        case '\'': escaped += "&#39;"; break;
        default: escaped += ch;
        }
    }
    return escaped;
}

string urlEncode(const string& value)
{
    const char hex[] = "0123456789ABCDEF";
    string encoded;
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

vector<fs::directory_entry> sortedEntries(const fs::path& directory)
{
    vector<fs::directory_entry> entries;
    std::error_code error;
    for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
    {
        entries.push_back(*it);
    }
    sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return left.path().filename().string() < right.path().filename().string();
    });
    return entries;
}

string filesJson(const fs::path& root)
{
    ostringstream output;
    output << "{\"files\":[";
    bool first = true;
    for (const auto& entry : sortedEntries(root))
    {
        std::error_code error;
        const bool directory = entry.is_directory(error);
        const auto size = directory ? 0 : entry.file_size(error);
        if (!first)
        {
            output << ',';
        }
        first = false;
        output << "{\"name\":\"" << jsonEscape(entry.path().filename().string())
               << "\",\"type\":\"" << (directory ? "directory" : "file")
               << "\",\"size\":" << (error ? 0 : size) << '}';
    }
    output << "]}";
    return output.str();
}

string directoryHtml(const fs::path& directory, string requestPath)
{
    if (requestPath.empty() || requestPath.back() != '/')
    {
        requestPath += '/';
    }
    ostringstream output;
    output << "<!doctype html><html><head><meta charset=\"utf-8\"><title>Index of "
           << htmlEscape(requestPath) << "</title></head><body><h1>Index of "
           << htmlEscape(requestPath) << "</h1><ul>";
    for (const auto& entry : sortedEntries(directory))
    {
        std::error_code error;
        const bool isDirectory = entry.is_directory(error);
        const string name = entry.path().filename().string();
        output << "<li><a href=\"" << requestPath << urlEncode(name)
               << (isDirectory ? "/" : "") << "\">" << htmlEscape(name)
               << (isDirectory ? "/" : "") << "</a></li>";
    }
    output << "</ul></body></html>";
    return output.str();
}

bool parseDecimal(const string& text, uint64_t& value)
{
    if (text.empty())
    {
        return false;
    }
    uint64_t result = 0;
    for (unsigned char ch : text)
    {
        if (!isdigit(ch))
        {
            return false;
        }
        const uint64_t digit = ch - '0';
        if (result > (UINT64_MAX - digit) / 10)
        {
            return false;
        }
        result = result * 10 + digit;
    }
    value = result;
    return true;
}

string trimHeaderValue(const string& value)
{
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && isspace(static_cast<unsigned char>(value[begin])))
    {
        ++begin;
    }
    while (end > begin && isspace(static_cast<unsigned char>(value[end - 1])))
    {
        --end;
    }
    return value.substr(begin, end - begin);
}

enum class RangeResult
{
    Ignore,
    Partial,
    Unsatisfiable
};

// 只支持单段 bytes 范围；多段或不支持的格式按 RFC 7233 允许忽略 Range。
RangeResult parseRangeHeader(const string& header, uint64_t fileSize,
    uint64_t& start, uint64_t& end)
{
    const string prefix = "bytes=";
    if (header.size() < prefix.size() ||
        strncasecmp(header.c_str(), prefix.c_str(), prefix.size()) != 0)
    {
        return RangeResult::Ignore;
    }
    const string spec = header.substr(prefix.size());
    if (spec.empty() || spec.find(',') != string::npos)
    {
        return RangeResult::Ignore;
    }
    const size_t dash = spec.find('-');
    if (dash == string::npos || spec.find('-', dash + 1) != string::npos)
    {
        return RangeResult::Ignore;
    }
    const string left = spec.substr(0, dash);
    const string right = spec.substr(dash + 1);

    if (left.empty())
    {
        if (right.empty())
        {
            return RangeResult::Ignore;
        }
        uint64_t suffix = 0;
        if (!parseDecimal(right, suffix))
        {
            return RangeResult::Ignore;
        }
        if (suffix == 0 || fileSize == 0)
        {
            return RangeResult::Unsatisfiable;
        }
        start = suffix >= fileSize ? 0 : fileSize - suffix;
        end = fileSize - 1;
        return RangeResult::Partial;
    }

    uint64_t rangeStart = 0;
    if (!parseDecimal(left, rangeStart))
    {
        return RangeResult::Ignore;
    }
    if (fileSize == 0 || rangeStart >= fileSize)
    {
        return RangeResult::Unsatisfiable;
    }
    start = rangeStart;
    end = fileSize - 1;
    if (!right.empty())
    {
        uint64_t rangeEnd = 0;
        if (!parseDecimal(right, rangeEnd))
        {
            return RangeResult::Ignore;
        }
        if (rangeEnd < start)
        {
            return RangeResult::Ignore;
        }
        if (rangeEnd < end)
        {
            end = rangeEnd;
        }
    }
    return RangeResult::Partial;
}

bool startsWith(const unsigned char* data, size_t size, const char* magic, size_t magicSize)
{
    return size >= magicSize && memcmp(data, magic, magicSize) == 0;
}

bool looksLikeUtf8Text(const unsigned char* data, size_t size)
{
    size_t i = 0;
    while (i < size)
    {
        const unsigned char ch = data[i];
        if (ch == 0)
        {
            return false;
        }
        size_t width = 1;
        if (ch < 0x80)
        {
            if (ch < 0x09 || (ch > 0x0d && ch < 0x20))
            {
                return false;
            }
        }
        else if (ch >= 0xc2 && ch <= 0xdf)
        {
            width = 2;
        }
        else if (ch >= 0xe0 && ch <= 0xef)
        {
            width = 3;
        }
        else if (ch >= 0xf0 && ch <= 0xf4)
        {
            width = 4;
        }
        else
        {
            return false;
        }
        if (i + width > size)
        {
            return false;
        }
        for (size_t j = 1; j < width; ++j)
        {
            if ((data[i + j] & 0xc0) != 0x80)
            {
                return false;
            }
        }
        i += width;
    }
    return true;
}

// 扩展名未知时读取文件头做轻量魔数推测，避免依赖系统 libmagic。
string sniffMimeType(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return "application/octet-stream";
    }
    unsigned char header[64];
    input.read(reinterpret_cast<char*>(header), sizeof(header));
    const size_t size = static_cast<size_t>(input.gcount());
    if (size == 0)
    {
        return "application/octet-stream";
    }

    static const unsigned char pngMagic[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (startsWith(header, size, reinterpret_cast<const char*>(pngMagic), sizeof(pngMagic)))
    {
        return "image/png";
    }
    if (size >= 3 && header[0] == 0xff && header[1] == 0xd8 && header[2] == 0xff)
    {
        return "image/jpeg";
    }
    if (startsWith(header, size, "GIF87a", 6) || startsWith(header, size, "GIF89a", 6))
    {
        return "image/gif";
    }
    if (size >= 12 && startsWith(header, size, "RIFF", 4))
    {
        if (startsWith(header + 8, size - 8, "WEBP", 4))
        {
            return "image/webp";
        }
        if (startsWith(header + 8, size - 8, "WAVE", 4))
        {
            return "audio/wav";
        }
        if (startsWith(header + 8, size - 8, "AVI ", 4))
        {
            return "video/x-msvideo";
        }
    }
    if (startsWith(header, size, "%PDF-", 5))
    {
        return "application/pdf";
    }
    if (size >= 4 && header[0] == 'P' && header[1] == 'K' &&
        (header[2] == 0x03 || header[2] == 0x05 || header[2] == 0x07) &&
        (header[3] == 0x04 || header[3] == 0x06 || header[3] == 0x08))
    {
        return "application/zip";
    }
    if (size >= 2 && header[0] == 0x1f && header[1] == 0x8b)
    {
        return "application/gzip";
    }
    if (startsWith(header, size, "BZh", 3))
    {
        return "application/x-bzip2";
    }
    if (size >= 6 && header[0] == 0xfd && header[1] == 0x37 && header[2] == 0x7a &&
        header[3] == 0x58 && header[4] == 0x5a && header[5] == 0x00)
    {
        return "application/x-xz";
    }
    if (size >= 6 && header[0] == 0x37 && header[1] == 0x7a && header[2] == 0xbc &&
        header[3] == 0xaf && header[4] == 0x27 && header[5] == 0x1c)
    {
        return "application/x-7z-compressed";
    }
    if (size >= 7 && header[0] == 'R' && header[1] == 'a' && header[2] == 'r' &&
        header[3] == '!' && header[4] == 0x1a && header[5] == 0x07 && header[6] == 0x00)
    {
        return "application/vnd.rar";
    }
    if (startsWith(header, size, "ID3", 3) ||
        (size >= 2 && header[0] == 0xff && (header[1] & 0xe0) == 0xe0))
    {
        return "audio/mpeg";
    }
    if (startsWith(header, size, "fLaC", 4))
    {
        return "audio/flac";
    }
    if (startsWith(header, size, "OggS", 4))
    {
        return "audio/ogg";
    }
    if (size >= 12 && startsWith(header + 4, size - 4, "ftyp", 4))
    {
        if (startsWith(header + 8, size - 8, "qt  ", 4))
        {
            return "video/quicktime";
        }
        return "video/mp4";
    }
    if (size >= 12 && startsWith(header + 4, size - 4, "webm", 4))
    {
        return "video/webm";
    }
    if (size >= 4 && header[0] == 0 && header[1] == 0 && header[2] == 1 && header[3] == 0)
    {
        return "image/x-icon";
    }
    if (size >= 2 && header[0] == 'B' && header[1] == 'M')
    {
        return "image/bmp";
    }
    if (size >= 4 && ((header[0] == 'I' && header[1] == 'I' && header[2] == 42 && header[3] == 0) ||
        (header[0] == 'M' && header[1] == 'M' && header[2] == 0 && header[3] == 42)))
    {
        return "image/tiff";
    }
    if (startsWith(header, size, "SQLite format 3\0", 16))
    {
        return "application/vnd.sqlite3";
    }
    if (size >= 4 && header[0] == 0x7f && header[1] == 'E' && header[2] == 'L' &&
        header[3] == 'F')
    {
        return "application/x-executable";
    }
    if (looksLikeUtf8Text(header, size))
    {
        return "text/plain; charset=utf-8";
    }
    return "application/octet-stream";
}

const map<string, const char*> g_mimeTypes = {
    {".html", "text/html; charset=utf-8"},
    {".htm", "text/html; charset=utf-8"},
    {".css", "text/css; charset=utf-8"},
    {".js", "text/javascript; charset=utf-8"},
    {".mjs", "text/javascript; charset=utf-8"},
    {".json", "application/json; charset=utf-8"},
    {".map", "application/json; charset=utf-8"},
    {".xml", "application/xml; charset=utf-8"},
    {".txt", "text/plain; charset=utf-8"},
    {".log", "text/plain; charset=utf-8"},
    {".md", "text/markdown; charset=utf-8"},
    {".markdown", "text/markdown; charset=utf-8"},
    {".csv", "text/csv; charset=utf-8"},
    {".yaml", "text/yaml; charset=utf-8"},
    {".yml", "text/yaml; charset=utf-8"},
    {".svg", "image/svg+xml"},
    {".ico", "image/x-icon"},
    {".bmp", "image/bmp"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif", "image/gif"},
    {".webp", "image/webp"},
    {".avif", "image/avif"},
    {".tif", "image/tiff"},
    {".tiff", "image/tiff"},
    {".pdf", "application/pdf"},
    {".zip", "application/zip"},
    {".gz", "application/gzip"},
    {".bz2", "application/x-bzip2"},
    {".xz", "application/x-xz"},
    {".7z", "application/x-7z-compressed"},
    {".rar", "application/vnd.rar"},
    {".tar", "application/x-tar"},
    {".doc", "application/msword"},
    {".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {".xls", "application/vnd.ms-excel"},
    {".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {".ppt", "application/vnd.ms-powerpoint"},
    {".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    {".odt", "application/vnd.oasis.opendocument.text"},
    {".ods", "application/vnd.oasis.opendocument.spreadsheet"},
    {".odp", "application/vnd.oasis.opendocument.presentation"},
    {".epub", "application/epub+zip"},
    {".apk", "application/vnd.android.package-archive"},
    {".wasm", "application/wasm"},
    {".mp3", "audio/mpeg"},
    {".wav", "audio/wav"},
    {".ogg", "audio/ogg"},
    {".oga", "audio/ogg"},
    {".opus", "audio/ogg"},
    {".m4a", "audio/mp4"},
    {".aac", "audio/aac"},
    {".flac", "audio/flac"},
    {".mid", "audio/midi"},
    {".midi", "audio/midi"},
    {".weba", "audio/webm"},
    {".mp4", "video/mp4"},
    {".m4v", "video/mp4"},
    {".webm", "video/webm"},
    {".mov", "video/quicktime"},
    {".avi", "video/x-msvideo"},
    {".mkv", "video/x-matroska"},
    {".ogv", "video/ogg"},
    {".ts", "video/mp2t"},
    {".woff", "font/woff"},
    {".woff2", "font/woff2"},
    {".ttf", "font/ttf"},
    {".otf", "font/otf"},
    {".eot", "application/vnd.ms-fontobject"},
};
}

int HttpRequest::hexToDec(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

HttpRequest::HttpRequest()
{
    reset();
}

HttpRequest::~HttpRequest()
{
}

void HttpRequest::reset()
{
    m_curState = PrecessState::ParseReqLine;
    m_method.clear();
    m_url.clear();
    m_version.clear();
    m_reqHeaders.clear();
    m_body.clear();
    m_bodyExpected = 0;
    m_bodyTooLarge = false;
    m_parseError = false;
    m_keepAlive = false;
    m_forceClose = false;
}

void HttpRequest::addHeader(const string key, const string value)
{
    if (!key.empty())
    {
        m_reqHeaders[key] = value;
    }
}

string HttpRequest::getHeader(const string key)
{
    for (const auto& item : m_reqHeaders)
    {
        if (strcasecmp(item.first.c_str(), key.c_str()) == 0)
        {
            return item.second;
        }
    }
    return string();
}

bool HttpRequest::shouldKeepAlive() const
{
    for (const auto& item : m_reqHeaders)
    {
        if (strcasecmp(item.first.c_str(), "Connection") != 0)
        {
            continue;
        }
        if (hasConnectionToken(item.second, "close"))
        {
            return false;
        }
        if (hasConnectionToken(item.second, "keep-alive"))
        {
            return true;
        }
    }
    return m_version == "HTTP/1.1";
}

bool HttpRequest::parseRequestLine(Buffer* readBuf)
{
    char* end = readBuf->findCRLF();
    if (end == nullptr)
    {
        if (readBuf->readableSize() > MaxRequestLineSize)
        {
            m_parseError = true;
            return true;
        }
        return false;
    }

    char* start = readBuf->data();
    const int lineSize = static_cast<int>(end - start);
    string line(start, lineSize);
    const auto firstSpace = line.find(' ');
    const auto secondSpace = firstSpace == string::npos ? string::npos : line.find(' ', firstSpace + 1);
    if (lineSize <= 0 || lineSize > MaxRequestLineSize || firstSpace == string::npos ||
        secondSpace == string::npos || line.find(' ', secondSpace + 1) != string::npos)
    {
        m_parseError = true;
        return true;
    }

    m_method = line.substr(0, firstSpace);
    m_url = line.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    m_version = line.substr(secondSpace + 1);
    if (m_method.empty() || m_url.empty() ||
        (m_version != "HTTP/1.0" && m_version != "HTTP/1.1"))
    {
        m_parseError = true;
        return true;
    }

    readBuf->readPosIncrease(lineSize + 2);
    m_curState = PrecessState::ParseReqHeaders;
    return true;
}

bool HttpRequest::parseRequestHeader(Buffer* readBuf)
{
    char* end = readBuf->findCRLF();
    if (end == nullptr)
    {
        if (readBuf->readableSize() > MaxHeaderLineSize)
        {
            m_parseError = true;
            return true;
        }
        return false;
    }

    char* start = readBuf->data();
    const int lineSize = static_cast<int>(end - start);
    if (lineSize == 0)
    {
        readBuf->readPosIncrease(2);
        if (!getHeader("Transfer-Encoding").empty())
        {
            m_parseError = true;
            return true;
        }
        const string contentLength = trimHeaderValue(getHeader("Content-Length"));
        if (!contentLength.empty())
        {
            uint64_t length = 0;
            if (!parseDecimal(contentLength, length))
            {
                m_parseError = true;
                return true;
            }
            if (length > MaxRequestBodySize)
            {
                m_bodyTooLarge = true;
                m_parseError = true;
                return true;
            }
            m_bodyExpected = length;
        }
        m_curState = m_bodyExpected > 0 ? PrecessState::ParseReqBody : PrecessState::ParseReqDone;
        return true;
    }
    if (lineSize > MaxHeaderLineSize || static_cast<int>(m_reqHeaders.size()) >= MaxHeaderCount)
    {
        m_parseError = true;
        return true;
    }

    string line(start, lineSize);
    const auto colon = line.find(':');
    if (colon == string::npos || colon == 0)
    {
        m_parseError = true;
        return true;
    }
    auto valueStart = colon + 1;
    while (valueStart < line.size() && (line[valueStart] == ' ' || line[valueStart] == '\t'))
    {
        ++valueStart;
    }
    addHeader(line.substr(0, colon), line.substr(valueStart));
    readBuf->readPosIncrease(lineSize + 2);
    return true;
}

bool HttpRequest::parseRequestBody(Buffer* readBuf)
{
    const uint64_t remaining = m_bodyExpected - m_body.size();
    const int available = readBuf->readableSize();
    const size_t take = static_cast<size_t>(remaining) < static_cast<size_t>(available)
        ? static_cast<size_t>(remaining) : static_cast<size_t>(available);
    if (take > 0)
    {
        m_body.append(readBuf->data(), take);
        readBuf->readPosIncrease(static_cast<int>(take));
    }
    if (m_body.size() >= m_bodyExpected)
    {
        m_curState = PrecessState::ParseReqDone;
        return true;
    }
    return false;
}

bool HttpRequest::parseHttpRequest(Buffer* readBuf, HttpResponse* response, Buffer* sendBuf, int socket)
{
    while (m_curState != PrecessState::ParseReqDone && !m_parseError)
    {
        bool progressed = false;
        if (m_curState == PrecessState::ParseReqLine)
        {
            progressed = parseRequestLine(readBuf);
        }
        else if (m_curState == PrecessState::ParseReqHeaders)
        {
            progressed = parseRequestHeader(readBuf);
        }
        else if (m_curState == PrecessState::ParseReqBody)
        {
            progressed = parseRequestBody(readBuf);
        }
        if (!progressed)
        {
            return false;
        }
    }

    if (m_parseError)
    {
        if (m_bodyTooLarge)
        {
            setResponse(response, StatusCode::PayloadTooLarge,
                "text/plain; charset=utf-8", "Payload Too Large\n");
        }
        else
        {
            setResponse(response, StatusCode::BadRequest, "text/plain; charset=utf-8",
                "Bad Request\n");
        }
        m_keepAlive = false;
    }
    else
    {
        m_keepAlive = shouldKeepAlive() && !m_forceClose;
        processHttpRequest(response);
        // setResponse 内部会 reset 响应对象，所以 HEAD 标记必须在路由之后重新设置
        response->setHeadOnly(strcasecmp(m_method.c_str(), "HEAD") == 0);
    }

    response->setKeepAlive(m_keepAlive);

    response->setRequestInfo(m_method, m_url, m_version);
    response->prepareMsg(sendBuf, socket);
    ServerMetrics::instance().requestCompleted(static_cast<int>(response->getStatusCode()));
    reset();
    return true;
}

bool HttpRequest::processHttpRequest(HttpResponse* response)
{
    response->reset();
    const bool isHead = strcasecmp(m_method.c_str(), "HEAD") == 0;
    const bool isOptions = strcasecmp(m_method.c_str(), "OPTIONS") == 0;
    const bool isPatch = strcasecmp(m_method.c_str(), "PATCH") == 0;
    const bool isDelete = strcasecmp(m_method.c_str(), "DELETE") == 0;
    if (!isOptions && !isPatch && !isDelete &&
        strcasecmp(m_method.c_str(), "GET") != 0 && !isHead)
    {
        setResponse(response, StatusCode::MethodNotAllowed, "text/plain; charset=utf-8",
            "Method Not Allowed\n");
        response->addHeader("Allow", "GET, HEAD, OPTIONS, PATCH, DELETE");
        return false;
    }
    response->setHeadOnly(isHead);

    if (isOptions)
    {
        // CORS 预检：不校验具体路径，也不返回正文
        response->setStatusCode(StatusCode::NoContent);
        response->addHeader("Allow", "GET, HEAD, OPTIONS, PATCH, DELETE");
        response->addHeader("Access-Control-Allow-Origin", "*");
        response->addHeader("Access-Control-Allow-Methods", "GET, HEAD, OPTIONS, PATCH, DELETE");
        const string requestedHeaders = getHeader("Access-Control-Request-Headers");
        response->addHeader("Access-Control-Allow-Headers",
            requestedHeaders.empty() ? "Content-Type, Range" : requestedHeaders);
        response->addHeader("Access-Control-Max-Age", "86400");
        return true;
    }

    string rawPath = m_url.substr(0, m_url.find('?'));
    string requestPath;
    if (!decodeMsg(rawPath, requestPath) || requestPath.empty() || requestPath.front() != '/' ||
        requestPath.find('\0') != string::npos)
    {
        setResponse(response, StatusCode::BadRequest, "text/plain; charset=utf-8", "Bad Request\n");
        return false;
    }

    if (requestPath == "/health")
    {
        setResponse(response, StatusCode::OK, "application/json; charset=utf-8",
            "{\"status\":\"ok\"}");
        return true;
    }
    if (requestPath == "/metrics")
    {
        setResponse(response, StatusCode::OK, "application/json; charset=utf-8",
            ServerMetrics::instance().toJson());
        return true;
    }

    std::error_code error;
    const fs::path root = fs::weakly_canonical(fs::current_path(), error);
    if (error)
    {
        setResponse(response, StatusCode::InternalServerError, "text/plain; charset=utf-8",
            "Internal Server Error\n");
        return false;
    }
    if (isPatch || isDelete)
    {
        constexpr const char* FileApiPrefix = "/api/files/";
        constexpr size_t FileApiPrefixLength = 11;
        if (requestPath.size() <= FileApiPrefixLength ||
            requestPath.compare(0, FileApiPrefixLength, FileApiPrefix) != 0)
        {
            setResponse(response, StatusCode::NotFound, "text/plain; charset=utf-8",
                "Not Found\n");
            return false;
        }
        const string name = requestPath.substr(FileApiPrefixLength);
        if (name.find('/') != string::npos || name == "." || name == "..")
        {
            setResponse(response, StatusCode::Forbidden, "text/plain; charset=utf-8",
                "Forbidden\n");
            return false;
        }
        fs::path candidate = fs::weakly_canonical(root / name, error);
        if (error || !isWithinRoot(root, candidate))
        {
            setResponse(response, StatusCode::Forbidden, "text/plain; charset=utf-8",
                "Forbidden\n");
            return false;
        }

        if (isDelete)
        {
            std::error_code existsError;
            const bool exists = fs::exists(candidate, existsError);
            if (existsError)
            {
                setResponse(response, StatusCode::InternalServerError,
                    "text/plain; charset=utf-8", "Internal Server Error\n");
                return false;
            }
            if (!exists)
            {
                setResponse(response, StatusCode::NotFound, "text/plain; charset=utf-8",
                    "Not Found\n");
                return false;
            }
            if (fs::is_directory(candidate, existsError))
            {
                setResponse(response, StatusCode::Forbidden, "text/plain; charset=utf-8",
                    "Forbidden\n");
                return false;
            }
            if (!fs::remove(candidate, error) || error)
            {
                setResponse(response, StatusCode::InternalServerError,
                    "text/plain; charset=utf-8", "Internal Server Error\n");
                return false;
            }
            response->setStatusCode(StatusCode::NoContent);
            return true;
        }

        const string offsetHeader = trimHeaderValue(getHeader("Upload-Offset"));
        uint64_t offset = 0;
        if (offsetHeader.empty() || !parseDecimal(offsetHeader, offset))
        {
            setResponse(response, StatusCode::BadRequest, "text/plain; charset=utf-8",
                "Bad Request\n");
            return false;
        }
        const uint64_t bodySize = m_body.size();
        if (offset + bodySize < offset)
        {
            setResponse(response, StatusCode::BadRequest, "text/plain; charset=utf-8",
                "Bad Request\n");
            return false;
        }

        std::error_code statError;
        uint64_t currentSize = 0;
        const bool exists = fs::exists(candidate, statError);
        if (statError)
        {
            setResponse(response, StatusCode::InternalServerError,
                "text/plain; charset=utf-8", "Internal Server Error\n");
            return false;
        }
        if (exists)
        {
            if (fs::is_directory(candidate, statError))
            {
                setResponse(response, StatusCode::Forbidden, "text/plain; charset=utf-8",
                    "Forbidden\n");
                return false;
            }
            currentSize = static_cast<uint64_t>(fs::file_size(candidate, statError));
            if (statError)
            {
                setResponse(response, StatusCode::InternalServerError,
                    "text/plain; charset=utf-8", "Internal Server Error\n");
                return false;
            }
        }
        else if (offset != 0)
        {
            setResponse(response, StatusCode::NotFound, "text/plain; charset=utf-8",
                "Not Found\n");
            return false;
        }

        if (offset > currentSize || offset + bodySize < currentSize)
        {
            setResponse(response, StatusCode::Conflict, "text/plain; charset=utf-8",
                "Offset mismatch\n");
            return false;
        }
        if (bodySize > 0 && offset < currentSize && offset + bodySize == currentSize)
        {
            setResponse(response, StatusCode::OK, "application/json; charset=utf-8",
                "{\"size\":" + to_string(currentSize) + "}");
            response->addHeader("Upload-Offset", to_string(currentSize));
            return true;
        }

        const int fd = open(candidate.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0)
        {
            setResponse(response, StatusCode::InternalServerError,
                "text/plain; charset=utf-8", "Internal Server Error\n");
            return false;
        }
        size_t written = 0;
        while (written < bodySize)
        {
            const ssize_t count = pwrite(fd, m_body.data() + written, bodySize - written,
                static_cast<off_t>(offset + written));
            if (count < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                close(fd);
                setResponse(response, StatusCode::InternalServerError,
                    "text/plain; charset=utf-8", "Internal Server Error\n");
                return false;
            }
            written += static_cast<size_t>(count);
        }
        close(fd);

        const uint64_t newSize = max(currentSize, offset + bodySize);
        setResponse(response, StatusCode::OK, "application/json; charset=utf-8",
            "{\"size\":" + to_string(newSize) + "}");
        response->addHeader("Upload-Offset", to_string(newSize));
        return true;
    }
    if (requestPath == "/api/files")
    {
        setResponse(response, StatusCode::OK, "application/json; charset=utf-8", filesJson(root));
        return true;
    }

    fs::path relative = fs::path(requestPath.substr(1)).lexically_normal();
    if (relative.is_absolute())
    {
        setResponse(response, StatusCode::Forbidden, "text/plain; charset=utf-8", "Forbidden\n");
        return false;
    }
    for (const auto& component : relative)
    {
        if (component == "..")
        {
            setResponse(response, StatusCode::Forbidden, "text/plain; charset=utf-8", "Forbidden\n");
            return false;
        }
    }

    fs::path candidate = fs::weakly_canonical(root / relative, error);
    if (error || !isWithinRoot(root, candidate))
    {
        setResponse(response, StatusCode::Forbidden, "text/plain; charset=utf-8", "Forbidden\n");
        return false;
    }
    if (!fs::exists(candidate, error))
    {
        setResponse(response, StatusCode::NotFound, "text/html; charset=utf-8",
            "<!doctype html><html><body><h1>404 Not Found</h1></body></html>");
        return false;
    }

    if (fs::is_directory(candidate, error))
    {
        fs::path index = candidate / "index.html";
        if (fs::is_regular_file(index, error))
        {
            candidate = index;
        }
        else
        {
            setResponse(response, StatusCode::OK, "text/html; charset=utf-8",
                directoryHtml(candidate, requestPath));
            return true;
        }
    }
    if (!fs::is_regular_file(candidate, error))
    {
        setResponse(response, StatusCode::Forbidden, "text/plain; charset=utf-8", "Forbidden\n");
        return false;
    }

    response->setFileName(candidate.string());
    string contentType = getFileType(candidate.string());
    if (contentType == "application/octet-stream")
    {
        contentType = sniffMimeType(candidate);
    }
    response->addHeader("Content-Type", contentType);
    response->addHeader("Accept-Ranges", "bytes");
    const uint64_t fileSize = static_cast<uint64_t>(fs::file_size(candidate, error));

    uint64_t start = 0;
    uint64_t end = 0;
    const RangeResult range = parseRangeHeader(getHeader("Range"), fileSize, start, end);
    if (range == RangeResult::Partial)
    {
        const uint64_t rangeSize = end - start + 1;
        response->setStatusCode(StatusCode::PartialContent);
        response->addHeader("Content-Range",
            "bytes " + to_string(start) + "-" + to_string(end) + "/" + to_string(fileSize));
        response->addHeader("Content-Length", to_string(rangeSize));
        response->setFileToSend(candidate.string(), start, rangeSize);
    }
    else if (range == RangeResult::Unsatisfiable)
    {
        setResponse(response, StatusCode::RangeNotSatisfiable,
            "text/plain; charset=utf-8", "Range Not Satisfiable\n");
        response->addHeader("Accept-Ranges", "bytes");
        response->addHeader("Content-Range", "bytes */" + to_string(fileSize));
    }
    else
    {
        response->setStatusCode(StatusCode::OK);
        response->addHeader("Content-Length", to_string(fileSize));
        response->setFileToSend(candidate.string(), 0, fileSize);
    }
    return true;
}

bool HttpRequest::decodeMsg(const string& from, string& decoded)
{
    decoded.clear();
    decoded.reserve(from.size());
    for (size_t i = 0; i < from.size(); ++i)
    {
        if (from[i] == '%')
        {
            if (i + 2 >= from.size() || !isxdigit(static_cast<unsigned char>(from[i + 1])) ||
                !isxdigit(static_cast<unsigned char>(from[i + 2])))
            {
                return false;
            }
            decoded += static_cast<char>(hexToDec(from[i + 1]) * 16 + hexToDec(from[i + 2]));
            i += 2;
        }
        else
        {
            decoded += from[i];
        }
    }
    return true;
}

const string HttpRequest::getFileType(const string name)
{
    const char* dot = strrchr(name.c_str(), '.');
    if (dot == nullptr)
    {
        return "application/octet-stream";
    }
    string extension(dot);
    for (char& ch : extension)
    {
        ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    }
    const auto it = g_mimeTypes.find(extension);
    return it == g_mimeTypes.end() ? "application/octet-stream" : it->second;
}
