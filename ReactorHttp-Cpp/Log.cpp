#include "Log.h"
#include <chrono>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>

namespace
{
std::mutex g_accessMutex;
std::mutex g_errorMutex;
FILE* g_accessFile = stdout;
FILE* g_errorFile = stderr;
bool g_accessOwned = false;
bool g_errorOwned = false;

FILE* openLogFile(const std::string& path, FILE* fallback, bool& owned)
{
    if (path.empty() || path == "-")
    {
        owned = false;
        return fallback;
    }
    FILE* file = fopen(path.c_str(), "a");
    if (file == nullptr)
    {
        owned = false;
        fprintf(stderr, "[log] cannot open log file %s: %s\n", path.c_str(), strerror(errno));
        return fallback;
    }
    setvbuf(file, nullptr, _IOLBF, 0);
    owned = true;
    return file;
}

void closeLogFile(FILE* file, bool owned)
{
    if (owned && file != nullptr)
    {
        fclose(file);
    }
}

std::string timestamp()
{
    char buffer[64];
    const std::time_t now = std::time(nullptr);
    struct tm local;
    if (localtime_r(&now, &local) == nullptr)
    {
        return "1970-01-01 00:00:00";
    }
    if (strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local) == 0)
    {
        return "1970-01-01 00:00:00";
    }
    return buffer;
}
}

namespace Log
{
void configure(const std::string& accessPath, const std::string& errorPath)
{
    std::lock_guard<std::mutex> accessLock(g_accessMutex);
    std::lock_guard<std::mutex> errorLock(g_errorMutex);
    closeLogFile(g_accessFile, g_accessOwned);
    closeLogFile(g_errorFile, g_errorOwned);
    g_accessFile = openLogFile(accessPath, stdout, g_accessOwned);
    g_errorFile = openLogFile(errorPath, stderr, g_errorOwned);
}

void access(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lock(g_accessMutex);
    fprintf(g_accessFile, "%s [access] ", timestamp().c_str());
    va_list args;
    va_start(args, fmt);
    vfprintf(g_accessFile, fmt, args);
    va_end(args);
    fprintf(g_accessFile, "\n");
}

void info(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lock(g_errorMutex);
    fprintf(g_errorFile, "%s [info] ", timestamp().c_str());
    va_list args;
    va_start(args, fmt);
    vfprintf(g_errorFile, fmt, args);
    va_end(args);
    fprintf(g_errorFile, "\n");
}

void warn(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lock(g_errorMutex);
    fprintf(g_errorFile, "%s [warn] ", timestamp().c_str());
    va_list args;
    va_start(args, fmt);
    vfprintf(g_errorFile, fmt, args);
    va_end(args);
    fprintf(g_errorFile, "\n");
}

void error(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lock(g_errorMutex);
    fprintf(g_errorFile, "%s [error] ", timestamp().c_str());
    va_list args;
    va_start(args, fmt);
    vfprintf(g_errorFile, fmt, args);
    va_end(args);
    fprintf(g_errorFile, "\n");
}

void debug(const char* fmt, ...)
{
    (void)fmt;
#ifndef NDEBUG
    std::lock_guard<std::mutex> lock(g_errorMutex);
    fprintf(g_errorFile, "%s [debug] ", timestamp().c_str());
    va_list args;
    va_start(args, fmt);
    vfprintf(g_errorFile, fmt, args);
    va_end(args);
    fprintf(g_errorFile, "\n");
#endif
}

void shutdown()
{
    std::lock_guard<std::mutex> accessLock(g_accessMutex);
    std::lock_guard<std::mutex> errorLock(g_errorMutex);
    fflush(g_accessFile);
    fflush(g_errorFile);
    closeLogFile(g_accessFile, g_accessOwned);
    closeLogFile(g_errorFile, g_errorOwned);
    g_accessFile = stdout;
    g_errorFile = stderr;
    g_accessOwned = false;
    g_errorOwned = false;
}
}
