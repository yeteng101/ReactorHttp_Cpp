# 服务器部署指南（阿里云 ECS 从零到 https://yeteng.xin）

> 精简实操版。原理、架构与排错详见同目录的 `DEPLOY.md`。
> 适用：Ubuntu 22.04/24.04 或 Debian 12，推荐 2 核 4GB 起步。

## 0. 一次性准备（控制台操作，5 分钟）

| 事项 | 操作 |
| --- | --- |
| 安全组 | ECS → 安全组 → 入方向放行 `80`、`443`；`22` 只放行你的 IP；`18080` 不要对公网放行 |
| 域名解析 | 域名 DNS 控制台加 A 记录：`yeteng.xin → 服务器公网 IP`（不是私网 172.x/10.x） |
| 系统 | 推荐 Ubuntu 22.04/24.04 或 Debian 12 |

检查公网 IP：

```bash
curl -s https://checkip.amazonaws.com   # 与阿里云控制台显示的「公网 IP」一致
```

## 1. 安装 Docker

```bash
sudo apt-get update
sudo apt-get install -y ca-certificates curl
curl -fsSL https://get.docker.com | sudo sh
sudo usermod -aG docker $USER
# 重新登录 SSH，让 docker 组生效
```

## 2. 方式 A（推荐）：直接拉 GHCR 镜像（不在服务器编译）

仓库已配置 CD（`.github/workflows/publish.yml`）：每次推送到 `feature/cloud-drive`
或 `main`，GitHub Actions 会先跑 `make test`，再把镜像推送到 GitHub Container Registry。
服务器只需要 Docker，不用 clone 源码、不用装编译环境。

可用镜像：

| 镜像 | 说明 |
| --- | --- |
| `ghcr.io/yeteng101/reactor-http:latest` | 最新构建，跟随 `feature/cloud-drive` |
| `ghcr.io/yeteng101/reactor-http:v3.1.0` | 打 tag 时的固定版本 |
| `ghcr.io/yeteng101/reactor-http:sha-xxxxxxx` | 精确到某次提交 |
| `ghcr.io/yeteng101/reactor-http-sidecar:latest` | OAuth 桥接（可选） |

首次部署（完成第 1 步装好 Docker 后）：

```bash
mkdir -p /opt/netdisk && cd /opt/netdisk

# 拉取部署三件套；compose 存成 docker-compose.yml，后续命令不用加 -f
curl -fsSL -o docker-compose.yml \
  https://raw.githubusercontent.com/yeteng101/ReactorHttp_Cpp/feature/cloud-drive/deploy/docker-compose.ghcr.yml
curl -fsSL -o Caddyfile \
  https://raw.githubusercontent.com/yeteng101/ReactorHttp_Cpp/feature/cloud-drive/deploy/Caddyfile
curl -fsSL -o .env \
  https://raw.githubusercontent.com/yeteng101/ReactorHttp_Cpp/feature/cloud-drive/deploy/.env.example
vi .env                      # 确认 DOMAIN=yeteng.xin，且 A 记录已指向本机公网 IP

docker compose pull          # 从 GHCR 拉镜像
docker compose up -d         # 启动 netdisk + sidecar + caddy
docker compose ps            # 三个服务都应为 Up
```

以后每次更新（GitHub 推送后自动出镜像）：

```bash
cd /opt/netdisk
docker compose pull
docker compose up -d         # 数据卷 netdisk-data / netdisk-users 原样保留
```

> **镜像仓库权限**：GHCR 包第一次发布后默认私有，二选一：
> 1. 公开（服务器免登录）：GitHub 右上角头像 → **Your packages** → `reactor-http`
>    → Package settings → Danger Zone → Change visibility → **Public**；
> 2. 保持私有：服务器执行
>    `echo <你的PAT> | docker login ghcr.io -u yeteng101 --password-stdin`
>    （PAT 在 GitHub → Settings → Developer settings → Personal access tokens 创建，
>    勾选 `read:packages` 即可）。

完成本节后可直接跳到第 7 步验证；第 3~6 步是在服务器上编译源码的方式 B。

## 3. 方式 B：拉源码在服务器上编译（可选）

本机生成密钥并加到 GitHub（Settings → SSH and GPG keys）：

本机生成密钥并加到 GitHub（Settings → SSH and GPG keys）：

```bash
ssh-keygen -t ed25519 -C "aliyun-ecs"
cat ~/.ssh/id_ed25519.pub    # 复制内容到 GitHub
```

服务器上：

```bash
git clone git@github.com:yeteng101/ReactorHttp_Cpp.git /opt/ReactorHttp-Cpp
cd /opt/ReactorHttp-Cpp
git checkout feature/cloud-drive
```

> 若只能用 HTTPS 且报 `RPC failed; curl 16 ... HTTP2 framing layer`：
> ```bash
> git config --global http.version HTTP/1.1
> git config --global http.postBuffer 524288000
> git clone -b feature/cloud-drive https://github.com/yeteng101/ReactorHttp_Cpp.git /opt/ReactorHttp-Cpp
> ```

## 4. 配置 .env（方式 B；方式 A 已在第 2 步完成）

```bash
cd /opt/ReactorHttp-Cpp/deploy
cp .env.example .env
```

检查 `.env` 里 `DOMAIN=yeteng.xin`（必须已解析到本机公网 IP）。AI、GitHub/Apple 登录
为可选项，参考文件内注释。

## 5. 第一个账号：直接网页注册

