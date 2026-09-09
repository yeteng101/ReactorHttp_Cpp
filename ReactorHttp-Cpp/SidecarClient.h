#pragma once
#include <string>

struct ServerContext;

/*
 * 本地 Sidecar（Python 桥接服务）HTTP 客户端。
 *
 * Sidecar 目前只负责一件需要外网 HTTPS/TLS 的事情：
 *   OAuth：GitHub / Apple 登录的授权码换 token / 拉取身份。
 * （AI 已改由 C++ 的 AiClient 直连，不再经过这里。）
 *
 * C++ 服务器只通过 127.0.0.1（或 compose 内网）调用它，浏览器永远
 * 不直接接触 Sidecar，因此 API Key 与 OAuth Secret 不会暴露到前端。
 * 调用为同步阻塞式，仅供低频管理类接口使用（个人网盘量级足够）。
 */
struct SidecarResult
{
    bool ok = false;
    int statusCode = 0;
    std::string contentType;
    std::string body;
    std::string error;
};

// pathAndQuery 形如 "/ai/chat" 或 "/oauth/github/callback?code=..&state=.."
bool sidecarCall(const ServerContext& context, const std::string& method,
    const std::string& pathAndQuery, const std::string& contentType,
    const std::string& body, SidecarResult& result);
