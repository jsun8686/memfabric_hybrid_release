cmake_minimum_required(VERSION 3.12 FATAL_ERROR)
project(poc_aiv_urma_host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RELEASE)
endif ()

add_executable(poc_aiv_urma
    poc_main.cpp
    poc_api.cpp
    poc_eid.cpp
    poc_tcp.cpp
)

target_include_directories(poc_aiv_urma PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
)

# 全部依赖走 dlopen，链接期仅需 dl/pthread
target_link_libraries(poc_aiv_urma PRIVATE dl pthread)
