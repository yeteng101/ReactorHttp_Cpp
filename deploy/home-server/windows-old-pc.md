# Windows 旧电脑部署（主推荐路线）

目标拓扑：

```text
手机/电脑 ──https──> 你的域名 ──> 阿里云 ECS(Caddy+frps)
                                       │  frp 隧道(旧电脑主动连出)
旧电脑(Docker) ── netdisk 容器:10000 ─┘
文件实际存在旧电脑磁盘 D:\AuroraDrive\data
```

旧电脑**不需要任何入站端口、不需要公网 IP**，只需要能上网。

## 第 0 步：确认旧电脑能跑 Docker

- 系统：Windows 10/11 64 位（或 Windows Server 2019+）
- CPU 需要支持并开启虚拟化（任务管理器 → 性能 → CPU → 虚拟化：已启用）
- 没开启的去 BIOS 打开 Intel VT-x / AMD-V
- BIOS 里没这个选项或电脑太老 → 改用 [linux-old-pc.md](linux-old-pc.md)

安装：

1. [Docker Desktop](https://www.docker.com/products/docker-desktop/) 安装包，装完重启
2. 设置 → General → 勾选 “Use the WSL 2 based engine”
3. PowerShell 验证：`docker version` 和 `docker compose version` 都正常

## 第 1 步：把代码放到旧电脑

安装 [Git for Windows](https://git-scm.com/download/win)，然后：

```powershell
mkdir D:\AuroraDrive
cd D:\AuroraDrive
git clone git@github.com:yeteng101/ReactorHttp_Cpp.git
cd ReactorHttp_Cpp
git checkout feature/home-server
cd deploy/home-server
```

## 第 2 步：准备数据目录和配置

```powershell
# 建两个文件夹：data 存网盘文件，conf 存账号
mkdir D:\AuroraDrive\data
mkdir D:\AuroraDrive\conf

# 生成网盘侧配置
Copy-Item .env.home.example .env
notepad .env        # 确认 NETDISK_DATA_DIR=D:/AuroraDrive/data

# 修改 frp 隧道配置
notepad frpc-home.toml
#  - serverAddr 改成你 ECS 公网 IP
#  - auth.token 改成和 ECS 上 frps.toml 一样的随机串
#  - customDomains 改成你的域名
```

## 第 3 步：创建账号并启动

```powershell
# 构建镜像 + 创建作者账号（只在首次和换密码时需要）
docker compose -f docker-compose.home.yml build netdisk
docker compose -f docker-compose.home.yml run --rm netdisk `
  /app/reactor-http --add-user author:你的强密码 `
  --users-file /etc/reactor-http/users.conf --drive-root /data

# 正式启动
docker compose -f docker-compose.home.yml up -d
docker compose -f docker-compose.home.yml ps
```

旧电脑本机浏览器打开 <http://127.0.0.1:10000> 登录测试；文件应出现在
`D:\AuroraDrive\data\author\` 下。

## 第 4 步：开机自启（无人值守）

1. Docker Desktop 设置 → General → 勾选 “Start Docker Desktop when you sign in”
2. `Win+R` 输入 `shell:startup` 回车，把 [start-netdisk.bat](start-netdisk.bat) 的
   快捷方式放进去

以后旧电脑开机 → Docker 自动启动 → 脚本自动拉起网盘和 frpc，全程不用碰它。

## 第 5 步：验收外网

在 ECS 上（见 [README.md](README.md) 的中继步骤）确认 frps + Caddy 已启动后，
从手机 4G/5G 访问 `https://你的域名`。

若打不开，按顺序查：

```powershell
docker compose -f docker-compose.home.yml logs -f frpc     # 隧道是否连接成功
docker compose -f docker-compose.home.yml logs -f netdisk  # 网盘是否正常
```

frpc 日志出现 `start proxy success` 说明隧道通；再在 ECS 上看 Caddy 日志。

## 常见坑

- **端口冲突**：Windows 上 10000 被占时，把 compose 里两个 `10000` 和
  frpc-home.toml 的 localPort 一起改。
- **镜像拉不下来**：Docker Desktop 设置里换国内镜像源，或科学上网后重试。
- **数据目录共享失败**：`.env` 里必须用正斜杠 `D:/AuroraDrive/data`，不要用 `D:\`。
- **杀毒软件拦截 frpc**：把 frpc 相关目录加白名单（它是主动外连的合法程序）。
- **合上笔记本盖子断网**：Windows 电源设置里把“合盖”改为“不采取任何操作”，
  并设置从不睡眠（服务器没有休息）。
