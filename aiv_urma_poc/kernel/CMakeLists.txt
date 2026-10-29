cmake_minimum_required(VERSION 3.12 FATAL_ERROR)
project(poc_aiv_hcomm_kernel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RELEASE)
endif ()

# A5(950) 固定为 ascend950pr_9599（与 MemFabric acc_offload A5 目标一致）
if (NOT SOC_VERSION)
    set(SOC_VERSION "ascend950pr_9599")
endif ()
message(STATUS "poc kernel: SOC_VERSION = ${SOC_VERSION}")

# asc-devkit 根目录：提供 adv_api/hcomm/hcomm.h（AscendC::Hcomm）。
# 获取方式见 README；hcomm.h 以相对路径引用 <root>/impl，因此 include 根必须是 <root>/include。
if (NOT ASC_DEVKIT_ROOT)
    set(ASC_DEVKIT_ROOT "$ENV{HOME}/asc-devkit")
endif ()
if (NOT EXISTS ${ASC_DEVKIT_ROOT}/include/adv_api/hcomm/hcomm.h)
    message(FATAL_ERROR "poc kernel: ${ASC_DEVKIT_ROOT}/include/adv_api/hcomm/hcomm.h not found, set -DASC_DEVKIT_ROOT=<path-to-asc-devkit>")
endif ()
message(STATUS "poc kernel: ASC_DEVKIT_ROOT = ${ASC_DEVKIT_ROOT}")

include(${CMAKE_CURRENT_SOURCE_DIR}/config_ascendc.cmake)

ascendc_library(poc_aiv_hcomm_kernel STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/poc_hcomm_kernel.cpp
)

ascendc_include_directories(poc_aiv_hcomm_kernel PRIVATE
    ${ASC_DEVKIT_ROOT}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
)

ascendc_compile_definitions(poc_aiv_hcomm_kernel PRIVATE ASCENDC_DUMP=0)
