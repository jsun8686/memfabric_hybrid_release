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
// 注：方案 B 下内核启动改回显式路径——dlsym aclrtCreateBinary/BinaryLoad/BinaryGetFunction/
// LaunchKernelWithHostArgs，加载 ascendc_fatbin_library 生成的正式算子二进制（fatbin），
// 符号按原始名 poc_hcomm_write/poc_hcomm_read 查询（无 ascendc_library auto-gen 改名）。

// rtGetDeviceInfo 的 infoType（值复刻自 MemFabric hybm_define.h / aiv_sdma 实测）
constexpr int32_t POC_INFO_TYPE_PHY_DIE_ID = 19;
constexpr int32_t POC_INFO_TYPE_SDID = 26;
constexpr int32_t POC_INFO_TYPE_SERVER_ID = 27;
constexpr int32_t POC_INFO_TYPE_SUPER_POD_ID = 28;
constexpr int32_t POC_INFO_TYPE_MAINBOARD_ID = 39;

// ─────── 显式 kernel 下发类型（取值逐一核对 CANN 开源 runtime include/external/acl/acl_rt.h） ───────
// aclrtLaunchKernelAttrId
constexpr int32_t POC_LAUNCH_ATTR_SCHEM_MODE = 1;   // ACL_RT_LAUNCH_KERNEL_ATTR_SCHEM_MODE
constexpr int32_t POC_LAUNCH_ATTR_ENGINE_TYPE = 3;  // ACL_RT_LAUNCH_KERNEL_ATTR_ENGINE_TYPE
constexpr int32_t POC_LAUNCH_ATTR_TIMEOUT_US = 8;   // ACL_RT_LAUNCH_KERNEL_ATTR_TIMEOUT_US

// aclrtBinaryLoadOptions（复刻 dl_cann_api_def.h / CANN acl_rt.h）
constexpr int32_t POC_BINARY_LOAD_OPT_LAZY_LOAD = 1;  // ACL_RT_BINARY_LOAD_OPT_LAZY_LOAD
union PocBinaryLoadOptionValue {
    uint32_t isLazyLoad;
    uint32_t magic;
    int32_t cpuKernelMode;
    uint32_t rsv[4];
};
struct PocBinaryLoadOption {
    int32_t type;  // aclrtBinaryLoadOptionType
    PocBinaryLoadOptionValue value;
};
struct PocBinaryLoadOptions {
    PocBinaryLoadOption *options;
    size_t numOpt;
};
// aclrtEngineType
constexpr int32_t POC_ENGINE_TYPE_AIC = 0;          // ACL_RT_ENGINE_TYPE_AIC
constexpr int32_t POC_ENGINE_TYPE_AIV = 1;          // ACL_RT_ENGINE_TYPE_AIV

struct PocTimeoutUs {
    uint32_t timeoutLow;
    uint32_t timeoutHigh;
};

// aclrtLaunchKernelAttrValue（union，最大成员 rsv[4] = 16B）
union PocLaunchKernelAttrValue {
    uint8_t schemMode;
    int32_t engineType;  // aclrtEngineType 为 int 枚举
    PocTimeoutUs timeoutUs;
    uint32_t rsv[4];
};

// aclrtLaunchKernelAttr
struct PocLaunchKernelAttr {
    int32_t id;
    PocLaunchKernelAttrValue value;
};

// aclrtLaunchKernelCfg
struct PocLaunchKernelCfg {
    PocLaunchKernelAttr *attrs;
    size_t numAttrs;
};

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
    // fatbin 显式加载 + AIV 下发（libascendcl 导出；签名对齐 CANN 开源 acl_rt.h，
    // aclrtBinary/aclrtBinHandle/aclrtFuncHandle/aclrtStream/aclrtPlaceHolderInfo* 均为 void* 系）
    void *(*aclrtCreateBinary)(const void *, size_t);
    int32_t (*aclrtDestroyBinary)(void *);
    int32_t (*aclrtBinaryLoad)(const void *, void **);
    // HCCL 950 AIV 路径（hccl_aiv_utils.cc）使用的加载方式：FromFile + LAZY_LOAD 选项
    int32_t (*aclrtBinaryLoadFromFile)(const char *, PocBinaryLoadOptions *, void **);
    int32_t (*aclrtBinaryUnLoad)(void *);
    int32_t (*aclrtBinaryGetFunction)(const void *, const char *, void **);
    int32_t (*aclrtLaunchKernelWithHostArgs)(void *, uint32_t, void *, PocLaunchKernelCfg *, void *, size_t,
                                             void *, size_t);
    // 诊断（可选）：打印 binHandle 下注册的 Function 数量，缺失不影响主流程
    int32_t (*aclrtBinaryGetFunctionCount)(const void *, uint32_t *);
    // 诊断（可选）：查询 runtime 视角的 kernel 参数布局（个数/偏移/大小），用于核对
    // host 侧 argsSize 是否与设备侧签名（含工具链追加的 overflow_status 尾参）匹配
    int32_t (*aclrtFunctionGetParamCount)(const void *, size_t *);
    int32_t (*aclrtFunctionGetParamInfo)(const void *, size_t, size_t *, size_t *);
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
