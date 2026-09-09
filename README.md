# 藤のnetdisk · ReactorHttp-Cpp 高性能网盘

基于 C++17 Reactor 模型的**生产级私有网盘**：事件驱动 + 动态线程池 + sendfile
零拷贝下载 + 分片断点续传 + Range 在线播放 + 账号登录 + 用户目录隔离，
并带 Notion 式文档编辑、AI 写作助手与 GitHub/Apple 登录。

## 功能

- **登录 / 注册**：网页**邮箱注册**（默认免邮箱验证，可用 `--no-register` 关闭）、
  本地账号（加盐 SHA-256）、**GitHub/Apple OAuth**（可选 sidecar 桥接），
  Cookie/Token 会话，第三方登录自动建号
- **文档编辑**：文本/Markdown 文件点击即编辑，`⌘/Ctrl+S` 保存，Markdown 预览
- **AI 助手**：`⌘/Ctrl+J` 召唤，**C++ 服务直接访问** OpenAI 兼容 API
  （OpenAI/DeepSeek/通义…，无需 sidecar），SSE 流式输出、实时显示思考过程，
  可整篇重写或把结果写回文档
- **文件管理**：目录列表、新建文件夹、重命名、移动、删除（含目录递归删除）
- **上传**：8MB 分片 + `Upload-Offset` 断点续传，中断后可续传；同名文件覆盖更新
- **下载**：sendfile 流式下载（Linux 零拷贝），大文件不占内存
- **在线观看**：视频/音频/图片/PDF/文本预览，支持 HTTP Range（拖动进度条）
- **高并发**：Reactor + 动态线程池、keep-alive、访问日志、优雅停机、连接上限保护
- **界面**：明亮暖橙主题 UI（卡片式仪表盘），登录/注册页 + 文件浏览器 + 拖拽上传 + 上传进度队列

## 快速开始

```bash
make release

# 创建账号（可多次执行以新增/重置密码）
./build/reactor-http --drive-root ./data --users-file ./users.conf \
  --add-user author:你的密码

# 启动
./build/reactor-http --port 18080 --root ./public \
  --min-workers 2 --max-workers 8 \
  --drive-root ./data --users-file ./users.conf
```

打开 <http://127.0.0.1:18080>。

启动后打开登录页点「注册」，用邮箱创建账号即可（也可以继续用 `--add-user`）。
AI 无需额外进程：登录后点右上角「AI 设置」填写 API 地址 / 模型 / Key，
配置保存在服务器 `ai-config.json`（权限 600）。

只有需要 **GitHub / Apple 第三方登录** 时才要启动 sidecar（默认监听
127.0.0.1:18666）：

```bash
python3 sidecar/bridge.py --config sidecar-config.json
```

## 目录结构

```text
ReactorHttp-Cpp/       C++ 服务端（核心源码）
  DriveServer.cpp     网盘 API：注册/登录/上传/下载/播放/管理/AI
  AiClient.cpp        AI 直连：OpenSSL TLS + SSE 流式输出 + 配置持久化
  UserStore/SessionStore/Sha256  账号与会话
  TcpServer/EventLoop/ThreadPool  Reactor 高并发骨架
public/                前端 SPA（index.html/style.css/app.js）
tests/                 协议 + 网盘回归测试
deploy/                Docker Compose / Caddy / systemd / 部署文档
sidecar/               GitHub/Apple OAuth 桥接（Python，可选）
```

## 测试

```bash
make test
```

## 部署

### 直接拉镜像（推荐）

仓库配置了 CD：每次推送到 `feature/cloud-drive` / `main`，GitHub Actions 会先跑
`make test`，再把镜像推送到 GHCR（见 `.github/workflows/publish.yml`）。
服务器不需要 clone 源码或编译，只要 Docker：

```bash
docker pull ghcr.io/yeteng101/reactor-http:latest
```

服务器部署（只拉镜像）完整步骤见
[deploy/SERVER_GUIDE.md](deploy/SERVER_GUIDE.md) 第 2 节，编排文件为
[deploy/docker-compose.ghcr.yml](deploy/docker-compose.ghcr.yml)；
更新时 `docker compose pull && docker compose up -d` 即可，数据卷保留。

### 源码构建

阿里云 ECS、Windows（Docker Desktop）、Linux systemd 三种方式的完整步骤见
[deploy/DEPLOY.md](deploy/DEPLOY.md)，一键编排文件在
[deploy/docker-compose.yml](deploy/docker-compose.yml)。

## 技术特性

- Linux `epoll` / macOS `select` 自适应；`sendfile`（Linux）与 `pread+send`（macOS）流式文件
- HTTP/1.1 keep-alive、流水线上限、空闲超时、优雅停机、连接级排空
- 路径穿越防护、请求体 64MB 上限、偏移一致性校验（409）、单用户家目录隔离
- 会话内存存储（重启即失效）；日志分级输出，指标接口 `/metrics`，健康检查 `/health`

> 注意：`drive-root` 是网盘数据目录，`webRoot/public` 只放前端静态资源，
> 两者分离，用户的文件不会通过静态路径被公开访问。
