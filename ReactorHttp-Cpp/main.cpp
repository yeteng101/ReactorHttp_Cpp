#include "Config.h"
#include "Log.h"
#include "ServerMetrics.h"
#include "TcpServer.h"

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <filesystem>
#include <unistd.h>

int main(int argc, char* argv[])
{
    ServerConfig config;
    if (!parseConfig(argc, argv, config))
    {
        return EXIT_FAILURE;
    }

    Log::configure(config.accessLogPath, config.errorLogPath);

    // 切换工作目录到静态根目录，后续路径解析都以它为准
    std::error_code error;
    if (!std::filesystem::is_directory(config.webRoot, error))
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

    TcpServer server(config);
    server.run();

    Log::shutdown();
    return 0;
}
