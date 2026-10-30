/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * AIV+URMA PoC is licensed under Mulan PSL v2.
 * PoC 用 dlopen/dlsym 加载 libascendcl / libruntime / libhcomm，
 * 类型定义复刻自 MemFabric src/hybm/csrc/under_api/dl_acl_api.h 与 hcomm include/hcomm_res.h。
 */

#ifndef POC_API_H
#define POC_API_H

#include <cstdint>
#include <cstdio>
#include <string>
#include "hcomm/hcomm_res.h"

// ─────────────────────── ACL / RT 类型（复刻 dl_acl_api.h 相关段） ───────────────────────
// 注：内核启动走 ASC 编译器生成的 <<<>>> 胶水（LaunchPocWrite/Read），本文件只保留
// 控制面所需的 acl 基础符号；不再 dlsym aclrtBinaryLoad*/KernelArgs*/LaunchKernelWithConfig

// rtGetDeviceInfo 的 infoType（值复刻自 MemFabric hybm_define.h / aiv_sdma 实测）
constexpr int32_t POC_INFO_TYPE_PHY_DIE_ID = 19;
constexpr int32_t POC_INFO_TYPE_SDID = 26;
constexpr int32_t POC_INFO_TYPE_SERVER_ID = 27;
constexpr int32_t POC_INFO_TYPE_SUPER_POD_ID = 28;
constexpr int32_t POC_INFO_TYPE_MAINBOARD_ID = 39;

// ─────────────────────── dlsym 加载 ───────────────────────
// 可选符号加载：缺失仅 WARN 不退出（调用方需判空并走 fallback）
#define POC_DLSYM_OPT(var, handle, name)                                                                    \
    do {                                                                                                   \
        (var) = reinterpret_cast<decltype(var)>(dlsym((handle), (name)));                                  \
        if ((var) == nullptr) {                                                                            \
            printf("[POC][WARN] dlsym %s not exported by this runtime, caller should fallback\n", (name));  \
        }                                                                                                  \
    } while (0)

#define POC_DLSYM(var, handle, name)                                                                       \
    do {                                                                                                   \
        (var) = reinterpret_cast<decltype(var)>(dlsym((handle), (name)));                                  \
        if ((var) == nullptr) {                                                                            \
            printf("[POC][ERROR] dlsym %s failed, lib=%s, dlerror=%s\n", (name), libPath.c_str(), dlerror()); \
            return -1;                                                                                     \
        }                                                                                                  \
    } while (0)

struct PocDynApi {
    // libascendcl
    int32_t (*aclInit)(const char *);
    int32_t (*aclFinalize)();
    int32_t (*aclrtSetDevice)(int32_t);
    int32_t (*aclrtResetDevice)(int32_t);
    int32_t (*aclrtGetDevice)(int32_t *);
    int32_t (*aclrtMalloc)(void **, size_t, int32_t);
    int32_t (*aclrtFree)(void *);
    int32_t (*aclrtMemcpy)(void *, size_t, const void *, size_t, int32_t);
    int32_t (*aclrtMemset)(void *, size_t, int32_t, size_t);
    int32_t (*aclrtCreateStream)(void **);
    int32_t (*aclrtDestroyStream)(void *);
    int32_t (*aclrtSynchronizeStream)(void *);
    // libruntime
    int32_t (*rtGetDeviceInfo)(uint32_t, int32_t, int32_t, int64_t *);
    int32_t (*aclrtGetPhyDevIdByLogicDevId)(int32_t, int32_t *);
    // libhcomm
    HcommResult (*HcommEndpointCreate)(const EndpointDesc *, EndpointHandle *);
    HcommResult (*HcommEndpointDestroy)(EndpointHandle);
    HcommResult (*HcommMemReg)(EndpointHandle, const char *, const CommMem *, HcommMemHandle *);
    HcommResult (*HcommMemUnreg)(EndpointHandle, HcommMemHandle);
    HcommResult (*HcommMemExport)(EndpointHandle, HcommMemHandle, void **, uint32_t *);
    HcommResult (*HcommMemImport)(EndpointHandle, const void *, uint32_t, CommMem *);
    HcommResult (*HcommMemUnimport)(EndpointHandle, const void *, uint32_t);
    HcommResult (*HcommChannelCreate)(EndpointHandle, CommEngine, HcommChannelDesc *, uint32_t, ChannelHandle *);
    HcommResult (*HcommChannelGetStatus)(const ChannelHandle *, uint32_t, int32_t *);
    HcommResult (*HcommChannelDestroy)(const ChannelHandle *, uint32_t);

    int Load(const std::string &cannLibPath);
};

int PocDynApiLoad(PocDynApi &api, const std::string &ascendHome);

#endif  // POC_API_H
