#pragma once
#include "AiClient.h"
#include "Config.h"
#include "SessionStore.h"
#include "UserStore.h"

#include <memory>
#include <string>

/*
 * 服务器级共享状态：网盘模式是否开启、数据目录、用户与会话存储。
 * 由 main 创建并传给 TcpServer，再下发给每个连接。
 */
struct ServerContext
{
    bool driveEnabled = false;
    bool registrationEnabled = true;
    std::string driveRoot;
    std::string usersFile;
    std::string sidecarUrl;   // 仅 GitHub/Apple OAuth 需要；AI 由 C++ 直连
    UserStore users;
    SessionStore sessions;
    // AI 配置 + 后台流式任务（C++ 直接访问 OpenAI 兼容接口）
    std::shared_ptr<AiService> ai;
};
