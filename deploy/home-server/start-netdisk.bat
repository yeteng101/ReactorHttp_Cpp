@echo off
rem 旧电脑一键启动（双击，或放到「启动」文件夹实现开机自启）
cd /d "%~dp0"

if not exist ".env" copy ".env.home.example" ".env" >nul
if not exist "home-data" mkdir "home-data"
if not exist "home-users" mkdir "home-users"

echo [1/2] 构建网盘镜像...
docker compose -f docker-compose.home.yml build netdisk

echo [2/2] 启动网盘与隧道...
docker compose -f docker-compose.home.yml up -d
docker compose -f docker-compose.home.yml ps
echo.
echo 本机访问: http://127.0.0.1:10000
echo 外网访问: 域名或 ECS IP（frp 配置正确时）
pause
