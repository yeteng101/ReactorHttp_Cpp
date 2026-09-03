#include "SessionStore.h"
#include "Sha256.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>
#include <sstream>

namespace
{
std::string randomToken()
{
    std::random_device device;
    std::string token;
    for (int i = 0; i < 8; ++i)
    {
        token += crypto::sha256Hex(std::to_string(device()));
    }
    return crypto::sha256Hex(token).substr(0, 64);
}
}

SessionStore::SessionStore() = default;

int SessionStore::sessionHours() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sessionHours;
}

void SessionStore::setSessionHours(int hours)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sessionHours = hours;
}

std::string SessionStore::create(const std::string& username)
{
    const std::string token = randomToken();
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::int64_t expiry = now + static_cast<std::int64_t>(m_sessionHours) * 3600;
        m_sessions[token] = {username, expiry};
        // 顺手清理过期会话，避免无限增长
        for (auto it = m_sessions.begin(); it != m_sessions.end();)
        {
            if (it->second.second <= now)
            {
                it = m_sessions.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
    return token;
}

std::string SessionStore::check(const std::string& token) const
{
    if (token.empty())
    {
        return std::string();
    }
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto item = m_sessions.find(token);
    if (item == m_sessions.end() || item->second.second <= now)
    {
        return std::string();
    }
    return item->second.first;
}

void SessionStore::remove(const std::string& token)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sessions.erase(token);
}