现在**不需要**预先建账号：启动后打开 `https://yeteng.xin`，点登录卡片的「注册」，
用邮箱 + 密码创建即可（默认不发验证邮件）。

想先用命令行建号、或想关闭网页注册，也可以：

```bash
docker compose run --rm netdisk \
  /app/reactor-http --add-user author:你的强密码 \
  --users-file /etc/reactor-http/users.conf --drive-root /data
```

> 关闭网页注册：在 netdisk 的 `command` 末尾追加 `--no-register`（方式 A 改
> `/opt/netdisk/docker-compose.yml`，方式 B 改 `deploy/docker-compose.yml`），
> 然后 `docker compose up -d --force-recreate netdisk`。

## 6. 构建并启动（方式 B）

```bash
docker compose up -d --build
docker compose ps                    # 三个服务都应为 Up
docker compose logs -f netdisk       # Ctrl+C 退出日志
```

首次构建会拉 `gcc:14-bookworm`、`python:3.12-slim` 等镜像并编译 C++ 与安装 Python
依赖，需几分钟。若在 `make release` 阶段报 `'strerror' was not declared...`，说明副本
太旧（该问题已修复并入库），执行 `git pull --rebase` 后重试。

## 7. 验证

```bash
curl -sI https://yeteng.xin | head -1          # 期待 HTTP/2 200
docker compose ps                              # netdisk/sidecar/caddy 全部 Up
```

浏览器打开 `https://yeteng.xin`，用刚注册的邮箱登录（或用第 5 步的账号）。
证书由 Caddy 自动申请（Let's Encrypt），首次签发需等约 30 秒~1 分钟。

## 8. 日常运维速查

```bash
# 查看状态/日志
docker compose ps
docker compose logs -f --tail=100 netdisk

# 健康检查（容器自带 /health）
docker inspect --format='{{.State.Health.Status}}' "$(docker compose ps -q netdisk)"

# 升级（方式 A：拉新镜像）
cd /opt/netdisk
docker compose pull && docker compose up -d

# 升级（方式 B：拉源码重新编译）
cd /opt/ReactorHttp-Cpp && git pull --rebase origin feature/cloud-drive
cd deploy && docker compose up -d --build

# 备份数据与账号
docker compose exec netdisk tar czf - -C /data . > netdisk-data-backup.tar.gz
docker compose exec netdisk tar czf - -C /etc/reactor-http . > netdisk-users-backup.tar.gz

# 新增/重置账号密码（覆盖写）
docker compose run --rm netdisk /app/reactor-http --add-user 用户名:新密码 \
  --users-file /etc/reactor-http/users.conf --drive-root /data
```

## 9. 快速排错

- **容器 Restarting**：`docker compose logs netdisk`；先看日志里的具体报错。
- **证书不签发**：A 记录是否指向公网 IP、80/443 是否放行、服务器时间是否准确。
- **想用 IP 直连调试**（无域名）：`docker-compose.yml` 里取消 `18080:18080` 注释并放行
  安全组 18080，只启 netdisk：`docker compose up -d --build netdisk`。仅限调试。
- **打不开/白屏**：请通过 `https://yeteng.xin` 访问，不要混用 IP:18080 与域名。

详细备份/回滚、裸机 systemd 部署与压测见 `DEPLOY.md`。

## 10. 开启 GitHub 登录（OAuth）

登录页的 GitHub 按钮由 `/api/oauth/status` 决定：sidecar 同时拿到
`clientId` 和 `clientSecret` 才显示。Docker 部署下**不需要**仓库根目录的
`sidecar-config.json`，直接在 `deploy/.env` 填环境变量即可（compose 会以
`SIDECAR_GITHUB_*` 注入 sidecar，优先级高于配置文件）。

### 10.1 GitHub 上创建 OAuth App

1. GitHub → Settings → Developer settings → OAuth Apps → **New OAuth App**；
2. Application name 随意（如 `fuji-netdisk`）；
3. Homepage URL 填 `https://yeteng.xin`；
4. **Authorization callback URL 必须填：**
   `https://yeteng.xin/api/oauth/github/callback`（GitHub 只允许 https
   回调，需等域名证书签发后再注册）；
5. 注册后复制 **Client ID**，并点 Generate a new client secret 复制 **Secret**。

### 10.2 服务器填入凭据并重启 sidecar

```bash
# 方式 A 在 /opt/netdisk，方式 B 在 /opt/ReactorHttp-Cpp/deploy
cd /opt/netdisk        # 或 cd /opt/ReactorHttp-Cpp/deploy
cp .env.example .env        # 还没有 .env 时
vim .env                    # 在文件末尾追加：
#   OAUTH_GITHUB_CLIENT_ID=你的ClientID
#   OAUTH_GITHUB_CLIENT_SECRET=你的ClientSecret

docker compose up -d --force-recreate sidecar
```

### 10.3 验证

```bash
curl -s https://yeteng.xin/api/oauth/status
# 期望输出：{"github":true,"apple":false}
```

刷新登录页（Cmd/Ctrl+Shift+R 强刷），出现「使用 GitHub 登录」按钮即可。
首次登录会在浏览器跳转到 GitHub 授权，授权后自动创建账号并种 Cookie。

> 若返回 `{"github":false,...}`：确认 `.env` 两个变量都已填且无多余空格；
> 然后 `docker compose logs sidecar` 看报错。Secret 泄露时到 GitHub 页面
> 重新生成即可，旧 Secret 立即失效。
