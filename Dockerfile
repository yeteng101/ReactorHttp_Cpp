# ---------- 构建阶段 ----------
FROM gcc:14-bookworm AS build
WORKDIR /src
COPY . .
RUN make release

# ---------- 运行阶段 ----------
FROM debian:bookworm-slim AS runtime

# 创建非 root 运行用户
RUN groupadd --system app && useradd --system --gid app --home-dir /app --no-create-home app

WORKDIR /app
COPY --from=build /src/build/reactor-http /app/reactor-http

# gcc:14 编译的二进制依赖较新的 libstdc++/libgcc，从构建镜像复制到运行时镜像
COPY --from=build /usr/lib/x86_64-linux-gnu/libstdc++.so.6* /usr/lib/x86_64-linux-gnu/
COPY --from=build /usr/lib/x86_64-linux-gnu/libgcc_s.so.1 /usr/lib/x86_64-linux-gnu/

# 静态文件目录
COPY public /srv/www
RUN chown -R app:app /srv/www && chmod -R a+rX /srv/www

# 网盘数据与账号文件挂载点（docker-compose/卷 挂到这两个目录）
RUN mkdir -p /data /etc/reactor-http && chown -R app:app /data /etc/reactor-http

USER app
EXPOSE 18080

VOLUME ["/data", "/etc/reactor-http"]

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
  CMD ["bash", "-c", "exec 3<>/dev/tcp/127.0.0.1/18080 && printf 'GET /health HTTP/1.1\\r\\nHost: localhost\\r\\nConnection: close\\r\\n\\r\\n' >&3 && grep -q '200 OK' <&3"]

CMD ["/app/reactor-http", "--port", "18080", "--root", "/srv/www", "--min-workers", "2", "--max-workers", "8", "--drive-root", "/data", "--users-file", "/etc/reactor-http/users.conf"]
