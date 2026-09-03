#include "Config.h"
#include "Log.h"
#include "ServerContext.h"
#include "ServerMetrics.h"
#include "TcpServer.h"

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <filesystem>
#include <unistd.h>

namespace fs = std::filesystem;

namespace
{
std::string absoluteNormalized(const std::string& path, std::error_code& error)
{
    if (path.empty())
    {
        return path;
    }
    fs::path absolute = fs::absolute(path, error);
    if (error)
    {
        return path;
    }
    return absolute.lexically_normal().string();
}

// 处理 --add-user：在 users 文件里创建/更新一个用户后立即退出
int addUserAndExit(ServerConfig& config, ServerContext& context, const std::string& program)
{
    const std::size_t colon = config.addUser.find(':');
    if (colon == std::string::npos)
    {
        fprintf(stderr, "Invalid --add-user value, expected name:password\n");
        return EXIT_FAILURE;
    }
    const std::string username = config.addUser.substr(0, colon);
    const std::string password = config.addUser.substr(colon + 1);

    std::error_code error;
    if (fs::exists(context.usersFile, error) && !error)
    {
        std::string loadError;
        if (!context.users.load(context.usersFile, loadError))
        {
            fprintf(stderr, "Cannot load users file %s: %s\n", context.usersFile.c_str(),
                loadError.c_str());
            return EXIT_FAILURE;
        }
    }
    std::string createError;
    if (!context.users.create(username, password, createError))
    {
        fprintf(stderr, "Cannot create user: %s\n", createError.c_str());
        return EXIT_FAILURE;
    }
    std::string saveError;
    if (!context.users.save(context.usersFile, saveError))
    {
        fprintf(stderr, "Cannot save users file %s: %s\n", context.usersFile.c_str(),
            saveError.c_str());
        return EXIT_FAILURE;
    }
    fprintf(stderr, "User '%s' saved to %s (%zu users total).\n", username.c_str(),
        context.usersFile.c_str(), context.users.count());
    fprintf(stderr, "Now start the server: %s --drive-root <data-dir> --users-file %s\n",
        program.c_str(), context.usersFile.c_str());
    return EXIT_SUCCESS;
}
}

int main(int argc, char* argv[])
{
    ServerConfig config;
    if (!parseConfig(argc, argv, config))
    {
        return EXIT_FAILURE;
    }

    Log::configure(config.accessLogPath, config.errorLogPath);

    // 网盘模式：先准备数据目录与用户文件（必须在 chdir 到 web root 之前做，
    // 因为 drive-root / users-file 可能是相对当前启动目录的路径）
    ServerContext context;
    std::error_code error;
    if (!config.driveRoot.empty() || !config.addUser.empty())
    {
        if (!config.driveRoot.empty())
        {
            context.driveRoot = absoluteNormalized(config.driveRoot, error);
            if (error || context.driveRoot.empty())
            {
                fprintf(stderr, "Invalid --drive-root: %s\n", config.driveRoot.c_str());
                return EXIT_FAILURE;
            }
            if (!fs::create_directories(context.driveRoot, error) && error)
            {
                fprintf(stderr, "Cannot create drive root %s: %s\n", context.driveRoot.c_str(),
                    error.message().c_str());
                return EXIT_FAILURE;
            }
        }
        context.usersFile = absoluteNormalized(config.usersFile, error);
        if (error || context.usersFile.empty())
        {
            fprintf(stderr, "Invalid --users-file: %s\n", config.usersFile.c_str());
            return EXIT_FAILURE;
        }
        // 保证 users 文件所在目录存在
        if (!fs::create_directories(fs::path(context.usersFile).parent_path(), error) && error)
        {
            fprintf(stderr, "Cannot create users directory: %s\n", error.message().c_str());
            return EXIT_FAILURE;
        }
        context.usersFile = fs::path(context.usersFile).lexically_normal().string();

        if (!config.addUser.empty())
        {
            return addUserAndExit(config, context, argv[0]);
        }

        // 正式启动前必须已配置至少一个用户，避免网盘锁死
        std::string loadError;
        if (!fs::exists(context.usersFile, error))
        {
            fprintf(stderr,
                "Users file %s does not exist. Create the first user first, for example:\n"
                "  %s --drive-root %s --users-file %s --add-user author:your-password\n",
                context.usersFile.c_str(), argv[0], context.driveRoot.c_str(),
                context.usersFile.c_str());
            return EXIT_FAILURE;
        }
        if (!context.users.load(context.usersFile, loadError))
        {
            fprintf(stderr, "Cannot load users file %s: %s\n", context.usersFile.c_str(),
                loadError.c_str());
            return EXIT_FAILURE;
        }
        if (context.users.count() == 0)
        {
            fprintf(stderr,
                "No users configured. Create the first user first, for example:\n"
                "  %s --drive-root %s --users-file %s --add-user author:your-password\n",
                argv[0], context.driveRoot.c_str(), context.usersFile.c_str());
            return EXIT_FAILURE;
        }
    }
    config.driveRoot = context.driveRoot;
    config.usersFile = context.usersFile;

    // 切换工作目录到静态根目录，后续路径解析都以它为准
    if (!fs::is_directory(config.webRoot, error))
    {
        fprintf(stderr, "web root is not a directory: %s\n", config.webRoot.c_str());
        return EXIT_FAILURE;
    }
    if (chdir(config.webRoot.c_str()) != 0)
    {
        perror("chdir");
        return EXIT_FAILURE;
    }

    // 忽略 SIGPIPE，避免写已关闭的连接时进程被信号杀死
    signal(SIGPIPE, SIG_IGN);

    Log::info("ReactorHttp-Cpp starting, port=%hu, root=%s, workers=%d-%d",
        config.port, config.webRoot.c_str(), config.minWorkers, config.maxWorkers);
    if (context.driveEnabled || !context.driveRoot.empty())
    {
        context.driveEnabled = true;
        Log::info("netdisk mode enabled, drive root=%s, users file=%s, users=%zu",
            context.driveRoot.c_str(), context.usersFile.c_str(), context.users.count());
    }

    TcpServer server(config, &context);
    server.run();

    Log::shutdown();
    return 0;
}
