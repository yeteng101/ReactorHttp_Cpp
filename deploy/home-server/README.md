# 旧电脑自建“硬件服务器”方案

这个分支把网盘服务真正跑在你自己家里的旧电脑上，文件全部存在旧电脑磁盘；
阿里云 ECS 从“网盘主机”降级成“公网门卫”（只转发流量）。

## 拓扑

```text
┌──────────────┐   https    ┌──────────────────────────────┐
│ 手机/浏览器   │ ─────────> │ 阿里云 ECS（你的公网入口）     │
└──────────────┘            │  Caddy：域名+HTTPS 自动证书    │
                            │  frps ：frp 服务端 (7000)     │
                            └──────────────┬───────────────┘
                                           │ frp 隧道（由旧电脑主动向外连，
                                           │  旧电脑不需要公网 IP/端口映射）
                            ┌──────────────┴───────────────┐
                            │ 家里旧电脑                     │
                            │  reactor-http 网盘 (10000)    │
                            │  文件落盘：D:\AuroraDrive\data │
                            └──────────────────────────────┘
```

## 文件说明

| 文件 | 部署在哪 | 作用 |
| --- | --- | --- |
| [docker-compose.relay.yml](docker-compose.relay.yml) | 阿里云 ECS | frps + Caddy 中继 |
| [frps.toml](frps.toml) | 阿里云 ECS | frp 服务端配置（token/端口） |
| [Caddyfile.relay](Caddyfile.relay) | 阿里云 ECS | 域名 HTTPS 反代到 frp |
| [docker-compose.home.yml](docker-compose.home.yml) | 旧电脑 | 网盘 + frpc 一键栈 |
| [frpc-home.toml](frpc-home.toml) | 旧电脑 | frp 客户端（连 ECS） |
| [reactor-http-home.service](reactor-http-home.service) | 旧电脑(Linux) | 网盘 systemd 服务 |
| [windows-old-pc.md](windows-old-pc.md) | - | Windows 旧电脑完整步骤 |
| [linux-old-pc.md](linux-old-pc.md) | - | Linux 旧电脑完整步骤 |
| [ddns/](ddns/) | 可选 | 家庭宽带“有公网 IP”时的 DDNS 方案 |

## 30 秒看懂两个概念

- **内网穿透（frp）**：家宽通常没有公网 IP，外面的请求进不来。frp 让旧电脑**主动**
  连出去到 ECS 上保持一条隧道，ECS 收到公网请求后从隧道原路转回旧电脑。
  你家 IP 怎么变、有没有公网 IP，都不影响。
- **DDNS**：只有你家宽带**有动态公网 IP** 时才有意义——域名 A 记录需要跟着变动的
  IP 自动更新。frp 方案下可以完全不装 DDNS。

详见 [ddns/README.md](ddns/README.md)。

## 快速操作

### 1. 阿里云 ECS（一次性搭中继）

```bash
cd ReactorHttp_Cpp
git checkout feature/home-server
cd deploy/home-server

# 改配置：生成 token 并粘进 frps.toml（frpc-home.toml 之后用同一个）
openssl rand -hex 16
nano frps.toml

# Caddy 用它申请 HTTPS（把域名 A 记录先解析到本机公网 IP）
echo 'DOMAIN=你的域名' > .env

docker compose -f docker-compose.relay.yml up -d
```

阿里云安全组放行：80、443 对所有来源；7000 仅对你家出口 IP（没有固定出口 IP 就
放 0.0.0.0/0，靠 frp token 保护）。

### 2. 旧电脑（Windows 参考第 0~5 步）

```powershell
cd D:\AuroraDrive\ReactorHttp_Cpp
git checkout feature/home-server
cd deploy/home-server
Copy-Item .env.home.example .env
notepad .env                # D 盘目录
notepad frpc-home.toml      # ECS IP + token + 域名

docker compose -f docker-compose.home.yml build netdisk
docker compose -f docker-compose.home.yml run --rm netdisk `
  /app/reactor-http --add-user author:你的密码 `
  --users-file /etc/reactor-http/users.conf --drive-root /data
docker compose -f docker-compose.home.yml up -d
```

浏览器访问 `https://你的域名` 完成验收。

## 为什么 frp 方案下不需要 DDNS

frpc 是**旧电脑主动外连** ECS 的固定 IP/域名，只要旧电脑能上网，家里出口 IP 如何
变动都与隧道无关。DDNS 解决的是“域名指向我家 IP”的问题，只有你想让公网请求
**直接进家**（路由器端口映射）时才需要。

## 三种形态怎么选

| 你的宽带 | 推荐方案 |
| --- | --- |
| 没有公网 IP（多数家宽） | frp 穿透（本目录默认方案） |
| 有动态公网 IP | DDNS + 路由器端口映射；或继续用 frp 更省心 |
| 有固定公网 IP（企业宽带） | 直连 + 域名 A 记录写死即可 |

## 安全提醒

- ECS 与旧电脑之间用 frp token + TLS；对外一律走域名 HTTPS
- 旧电脑磁盘建议整盘加密（Windows 用 BitLocker），丢了也不怕
- 定期把 `D:\AuroraDrive\data` 备份到移动硬盘/另一个机器
