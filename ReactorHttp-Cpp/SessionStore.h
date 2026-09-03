#pragma once
#include <map>
#include <mutex>
#include <string>

/*
 * 内存会话存储。登录成功后生成随机 token，
 * 后续请求通过 Cookie sid= 或 Authorization: Bearer 或 ?token= 携带。
 * 进程重启后所有会话失效（生产环境后续可换 Redis）。
 */
class SessionStore
{
public:
    SessionStore();
    // 创建会话，返回 token
    std::string create(const std::string& username);
    // 校验并返回用户名；失效返回空串
    std::string check(const std::string& token) const;
    void remove(const std::string& token);
    int sessionHours() const;
    void setSessionHours(int hours);

private:
    mutable std::mutex m_mutex;
    std::map<std::string, std::pair<std::string, std::int64_t>> m_sessions;
    int m_sessionHours = 24;
};
