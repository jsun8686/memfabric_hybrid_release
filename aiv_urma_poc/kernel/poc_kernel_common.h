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
 *
 * PoC 内核公共部分（write/read 两个 kernel 文件共用）。
 * 本工程走 ASC 语言路径（find_package(ASC) + LANGUAGE ASC，bisheng 一次编译）：
 *   - kernel 函数用 __vector__ 修饰，类型标识/注册/镜像内嵌由编译器自动生成
 *   - 无需手写 .ascend.meta 段与 EXPORT_AIV_META_INFO（那是 tikcpp ascendc_library 路径的做法）
 * 注意：每个编译单元只定义一个 kernel 函数（工具链约束），公共辅助提取到本头文件。
 */

#ifndef POC_KERNEL_COMMON_H
#define POC_KERNEL_COMMON_H

#include "kernel_operator.h"
#include "adv_api/hcomm/hcomm.h"
#include "poc_common.h"

using namespace AscendC;

constexpr uint32_t POC_HCOMM_WS_SIZE = 512;  // HCOMM_URMA_TMP_BUF_SIZE，32 字节对齐由 TBuf 保证

// 单核执行：PoC 只需一个 vector core 驱动一条 channel（多核切分属于阶段 1 工作）
static inline __aicore__ void PocWriteStatus(uint64_t statusAddr, uint64_t value)
{
    if (statusAddr != 0) {
        *(volatile uint64_t *)statusAddr = value;
    }
}

// 传输公共实现：Init(512B UB workspace) -> 分块 Nbi(commit=false) -> Commit -> Drain
// 错误码分段：0x1xx=Init 失败、0x2xx=分块 Nbi 失败（内嵌偏移与返回码）、0x3xx=Commit、0x4xx=Drain
static inline __aicore__ int32_t PocTransferImpl(const PocKernelArgs *args, bool isRead)
{
    TPipe pipe;
    // 官方 01 示例：hcomm workspace 用 VECOUT 位置
    TBuf<TPosition::VECOUT> wsBuf;
    pipe.InitBuffer(wsBuf, POC_HCOMM_WS_SIZE);
    LocalTensor<uint8_t> ws = wsBuf.Get<uint8_t>();

    Hcomm<COMM_PROTOCOL_UBC_CTP> hcomm;
    int32_t ret = hcomm.Init(ws, POC_HCOMM_WS_SIZE);
    if (ret != 0) {
        return 0x100 + ret;  // Init 失败
    }

    const uint64_t chunk = (args->chunkSize != 0) ? args->chunkSize : POC_DEFAULT_CHUNK;
    uint64_t offset = 0;
    while (offset < args->size) {
        const uint64_t cur = (args->size - offset < chunk) ? (args->size - offset) : chunk;
        // 注意：GM_ADDR 是宏，const 不能加在 GM_ADDR 前（会展开成底层 const，与 WriteNbi/ReadNbi 形参冲突）
        GM_ADDR dst = isRead ? reinterpret_cast<GM_ADDR>(args->localAddr + offset)
                             : reinterpret_cast<GM_ADDR>(args->remoteAddr + offset);
        GM_ADDR src = isRead ? reinterpret_cast<GM_ADDR>(args->remoteAddr + offset)
                             : reinterpret_cast<GM_ADDR>(args->localAddr + offset);
        // commit=false：攒 WQE，循环结束后一次性 Commit，减少 doorbell 次数
        if (isRead) {
            ret = hcomm.template ReadNbi<false>(args->channel, dst, src, cur);
        } else {
            ret = hcomm.template WriteNbi<false>(args->channel, dst, src, cur);
        }
        if (ret != 0) {
            return 0x200 + static_cast<int32_t>(offset & 0xFFFF) + (ret << 16);  // 分块失败，编码偏移与返回码
        }
        offset += cur;
    }

    ret = hcomm.Commit(args->channel);
    if (ret != 0) {
        return 0x300 + ret;  // Commit 失败
    }
    ret = hcomm.Drain(args->channel);
    if (ret != 0) {
        return 0x400 + ret;  // Drain 失败（超时/通道异常）
    }
    return 0;
}

#endif  // POC_KERNEL_COMMON_H
