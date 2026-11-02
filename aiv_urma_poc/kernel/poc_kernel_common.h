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

// 状态页 UB 镜像 + DataCopy 整页刷出（950/AIV 写回铁律）：
// 官方 fatbin 样例 6_memory_loaded_vector_add 在同机（Ascend950PR_9589 / CANN 9.1.0）实测
// PASS，其唯一写回路径是 UB(LocalTensor) -> GM 的 DataCopy（MTE3 引擎）；
// 而本 PoC 此前的 *(volatile uint64_t*)addr 裸指针标量 GM 写对 host D2H 恒不可见
//（加 pipe_barrier/dsb/dci 冲刷同样无效，a917503 实验证伪）——AIV 核内标量 store 与
// MTE3 写回是两条硬件路径，前者在 950 上不作为对 host 可见的写回路径使用。
// DataCopy 最小粒度 32B，故状态页（64B = 8 x u64：[0]=status [1..4]=echo）整页镜像在 UB，
// 每次更新整页刷出。
struct PocStatusWriter {
    TPipe pipe;                     // 与传输 workspace 共用一个 TPipe（AscendC 惯例单 pipe）
    TBuf<TPosition::VECOUT> buf;    // 64B 状态页镜像
    LocalTensor<uint64_t> ub;
    uint64_t gmAddr = 0;
};

static inline __aicore__ void PocStatusInit(PocStatusWriter &w, uint64_t statusAddr)
{
    w.gmAddr = statusAddr;
    if (statusAddr == 0) {
        return;
    }
    w.pipe.InitBuffer(w.buf, 64);
    w.ub = w.buf.Get<uint64_t>();
    for (uint32_t i = 0; i < 8; i++) {
        w.ub(i) = 0;
    }
}

// 标量写 UB 与 MTE3 读 UB 之间、以及 MTE3 读 UB 与下一次标量改写之间的硬同步
static inline __aicore__ void PocStatusFlush(PocStatusWriter &w)
{
    if (w.gmAddr == 0) {
        return;
    }
    constexpr TEventID evtSMte3 = 0;
    constexpr TEventID evtMte3S = 1;
    SetFlag<HardEvent::S_MTE3>(evtSMte3);
    WaitFlag<HardEvent::S_MTE3>(evtSMte3);
    GlobalTensor<uint64_t> gm;
    gm.SetGlobalBuffer(reinterpret_cast<__gm__ uint64_t *>(w.gmAddr), 8);
    DataCopy(gm, w.ub, 8);  // UB -> GM 整页 64B
    SetFlag<HardEvent::MTE3_S>(evtMte3S);
    WaitFlag<HardEvent::MTE3_S>(evtMte3S);
}

static inline __aicore__ void PocStatusSet(PocStatusWriter &w, uint32_t idx, uint64_t value)
{
    if (w.gmAddr == 0) {
        return;
    }
    w.ub(idx) = value;
    PocStatusFlush(w);
}

// c310/A5 收尾 flush：对齐官方约定保留（<<<>>> 路径由 auto-gen 在每个 kernel wrapper
// 尾部恒追加，asc-devkit extract_host_stub.py _generate_barrier_sync()，__DAV_C310__
// 守卫；ascc 插件 asc_dev_stub_generator.cpp 对 ASCEND950 恒追加）。本工程是 fatbin 纯
// 设备编译、不经过 stub 生成器，故手动补齐。注意：实测它并不能让 AIV 裸指针标量 GM store
// 对 host 可见（a917503 实验证伪）——状态写回的可见性由 UB->GM DataCopy 路径保证，
// 见上方 PocStatusWriter 注释。
static inline __aicore__ void PocKernelEpilogue()
{
#if defined(__DAV_C310__)
    pipe_barrier(PIPE_ALL);
    dsb(mem_dsb_t::DSB_ALL);
    dci();
#endif
}

// 诊断：把 kernel 实际收到的参数回显到状态页 [1..4]，host 侧对照打印值可判别
// "kernel 没跑" 与 "跑了但参数错位/为零" 两类故障
static inline __aicore__ void PocEchoArgs(PocStatusWriter &w, const PocKernelArgs *args)
{
    if (w.gmAddr == 0) {
        return;
    }
    w.ub(1) = args->size;
    w.ub(2) = args->chunkSize;
    w.ub(3) = args->channel;
    w.ub(4) = args->statusAddr;
    PocStatusFlush(w);
}

// 传输公共实现：Init(512B UB workspace) -> 分块 Nbi(commit=false) -> Commit -> Drain
// 错误码分段：0x1xx=Init 失败、0x2xx=分块 Nbi 失败（内嵌偏移与返回码）、0x3xx=Commit、0x4xx=Drain
static inline __aicore__ int32_t PocTransferImpl(const PocKernelArgs *args, bool isRead,
                                                 PocStatusWriter &w)
{
    // 官方 01 示例：hcomm workspace 用 VECOUT 位置；与状态页共用 w.pipe
    TBuf<TPosition::VECOUT> wsBuf;
    w.pipe.InitBuffer(wsBuf, POC_HCOMM_WS_SIZE);
    LocalTensor<uint8_t> ws = wsBuf.Get<uint8_t>();

    Hcomm<COMM_PROTOCOL_UBC_CTP> hcomm;
    int32_t ret = hcomm.Init(ws, POC_HCOMM_WS_SIZE);
    if (ret != 0) {
        return 0x100 + ret;  // Init 失败
    }
    PocStatusSet(w, 0, POC_STAGE_INIT_OK);

    const uint64_t chunk = (args->chunkSize != 0) ? args->chunkSize : POC_DEFAULT_CHUNK;
    uint64_t offset = 0;
    while (offset < args->size) {
        const uint64_t cur = (args->size - offset < chunk) ? (args->size - offset) : chunk;
        // 注意：GM_ADDR 是宏，const 不能加在 GM_ADDR 前（会展开成底层 const，与 WriteNbi/ReadNbi 形参冲突）
        GM_ADDR dst = isRead ? reinterpret_cast<GM_ADDR>(args->localAddr + offset)
                             : reinterpret_cast<GM_ADDR>(args->remoteAddr + offset);
        GM_ADDR src = isRead ? reinterpret_cast<GM_ADDR>(args->remoteAddr + offset)
                             : reinterpret_cast<GM_ADDR>(args->localAddr + offset);
        // write/read 均使用 commit=false 先攒 WQE，循环结束后统一 Commit，验证批量提交路径
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

    PocStatusSet(w, 0, POC_STAGE_QUEUE_OK);

    // 发布循环中攒下的 WQE
    ret = hcomm.Commit(args->channel);
    if (ret != 0) {
        return 0x300 + ret;  // Commit 失败
    }
    PocStatusSet(w, 0, POC_STAGE_COMMIT_OK);
    ret = hcomm.Drain(args->channel);
    if (ret != 0) {
        return 0x400 + ret;  // Drain 失败（超时/通道异常）
    }
    return 0;
}

#endif  // POC_KERNEL_COMMON_H
