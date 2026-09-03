# DDNS 是什么、什么时候需要

## 背景

域名系统（DNS）把 `pan.example.com` 解析成 IP。普通家庭宽带的公网 IP 往往是
**动态的**：运营商每次拨号（几天到几个月一次）都会换 IP。如果域名 A 记录一直写死
旧 IP，网络一重拨号域名就失效。

DDNS（Dynamic DNS，动态域名解析）就是**一个定时任务**：每隔几分钟检测本机当前公网
IP，变了就自动调用云解析 API 修改 A 记录，让域名永远指向家里最新 IP。

## 先判断：你家宽带到底有没有公网 IP？

两种方法互相印证：

1. 登录光猫/路由器，看 **WAN 口 IP**；
2. 电脑打开 <https://myip.ipip.net> 看出口 IP。

两个 IP 一致 → 有公网 IP，可以用 DDNS + 路由器端口映射。
两个 IP 不一致（路由器 WAN 是 `100.64.x.x`、`10.x.x.x`、`172.16.x.x` 等私有段）→
你在运营商大内网（CGNAT）里，**DDNS 没有用**，必须用 frp 内网穿透（推荐本仓库方案）。

打运营商客服（电信 10000 / 联通 10010 / 移动 10086）说：
“我家宽带需要公网 IP，用来远程访问家里的服务器”，多数地区可免费申请。

## 用 ddns-go 自动更新（阿里云示例）

1. 阿里云控制台 → RAM 访问控制 → 创建用户，勾选“编程访问”，生成 AccessKey；
   给该用户授权策略 `AliyunDNSFullAccess`（只给 DNS 权限，别用主账号 Key）。
2. 运行：

```bash
cd deploy/home-server/ddns
docker compose -f docker-compose.ddns-go.yml up -d
```

3. 打开 `http://127.0.0.1:9876`，填：

   - 服务商：阿里云
   - AccessKeyId / AccessKey Secret：上面创建的
   - IPv4 来源：通过接口获取（默认即可）
   - 域名：`pan.example.com`
   - 同步间隔：5 分钟

4. 保存后它会在后台自动更新 A 记录。

### 其它常见 DDNS 渠道

- 路由器自带 DDNS（老毛子/OpenWrt/华硕/TP-Link 高端款）：填阿里云 Key 即用
- 群晖/威联通自带 DDNS
- 花生壳/蒲公英：适合不想折腾域名的人，会给你一个 `xxx.oicp.net` 之类域名

> 使用 frp 穿透方案时（frpc 主动连 ECS），**家里 IP 怎么变都不影响**，DDNS 可以完全不装；
> 只有“家里有公网 IP、想直连”时才需要 DDNS。
