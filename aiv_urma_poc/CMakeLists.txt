# ----------------------------------------------------------------------------------------------------------
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# AIV+URMA PoC is licensed under Mulan PSL v2.
# ----------------------------------------------------------------------------------------------------------
# ASC 一体化构建（对齐官方 asc-comm 示例 04_simt_urma / 01_hcomm_write_read_nbi）：
#   - kernel 源文件以 ASC 语言编译（bisheng 一次编译完成 host/device 分裂、<<<>>> 胶水、镜像内嵌）
#   - host 源文件按普通 C++ 编译（dlsym 加载 libascendcl/libhcomm，保持 MemFabric 同构控制面）
#   - 设备镜像与启动注册由编译器/运行时闭环处理，不再手工 aclrtBinaryLoad*/GetFunction
# 前置：source <CANN>/set_env.sh（ASCEND_HOME_PATH 生效）
# 构建：./build.sh（内部执行 cmake -DCMAKE_ASC_ARCHITECTURES=dav-3510）
# ----------------------------------------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.16)

# ASC cmake 模块来自 CANN 自带 ascendc_kernel_cmake（asc-devkit 安装版，离线可用）
set(CMAKE_ASC_ARCHITECTURES "dav-3510" CACHE STRING "NPU architecture: dav-3510 = Ascend 950PR/950DT")
if(NOT DEFINED ENV{ASCEND_HOME_PATH})
    message(FATAL_ERROR "ASCEND_HOME_PATH not set, please source CANN set_env.sh first")
endif()
list(APPEND CMAKE_MODULE_PATH "$ENV{ASCEND_HOME_PATH}/tools/tikcpp/ascendc_kernel_cmake/asc_modules")
find_package(ASC REQUIRED)

project(poc_aiv_urma LANGUAGES ASC CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RELEASE)
endif()

# asc-devkit 根目录：提供 adv_api/hcomm/hcomm.h（AscendC::Hcomm 设备侧 API）
if(NOT ASC_DEVKIT_ROOT)
    set(ASC_DEVKIT_ROOT "$ENV{HOME}/asc-devkit" CACHE PATH "asc-devkit root for hcomm.h")
endif()
if(NOT EXISTS ${ASC_DEVKIT_ROOT}/include/adv_api/hcomm/hcomm.h)
    message(FATAL_ERROR "${ASC_DEVKIT_ROOT}/include/adv_api/hcomm/hcomm.h not found, set -DASC_DEVKIT_ROOT=<path>")
endif()
message(STATUS "poc: ASC_DEVKIT_ROOT = ${ASC_DEVKIT_ROOT}")

# kernel 源以 ASC 语言编译（每文件一个 kernel，launch 封装与 kernel 同文件）
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/kernel/poc_hcomm_write.cpp PROPERTIES LANGUAGE ASC)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/kernel/poc_hcomm_read.cpp PROPERTIES LANGUAGE ASC)

add_executable(poc_aiv_urma
    host/poc_main.cpp
    host/poc_api.cpp
    host/poc_eid.cpp
    host/poc_tcp.cpp
    kernel/poc_hcomm_write.cpp
    kernel/poc_hcomm_read.cpp
)

set_target_properties(poc_aiv_urma PROPERTIES
    LINKER_LANGUAGE ASC
)

target_include_directories(poc_aiv_urma PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CMAKE_CURRENT_SOURCE_DIR}/host
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${ASC_DEVKIT_ROOT}/include
)

# 链接对齐官方示例 04（去掉 hcomm/hccl：控制面走 dlsym，不引入编译期依赖）；
# ascendcl/acl_rt/runtime 供 <<<>>> 展开的运行时胶水使用
target_link_directories(poc_aiv_urma PRIVATE
    $ENV{ASCEND_HOME_PATH}/lib64
    $ENV{ASCEND_HOME_PATH}/${CMAKE_SYSTEM_PROCESSOR}-linux/lib64
)
target_link_libraries(poc_aiv_urma PRIVATE
    ascendcl
    acl_rt
    runtime
    c_sec
    dl
    pthread
)

target_compile_options(poc_aiv_urma PRIVATE
    $<$<COMPILE_LANGUAGE:ASC>:--npu-arch=${CMAKE_ASC_ARCHITECTURES}>
)
