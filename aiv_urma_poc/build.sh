#!/bin/bash
# AIV+URMA PoC 构建脚本（在 A5 环境执行）
# 方案 B：ascendc_fatbin_library 生成正式算子二进制（fatbin），host 运行时显式加载
# 前置：source CANN 环境变量（set_env.sh），ASCEND_HOME_PATH 生效
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build/poc"

if [ -z "${ASCEND_HOME_PATH}" ]; then
    echo "[POC][ERROR] ASCEND_HOME_PATH not set, please source CANN set_env.sh first"
    exit 1
fi
echo "[POC] ASCEND_HOME_PATH = ${ASCEND_HOME_PATH}"

# fatbin 目标架构：A5 = Ascend 950PR/950DT = dav-3510（ascendc.cmake 读取 ASCEND_COMPUTE_UNIT；
# 若编译报 unknown arch，请查 $ASCEND_HOME_PATH/tools/tikcpp/ascendc_kernel_cmake/ascendc.cmake
# 中该变量支持的取值，并用 POC_ASC_COMPUTE_UNIT=<值> 覆盖）
export ASCEND_COMPUTE_UNIT="${POC_ASC_COMPUTE_UNIT:-dav-3510}"
echo "[POC] ASCEND_COMPUTE_UNIT = ${ASCEND_COMPUTE_UNIT}"

# 全量清建：PoC 体量小，且 fatbin 增量行为未验证
rm -rf "${BUILD_DIR}"

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
    -DASC_DEVKIT_ROOT="${ASC_DEVKIT_ROOT:-$HOME/asc-devkit}" \
    -DCMAKE_BUILD_TYPE=RELEASE \
    -DCMAKE_INSTALL_PREFIX="${BUILD_DIR}"
cmake --build "${BUILD_DIR}" -j"$(nproc)"
# fatbin 产物随 install 落盘：${BUILD_DIR}/fatbin/poc_hcomm_kernel/poc_hcomm_kernel.o
cmake --install "${BUILD_DIR}" > /dev/null

BIN="${BUILD_DIR}/poc_aiv_urma"
if [ ! -f "${BIN}" ]; then
    echo "[POC][ERROR] build output not found: ${BIN}"
    exit 1
fi

FATBIN="${BUILD_DIR}/fatbin/poc_hcomm_kernel/poc_hcomm_kernel.o"
if [ ! -f "${FATBIN}" ]; then
    FATBIN="$(find "${BUILD_DIR}" -name 'poc_hcomm_kernel.o' | head -1)"
fi
if [ -z "${FATBIN}" ] || [ ! -f "${FATBIN}" ]; then
    echo "[POC][ERROR] fatbin output not found under ${BUILD_DIR} (expected fatbin/poc_hcomm_kernel/poc_hcomm_kernel.o)"
    exit 1
fi
# 拷贝到可执行文件同目录：host 默认加载 <exe_dir>/poc_hcomm_kernel.o（--kernel-image 可覆盖）
cp -f "${FATBIN}" "${BUILD_DIR}/poc_hcomm_kernel.o"

echo "[POC] build done:"
echo "  host bin : ${BIN}"
echo "  fatbin   : ${BUILD_DIR}/poc_hcomm_kernel.o"
echo "[POC] 验收建议：readelf -sW ${BUILD_DIR}/poc_hcomm_kernel.o | grep poc_hcomm"
echo "      期望看到无后缀的 GLOBAL FUNC 符号 poc_hcomm_write / poc_hcomm_read"
