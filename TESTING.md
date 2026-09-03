# C++ Reactor HTTP 运行与压测

## 本地运行

Linux 会使用 `epoll`，macOS 会回退到 `select`，便于本地功能验证。

```bash
cd ReactorHttp-Cpp
make release
./build/reactor-http --port 18080 --root ./public --min-workers 2 --max-workers 8
```

（旧式位置参数 `./build/reactor-http 18080 ./public 2 8` 仍然兼容。）

可用参数见 `deploy/DEPLOY.md` 第 4 节。

另开终端验证：

```bash
curl -i http://127.0.0.1:18080/
curl -i http://127.0.0.1:18080/index.html
curl -i -H "Range: bytes=0-1023" http://127.0.0.1:18080/index.html
curl -i -X OPTIONS http://127.0.0.1:18080/health
curl -X PATCH -H "Upload-Offset: 0" --data-binary @part1 http://127.0.0.1:18080/api/files/upload.bin
curl -X DELETE http://127.0.0.1:18080/api/files/upload.bin
curl -i http://127.0.0.1:18080/health
curl -i http://127.0.0.1:18080/metrics
curl -i http://127.0.0.1:18080/api/files
```

接口用途：

- `/health`：容器健康检查，正常时返回 `{"status":"ok"}`
- `/metrics`：运行时长、请求数、活跃连接数和错误数
- `/api/files`：以 JSON 列出静态根目录第一层文件
- `PATCH /api/files/<name>`：分片续传写文件，用 `Upload-Offset` 指定写入位置，成功返回 `200` JSON 和新文件大小
- `DELETE /api/files/<name>`：删除静态根目录下的文件，成功返回 `204`，不存在返回 `404`

HTTP/1.1 默认使用 Keep-Alive，HTTP/1.0 默认使用短连接；请求可通过 `Connection: close` 主动关闭。连接空闲 30 秒后会被自动清理，单次读取最多处理 16 个流水线请求，避免单个连接长时间占用 Worker。

动态线程池的策略：新连接优先分配给当前连接数最少的 Worker；单个 Worker 连接数达到约 64 时扩容，最多扩容到最大值；低负载持续 30 秒后标记一个 Worker 为 draining，连接全部关闭后再回收，最少保留最小值。

`/metrics` 返回的 `worker_threads` 、`min_worker_threads` 、`max_worker_threads` 、`draining_worker_threads` 、`idle_timeouts` 和 `rejected_connections` 可用来观察扩缩容、超时清理与连接上限拒绝情况。

生产特性：

- Linux 上用 `sendfile` 零拷贝流式发送静态文件，大文件不占用事件线程
- HEAD 请求、`Date` 响应头、正确的 `Content-Length`/`Connection`/`Keep-Alive`
- MIME 类型：常用扩展名映射；未知扩展名时读取文件头魔数推测，回退 `application/octet-stream`
- Range 断点续传：`Accept-Ranges: bytes`，支持 `bytes=start-end`、`bytes=start-`、`bytes=-suffix`，返回 `206 Partial Content`，越界范围返回 `416 Range Not Satisfiable`
- OPTIONS/CORS：预检返回 `204 No Content`，带 `Allow` 与 `Access-Control-Allow-*` 头并回显请求头；普通响应也带 `Access-Control-Allow-Origin: *`
- 请求体：支持按 `Content-Length` 读取正文（上限 64MB），`Transfer-Encoding` 暂不支持
- 文件操作：`PATCH` 分片续传支持幂等重试（重复分片直接返回当前大小），偏移不匹配返回 `409`；`DELETE` 删除文件
- `--max-connections` 连接上限（超出回 503）、`--max-requests-per-connection` 单连接请求上限
- 访问日志（`--access-log`）与错误日志（`--error-log`）
- SIGTERM/SIGINT 优雅停机：先停止 accept，等待已有连接排空，超时后强制关闭

不启动网络服务也可运行请求解析和路径安全回归测试：

```bash
make test
```

也可以使用 Docker：

```bash
docker build -t reactor-http-cpp .
docker run --rm -p 18080:10000 reactor-http-cpp
```

最后两个参数是工作线程最小值和最大值。例如 `2 8` 表示启动时先创建 2 个 Worker，连接较多时最多扩容到 8 个。只传一个值时保持固定线程模式，例如 `./build/reactor-http 18080 ./public 4` 仍是固定 4 个 Worker。

这里使用 `18080` 是因为当前 Mac 的 `127.0.0.1:10000` 可能已被其他程序占用。

## 并发压力测试

压测工具应在另一台机器运行，否则服务端与压测端会争抢同一台机器的 CPU。先逐级加压，并记录吞吐、失败数和延迟，不要一开始就使用很高并发。

