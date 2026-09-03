# ReactorHttp-Cpp 运行、网盘接口与压测

## 1. 本地运行（网盘模式）

Linux 使用 `epoll`，macOS 回退 `select`，便于本地验证。

```bash
cd ReactorHttp-Cpp
make release

# 首次先创建账号（users.conf 只保存加盐 SHA-256 散列，权限 0600）
./build/reactor-http --drive-root ./data --users-file ./users.conf \
  --add-user author:test123456

# 启动网盘服务
./build/reactor-http --port 18080 --root ./public \
  --min-workers 2 --max-workers 8 \
  --drive-root ./data --users-file ./users.conf
```

浏览器打开 <http://127.0.0.1:18080> 登录后即可上传/下载/预览。
旧式位置参数 `./build/reactor-http 18080 ./public 2 8` 仍兼容（静态服务器模式）。
更多参数与部署见 `deploy/DEPLOY.md`。

## 2. 基础接口

```bash
curl -i http://127.0.0.1:18080/health            # {"status":"ok"}
curl -i http://127.0.0.1:18080/metrics           # 进程指标 JSON
curl -i -H "Range: bytes=0-1023" http://127.0.0.1:18080/index.html
```

静态文件特性：

- `/health`、`/metrics` 健康检查与指标（只允许 GET/HEAD）
- MIME 扩展名映射，未知类型读文件头魔数推测，回退 `application/octet-stream`
- Range 断点续传：`bytes=start-end`、`start-`、`-suffix`，返回 `206`；越界返回 `416`
- HEAD/Date/Content-Length/Keep-Alive 齐全；OPTIONS 预检返回 `204` + CORS 头
- 请求体按 Content-Length 读取，上限 64MB，暂不支持 Transfer-Encoding
- `PATCH /api/files/<name>` 分片续传、`DELETE /api/files/<name>` 删除（静态目录，公开模式）
- sendfile（Linux）流式发送大文件；SIGTERM/SIGINT 优雅停机

## 3. 网盘接口（`--drive-root` 开启后）

```bash
# 登录：返回 token，Set-Cookie 写入 sid
curl -c cookies.txt -H 'Content-Type: application/json' \
  -d '{"username":"author","password":"test123456"}' \
  http://127.0.0.1:18080/api/login

curl -b cookies.txt http://127.0.0.1:18080/api/me
curl -b cookies.txt http://127.0.0.1:18080/api/drive/list                 # 当前用户根目录
curl -b cookies.txt -X POST 'http://127.0.0.1:18080/api/drive/mkdir?path=docs'

# 分片续传：第一片 offset=0，之后按返回的 size 继续
printf 'hello' | curl -b cookies.txt -X PATCH -H 'Upload-Offset: 0' \
  --data-binary @- 'http://127.0.0.1:18080/api/drive/file/docs/a.txt'
printf ' world' | curl -b cookies.txt -X PATCH -H 'Upload-Offset: 5' \
  --data-binary @- 'http://127.0.0.1:18080/api/drive/file/docs/a.txt'

# 覆盖更新 / 下载 / Range 在线播放 / 重命名 / 删除
curl -b cookies.txt -X PATCH -H 'Upload-Offset: 0' -H 'X-Overwrite: 1' \
  --data-binary @new 'http://127.0.0.1:18080/api/drive/file/docs/a.txt'
curl -b cookies.txt -OJ 'http://127.0.0.1:18080/api/drive/download?path=docs/a.txt'
curl -b cookies.txt -i -H 'Range: bytes=0-3' \
  'http://127.0.0.1:18080/api/drive/stream?path=docs/a.txt'
curl -b cookies.txt -X POST \
  'http://127.0.0.1:18080/api/drive/rename?from=docs/a.txt&to=docs/b.txt'
curl -b cookies.txt -X DELETE 'http://127.0.0.1:18080/api/drive/remove?path=docs/b.txt'
```

接口一览：

| 方法与路径 | 说明 | 鉴权 |
| --- | --- | --- |
| `POST /api/login` | 用户名密码登录（form/JSON） | 公开 |
| `POST /api/logout` | 注销当前会话 | 登录 |
| `GET /api/me` | 返回当前用户名 | 登录 |
| `GET /api/drive/list?path=` | 列目录（含磁盘空间） | 登录 |
| `GET /api/drive/stat?path=` | 查文件大小/类型 | 登录 |
| `POST /api/drive/mkdir?path=` | 建目录（可多级） | 登录 |
| `PATCH /api/drive/file/<rel>` | 分片上传（`Upload-Offset`；`X-Overwrite: 1` 覆盖） | 登录 |
| `DELETE /api/drive/remove?path=` | 删除文件或整棵目录 | 登录 |
| `POST /api/drive/rename?from=&to=` | 重命名/移动 | 登录 |
| `GET /api/drive/download?path=` | 下载（attachment） | 登录 |
| `GET /api/drive/stream?path=` | 在线预览/播放，支持 Range | 登录 |

会话可通过 `Cookie: sid=`、`Authorization: Bearer` 或 `?token=` 传递。
每个用户只能访问 `drive-root/<用户名>/` 下的文件，路径穿越（`..`、绝对路径）返回 403。

## 4. 回归测试

```bash
make test
```

依次运行：

- `build/http-request-test`：HTTP 解析、方法白名单、路径安全、HEAD/Date、Range、PATCH/DELETE
- `build/drive-test`：登录鉴权、401、家目录隔离、分片续传、覆盖更新、Range、递归删除、注销

## 5. 并发压力测试

压测机应与服务端分开，避免 CPU 相互干扰。逐级加压并记录吞吐、延迟与错误。

```bash
wrk -t4 -c64  -d30s --latency http://SERVER:18080/index.html
wrk -t8 -c300 -d60s --latency http://SERVER:18080/index.html
wrk -t8 -c1000 -d60s --latency http://SERVER:18080/index.html
```

观察服务端：

```bash
top
docker stats            # 容器 CPU/内存
curl -s http://127.0.0.1:18080/metrics
```

`/metrics` 返回 `worker_threads`、`draining_worker_threads`、`idle_timeouts`、
`rejected_connections` 等字段，用于观察动态线程池与连接上限。

要点：

- 每档至少重复 3 次取中位数，重点看 Requests/sec、p50/p90/p99、socket errors
- 公网压测先确认带宽；测服务本身用同地域私网/内网 IP
- 压力打满单台压测机时用多台或阿里云 PTS
- 大文件下载压测时 `--root` 指向含大文件的目录，验证 sendfile 流式路径
- 网盘登录鉴权压测用 `GET /api/drive/list`（携带 Cookie），可测会话查询并发

## 6. Docker 运行示例

```bash
docker build -t fuji-netdisk:3.0 .

# 首次建号（挂载卷，users.conf 会持久化）
docker run --rm \
  -v fuji-netdisk-data:/data -v fuji-netdisk-users:/etc/reactor-http \
  fuji-netdisk:3.0 --add-user author:test123456 \
  --users-file /etc/reactor-http/users.conf --drive-root /data

# 正式运行（容器内 18080 映射到本机 18080）
docker run -d --name fuji-netdisk --restart unless-stopped \
  -p 18080:18080 \
  -v fuji-netdisk-data:/data -v fuji-netdisk-users:/etc/reactor-http \
  fuji-netdisk:3.0
```

生产（域名 + HTTPS）推荐直接使用 `deploy/docker-compose.yml` + Caddy，见 `deploy/DEPLOY.md`。
