#!/bin/bash
# AIV+URMA PoC 构建脚本（在 A5 环境执行）
# 前置：source CANN 环境变量（set_env.sh），ASCEND_HOME_PATH 生效
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

if [ -z "${ASCEND_HOME_PATH}" ]; then
    echo "[POC][ERROR] ASCEND_HOME_PATH not set, please source CANN set_env.sh first"
    exit 1
fi
echo "[POC] ASCEND_HOME_PATH = ${ASCEND_HOME_PATH}"

mkdir -p "${BUILD_DIR}/kernel" "${BUILD_DIR}/host"

# ── 1. AIV 内核（AscendC -> 静态库 -> 解包出 .o） ──
# ASC_DEVKIT_ROOT：asc-devkit 仓位置（提供 AscendC::Hcomm），默认 ~/asc-devkit
cmake -S "${SCRIPT_DIR}/kernel" -B "${BUILD_DIR}/kernel" \
    -DASCEND_HOME_PATH="${ASCEND_HOME_PATH}" \
    -DASC_DEVKIT_ROOT="${ASC_DEVKIT_ROOT:-$HOME/asc-devkit}" \
    -DCMAKE_BUILD_TYPE=RELEASE
cmake --build "${BUILD_DIR}/kernel" -j

KERNEL_A="$(find "${BUILD_DIR}/kernel" -name 'libpoc_aiv_hcomm_kernel.a' | head -1)"
if [ -z "${KERNEL_A}" ]; then
    echo "[POC][ERROR] kernel static lib not found"
    exit 1
fi
pushd "${BUILD_DIR}/kernel" > /dev/null
ar x "${KERNEL_A}"
popd > /dev/null
KERNEL_OBJ="$(find "${BUILD_DIR}/kernel" -name 'poc_hcomm_kernel.o' | head -1)"
if [ -z "${KERNEL_OBJ}" ]; then
    echo "[POC][ERROR] poc_hcomm_kernel.o not extracted from ${KERNEL_A}"
    exit 1
fi
echo "[POC] kernel object: ${KERNEL_OBJ}"

# ── 2. Host 可执行 ──
cmake -S "${SCRIPT_DIR}/host" -B "${BUILD_DIR}/host" -DCMAKE_BUILD_TYPE=RELEASE
cmake --build "${BUILD_DIR}/host" -j

echo "[POC] build done:"
echo "  host : ${BUILD_DIR}/host/poc_aiv_urma"
echo "  kernel .o : ${KERNEL_OBJ} (拷贝到运行目录或用 --kernel-o 指定)"
