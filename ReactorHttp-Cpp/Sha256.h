#pragma once
#include <cstdint>
#include <string>

// 自包含 SHA-256（无 OpenSSL 依赖），用于口令散列与会话令牌
namespace crypto
{
std::string sha256Hex(const std::string& data);
}
