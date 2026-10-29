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
 * AIV direct-drive URMA PoC kernel (A5 / __NPU_ARCH__ 3510).
 * 验证目标：
 *   1. AscendC::Hcomm<UBC_CTP> 在 AIV 核上 Init/WriteNbi/ReadNbi/Commit/Drain 全流程
 *   2. channel handle 由 host 直接下传即可定位设备侧 ChannelEntity
 *   3. Drain 返回后数据可见性成立（host 侧 memcmp 校验）
 */

#include "kernel_operator.h"
#include "adv_api/hcomm/hcomm.h"
#include "poc_common.h"

// 函数级 meta：标识本 kernel 为 AIV 类型（值取自 CANN KernelType 枚举，K_TYPE_AIV=3，
// F_TYPE_KTYPE=1，布局与 hcomm AIV 算子开发指南一致）
#ifndef POC_FUN_LEVEL_KTYPE_DEFINED
#define POC_FUN_LEVEL_KTYPE_DEFINED
struct PocBaseTlv {
    unsigned short type;
    unsigned short len;
};
struct PocFunMetaKType {
    PocBaseTlv head;
    unsigned int ktype;
};
struct PocFunLevelKType {
    struct PocFunMetaKType ktypeMeta;
};
constexpr unsigned short POC_F_TYPE_KTYPE = 1;
constexpr unsigned int POC_K_TYPE_AIV = 3;
#endif

#define EXPORT_AIV_META_INFO(kernel_name)                                                              \
    static const struct PocFunLevelKType kernel_name##_kernel_type_section __attribute__(              \
        (used, section(".ascend.meta." #kernel_name))) = {{POC_F_TYPE_KTYPE, sizeof(unsigned int),     \
                                                           POC_K_TYPE_AIV}};

using namespace AscendC;

constexpr uint32_t POC_HCOMM_WS_SIZE = 512;  // HCOMM_URMA_TMP_BUF_SIZE，32 字节对齐由 TBuf 保证

// 单核执行：PoC 只需一个 vector core 驱动一条 channel（多核切分属于阶段 1 工作）
static inline __aicore__ void WriteStatus(uint64_t statusAddr, uint64_t value)
{
    if (statusAddr != 0) {
        *(volatile uint64_t *)statusAddr = value;
    }
}

static inline __aicore__ int32_t PocTransferImpl(const PocKernelArgs *args, bool isRead)
{
    TPipe pipe;
    TBuf<TPosition::VECCALC> wsBuf;
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

extern "C" __global__ __aicore__ void poc_hcomm_write(PocKernelArgs args)
{
    if (GetBlockIdx() != 0) {
        return;
    }
    WriteStatus(args.statusAddr, POC_STATUS_PENDING);
    const int32_t ret = PocTransferImpl(&args, false);
    WriteStatus(args.statusAddr, (ret == 0) ? POC_STATUS_OK : (POC_STATUS_FAIL_BASE + ret));
}
EXPORT_AIV_META_INFO(poc_hcomm_write)

extern "C" __global__ __aicore__ void poc_hcomm_read(PocKernelArgs args)
{
    if (GetBlockIdx() != 0) {
        return;
    }
    WriteStatus(args.statusAddr, POC_STATUS_PENDING);
    const int32_t ret = PocTransferImpl(&args, true);
    WriteStatus(args.statusAddr, (ret == 0) ? POC_STATUS_OK : (POC_STATUS_FAIL_BASE + ret));
}
EXPORT_AIV_META_INFO(poc_hcomm_read)
