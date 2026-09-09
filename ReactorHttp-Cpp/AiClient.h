#pragma once
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/*
 * AI 客户端：由 C++ 服务器直接访问 OpenAI 兼容接口（HTTPS/OpenSSL），
 * 不再依赖 Python sidecar 转发。
 *
 * 设计要点：
 *   - 配置（baseUrl / model / apiKey）保存在服务器上的 ai-config.json（权限 600），
 *     通过网页「AI 设置」读写，Key 永远不下发到浏览器；
 *   - 聊天采用 SSE 流式读取，边生成边把增量文本写入内存任务，
 *     浏览器用 job id 轮询即可实时看到「思考过程」；
 *   - 生成过程放在后台线程，避免阻塞 Reactor 事件循环。
 */

struct AiConfig
{
    std::string baseUrl = "https://api.openai.com/v1";
    std::string model = "gpt-4o-mini";
    std::string apiKey;
    bool insecureTls = false;    // 仅调试用：跳过证书校验
};

struct AiJob
{
    std::mutex mutex;
    std::string text;            // 已生成的完整文本（增量累积）
    std::string model;
    std::string error;
    bool done = false;
    bool ok = false;
    std::chrono::steady_clock::time_point createdAt = std::chrono::steady_clock::now();
};

// 直接向 OpenAI 兼容接口发起流式 chat/completions 请求。
// onDelta 每收到一段增量文本调用一次；成功返回 true。
bool aiChatStream(const AiConfig& config, const std::string& requestJson,
    const std::function<void(const std::string&)>& onDelta,
    std::string& fullText, std::string& model, std::string& error);

class AiService
{
public:
    AiService() = default;

    bool load(const std::string& path, std::string& error);
    bool save(std::string& error) const;
    bool update(const std::string& baseUrl, const std::string& model,
        const std::string& apiKey, bool keyProvided, bool insecureTls,
        bool insecureProvided, std::string& error);
    AiConfig snapshot() const;
    std::string configPath() const;

    // 启动一个后台流式任务，返回 job id（失败返回空串）
    std::string startJob(const std::string& requestJson, std::string& error);
    std::shared_ptr<AiJob> findJob(const std::string& id);

private:
    void pruneLocked();

    mutable std::mutex m_mutex;
    std::string m_path;
    AiConfig m_config;
    std::map<std::string, std::shared_ptr<AiJob>> m_jobs;
};
