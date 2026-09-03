#pragma once
#include <string>

struct ServerContext;
class HttpRequest;
class HttpResponse;

/*
 * 网盘 API 服务（仅在 --drive-root 指定时启用）：
 *
 *   POST   /api/login?                       登录（表单 username=&password=，公开）
 *   POST   /api/logout                       注销（需登录）
 *   GET    /api/me                           返回当前用户（需登录）
 *   GET    /api/drive/list?path=<dir>        列出目录
 *   GET    /api/drive/stat?path=<file>       查询文件状态（存在性/大小）
 *   POST   /api/drive/mkdir?path=<dir>       创建目录
 *   PATCH  /api/drive/file/<rel>             断点续传上传（Upload-Offset 头）
 *   DELETE /api/drive/remove?path=<rel>      删除文件或空目录
 *   POST   /api/drive/rename?from=&to=       重命名/移动
 *   GET    /api/drive/download?path=<rel>    下载（attachment）
 *   GET    /api/drive/stream?path=<rel>      在线播放/预览（支持 Range）
 *
 * 所有 /api/drive/ 与 /api/me、/api/logout 都需要登录。
 * 会话令牌通过 Cookie sid= 或 Authorization: Bearer 或 ?token= 传递。
 */
namespace Drive
{
bool handle(ServerContext& context, const std::string& requestPath, HttpRequest& request,
    HttpResponse& response);
}
