#pragma once
#include <cstddef>
#include <string>

/*
 * 服务器运行配置。支持两种命令行形式：
 *
 *   reactor-http --port 10000 --root /srv/www --min-workers 2 --max-workers 8
 *   reactor-http 10000 /srv/www 2 8          （旧版兼容）
 */
struct ServerConfig
{
    unsigned short port = 10000;
    std::string webRoot = ".";
    int minWorkers = 2;
    int maxWorkers = 8;
    std::size_t maxConnections = 10000;          // 全局最大并发连接
    int maxRequestsPerConnection = 100000;       // 单连接最大请求数，超过后强制关闭（大文件分片续传按请求计数）
    int idleTimeoutSeconds = 30;                 // 空闲超时（秒）
    int gracefulShutdownSeconds = 10;            // 优雅停机最长等待（秒）
    std::string accessLogPath = "-";             // "-" 表示 stdout
    std::string errorLogPath = "-";              // "-" 表示 stderr
    std::string driveRoot;                       // 网盘数据目录；为空表示关闭网盘
    std::string usersFile = "users.conf";        // 用户口令文件
    std::string addUser;                         // "name:password"，创建/更新用户后退出
    std::string sidecarUrl = "http://127.0.0.1:18666"; // AI/OAuth 本地桥接服务
};

// 解析失败或请求帮助时返回 false；参数合法时填充 config 并返回 true
bool parseConfig(int argc, char* argv[], ServerConfig& config);
void printUsage(const char* program);
