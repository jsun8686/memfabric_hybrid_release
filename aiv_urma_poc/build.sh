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

# ── 1. AIV 内核（AscendC 编译，取最终 merge 的设备镜像 .o） ──
# ASC_DEVKIT_ROOT：asc-devkit 仓位置（提供 AscendC::Hcomm），默认 ~/asc-devkit
cmake -S "${SCRIPT_DIR}/kernel" -B "${BUILD_DIR}/kernel" \
    -DASCEND_HOME_PATH="${ASCEND_HOME_PATH}" \
    -DASC_DEVKIT_ROOT="${ASC_DEVKIT_ROOT:-$HOME/asc-devkit}" \
    -DCMAKE_BUILD_TYPE=RELEASE
cmake --build "${BUILD_DIR}/kernel" -j

# 设备镜像说明：ascendc 工具链最终产物是 <name>_merge_obj_dir/device_aiv.o
# （ld.lld -static 合成的完整设备对象，供 aclrtBinaryLoadFromFile 加载）；
# .a 里的 poc_hcomm_kernel.cpp.o 只是 host 侧编译中间产物，不能用于加载。
KERNEL_OBJ="$(find "${BUILD_DIR}/kernel" -path '*_merge_obj_dir/device_aiv.o' | head -1)"
if [ -z "${KERNEL_OBJ}" ]; then
    echo "[POC][WARN] merge_obj device_aiv.o not found, fallback to aiv_device_dir"
    KERNEL_OBJ="$(find "${BUILD_DIR}/kernel" -path '*_aiv_device_dir/device_aiv.o' | head -1)"
fi
if [ -z "${KERNEL_OBJ}" ]; then
    echo "[POC][ERROR] device_aiv.o not found under ${BUILD_DIR}/kernel"
    exit 1
fi
# 重命名为 poc_hcomm_kernel.o，与 run_*.sh 的 find 及 --kernel-o 默认路径保持一致
cp -f "${KERNEL_OBJ}" "${BUILD_DIR}/kernel/poc_hcomm_kernel.o"
KERNEL_OBJ="${BUILD_DIR}/kernel/poc_hcomm_kernel.o"
echo "[POC] kernel object: ${KERNEL_OBJ} (from $(find "${BUILD_DIR}/kernel" -path '*_merge_obj_dir/device_aiv.o' | head -1))"

# ── 2. Host 可执行 ──
cmake -S "${SCRIPT_DIR}/host" -B "${BUILD_DIR}/host" -DCMAKE_BUILD_TYPE=RELEASE
cmake --build "${BUILD_DIR}/host" -j

echo "[POC] build done:"
echo "  host : ${BUILD_DIR}/host/poc_aiv_urma"
echo "  kernel .o : ${KERNEL_OBJ} (拷贝到运行目录或用 --kernel-o 指定)"
