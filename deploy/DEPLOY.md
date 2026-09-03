# 藤のnetdisk（ReactorHttp-Cpp）生产部署指南

本文覆盖三种场景，任选其一：

1. **阿里云 ECS（Linux）+ Docker Compose + Caddy 域名 HTTPS**（推荐，公网可用）
2. **Windows 电脑 / Windows Server + Docker Desktop**（同一套 Docker 部署）
3. **Linux 裸机 + systemd 直接运行**（无容器开销）

网盘模式开启后，所有文件都存放在 `--drive-root` 下，**每个登录用户一个私有的子目录**，
账号口令保存在 `--users-file` 指定的 `users.conf`（内容为
`用户名:盐:sha256(密码:盐)`，文件权限 0600）。

---

## 1. 先本地构建与自测

```bash
cd ReactorHttp-Cpp
make clean
make release        # -O2 生产编译
make test           # HTTP + 网盘接口回归测试
```

创建第一个账号（任意目录都可运行，示例在当前目录）：

```bash
./build/reactor-http \
  --drive-root ./data \
  --users-file ./users.conf \
  --add-user author:你的密码
```

本地启动网盘：

```bash
./build/reactor-http \
  --port 18080 \
  --root ./public \
  --min-workers 2 --max-workers 8 \
  --drive-root ./data \
  --users-file ./users.conf
```

浏览器打开 <http://127.0.0.1:18080> 即可看到登录页。

---

## 2. 阿里云 ECS（Linux）Docker 部署

### 2.1 准备服务器

- 系统推荐 Ubuntu 22.04/24.04 或 Debian 12，2 核 4GB 起步（网盘按并发和文件量加配）。
- 登录阿里云控制台 → ECS → 实例 → 安全组 → 配置规则：
  - 放行 **80（HTTP）** 和 **443（HTTPS）**；
  - 容器内部用的 10000 端口**不需要**对公网放行；
  - 22 端口建议只允许你自己的 IP。
- 如果还没有域名：在阿里云/其他注册商买一个域名，DNS 控制台添加 **A 记录**指向服务器公网 IP。

### 2.2 安装 Docker

```bash
sudo apt-get update
sudo apt-get install -y ca-certificates curl
curl -fsSL https://get.docker.com | sudo sh
sudo usermod -aG docker $USER
# 重新登录一次 SSH 使 docker 组生效
```

### 2.3 拉代码并部署

```bash
git clone git@github.com:yeteng101/ReactorHttp_Cpp.git
cd ReactorHttp_Cpp
git checkout feature/cloud-drive        # 网盘功能所在分支
cd deploy

# 配置域名（改成你自己的，并把 A 记录解析到本机公网 IP）
cp .env.example .env
sed -i '' 's/^DOMAIN=.*/DOMAIN=pan.example.com/' .env    # Linux 上 sed -i 不带 ''

# 第一次创建作者账号（数据卷会自动建好）
docker compose run --rm netdisk \
  /app/reactor-http --add-user author:你的强密码 \
  --users-file /etc/reactor-http/users.conf --drive-root /data

# 启动网盘 + Caddy（自动申请 HTTPS 证书）
docker compose up -d --build

# 查看状态与日志
docker compose ps
docker compose logs -f netdisk
```

几分钟后访问 `https://你的域名`，用刚才的 author 账号登录。

### 2.4 不想要域名？先 IP 直连测试

```bash
docker compose run --rm netdisk \
  /app/reactor-http --add-user author:你的密码 \
  --users-file /etc/reactor-http/users.conf --drive-root /data

# 编辑 deploy/docker-compose.yml，把 netdisk 服务里的
#   # ports:
#   #   - "10000:10000"
# 两行取消注释，然后只启动网盘服务：
docker compose up -d --build netdisk
docker compose ps
```

然后在阿里云安全组放行 **10000** 端口，浏览器访问 `http://公网IP:10000`。
（仅调试：HTTP 明文 + 无 HTTPS 时密码会明文传输，正式使用请用域名 + Caddy。）

### 2.5 域名和 IP 的关系（必读）

- **IP 是服务器的门牌号**，别人要访问你的网站，最后总得连到一个 IP。
- **域名是给人记的名字**，DNS 系统负责把 `pan.example.com` 翻译成 IP。
  A 记录写的就是「这个名字 → 哪个 IP」。
- `172.16.0.0 ~ 172.31.255.255`（还有 `10.x`、`192.168.x`）是**私网地址**，
  只能在内网（你家里 / 云厂商 VPC 内部）用，公网用户连不进去。
  公网 IP 形如 `121.196.x.x`，才是阿里云给 ECS 的那一个。
