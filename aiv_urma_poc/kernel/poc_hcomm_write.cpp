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
 * AIV direct-drive URMA PoC write kernel（A5 / __NPU_ARCH__ 3510）。
 * 本地 GM -> 对端 GM（AscendC::Hcomm<UBC_CTP>::WriteNbi -> Commit -> Drain）。
 * 单独成文件：ascendc 工具链要求一个编译单元一个 kernel 函数。
 */

#include "poc_kernel_common.h"

extern "C" __global__ __aicore__ void poc_hcomm_write(PocKernelArgs args)
{
    if (GetBlockIdx() != 0) {
        return;
    }
    PocWriteStatus(args.statusAddr, POC_STATUS_PENDING);
    const int32_t ret = PocTransferImpl(&args, false);
    PocWriteStatus(args.statusAddr, (ret == 0) ? POC_STATUS_OK : (POC_STATUS_FAIL_BASE + ret));
}
EXPORT_AIV_META_INFO(poc_hcomm_write)
