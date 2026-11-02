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
 * AIV direct-drive URMA PoC read kernel（A5 / dav-3510，fatbin 设备侧编译单元）。
 * 对端 GM -> 本地 GM（AscendC::Hcomm<UBC_CTP>::ReadNbi -> Commit -> Drain）。
 * 单独成文件：每个编译单元一个 kernel 函数。
 * 声明为 __attribute__((aiv)) __global__ __aicore__：c310 动态模式下 bisheng 要求
 * kernel func 显式携带核型属性；同时 extract_host_stub.py 的签名正则要求
 * [aicore] 之后紧跟"返回类型 函数名"，因此核型属性必须放在 __global__ 之前，
 * 不能插在 __aicore__ 与返回类型之间（否则 host stub 生成抛 FuncNameNotFound）。
 * fatbin 路径不做 auto-gen 改名，符号保持原始名 poc_hcomm_read，
 * host 侧 aclrtBinaryGetFunction 按原名查询。
 */

#include "poc_kernel_common.h"

extern "C" __attribute__((aiv)) __global__ __aicore__ void poc_hcomm_read(PocKernelArgs args)
{
    if (GetBlockIdx() != 0) {
        return;
    }
    PocWriteStatus(args.statusAddr, POC_STATUS_PENDING);
    const int32_t ret = PocTransferImpl(&args, true);
    PocWriteStatus(args.statusAddr, (ret == 0) ? POC_STATUS_OK : (POC_STATUS_FAIL_BASE + ret));
}
// host 侧启动：poc_main.cpp 显式 aclrtBinaryLoad + aclrtBinaryGetFunction("poc_hcomm_read")
// + aclrtLaunchKernelWithHostArgs(AIV)，本文件不再提供 <<<>>> 封装（fatbin 为纯设备编译）
