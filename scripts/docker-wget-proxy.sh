#!/usr/bin/env bash
# 容器内执行：配置 wget 全局代理后执行传入的命令
# 用法: bash /host-project/scripts/docker-wget-proxy.sh '命令'
set -euo pipefail

PROXY_HOST="${PROXY_HOST:-host.docker.internal}"
PROXY_PORT="${PROXY_PORT:-7897}"

cat > /etc/wgetrc <<EOF
use_proxy = yes
http_proxy = http://${PROXY_HOST}:${PROXY_PORT}
https_proxy = http://${PROXY_HOST}:${PROXY_PORT}
EOF

echo "wget proxy: ${PROXY_HOST}:${PROXY_PORT}"

exec bash -lc "$1"