- 有了域名后不要访问 `http://IP:10000`，而是配好 Caddy 后访问
  `https://你的域名`：域名负责好记 + 证书校验，Caddy 负责 HTTPS，IP 只躲在后面。

### 2.6 文档编辑器 + AI 助手

本分支前端已内置：

- **在线文档编辑**：`.md/.txt/.json/.py/...` 点击文档图标即可编辑，`⌘/Ctrl+S` 保存；
- **Markdown 预览**：编辑器右上角「预览」按钮；
- **AI 助手**：`⌘/Ctrl+J` 在任意文档里召唤，可润色 / 改写 / 翻译 / 总结 /
  自定义指令，结果一键「替换全文」或「插到文末」；
- **AI 设置**：AI 面板右上角齿轮填写 OpenAI 兼容接口（OpenAI / DeepSeek /
  通义 / Moonshot），API Key 只存服务器 sidecar，不下发浏览器。

在 `deploy/.env` 里填（或用网页设置）：

```bash
AI_BASE_URL=https://api.openai.com/v1   # DeepSeek: https://api.deepseek.com/v1
AI_MODEL=gpt-4o-mini                    # DeepSeek: deepseek-chat
AI_API_KEY=sk-xxxxxxxx
```

### 2.7 GitHub / Apple 第三方登录

先在 `.env` 中配置对应变量（见 `.env.example`），重启生效：

```bash
docker compose up -d
docker compose logs -f sidecar   # 出现 listening 即成功
```

GitHub 注册（免费）：

1. GitHub → Settings → Developer settings → OAuth Apps → New OAuth App；
2. Homepage URL 填 `https://你的域名`；
3. Authorization callback URL 填 `https://你的域名/api/oauth/github/callback`；
4. 把 Client ID / Client Secret 填进 `.env` 的 `OAUTH_GITHUB_*`。

Apple（需要 Apple Developer 会员）：

1. developer.apple.com → Certificates/Identifiers → Services 里建 Service ID，
   勾选 Sign in with Apple；
2. 配置域名与回调 `https://你的域名/api/oauth/apple/callback`；
3. 创建 Sign in with Apple Key（.p8），把 Team ID / Key ID / Service ID 和私钥
   路径填进 `.env`，并把 p8 挂载进 sidecar 容器（docker-compose 里有注释示例）。

登录页会出现对应按钮；首次第三方登录会自动创建该用户自己的网盘目录。

---

## 3. Windows 电脑 / Windows Server 部署

核心思路：**Windows 上跑 Linux 容器（Docker Desktop），应用代码与 Linux 完全一致**。
本项目依赖 Linux 的 epoll/sendfile/`fork` 无关的 POSIX API，Docker 容器内是 Debian，
因此不需要在 Windows 上编译 C++。

### 3.1 安装 Docker Desktop（Windows 10/11 专业版或 Windows Server）

