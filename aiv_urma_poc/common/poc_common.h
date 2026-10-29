/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * AIV+URMA PoC is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#ifndef AIV_URMA_POC_COMMON_H
#define AIV_URMA_POC_COMMON_H

#include <cstdint>

// Host 与 AIV 内核共用的下传参数（布局两端一致，勿随意改字段顺序）
struct PocKernelArgs {
    uint64_t channel;     // HcommChannelCreate 返回的 ChannelHandle
    uint64_t localAddr;   // 本端 GM buffer 地址
    uint64_t remoteAddr;  // 对端 GM buffer 地址（HcommMemImport 得到的 remoteMem.addr）
    uint64_t size;        // 本次搬运总字节数
    uint64_t chunkSize;   // 每次 WriteNbi/ReadNbi 的分块字节数
    uint64_t statusAddr;  // GM 状态回写地址：0=pending, 1=ok, 0x1000+ret=fail
};

constexpr uint64_t POC_STATUS_PENDING = 0;
constexpr uint64_t POC_STATUS_OK = 1;
constexpr uint64_t POC_STATUS_FAIL_BASE = 0x1000;

// 默认参数
constexpr uint64_t POC_DEFAULT_BUF_SIZE = 1ULL << 20;  // 1MB
constexpr uint64_t POC_DEFAULT_CHUNK = 64ULL << 10;    // 64KB
constexpr uint16_t POC_DEFAULT_OOB_PORT = 26000;       // PoC 自己的 TCP 带外通道
constexpr uint16_t POC_DEFAULT_HCOMM_PORT = 60001;     // AivUrmaChannel OOB socket（对齐 hcomm 默认值）

// TCP 带外交换的消息（固定小包，足够 PoC 使用）
constexpr uint32_t POC_MSG_MAGIC = 0x506F6350;  // "PoCP"
constexpr uint32_t POC_MAX_MEM_DESC = 4096;

#endif  // AIV_URMA_POC_COMMON_H
