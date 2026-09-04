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

## 2. 拉代码（建议用 SSH，避开 GitHub HTTP/2 断流）

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

## 3. 配置 .env

```bash
cd /opt/ReactorHttp-Cpp/deploy
cp .env.example .env
```

检查 `.env` 里 `DOMAIN=yeteng.xin`（必须已解析到本机公网 IP）。AI、GitHub/Apple 登录
为可选项，参考文件内注释。

## 4. 创建第一个账号

```bash
docker compose run --rm netdisk \
  /app/reactor-http --add-user author:你的强密码 \
  --users-file /etc/reactor-http/users.conf --drive-root /data
```

> 忘了这步容器会一直 Restarting（日志会提示先 --add-user）。

## 5. 构建并启动

```bash
docker compose up -d --build
docker compose ps                    # 三个服务都应为 Up
docker compose logs -f netdisk       # Ctrl+C 退出日志
```

首次构建会拉 `gcc:14-bookworm`、`python:3.12-slim` 等镜像并编译 C++ 与安装 Python
依赖，需几分钟。若在 `make release` 阶段报 `'strerror' was not declared...`，说明副本
太旧（该问题已修复并入库），执行 `git pull --rebase` 后重试。

## 6. 验证

```bash
curl -sI https://yeteng.xin | head -1          # 期待 HTTP/2 200
docker compose ps                              # netdisk/sidecar/caddy 全部 Up
```

浏览器打开 `https://yeteng.xin`，用第 4 步的账号登录。
证书由 Caddy 自动申请（Let's Encrypt），首次签发需等约 30 秒~1 分钟。

## 7. 日常运维速查

```bash
# 查看状态/日志
docker compose ps
docker compose logs -f --tail=100 netdisk

# 健康检查（容器自带 /health）
docker inspect --format='{{.State.Health.Status}}' fuji-netdisk

# 升级
cd /opt/ReactorHttp-Cpp
git pull --rebase origin feature/cloud-drive
cd deploy
docker compose up -d --build

# 备份数据与账号
docker compose exec netdisk tar czf - -C /data . > netdisk-data-backup.tar.gz
docker compose exec netdisk tar czf - -C /etc/reactor-http . > netdisk-users-backup.tar.gz

# 新增/重置账号密码（覆盖写）
docker compose run --rm netdisk /app/reactor-http --add-user 用户名:新密码 \
  --users-file /etc/reactor-http/users.conf --drive-root /data
```

## 8. 快速排错

- **容器 Restarting**：`docker compose logs netdisk`；最常见是没执行 `--add-user`。
- **证书不签发**：A 记录是否指向公网 IP、80/443 是否放行、服务器时间是否准确。
- **想用 IP 直连调试**（无域名）：`docker-compose.yml` 里取消 `18080:18080` 注释并放行
  安全组 18080，只启 netdisk：`docker compose up -d --build netdisk`。仅限调试。
- **打不开/白屏**：请通过 `https://yeteng.xin` 访问，不要混用 IP:18080 与域名。

详细备份/回滚、裸机 systemd 部署与压测见 `DEPLOY.md`。