1. 安装 [Docker Desktop](https://www.docker.com/products/docker-desktop/)。
2. 设置里确保使用 **WSL 2 backend**，并安装 WSL2 内核。
3. 打开 PowerShell，验证：`docker version`、`docker compose version`。

### 3.2 部署

```powershell
git clone git@github.com:yeteng101/ReactorHttp_Cpp.git
cd ReactorHttp_Cpp
git checkout feature/cloud-drive
cd deploy

# 复制 .env 并填域名
Copy-Item .env.example .env
notepad .env

# 建账号
docker compose run --rm netdisk /app/reactor-http --add-user author:你的密码 --users-file /etc/reactor-http/users.conf --drive-root /data

# 启动（自动拉取/构建镜像并启动 Caddy）
docker compose up -d --build
docker compose ps
```

### 3.3 Windows 常见网络情况

- **云上的 Windows Server**：控制台/防火墙放行 80、443，域名 A 记录指向该公网 IP，与阿里云 ECS 一致。
- **家里的 Windows 电脑**：光猫/路由器需要把 80、443 端口转发到这台电脑；宽带没有公网 IP 时，
  用 DDNS（如花生壳）+ 端口映射，或用 frp/Cloudflare Tunnel 把流量转发进来。
  域名解析到 DDNS 域名或隧道地址即可。
- Docker Desktop 的 volume 实际存放在 WSL2 里；要备份/迁移数据卷时用
  `docker compose down` 后执行 `docker volume ls | grep netdisk-data`，
  再用 `docker volume inspect <卷名>` 查看挂载点目录复制。

---

## 4. Linux 裸机 + systemd（可选，无容器）

```bash
sudo apt-get install -y g++ make
cd ReactorHttp-Cpp
make release

sudo useradd --system --home /srv/netdisk --no-create-home netdisk
sudo mkdir -p /srv/netdisk/data /srv/netdisk/conf /var/log/reactor-http
sudo chown -R netdisk:netdisk /srv/netdisk /var/log/reactor-http

sudo -u netdisk ./build/reactor-http \
  --drive-root /srv/netdisk/data \
  --users-file /srv/netdisk/conf/users.conf \
  --add-user author:你的密码

sudo cp deploy/reactor-http.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now reactor-http
```

如果希望 Caddy 反代：参考 `deploy/Caddyfile`，把 `reverse_proxy` 目标从 `netdisk:10000`
改成 `127.0.0.1:10000`，并注释掉容器相关的 `{$DOMAIN}` 站点块。

---

## 5. 账号管理

```bash
# 新增/修改账号密码（重新执行即可覆盖密码）
docker compose run --rm netdisk \
  /app/reactor-http --add-user 新用户名:新密码 \
  --users-file /etc/reactor-http/users.conf --drive-root /data

# 查看已有账号（只读 users.conf）
sudo cat /srv/netdisk/conf/users.conf   # 裸机
docker compose exec netdisk cat /etc/reactor-http/users.conf

# 删除账号：编辑 users.conf 删除对应行，然后
docker compose restart netdisk
```

> 安全提示：`users.conf` 只保存加盐 SHA-256 口令散列，不含明文密码；
> 生产环境建议让账号走 HTTPS 登录（Caddy 已默认提供）。
> 服务器重启后内存会话全部失效（需重新登录），如需会话持久化可后续接入 Redis。

---

## 6. 数据备份 / 升级 / 回滚

### 备份

```bash
# 容器方式：直接把数据卷内容打包到当前目录
docker compose exec netdisk tar czf - -C /data . > netdisk-data-backup.tar.gz
docker compose exec netdisk tar czf - -C /etc/reactor-http . > netdisk-users-backup.tar.gz

# 裸机方式：直接备份 /srv/netdisk/data 和 users.conf 即可
```

### 升级

```bash
git pull --rebase origin feature/cloud-drive
docker compose up -d --build          # 镜像标签不变时加 --force-recreate
docker compose ps
```

### 回滚

```bash
# 旧镜像标签示例：docker-compose.yml 里 image 改成上一版标签后重启
docker compose stop netdisk caddy
docker compose up -d --no-build --force-recreate
```

---

## 7. 性能与压测

服务本身是 Reactor + 线程池 + sendfile 流式发送，下载大文件不会占内存。
压测时注意：

```bash
# 在客户端（不要在被测服务器上压，否则 CPU 互相干扰）安装 wrk
brew install wrk                      # macOS
sudo apt-get install -y wrk           # Linux

wrk -t4 -c64 -d30s --latency https://pan.example.com/api/drive/stream?path=某文件
```

阿里云 ECS 带宽（如 5Mbps 公网）通常是吞吐瓶颈；内网压测可以用 `-H "Host: ..."` 直连
服务器内网 IP:10000 以测出真实服务能力。参考参数：

- 登录接口：`wrk -t4 -c100 -d30s --latency http://127.0.0.1:10000/api/me`
- 静态页：`wrk -t8 -c300 -d60s --latency http://127.0.0.1:10000/index.html`

调整 worker：

```bash
# 一般建议 min=max=CPU 核数（例如 4 核：--min-workers 4 --max-workers 4）
# 修改 deploy/docker-compose.yml 里的 --min-workers / --max-workers 后：
docker compose up -d --force-recreate netdisk
```

---

## 8. 常见问题

- **`GLIBC_2.34 not found / GLIBCXX_3.4.30 not found`**：说明二进制是在高版本 glibc
  环境编译、跑到旧系统。本项目 Dockerfile 用 `gcc:14-bookworm` 构建 + `bookworm-slim`
  运行，两者一致，不存在该问题；裸机请用 Debian 12 / Ubuntu 22.04+ 编译运行。
- **容器一直 Restarting**：先看日志 `docker logs fuji-netdisk`。最常见原因是
  还没执行 `--add-user`（启动日志会提示），或 `/data`、`/etc/reactor-http` 无写权限。
- **HTTPS 证书不自动签发**：确认域名 A 记录已生效、服务器 80/443 从公网可达、
  没有在 Caddy 前面再套一层占用 80 端口的服务。
- **上传几十 GB 大文件**：前端按 8MB 分片 + 断点续传，中断后重新选择同一文件会自动续传；
  单次请求体上限 64MB（服务端保护），分片不受此限制。
- **跨端口/跨域访问打不开**：正式使用请通过同一域名（Caddy 反代），登录 Cookie 是
  SameSite=Lax 且可带 Secure；用 IP:10000 直连测试时也请用同一 IP:端口访问页面。
