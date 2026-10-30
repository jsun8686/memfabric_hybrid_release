#!/bin/bash
# AIV+URMA PoC 构建脚本（在 A5 环境执行）
# ASC 一体化构建：bisheng 单次编译 host+kernel（设备镜像内嵌可执行，无需单独 .o）
# 前置：source CANN 环境变量（set_env.sh），ASCEND_HOME_PATH 生效
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build/poc"

if [ -z "${ASCEND_HOME_PATH}" ]; then
    echo "[POC][ERROR] ASCEND_HOME_PATH not set, please source CANN set_env.sh first"
    exit 1
fi
echo "[POC] ASCEND_HOME_PATH = ${ASCEND_HOME_PATH}"

# 全量清建：PoC 体量小，且 ASC 增量行为未验证
rm -rf "${BUILD_DIR}"

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
    -DASC_DEVKIT_ROOT="${ASC_DEVKIT_ROOT:-$HOME/asc-devkit}" \
    -DCMAKE_BUILD_TYPE=RELEASE
cmake --build "${BUILD_DIR}" -j"$(nproc)"

BIN="${BUILD_DIR}/poc_aiv_urma"
if [ ! -f "${BIN}" ]; then
    echo "[POC][ERROR] build output not found: ${BIN}"
    exit 1
fi
echo "[POC] build done: ${BIN}"
