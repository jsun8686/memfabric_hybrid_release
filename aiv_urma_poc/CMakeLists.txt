# ----------------------------------------------------------------------------------------------------------
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# AIV+URMA PoC is licensed under Mulan PSL v2.
# ----------------------------------------------------------------------------------------------------------
# 方案 B：CANN 官方"非混合编程"路径（对齐官方 AddKernelInvocationAcl 样例）
#   - kernel：ascendc_fatbin_library 经 bisheng 编译为独立算子二进制（正式产物，
#     符号保留原始名 poc_hcomm_write/poc_hcomm_read；不像 ascendc_library auto-gen
#     那样被改写成 <name>_0/_1——旧路径 aclrtBinaryGetFunction 107000 的根因）
#   - host：纯 C++ 可执行，dlopen/dlsym libascendcl 后
#     aclrtCreateBinary -> aclrtBinaryLoad -> aclrtBinaryGetFunction(原名)
#     -> aclrtLaunchKernelWithHostArgs（attr 带 ACL_RT_ENGINE_TYPE_AIV）
# 前置：source <CANN>/set_env.sh（ASCEND_HOME_PATH 生效）
# 板型：SOC_VERSION 默认 ascend950pr_9599（A5 = Ascend 950PR），
#       950DT 或其他变体用 cmake -DSOC_VERSION=ascend950dt_<id> 或
#       build.sh 的 POC_SOC_VERSION=<值> 覆盖（见下方 SOC_VERSION 注释）
# 构建：./build.sh（产物 build/poc/poc_aiv_urma + build/poc/poc_hcomm_kernel.o）
# ----------------------------------------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.16)
project(poc_aiv_urma LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RELEASE)
endif()

if(NOT DEFINED ENV{ASCEND_HOME_PATH})
    message(FATAL_ERROR "ASCEND_HOME_PATH not set, please source CANN set_env.sh first")
endif()

# SOC_VERSION：legacy ascendc.cmake 路径的必填 CMake 变量
# （tools/tikcpp/ascendc_kernel_cmake/legacy_modules/host_config.cmake 强校验，
#  未定义直接 FATAL_ERROR "SOC_VERSION value not set."；它不认 fwk/opp 路径用的
#  环境变量 ASCEND_COMPUTE_UNIT）。
# A5 取值：950PR -> ascend950pr_<id>，950DT -> ascend950dt_<id>；
# 合法全集见 host_config.cmake 的 ascend950_list，或对照本机已安装的平台配置：
#   ls $ASCEND_HOME_PATH/compiler/data/platform_config/ | grep -i 950
# 板型确认：npu-smi info -t board -i <npu_id>
if(NOT DEFINED SOC_VERSION)
    set(SOC_VERSION "ascend950pr_9599" CACHE STRING "SoC version for ascendc fatbin build (A5: ascend950pr_<id> / ascend950dt_<id>)")
endif()
message(STATUS "poc: SOC_VERSION = ${SOC_VERSION}")

# host_config.cmake 以 CMake 变量方式引用 ASCEND_CANN_PACKAGE_PATH（不读环境变量），
# 官方样例惯例是 cmake -DASCEND_CANN_PACKAGE_PATH=<CANN 包根> 传入；
# 这里允许从环境变量兜底，再退化为 ASCEND_HOME_PATH。
if(NOT DEFINED ASCEND_CANN_PACKAGE_PATH)
    if(DEFINED ENV{ASCEND_CANN_PACKAGE_PATH})
        set(ASCEND_CANN_PACKAGE_PATH "$ENV{ASCEND_CANN_PACKAGE_PATH}")
    else()
        set(ASCEND_CANN_PACKAGE_PATH "$ENV{ASCEND_HOME_PATH}")
    endif()
endif()
message(STATUS "poc: ASCEND_CANN_PACKAGE_PATH = ${ASCEND_CANN_PACKAGE_PATH}")

# ascendc_fatbin_library 来自 CANN 自带 ascendc_kernel_cmake（纯设备侧编译，无需 ASC 语言工具链）
set(ASCENDC_CMAKE "$ENV{ASCEND_HOME_PATH}/tools/tikcpp/ascendc_kernel_cmake/ascendc.cmake")
if(NOT EXISTS ${ASCENDC_CMAKE})
    message(FATAL_ERROR
        "${ASCENDC_CMAKE} not found. 确认 CANN(>=9.1) 安装包含 ascendc_kernel_cmake；"
        "可用 find $ENV{ASCEND_HOME_PATH} -name 'ascendc.cmake' 定位后修正本路径")
endif()
include(${ASCENDC_CMAKE})

# asc-devkit 根目录：提供 adv_api/hcomm/hcomm.h（AscendC::Hcomm 设备侧 API）与 kernel_operator.h
if(NOT ASC_DEVKIT_ROOT)
    set(ASC_DEVKIT_ROOT "$ENV{HOME}/asc-devkit" CACHE PATH "asc-devkit root for hcomm.h")
endif()
if(NOT EXISTS ${ASC_DEVKIT_ROOT}/include/adv_api/hcomm/hcomm.h)
    message(FATAL_ERROR "${ASC_DEVKIT_ROOT}/include/adv_api/hcomm/hcomm.h not found, set -DASC_DEVKIT_ROOT=<path>")
endif()
message(STATUS "poc: ASC_DEVKIT_ROOT = ${ASC_DEVKIT_ROOT}")

# ── kernel：独立算子二进制（fatbin）──
# cmake --install 后产物位于 ${CMAKE_INSTALL_PREFIX}/fatbin/poc_hcomm_kernel/poc_hcomm_kernel.o
ascendc_fatbin_library(poc_hcomm_kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/kernel/poc_hcomm_write.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/kernel/poc_hcomm_read.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/kernel/poc_selftest.cpp
)
if(COMMAND ascendc_include_directories)
    # 注意签名：ascendc_include_directories(<target> <scope> <dirs...>)，
    # 内部写入 <target>_interface 的 INCLUDES 属性传递给设备侧子工程
    ascendc_include_directories(poc_hcomm_kernel PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/common
        ${ASC_DEVKIT_ROOT}/include
    )
else()
    # 兼容旧版 ascendc.cmake：退化为普通 target include
    target_include_directories(poc_hcomm_kernel PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/common
        ${ASC_DEVKIT_ROOT}/include
    )
endif()

# ── host：纯 C++，无 ASC 语言、无链接期 CANN 库依赖（控制面/数据面全部 dlopen/dlsym）──
add_executable(poc_aiv_urma
    host/poc_main.cpp
    host/poc_api.cpp
    host/poc_eid.cpp
    host/poc_tcp.cpp
)
target_include_directories(poc_aiv_urma PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CMAKE_CURRENT_SOURCE_DIR}/host
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(poc_aiv_urma PRIVATE dl pthread)
