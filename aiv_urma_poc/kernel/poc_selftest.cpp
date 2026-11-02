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
extern "C" __global__ __aicore__ void poc_selftest(uint64_t statusAddr, uint64_t magic)
{
    if (AscendC::GetBlockIdx() == 0 && statusAddr != 0) {
        *(volatile uint64_t *)statusAddr = magic;
    }
    // c310 收尾 flush（与 poc_kernel_common.h 的 PocKernelEpilogue 同一序列；此处内联
    // 展开以保持本文件不依赖 hcomm 头文件）：缺了它 AIV 的 GM 写对 host D2H 不可见。
    // 依据：auto-gen 对 <<<>>> wrapper 恒追加（extract_host_stub.py，__DAV_C310__ 守卫），
    // ascc 插件对 ASCEND950 恒追加（asc_dev_stub_generator.cpp）。
#if defined(__DAV_C310__)
    pipe_barrier(PIPE_ALL);
    dsb(mem_dsb_t::DSB_ALL);
    dci();
#endif
}
