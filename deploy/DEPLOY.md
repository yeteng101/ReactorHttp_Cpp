# ReactorHttp-Cpp 生产部署指南（阿里云 ECS）

本文给出两种可用的生产部署方式：

1. **Docker 容器**（推荐，服务器上隔离干净、升级方便）
2. **systemd 直接运行**（二进制 + systemd 托管，无容器开销）

两种方式都可以配合 Caddy 自动 HTTPS。

---

## 0. 本地构建与自测

```bash
cd ReactorHttp-Cpp
make clean
make release          # 生产编译（-O2）
make test             # 回归测试（HTTP 解析、HEAD、Date、配置解析、线程池）
```

本地启动（macOS 用 select，Linux 用 epoll）：

```bash
./build/reactor-http --port 18080 --root ./public --min-workers 2 --max-workers 8
curl -i http://127.0.0.1:18080/health
curl -i http://127.0.0.1:18080/metrics
```

---

## 1. Docker 方式（阿里云 ECS 上推荐）

### 1.1 在服务器上准备目录

```bash
# 上传项目（假设放在 /opt/ReactorHttp-Cpp）
scp -r ReactorHttp-Cpp root@121.196.236.68:/opt/

# 静态文件目录用宿主机挂载，方便不改镜像直接换内容
mkdir -p /opt/ReactorHttp-Cpp/public
```

### 1.2 构建镜像

```bash
cd /opt/ReactorHttp-Cpp
docker build --no-cache -t reactor-http-cpp:2.0 .
```

### 1.3 启动容器

```bash
docker rm -f reactor-http-cpp 2>/dev/null

docker run -d \
  --name reactor-http-cpp \
  --restart unless-stopped \
  --ulimit nofile=200000:200000 \
  -p 10000:10000 \
  -v /opt/ReactorHttp-Cpp/public:/srv/www:ro \
  reactor-http-cpp:2.0
```

验证：

```bash
curl -i http://127.0.0.1:10000/health
curl -i http://127.0.0.1:10000/metrics
```

查看日志：

```bash
docker logs -f --tail 100 reactor-http-cpp
```

### 1.4 优雅重启（不中断太久）

```bash
docker restart reactor-http-cpp
```

容器收到 SIGTERM 后会：先停止接受新连接 → 等待已建立的请求发完（默认最多 10 秒）→ 退出。

---

## 2. systemd 直接运行方式

### 2.1 上传编译产物和静态文件

在**你的 Mac**（或 CI 服务器）上编译，然后上传：

```bash
cd ReactorHttp-Cpp
make clean && make release
scp build/reactor-http root@121.196.236.68:/opt/ReactorHttp-Cpp/build/
scp -r public root@121.196.236.68:/opt/ReactorHttp-Cpp/
```

> 注意：在服务器上直接编译也可以（服务器装 g++ 即可）。
> 因为服务器是 Ubuntu 22.04，直接在服务器上用系统 g++ 编译最省事。

### 2.2 安装 systemd 服务

```bash
# 准备日志目录
mkdir -p /var/log/reactor-http
chown www-data:www-data /var/log/reactor-http

# 安装服务文件
cp deploy/reactor-http.service /etc/systemd/system/reactor-http.service
systemctl daemon-reload
systemctl enable --now reactor-http
systemctl status reactor-http
```

### 2.3 常用运维命令

```bash
systemctl start reactor-http        # 启动
systemctl stop reactor-http         # 停止（走优雅停机）
systemctl restart reactor-http      # 重启
journalctl -u reactor-http -f       # 看日志
```

---

## 3. Caddy 自动 HTTPS（可选，推荐）

Caddy 帮你自动申请/续期 Let's Encrypt 证书，并把 443 的 HTTPS 请求转发到本机 10000 端口。

### 3.1 安装 Caddy

```bash
apt install -y debian-keyring debian-archive-keyring apt-transport-https curl
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' | gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' | tee /etc/apt/sources.list.d/caddy-stable.list
apt update
apt install -y caddy
```

### 3.2 配置反向代理

把 `deploy/Caddyfile` 里的 `your-domain.com` 改成你的域名，然后：

```bash
cp deploy/Caddyfile /etc/caddy/Caddyfile
systemctl reload caddy
```

之后访问 `https://your-domain.com` 即可，Reactor 服务不需要直接暴露公网端口。

### 3.3 阿里云安全组

- 只开放 **80（HTTP）/443（HTTPS）** 给 Caddy；
- **10000 端口不要开放公网**，只让 Caddy 通过 `127.0.0.1:10000` 访问；
- 需要压测时，可以临时放行 10000，压测完删掉。

---

## 4. 完整配置参数

```
--port <n>                          监听端口（默认 10000）
--root <dir>                        静态文件根目录（默认当前目录）
--min-workers <n>                   最小工作线程（默认 2）
--max-workers <n>                   最大工作线程（默认 8）
--max-connections <n>               最大并发连接（默认 10000，超出回 503）
--max-requests-per-connection <n>   单连接最大请求数（默认 1000，超出强制关闭）
--idle-timeout <n>                  keep-alive 空闲超时秒数（默认 30）
--graceful-shutdown <n>             优雅停机最多等待秒数（默认 10）
--access-log <path|- >              访问日志，- 表示 stdout
--error-log <path|- >               错误日志，- 表示 stderr
--help                              帮助
```

也兼容旧式位置参数：`reactor-http 10000 /srv/www 2 8`

---

## 5. 压测建议

压测机不要和服务器共用一台机器（否则 CPU 互相抢）。建议：

1. 先在服务器上用 `curl` 确认接口正常；
2. 从另一台机器（或本地 Mac）执行：

```bash
wrk -t2 -c20 -d10s --latency http://SERVER_IP:10000/index.html     # 预热
wrk -t4 -c100 -d60s --latency http://SERVER_IP:10000/index.html    # 正式
wrk -t8 -c500 -d60s --latency http://SERVER_IP:10000/index.html    # 加压
```

3. 观察三个指标：**Requests/sec**（吞吐）、**Latency 分位**（延迟）、**Socket errors**（失败）。
4. 压测期间用 `/metrics` 看 `active_connections` 和 `rejected_connections`，确认连接上限是否生效。

---

## 6. 常见问题

**Q: 为什么旧版本 curl 报错、wrk 全是 read error？**
A: 旧版响应没有正确返回 `Connection` 头，keep-alive 行为不对。新版已修复，并按 HTTP 规范返回 `Connection: keep-alive/close` 和 `Keep-Alive` 头。

**Q: 大文件会卡住服务器吗？**
A: 不会。静态文件在 Linux 上用 `sendfile` 零拷贝流式发送，一次最多发 1MB，发送缓冲满就等下一次写事件，不阻塞事件循环。

**Q: 怎么做到不中断地更新文件？**
A: 静态文件放到宿主机 `/opt/ReactorHttp-Cpp/public` 并挂载进容器，直接替换文件即可，无需重启。改代码才需要重新构建镜像。
