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
using aclrtBinHandle = void *;
using aclrtFuncHandle = void *;
using aclrtArgsHandle = void *;
using aclrtParamHandle = void *;

enum class PocBinaryLoadOptionType : int32_t {
    LAZY_LOAD = 1,
    MAGIC = 2,  // ACL_RT_BINARY_LOAD_OPT_MAGIC：显式声明二进制内核类型
    CPU_KERNEL_MODE = 3,
};
union PocBinaryLoadOptionValue {
    uint32_t isLazyLoad;
    uint32_t magic;
    int32_t cpuKernelMode;
    uint32_t rsv[4];
};
struct PocBinaryLoadOption {
    PocBinaryLoadOptionType type;
    PocBinaryLoadOptionValue value;
};
struct PocBinaryLoadOptions {
    PocBinaryLoadOption *options;
    size_t numOpt;
};

// AIV(vector core) 设备镜像的 ELF 类型 magic（取值与 acl_rt.h 一致，参照 hcomm 仓 vendor 副本：
// ACL_RT_BINARY_MAGIC_ELF_VECTOR_CORE = 0x41415246U）。加载 device_aiv.o 时必须带此选项，
// 否则 runtime 按默认 AI Core 语义解析，函数表为空 -> aclrtBinaryGetFunction 报 107000
constexpr uint32_t POC_BINARY_MAGIC_ELF_VECTOR_CORE = 0x41415246U;

enum PocLaunchKernelAttrId {
    POC_ATTR_SCHEM_MODE = 1,
    POC_ATTR_ENGINE_TYPE = 3,
    POC_ATTR_TIMEOUT = 7,
};
union PocLaunchKernelAttrValue {
    uint8_t schemMode;
    uint32_t engineType;
    uint16_t timeout;
    uint32_t rsv[4];
};
struct PocLaunchKernelAttr {
    PocLaunchKernelAttrId id;
    PocLaunchKernelAttrValue value;
};
struct PocLaunchKernelCfg {
    PocLaunchKernelAttr *attrs;
    size_t numAttrs;
};

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
    int32_t (*aclrtBinaryLoadFromFile)(const char *, PocBinaryLoadOptions *, aclrtBinHandle *);
    // 裸 ELF 内核镜像（如 device_aiv.o）必须走 FromData + VECTOR_CORE magic（对齐
    // asc-devkit RegisterAscendBinary）；FromFile 的设计输入是 AICPU 内核包 json
    int32_t (*aclrtBinaryLoadFromData)(const char *, size_t, PocBinaryLoadOptions *, aclrtBinHandle *);
    int32_t (*aclrtBinaryGetFunction)(aclrtBinHandle, const char *, aclrtFuncHandle *);
    int32_t (*aclrtKernelArgsInit)(aclrtFuncHandle, aclrtArgsHandle *);
    int32_t (*aclrtKernelArgsAppend)(aclrtArgsHandle, void *, size_t, aclrtParamHandle *);
    int32_t (*aclrtKernelArgsFinalize)(aclrtArgsHandle);
    int32_t (*aclrtLaunchKernelWithConfig)(aclrtFuncHandle, uint32_t, void *, PocLaunchKernelCfg *,
                                           aclrtArgsHandle, void *);
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
