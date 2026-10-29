#!/bin/bash
# 服务器端（数据接收/被拉取方）示例：在 A5 服务器上执行
# 用法: ./run_server.sh [device_id]
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEVICE="${1:-0}"

"${DIR}/build/host/poc_aiv_urma" \
    --role server \
    --device "${DEVICE}" \
    --verify write \
    --kernel-o "$(find "${DIR}/build/kernel" -name 'poc_hcomm_kernel.o' | head -1)"
