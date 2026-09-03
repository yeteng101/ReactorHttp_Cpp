#pragma once
#include <stdarg.h>
#include <stdio.h>
#include <string>

/*
 * 生产级日志模块：
 *  - 线程安全（内部有互斥锁）
 *  - 输出带时间戳，可配置到文件或 stdout/stderr
 *  - access 日志用于记录每个 HTTP 请求
 *
 * 旧代码中的 Debug(...) / Error(...) 宏保留，映射到新日志接口；
 * Error 只记录错误日志，不再像旧版那样直接 exit(0)。
 */
namespace Log
{
    // 初始化输出目标；路径为 "-" 时 access 用 stdout、error 用 stderr
    void configure(const std::string& accessPath, const std::string& errorPath);
    void access(const char* fmt, ...);
    void info(const char* fmt, ...);
    void warn(const char* fmt, ...);
    void error(const char* fmt, ...);
    void debug(const char* fmt, ...);
    // 刷新并关闭日志文件（进程退出前调用）
    void shutdown();
}

#define Debug(...) ::Log::debug(__VA_ARGS__)
#define Error(...) ::Log::error(__VA_ARGS__)
