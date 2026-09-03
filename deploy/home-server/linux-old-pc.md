# Linux 旧电脑部署（硬件太老跑不动 Docker Desktop 时）

把旧电脑直接装成 Linux 服务器是最“硬件服务器”的做法，2GB 内存的古董机也能流畅跑。
推荐 Ubuntu Server 24.04 LTS（安装时选 SSH Server；整盘安装会清空 Windows，
想保留系统就装双系统）。

## 第 1 步：安装依赖并把代码拉下来

```bash
sudo apt update
sudo apt install -y git build-essential

sudo mkdir -p /srv/auroradrive
cd /srv/auroradrive
git clone git@github.com:yeteng101/ReactorHttp_Cpp.git
cd ReactorHttp_Cpp
git checkout feature/home-server
make release
```

## 第 2 步：创建数据目录、账号、systemd 服务

```bash
cd /srv/auroradrive/ReactorHttp_Cpp

sudo useradd --system --home /srv/auroradrive --no-create-home netdisk
sudo mkdir -p /srv/auroradrive/data /srv/auroradrive/conf /var/log/reactor-http
sudo chown -R netdisk:netdisk /srv/auroradrive /var/log/reactor-http

sudo -u netdisk ./build/reactor-http \
  --drive-root /srv/auroradrive/data \
  --users-file /srv/auroradrive/conf/users.conf \
  --add-user author:你的强密码

# 用仓库里与本文档目录约定一致的专用 unit（路径不同就先 nano 修改）
sudo cp deploy/home-server/reactor-http-home.service /etc/systemd/system/reactor-http.service
sudo systemctl daemon-reload
sudo systemctl enable --now reactor-http

curl -i http://127.0.0.1:10000/health
```

文件实际存在 `/srv/auroradrive/data/<用户名>/`。

## 第 3 步：装 frpc（frp 0.61.1 与 ECS 端一致）

```bash
cd /tmp
wget https://github.com/fatedier/frp/releases/download/v0.61.1/frp_0.61.1_linux_amd64.tar.gz
tar xzf frp_0.61.1_linux_amd64.tar.gz
sudo cp frp_0.61.1_linux_amd64/frpc /usr/local/bin/

sudo mkdir -p /etc/frp
sudo cp /srv/auroradrive/ReactorHttp_Cpp/deploy/home-server/frpc-home.toml /etc/frp/frpc.toml
sudo nano /etc/frp/frpc.toml        # 改 ECS IP / token / 域名
```

新建 systemd 服务：

```bash
sudo tee /etc/systemd/system/frpc.service >/dev/null <<'EOF'
[Unit]
Description=frp client tunnel to public relay
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=/usr/local/bin/frpc -c /etc/frp/frpc.toml
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable --now frpc
systemctl status frpc --no-pager
```

看到 `start proxy success` 即隧道已通。

> x86 32 位古董机把下载包换成 `linux_386`；ARM 机器换成 `linux_arm64`。
