#pragma once
#include <map>
#include <mutex>
#include <string>

/*
 * 用户存储。users.conf 每行一个用户：
 *   username:salt:sha256(password + ":" + salt)
 * 提供 load/verify/create，并发安全。
 */
class UserStore
{
public:
    bool load(const std::string& path, std::string& error);
    bool save(const std::string& path, std::string& error) const;
    bool verify(const std::string& username, const std::string& password) const;
    bool create(const std::string& username, const std::string& password, std::string& error);
    // OAuth 登录：user 不存在则自动建号（hash 记为 oauth:<origin>，不能用密码登录）；
    // 已存在同名普通账号则返回错误，防止第三方身份静默接管本地账号。
    bool ensureOAuthUser(const std::string& username, const std::string& origin,
        std::string& error);
    std::size_t count() const;

private:
    mutable std::mutex m_mutex;
    std::string m_path;
    struct Record
    {
        std::string salt;
        std::string hash;
    };
    std::map<std::string, Record> m_users;
};
