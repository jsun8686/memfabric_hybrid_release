/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * AIV+URMA PoC is licensed under Mulan PSL v2.
 * You may use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of the Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 *
 * 自检内核（A5 / dav-3510，fatbin 设备侧编译单元）——故意不依赖 Hcomm/URMA。
 * 用途：当 hcomm 内核"注册了但不执行"时，用最小内核切分故障域：
 *   - 自检 PASS → fatbin 加载 + AIV 下发 + GM 写回链路完好，问题收敛到
 *     hcomm 内核的设备侧符号（如 URMA 原语的未解析引用）
 *   - 自检也失败 → 问题在 fatbin 格式/下发链路本身，与 hcomm 无关
 * 单独成文件：每个编译单元一个 kernel 函数（工具链约束）。
 */

#include "kernel_operator.h"

// 写回 magic 到 statusAddr；参数走与正式内核相同的打包下传约定（值打包 + 工具链尾参）
// 写回路径与官方 fatbin 样例 6_memory_loaded_vector_add 对齐：UB(LocalTensor) -> GM
// DataCopy（MTE3）。950/AIV 上裸指针标量 GM store 对 host D2H 恒不可见（dsb/dci 无效，
// 实验证伪）；样例同机实测 PASS 证明 DataCopy 是可见写回路径。本文件不依赖 hcomm 头文件。
extern "C" __global__ __aicore__ void poc_selftest(uint64_t statusAddr, uint64_t magic)
{
    using namespace AscendC;
    if (AscendC::GetBlockIdx() == 0 && statusAddr != 0) {
        TPipe pipe;
        TBuf<TPosition::VECOUT> buf;
        pipe.InitBuffer(buf, 64);
        LocalTensor<uint64_t> ub = buf.Get<uint64_t>();
        for (uint32_t i = 0; i < 8; i++) {
            ub(i) = 0;
        }
        ub(0) = magic;
        SetFlag<HardEvent::S_MTE3>(0);
        WaitFlag<HardEvent::S_MTE3>(0);
        GlobalTensor<uint64_t> gm;
        gm.SetGlobalBuffer(reinterpret_cast<__gm__ uint64_t *>(statusAddr), 8);
        DataCopy(gm, ub, 8);  // 整页 64B 刷出，DataCopy 最小粒度 32B
        SetFlag<HardEvent::MTE3_S>(1);
        WaitFlag<HardEvent::MTE3_S>(1);
    }
    // c310 收尾 flush：保留（auto-gen/ascc 对 950 恒追加同序列，对 DataCopy 路径无害）
#if defined(__DAV_C310__)
    pipe_barrier(PIPE_ALL);
    dsb(mem_dsb_t::DSB_ALL);
    dci();
#endif
}
