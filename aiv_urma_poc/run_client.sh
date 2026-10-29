#!/bin/bash
# 客户端（数据发起方）示例：在另一台 A5 服务器上执行
# 用法: ./run_client.sh <server_ip> [device_id]
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PEER_IP="${1:?usage: run_client.sh <server_ip> [device_id]}"
DEVICE="${2:-0}"

"${DIR}/build/host/poc_aiv_urma" \
    --role client \
    --peer-ip "${PEER_IP}" \
    --device "${DEVICE}" \
    --verify write \
    --kernel-o "$(find "${DIR}/build/kernel" -name 'poc_hcomm_kernel.o' | head -1)"