```bash
# 预热：20 并发，10 秒
wrk -t2 -c20 -d10s --latency http://SERVER_IP:10000/index.html

# 正式测试：100/500/1000 并发，各 60 秒
wrk -t4 -c100  -d60s --latency http://SERVER_IP:10000/index.html
wrk -t8 -c500  -d60s --latency http://SERVER_IP:10000/index.html
wrk -t8 -c1000 -d60s --latency http://SERVER_IP:10000/index.html
```

这些命令使用 HTTP/1.1 Keep-Alive，适合观察连接复用后的吞吐。若要测短连接额外开销，可显式添加 `-H "Connection: close"`，并单独记录结果。

没有 `wrk` 时可以先用 ApacheBench：

```bash
ab -n 100000 -c 100 http://SERVER_IP:10000/index.html
```

测试时在服务端同时观察：

```bash
top
ss -s
pidstat -p $(pgrep -n reactor-http) 1
```

主要看 `Requests/sec`、非 2xx/3xx 响应、socket 错误，以及 p50/p90/p99 延迟。每档至少重复三次，取中位数。

## 阿里云 ECS 测试

建议准备两台同地域、同 VPC 的按量付费 ECS：一台运行服务，一台运行 `wrk`。用私网 IP 压测可以排除公网带宽和公网抖动；若目标就是测试公网入口，再单独进行公网测试。

### 使用 Docker 部署服务端

以下命令适用于当前 ECS 目录 `/opt/ReactorHttp-Cpp`、容器名 `reactor-http-cpp`和端口 `10000`。直接在 ECS 上构建，可以生成与实例 CPU 架构匹配的镜像。

```bash
sudo apt update
sudo apt install -y docker.io git
sudo systemctl enable --now docker

git clone YOUR_REPOSITORY_URL
cd YOUR_REPOSITORY/ReactorHttp-Cpp
sudo docker build -t reactor-http-cpp:2.0 .
sudo docker run -d \
  --name reactor-http-cpp \
  --restart unless-stopped \
  --ulimit nofile=200000:200000 \
  -p 10000:10000 \
  -v /opt/ReactorHttp-Cpp/public:/srv/www:ro \
  reactor-http-cpp:2.0

curl -i http://127.0.0.1:10000/index.html
curl -i http://127.0.0.1:10000/health
sudo docker logs reactor-http-cpp
```

从 Mac 更新到当前阿里云 ECS：

```bash
# 在 Mac 上执行
scp -r "/Users/andye/代码/ReactorHttp-Cpp" root@121.196.236.68:/opt/

# SSH 登录 ECS 后执行
cd /opt/ReactorHttp-Cpp
make test
docker build --no-cache -t reactor-http-cpp:2.0 .
docker rm -f reactor-http-cpp
docker run -d \
  --name reactor-http-cpp \
  --restart unless-stopped \
  --ulimit nofile=200000:200000 \
  -p 10000:10000 \
  -v /opt/ReactorHttp-Cpp/public:/srv/www:ro \
  reactor-http-cpp:2.0
```

`docker rm -f` 删除旧容器，不会删除旧镜像或宿主机的 `public` 目录。确认新容器正常：

```bash
docker ps
docker inspect --format '{{.State.Health.Status}}' reactor-http-cpp
docker logs --tail 100 reactor-http-cpp
curl -i http://127.0.0.1:10000/health
curl -s http://127.0.0.1:10000/metrics
curl -s http://127.0.0.1:10000/api/files
```

服务器本机正常后，再在 Mac 访问 `http://121.196.236.68:10000/`。如果外网打不开，检查阿里云安全组是否只允许你当前公网 IP 访问 TCP `10000`。

### 不使用 Docker

服务端（Ubuntu 24.04 示例）：

```bash
sudo apt update
sudo apt install -y build-essential git
git clone YOUR_REPOSITORY_URL
cd YOUR_REPOSITORY/ReactorHttp-Cpp
make release
./build/reactor-http 10000 ./public 4
```

压测端：

```bash
sudo apt update
sudo apt install -y wrk
wrk -t8 -c500 -d60s --latency http://SERVER_PRIVATE_IP:10000/index.html
```

安全组只需允许压测机的私网 IP 访问服务端 TCP `10000`。不要向 `0.0.0.0/0` 暴露压测端口。测试前确认两台 ECS 的实例规格、系统镜像和服务端线程数并记录在结果中。

这个项目已经加入基本路径隔离、请求限制和健康检查，但仍属于教学用 HTTP 服务器。它还没有 TLS、鉴权、限流、超时管理、日志轮转和完整的可观测性，不建议直接替代 Nginx 承载真实业务。

高并发前可临时提高文件描述符上限：

```bash
ulimit -n 200000
```

当单台压测机 CPU 或网卡先打满时，应增加压测机，或者使用阿里云 PTS 创建 HTTP GET 场景，并将目标 URL 设为 `/index.html`。PTS 适合多地域、公网链路和分布式发压；同 VPC 的两台 ECS 更适合测服务器本身的极限。
