#pragma once
#include "Config.h"
#include "SessionStore.h"
#include "UserStore.h"

#include <string>

/*
 * 服务器级共享状态：网盘模式是否开启、数据目录、用户与会话存储。
 * 由 main 创建并传给 TcpServer，再下发给每个连接。
 */
struct ServerContext
{
    bool driveEnabled = false;
    std::string driveRoot;
    std::string usersFile;
    std::string sidecarUrl;   // 由 main 从 ServerConfig 拷入
    UserStore users;
    SessionStore sessions;
};
