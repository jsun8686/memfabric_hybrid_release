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

# SOC_VERSION：legacy ascendc_kernel_cmake 路径只认这个 CMake 变量
# （host_config.cmake 强校验；环境变量 ASCEND_COMPUTE_UNIT 是 fwk/opp 框架路径用的，此处无效）。
# A5 = 950PR/950DT：950PR 默认 ascend950pr_9599；950DT 或其他变体用
#   POC_SOC_VERSION=ascend950dt_<id> ./build.sh 覆盖。
# 合法取值清单：grep -n "ascend950_list" \
#   $ASCEND_HOME_PATH/tools/tikcpp/ascendc_kernel_cmake/legacy_modules/host_config.cmake
# 或对照本机平台配置：ls $ASCEND_HOME_PATH/compiler/data/platform_config/ | grep -i 950
SOC_VERSION="${POC_SOC_VERSION:-ascend950pr_9599}"
echo "[POC] SOC_VERSION = ${SOC_VERSION}"

# host_config.cmake 要求 ASCEND_CANN_PACKAGE_PATH 指向已安装的 CANN 包根目录；
# 注意它以 CMake 变量（而非环境变量）方式被引用，所以下面 cmake 要显式 -D 传入
export ASCEND_CANN_PACKAGE_PATH="${ASCEND_CANN_PACKAGE_PATH:-${ASCEND_HOME_PATH}}"

# 全量清建：PoC 体量小，且 fatbin 增量行为未验证
rm -rf "${BUILD_DIR}"

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
    -DSOC_VERSION="${SOC_VERSION}" \
    -DASCEND_CANN_PACKAGE_PATH="${ASCEND_CANN_PACKAGE_PATH}" \
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
