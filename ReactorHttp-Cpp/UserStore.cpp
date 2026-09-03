#include "UserStore.h"
#include "Sha256.h"

#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <random>
#include <sys/stat.h>

namespace
{
std::string randomSalt()
{
    static std::mt19937_64 generator(static_cast<std::uint64_t>(
        std::time(nullptr)) ^ static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    const char* alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::string salt;
    for (int i = 0; i < 16; ++i)
    {
        salt += alphabet[generator() % 62];
    }
    return salt;
}

std::string hashPassword(const std::string& password, const std::string& salt)
{
    return crypto::sha256Hex(password + ":" + salt);
}
}

bool UserStore::load(const std::string& path, std::string& error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_path = path;
    m_users.clear();
    std::ifstream input(path);
    if (!input.is_open())
    {
        error = "cannot open users file: " + path;
        return false;
    }
    std::string line;
    int lineNumber = 0;
    while (std::getline(input, line))
    {
        ++lineNumber;
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        const std::size_t firstColon = line.find(':');
        if (firstColon == std::string::npos)
        {
            continue;
        }
        const std::size_t secondColon = line.find(':', firstColon + 1);
        if (secondColon == std::string::npos)
        {
            error = "users file line " + std::to_string(lineNumber) + " is malformed";
            return false;
        }
        Record record;
        record.salt = line.substr(firstColon + 1, secondColon - firstColon - 1);
        record.hash = line.substr(secondColon + 1);
        m_users[line.substr(0, firstColon)] = record;
    }
    return true;
}

bool UserStore::save(const std::string& path, std::string& error) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::ofstream output(path, std::ios::trunc);
    if (!output.is_open())
    {
        error = "cannot write users file: " + path;
        return false;
    }
    for (const auto& item : m_users)
    {
        output << item.first << ':' << item.second.salt << ':'
               << item.second.hash << '\n';
    }
    if (!output.good())
    {
        error = "failed while writing users file: " + path;
        return false;
    }
    output.close();
    chmod(path.c_str(), 0600);    // 口令文件仅所有者可读写
    return true;
}

bool UserStore::verify(const std::string& username, const std::string& password) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto item = m_users.find(username);
    if (item == m_users.end())
    {
        return false;
    }
    return hashPassword(password, item->second.salt) == item->second.hash;
}

bool UserStore::create(const std::string& username, const std::string& password, std::string& error)
{
    const bool unsafeUsername =
        username.empty() || username == "." || username == ".." ||
        username.find(':') != std::string::npos ||
        username.find('/') != std::string::npos ||
        username.find('\\') != std::string::npos ||
        username.find('\n') != std::string::npos ||
        username.find('\r') != std::string::npos ||
        username.find('\t') != std::string::npos;
    if (unsafeUsername)
    {
        error = "invalid username (must not contain : / \\ whitespace or path separators)";
        return false;
    }
    if (password.size() < 6)
    {
        error = "password must be at least 6 characters";
        return false;
    }
    const std::string salt = randomSalt();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_users[username] = Record{salt, hashPassword(password, salt)};
        if (m_path.empty())
        {
            return true;    // 只创建到内存（测试/临时场景）
        }
    }
    return save(m_path, error);
}

std::size_t UserStore::count() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_users.size();
}
