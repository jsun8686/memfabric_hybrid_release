/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * AIV+URMA PoC is licensed under Mulan PSL v2.
 */

#include "poc_api.h"

#include <dlfcn.h>

static int LoadOneLib(void *&handle, const std::string &path)
{
    handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (handle == nullptr) {
        printf("[POC][ERROR] dlopen %s failed: %s\n", path.c_str(), dlerror());
        return -1;
    }
    return 0;
}

int PocDynApi::Load(const std::string &ascendHome)
{
    std::string libPath;
    void *aclHandle = nullptr;
    libPath = ascendHome + "/lib64/libascendcl.so";
    if (LoadOneLib(aclHandle, libPath) != 0) {
        return -1;
    }
    POC_DLSYM(aclInit, aclHandle, "aclInit");
    POC_DLSYM(aclFinalize, aclHandle, "aclFinalize");
    POC_DLSYM(aclrtSetDevice, aclHandle, "aclrtSetDevice");
    POC_DLSYM(aclrtResetDevice, aclHandle, "aclrtResetDevice");
    POC_DLSYM(aclrtGetDevice, aclHandle, "aclrtGetDevice");
    POC_DLSYM(aclrtMalloc, aclHandle, "aclrtMalloc");
    POC_DLSYM(aclrtFree, aclHandle, "aclrtFree");
    POC_DLSYM(aclrtMemcpy, aclHandle, "aclrtMemcpy");
    POC_DLSYM(aclrtMemset, aclHandle, "aclrtMemset");
    POC_DLSYM(aclrtCreateStream, aclHandle, "aclrtCreateStream");
    POC_DLSYM(aclrtDestroyStream, aclHandle, "aclrtDestroyStream");
    POC_DLSYM(aclrtSynchronizeStream, aclHandle, "aclrtSynchronizeStream");

    // 对齐 MemFabric dl_acl_api.cpp：只 dlopen libascendcl.so 一个库，
    // aclrt* 公开接口与 rt* 内部接口（rtGetDeviceInfo 等）均由 libascendcl.so 导出；
    // 不再单独加载 libruntime.so（CANN 9.1.0 的 libruntime 不导出 launch 系列 aclrt* 符号）。
    POC_DLSYM(rtGetDeviceInfo, aclHandle, "rtGetDeviceInfo");
    // OPTIONAL：部分 CANN 版本未导出（MemFabric 同为 OPTIONAL），缺失时调用方 fallback 用 logicId
    POC_DLSYM_OPT(aclrtGetPhyDevIdByLogicDevId, aclHandle, "aclrtGetPhyDevIdByLogicDevId");

    // 方案 B：fatbin 显式加载与 AIV 下发（对齐官方 AddKernelInvocationAcl 样例的
    // aclrtCreateBinary/aclrtBinaryLoad/aclrtBinaryGetFunction 调用序）
    POC_DLSYM(aclrtCreateBinary, aclHandle, "aclrtCreateBinary");
    POC_DLSYM(aclrtDestroyBinary, aclHandle, "aclrtDestroyBinary");
    POC_DLSYM(aclrtBinaryLoad, aclHandle, "aclrtBinaryLoad");
    // OPTIONAL：缺失时回退 CreateBinary+BinaryLoad 内存路径
    POC_DLSYM_OPT(aclrtBinaryLoadFromFile, aclHandle, "aclrtBinaryLoadFromFile");
    POC_DLSYM(aclrtBinaryUnLoad, aclHandle, "aclrtBinaryUnLoad");
    POC_DLSYM(aclrtBinaryGetFunction, aclHandle, "aclrtBinaryGetFunction");
    POC_DLSYM(aclrtLaunchKernelWithHostArgs, aclHandle, "aclrtLaunchKernelWithHostArgs");
    POC_DLSYM_OPT(aclrtBinaryGetFunctionCount, aclHandle, "aclrtBinaryGetFunctionCount");
    POC_DLSYM_OPT(aclrtFunctionGetParamCount, aclHandle, "aclrtFunctionGetParamCount");
    POC_DLSYM_OPT(aclrtFunctionGetParamInfo, aclHandle, "aclrtFunctionGetParamInfo");

    void *hcommHandle = nullptr;
    libPath = ascendHome + "/lib64/libhcomm.so";
    if (LoadOneLib(hcommHandle, libPath) != 0) {
        libPath = ascendHome + "/runtime/lib64/hcomm/libhcomm.so";
        if (LoadOneLib(hcommHandle, libPath) != 0) {
            return -1;
        }
    }
    POC_DLSYM(HcommEndpointCreate, hcommHandle, "HcommEndpointCreate");
    POC_DLSYM(HcommEndpointDestroy, hcommHandle, "HcommEndpointDestroy");
    POC_DLSYM(HcommMemReg, hcommHandle, "HcommMemReg");
    POC_DLSYM(HcommMemUnreg, hcommHandle, "HcommMemUnreg");
    POC_DLSYM(HcommMemExport, hcommHandle, "HcommMemExport");
    POC_DLSYM(HcommMemImport, hcommHandle, "HcommMemImport");
    POC_DLSYM(HcommMemUnimport, hcommHandle, "HcommMemUnimport");
    POC_DLSYM(HcommChannelCreate, hcommHandle, "HcommChannelCreate");
    POC_DLSYM(HcommChannelGetStatus, hcommHandle, "HcommChannelGetStatus");
    POC_DLSYM(HcommChannelDestroy, hcommHandle, "HcommChannelDestroy");

    printf("[POC] all dynamic symbols loaded (libascendcl + libhcomm)\n");
    return 0;
}
