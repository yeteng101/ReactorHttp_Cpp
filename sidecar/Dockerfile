FROM python:3.12-slim

WORKDIR /app
COPY requirements.txt ./
RUN pip install --no-cache-dir -r requirements.txt
COPY bridge.py ./

# /cfg 放配置（compose 挂载命名卷，AI 配置页可直接写回）
RUN mkdir -p /cfg && chmod 1777 /cfg
EXPOSE 18666
CMD ["python", "-u", "bridge.py", "--config", "/cfg/config.json"]
