#include "Config.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>

namespace
{
bool parseNumber(const char* value, long min, long max, long& result)
{
    char* end = nullptr;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < min || parsed > max)
    {
        return false;
    }
    result = parsed;
    return true;
}
}

void printUsage(const char* program)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  or:  %s <port> <web-root> [min-workers] [max-workers]\n"
        "\nOptions:\n"
        "  --port <n>                     listen port (1-65535, default 10000)\n"
        "  --root <dir>                   static web root (default .)\n"
        "  --min-workers <n>              min worker threads (default 2)\n"
        "  --max-workers <n>              max worker threads (default 8)\n"
        "  --max-connections <n>          max concurrent connections (default 10000)\n"
        "  --max-requests-per-connection <n>  max requests per keep-alive connection\n"
        "  --idle-timeout <n>             idle keep-alive timeout in seconds (default 30)\n"
        "  --graceful-shutdown <n>        max seconds to wait for connections to drain\n"
        "  --access-log <path|- >         access log file, - for stdout\n"
        "  --error-log <path|- >          error log file, - for stderr\n"
        "  --drive-root <dir>             enable netdisk mode with this data dir\n"
        "  --users-file <path>            user password file (default users.conf)\n"
        "  --add-user <name:password>     create/update a user then exit\n"
        "  --help                         show this help\n",
        program, program);
}

bool parseConfig(int argc, char* argv[], ServerConfig& config)
{
    if (argc <= 1)
    {
        return true;    // 全部使用默认值
    }

    const bool legacyMode = argv[1][0] != '-';
    if (legacyMode)
    {
        if (argc < 3 || argc > 5)
        {
            printUsage(argv[0]);
            return false;
        }
        long portValue = 0;
        long minThreads = config.minWorkers;
        long maxThreads = config.maxWorkers;
        if (!parseNumber(argv[1], 1, 65535, portValue) ||
            (argc >= 4 && !parseNumber(argv[3], 1, 256, minThreads)) ||
            (argc == 5 && !parseNumber(argv[4], minThreads, 256, maxThreads)))
        {
            fprintf(stderr, "Invalid port or worker thread count.\n");
            return false;
        }
        if (argc == 4)
        {
            maxThreads = minThreads;
        }
        config.port = static_cast<unsigned short>(portValue);
        config.webRoot = argv[2];
        config.minWorkers = static_cast<int>(minThreads);
        config.maxWorkers = static_cast<int>(maxThreads);
        return true;
    }

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto needValue = [&](const char* name) -> const char* {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "Missing value for %s\n", name);
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h")
        {
            printUsage(argv[0]);
            return false;
        }
        else if (arg == "--port")
        {
            const char* value = needValue("--port");
            long parsed = 0;
            if (value == nullptr || !parseNumber(value, 1, 65535, parsed))
            {
                fprintf(stderr, "Invalid --port value.\n");
                return false;
            }
            config.port = static_cast<unsigned short>(parsed);
        }
        else if (arg == "--root")
        {
            const char* value = needValue("--root");
            if (value == nullptr || *value == '\0')
            {
                fprintf(stderr, "Invalid --root value.\n");
                return false;
            }
            config.webRoot = value;
        }
        else if (arg == "--min-workers")
        {
            const char* value = needValue("--min-workers");
            long parsed = 0;
            if (value == nullptr || !parseNumber(value, 1, 256, parsed))
            {
                fprintf(stderr, "Invalid --min-workers value.\n");
                return false;
            }
            config.minWorkers = static_cast<int>(parsed);
            if (config.maxWorkers < config.minWorkers)
            {
                config.maxWorkers = config.minWorkers;
            }
        }
        else if (arg == "--max-workers")
        {
            const char* value = needValue("--max-workers");
            long parsed = 0;
            if (value == nullptr || !parseNumber(value, 1, 256, parsed))
            {
                fprintf(stderr, "Invalid --max-workers value.\n");
                return false;
            }
            config.maxWorkers = static_cast<int>(parsed);
            if (config.minWorkers > config.maxWorkers)
            {
                fprintf(stderr, "--max-workers must be >= --min-workers.\n");
                return false;
            }
        }
        else if (arg == "--max-connections")
        {
            const char* value = needValue("--max-connections");
            long parsed = 0;
            if (value == nullptr || !parseNumber(value, 1, 10000000, parsed))
            {
                fprintf(stderr, "Invalid --max-connections value.\n");
                return false;
            }
            config.maxConnections = static_cast<std::size_t>(parsed);
        }
        else if (arg == "--max-requests-per-connection")
        {
            const char* value = needValue("--max-requests-per-connection");
            long parsed = 0;
            if (value == nullptr || !parseNumber(value, 1, 100000000, parsed))
            {
                fprintf(stderr, "Invalid --max-requests-per-connection value.\n");
                return false;
            }
            config.maxRequestsPerConnection = static_cast<int>(parsed);
        }
        else if (arg == "--idle-timeout")
        {
            const char* value = needValue("--idle-timeout");
            long parsed = 0;
            if (value == nullptr || !parseNumber(value, 1, 86400, parsed))
            {
                fprintf(stderr, "Invalid --idle-timeout value.\n");
                return false;
            }
            config.idleTimeoutSeconds = static_cast<int>(parsed);
        }
        else if (arg == "--graceful-shutdown")
        {
            const char* value = needValue("--graceful-shutdown");
            long parsed = 0;
            if (value == nullptr || !parseNumber(value, 1, 600, parsed))
            {
                fprintf(stderr, "Invalid --graceful-shutdown value.\n");
                return false;
            }
            config.gracefulShutdownSeconds = static_cast<int>(parsed);
        }
        else if (arg == "--access-log")
        {
            const char* value = needValue("--access-log");
            if (value == nullptr)
            {
                return false;
            }
            config.accessLogPath = value;
        }
        else if (arg == "--error-log")
        {
            const char* value = needValue("--error-log");
            if (value == nullptr)
            {
                return false;
            }
            config.errorLogPath = value;
        }
        else if (arg == "--drive-root")
        {
            const char* value = needValue("--drive-root");
            if (value == nullptr || *value == '\0')
            {
                fprintf(stderr, "Invalid --drive-root value.\n");
                return false;
            }
            config.driveRoot = value;
        }
        else if (arg == "--users-file")
        {
            const char* value = needValue("--users-file");
            if (value == nullptr || *value == '\0')
            {
                fprintf(stderr, "Invalid --users-file value.\n");
                return false;
            }
            config.usersFile = value;
        }
        else if (arg == "--add-user")
        {
            const char* value = needValue("--add-user");
            if (value == nullptr)
            {
                return false;
            }
            config.addUser = value;
        }
        else
        {
            fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            printUsage(argv[0]);
            return false;
        }
    }
    return true;
}
