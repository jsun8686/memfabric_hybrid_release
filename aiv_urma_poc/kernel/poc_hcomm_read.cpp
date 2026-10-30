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
 * AIV direct-drive URMA PoC read kernel（A5 / dav-3510，ASC 语言编译单元）。
 * 对端 GM -> 本地 GM（AscendC::Hcomm<UBC_CTP>::ReadNbi -> Commit -> Drain）。
 * 单独成文件：每个编译单元一个 kernel 函数。
 * __vector__ 修饰符：标识 vector core（AIV）内核，ASC 编译器据此自动生成类型
 * 标识与注册信息（镜像内嵌，host 侧无需手工 aclrtBinaryLoad*/GetFunction）。
 */

#include "poc_kernel_common.h"

extern "C" __vector__ __global__ __aicore__ void poc_hcomm_read(PocKernelArgs args)
{
    if (GetBlockIdx() != 0) {
        return;
    }
    PocWriteStatus(args.statusAddr, POC_STATUS_PENDING);
    const int32_t ret = PocTransferImpl(&args, true);
    PocWriteStatus(args.statusAddr, (ret == 0) ? POC_STATUS_OK : (POC_STATUS_FAIL_BASE + ret));
}

// host 侧 launch 封装：<<<>>> 由 bisheng 编译器展开（对齐官方 asc-comm 示例
// 04_simt_urma 的 LaunchSimtUrma 模式），main 经 extern 调用
void LaunchPocRead(void *stream, PocKernelArgs args)
{
    poc_hcomm_read<<<1, 0, stream>>>(args);
}
