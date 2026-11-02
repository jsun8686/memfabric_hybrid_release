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
 * 声明为 __global__ __aicore__ __attribute__((aiv))：c310 动态模式下 bisheng 要求
 * kernel func 显式携带核型属性（__attribute__((aiv))/(aic) 或 core_ratio），
 * __vector__ 关键字在这版工具链不计入该校验； preprocess 也依据该属性把 kernel
 * 分类进 AIV 编译目录（AIC 子构建自动跳过）。
 * fatbin 路径不做 auto-gen 改名，符号保持原始名 poc_hcomm_read，
 * host 侧 aclrtBinaryGetFunction 按原名查询。
 */

#include "poc_kernel_common.h"

extern "C" __global__ __aicore__ __attribute__((aiv)) void poc_hcomm_read(PocKernelArgs args)
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
