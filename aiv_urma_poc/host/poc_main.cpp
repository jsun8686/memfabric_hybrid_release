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
 * AIV+URMA 直驱 PoC（阶段 0）：
 *   Host 纯 HCOMM 控制面（Endpoint/MemReg/Export/Import + COMM_ENGINE_AIV 通道）
 *   + AIV 内核 AscendC::Hcomm<UBC_CTP> WriteNbi/ReadNbi/Commit/Drain 数据面。
 *   两机各跑一个角色：--role server（数据接收方）/ --role client（数据发起方）。
 */

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "hcomm/hcomm_res.h"
#include "poc_api.h"
#include "poc_common.h"
#include "poc_eid.h"
#include "poc_tcp.h"

// 方案 B：kernel 由 ascendc_fatbin_library 编成独立算子二进制（fatbin .o），host 显式
// aclrtCreateBinary -> aclrtBinaryLoad -> aclrtBinaryGetFunction(原名) ->
// aclrtLaunchKernelWithHostArgs(ACL_RT_ENGINE_TYPE_AIV)，不再使用 <<<>>> 内嵌启动链。

namespace {

uint64_t PocNowUs()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

struct PocOptions {
    std::string role = "client";            // server | client
    std::string peerIp;                     // client 必填
    uint16_t oobPort = POC_DEFAULT_OOB_PORT;
    int32_t device = 0;
    uint64_t size = POC_DEFAULT_BUF_SIZE;
    uint64_t chunk = POC_DEFAULT_CHUNK;
    uint16_t hcommPort = POC_DEFAULT_HCOMM_PORT;
    std::string eidHex;
    std::string rootinfo = "/etc/hccl_rootinfo.json";
    std::string verify = "write";           // write | read
    uint32_t statusTimeoutSec = 120;
    std::string kernelImage;                // fatbin 路径，默认 <exe 同目录>/poc_hcomm_kernel.o
    bool selfTest = false;                  // --self-test：不建链不碰 Hcomm，仅验证 fatbin->AIV->GM 链路
};

void Usage(const char *bin)
{
    printf("usage: %s --role server|client [options]\n"
           "  --peer-ip IP        client: peer server IP (required for client)\n"
           "  --oob-port N        PoC TCP out-of-band port (default %u)\n"
           "  --device N          aclrtSetDevice id (default 0)\n"
           "  --size BYTES        transfer size (default %llu)\n"
           "  --chunk BYTES       per-WriteNbi chunk (default %llu)\n"
           "  --hcomm-port N      hcomm channel OOB socket port (default %u)\n"
           "  --eid 32HEX         override local EID (skip rootinfo)\n"
           "  --rootinfo PATH     rootinfo path (default /etc/hccl_rootinfo.json)\n"
           "  --verify write|read write: client->server write path; read: client pull from server\n"
           "  --status-timeout S  channel ready wait timeout seconds (default %u)\n"
           "  --kernel-image PATH fatbin kernel image (default <exe_dir>/poc_hcomm_kernel.o)\n"
           "  --self-test         自检模式：加载 fatbin 并启动 poc_selftest 内核（不建链、不碰 Hcomm），\n"
           "                    用于切分「下发链路坏」与「hcomm 内核符号未解析」两类故障\n",
           bin, POC_DEFAULT_OOB_PORT, (unsigned long long)POC_DEFAULT_BUF_SIZE,
           (unsigned long long)POC_DEFAULT_CHUNK, POC_DEFAULT_HCOMM_PORT, 120U);
}

int ParseArgs(int argc, char **argv, PocOptions &opt)
{
    for (int i = 1; i < argc; i++) {
        const std::string k = argv[i];
        auto next = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                printf("[POC][ERROR] missing value for %s\n", name);
                exit(1);
            }
            return argv[++i];
        };
        if (k == "--role") {
            opt.role = next("--role");
        } else if (k == "--peer-ip") {
            opt.peerIp = next("--peer-ip");
        } else if (k == "--oob-port") {
            opt.oobPort = static_cast<uint16_t>(atoi(next("--oob-port")));
        } else if (k == "--device") {
            opt.device = atoi(next("--device"));
        } else if (k == "--size") {
            opt.size = strtoull(next("--size"), nullptr, 0);
        } else if (k == "--chunk") {
            opt.chunk = strtoull(next("--chunk"), nullptr, 0);
        } else if (k == "--hcomm-port") {
            opt.hcommPort = static_cast<uint16_t>(atoi(next("--hcomm-port")));
        } else if (k == "--eid") {
            opt.eidHex = next("--eid");
        } else if (k == "--rootinfo") {
            opt.rootinfo = next("--rootinfo");
        } else if (k == "--verify") {
            opt.verify = next("--verify");
        } else if (k == "--status-timeout") {
            opt.statusTimeoutSec = static_cast<uint32_t>(atoi(next("--status-timeout")));
        } else if (k == "--kernel-image") {
            opt.kernelImage = next("--kernel-image");
        } else if (k == "--self-test") {
            opt.selfTest = true;
        } else if (k == "--help" || k == "-h") {
            Usage(argv[0]);
            exit(0);
        } else {
            printf("[POC][ERROR] unknown arg: %s\n", k.c_str());
            Usage(argv[0]);
            return -1;
        }
    }
    if (opt.role != "server" && opt.role != "client") {
        printf("[POC][ERROR] --role must be server or client\n");
        return -1;
    }
    if (opt.role == "client" && opt.peerIp.empty()) {
        printf("[POC][ERROR] client requires --peer-ip\n");
        return -1;
    }
    if (opt.verify != "write" && opt.verify != "read") {
        printf("[POC][ERROR] --verify must be write or read\n");
        return -1;
    }
    if (opt.size < sizeof(uint64_t) || opt.chunk == 0) {
        printf("[POC][ERROR] --size must be >= 8 and --chunk must be > 0\n");
        return -1;
    }
    return 0;
}

// ─────────────────── OOB 交换消息 ───────────────────
struct PocExchangeMsg {
    uint32_t magic;          // POC_MSG_MAGIC
    EndpointDesc endpoint;   // 本端 EndpointDesc（含 EID）
    uint32_t memDescLen;
    uint16_t hcommPort;      // 本端 hcomm OOB 端口（server 为监听端口）
    uint16_t reserved;
};

int ExchangePeerInfo(int fd, bool isServer, const EndpointDesc &localDesc, uint32_t memDescLen,
                     const void *memDesc, uint16_t localHcommPort, EndpointDesc &peerDesc,
                     std::vector<uint8_t> &peerMemDesc, uint16_t &peerHcommPort)
{
    PocExchangeMsg out {};
    out.magic = POC_MSG_MAGIC;
    out.endpoint = localDesc;
    out.memDescLen = memDescLen;
    out.hcommPort = localHcommPort;

    std::vector<uint8_t> sendBuf(sizeof(out) + memDescLen);
    memcpy(sendBuf.data(), &out, sizeof(out));
    memcpy(sendBuf.data() + sizeof(out), memDesc, memDescLen);

    if (PocTcp::SendAll(fd, sendBuf.data(), sendBuf.size()) != 0) {
        printf("[POC][ERROR] oob send peer info failed\n");
        return -1;
    }

    PocExchangeMsg in {};
    if (PocTcp::RecvAll(fd, &in, sizeof(in)) != 0) {
        printf("[POC][ERROR] oob recv peer header failed\n");
        return -1;
    }
    if (in.magic != POC_MSG_MAGIC || in.memDescLen == 0 || in.memDescLen > POC_MAX_MEM_DESC) {
        printf("[POC][ERROR] invalid peer header, magic=0x%x descLen=%u\n", in.magic, in.memDescLen);
        return -1;
    }
    peerMemDesc.resize(in.memDescLen);
    if (PocTcp::RecvAll(fd, peerMemDesc.data(), peerMemDesc.size()) != 0) {
        printf("[POC][ERROR] oob recv peer memDesc failed\n");
        return -1;
    }
    peerDesc = in.endpoint;
    peerHcommPort = in.hcommPort;
    (void)isServer;
    return 0;
}

void FillPattern(std::vector<uint64_t> &host, uint64_t words, uint64_t salt)
{
    host.assign(words, 0);
    for (uint64_t i = 0; i < words; i++) {
        host[i] = (i * 0x9E3779B97F4A7C15ULL) ^ salt;
    }
}

int CheckBuffer(PocDynApi &api, void *devBuf, uint64_t size, uint64_t salt, size_t &firstBadIdx)
{
    std::vector<uint64_t> host(size / sizeof(uint64_t) + 1, 0);
    const int32_t ret = api.aclrtMemcpy(host.data(), host.size() * sizeof(uint64_t), devBuf, size, 2 /*D2H*/);
    if (ret != 0) {
        printf("[POC][ERROR] D2H memcpy failed, ret=%d\n", ret);
        return -1;
    }
    const size_t words = size / sizeof(uint64_t);
    for (size_t i = 0; i < words; i++) {
        const uint64_t expect = ((uint64_t)i * 0x9E3779B97F4A7C15ULL) ^ salt;
        if (host[i] != expect) {
            firstBadIdx = i;
            // dump 诊断：首个坏 word 与前 4 个 word 实际值，区分"未写入（全 0）/错位/内容翻转"
            printf("[POC][DEBUG] first mismatch at word %zu: actual=0x%016llx expected=0x%016llx\n", i,
                   (unsigned long long)host[i], (unsigned long long)expect);
            for (size_t j = 0; j < 4 && j < words; j++) {
                printf("[POC][DEBUG] word[%zu]=0x%016llx\n", j, (unsigned long long)host[j]);
            }
            return 1;
        }
    }
    return 0;
}

// read 模式的完成握手：client 完成 ReadNbi/Drain/校验后通知 server，server 才能释放内存与通道
int NotifyReadResult(int fd, bool success)
{
    const char sig = success ? 'D' : 'F';
    if (PocTcp::SendAll(fd, &sig, 1) != 0) {
        printf("[POC][ERROR] notify server read result (%c) failed\n", sig);
        return -1;
    }
    return 0;
}

// fatbin 默认路径：<可执行文件同目录>/poc_hcomm_kernel.o（build.sh 已拷贝到位）
std::string DefaultKernelImagePath()
{
    char exePath[4096] = {0};
    const ssize_t n = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (n <= 0) {
        return "poc_hcomm_kernel.o";
    }
    exePath[n] = '\0';
    std::string dir(exePath);
    const auto pos = dir.find_last_of('/');
    if (pos != std::string::npos) {
        dir.resize(pos);
    }
    return dir + "/poc_hcomm_kernel.o";
}

// 方案 B 显式加载：aclrtCreateBinary -> aclrtBinaryLoad -> aclrtBinaryGetFunction(原名)。
// binBuf 以出参带回并随 main 作用域存活，避免运行时在 load 之后再次访问二进制数据时悬空。
int LoadPocKernel(PocDynApi &api, const std::string &imagePath, const char *funcName,
                  std::vector<char> &binBuf, void *&binHandle, void *&func)
{
    std::ifstream file(imagePath, std::ios::binary | std::ios::ate);
    if (!file.good()) {
        printf("[POC][ERROR] open kernel image failed: %s\n", imagePath.c_str());
        return -1;
    }
    const auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    binBuf.resize(static_cast<size_t>(size));
    if (!file.read(binBuf.data(), size)) {
        printf("[POC][ERROR] read kernel image failed: %s\n", imagePath.c_str());
        return -1;
    }
    printf("[POC] kernel image: %s (%zu bytes)\n", imagePath.c_str(), binBuf.size());

    // 优先对齐 HCCL 950 AIV 生产路径（hccl_aiv_utils.cc）：aclrtBinaryLoadFromFile + LAZY_LOAD；
    // 符号缺失或加载失败时回退 CreateBinary + BinaryLoad 内存路径
    int32_t ret = -1;
    if (api.aclrtBinaryLoadFromFile != nullptr) {
        PocBinaryLoadOption loadOpt {};
        loadOpt.type = POC_BINARY_LOAD_OPT_LAZY_LOAD;
        loadOpt.value.isLazyLoad = 1;
        PocBinaryLoadOptions loadOpts {&loadOpt, 1};
        ret = api.aclrtBinaryLoadFromFile(imagePath.c_str(), &loadOpts, &binHandle);
        if (ret == 0 && binHandle != nullptr) {
            printf("[POC] kernel binary loaded via aclrtBinaryLoadFromFile(LAZY_LOAD)\n");
        } else {
            printf("[POC][WARN] aclrtBinaryLoadFromFile ret=%d binHandle=%p, fallback to CreateBinary+BinaryLoad\n",
                   ret, binHandle);
            binHandle = nullptr;
        }
    }
    if (binHandle == nullptr) {
        void *binDesc = api.aclrtCreateBinary(binBuf.data(), binBuf.size());
        if (binDesc == nullptr) {
            printf("[POC][ERROR] aclrtCreateBinary failed\n");
            return -1;
        }
        ret = api.aclrtBinaryLoad(binDesc, &binHandle);
        (void)api.aclrtDestroyBinary(binDesc);
        if (ret != 0 || binHandle == nullptr) {
            printf("[POC][ERROR] aclrtBinaryLoad ret=%d binHandle=%p\n", ret, binHandle);
            return -1;
        }
        printf("[POC] kernel binary loaded via aclrtCreateBinary+aclrtBinaryLoad\n");
    }
    // 诊断：打印 fatbin 中注册的 Function 数量（符号缺失的旧版本 runtime 上自动跳过）
    if (api.aclrtBinaryGetFunctionCount != nullptr) {
        uint32_t fnCount = 0;
        if (api.aclrtBinaryGetFunctionCount(binHandle, &fnCount) == 0) {
            printf("[POC] kernel image functions: %u\n", fnCount);
        }
    }
    ret = api.aclrtBinaryGetFunction(binHandle, funcName, &func);
    if (ret != 0 || func == nullptr) {
        // 若再次见到 107000：先用 readelf -sW 确认 fatbin 中是否存在同名的 GLOBAL FUNC 符号
        printf("[POC][ERROR] aclrtBinaryGetFunction(%s) ret=%d\n", funcName, ret);
        return -1;
    }
    printf("[POC] kernel function ready: %s\n", funcName);
    // 诊断：runtime 视角的参数布局。非 cpu 模式工具链会在 kernel 签名尾部追加
    // GM_ADDR overflow_status（8B），预期 param0=PocKernelArgs(48B@0)、param1=8B@48；
    // host 侧 argsSize 只含用户参数（48B），尾参由 runtime 自行处理
    if (api.aclrtFunctionGetParamCount != nullptr && api.aclrtFunctionGetParamInfo != nullptr) {
        size_t paramCount = 0;
        if (api.aclrtFunctionGetParamCount(func, &paramCount) == 0) {
            printf("[POC] kernel param count: %zu (expect %zu user + 1 overflow tail)\n",
                   paramCount, sizeof(PocKernelArgs) / sizeof(uint64_t));
            for (size_t i = 0; i < paramCount && i < 4; ++i) {
                size_t off = 0, sz = 0;
                if (api.aclrtFunctionGetParamInfo(func, i, &off, &sz) == 0) {
                    printf("[POC]   param[%zu]: offset=%zu size=%zu\n", i, off, sz);
                }
            }
        }
    }
    return 0;
}

// 诊断：状态/阶段标记解码（与 common/poc_common.h、kernel/poc_kernel_common.h 保持一致）
std::string DecodeKernelStatus(uint64_t st)
{
    char buf[128];
    switch (st) {
        case POC_STATUS_PENDING:  // 0：连入口标记都没写上
            return "kernel 未执行（或 statusAddr 对 kernel 不可见/为 0）";
        case POC_STATUS_OK:
            return "OK";
        case POC_STAGE_ENTERED:
            return "已进 kernel 入口，停在 Hcomm Init 之前/之中（疑似 Init 卡死或设备异常）";
        case POC_STAGE_INIT_OK:
            return "Init 成功，停在分块 Nbi 循环中（疑似 WriteNbi/ReadNbi 卡死）";
        case POC_STAGE_QUEUE_OK:
            return "Nbi 全部入队，停在 Commit 之前/之中";
        case POC_STAGE_COMMIT_OK:
            return "Commit 成功，停在 Drain（超时/通道异常未完成）";
        default:
            break;
    }
    if (st >= POC_STATUS_FAIL_BASE) {
        const uint64_t code = st - POC_STATUS_FAIL_BASE;
        const char *stage = (code >= 0x400) ? "Drain" : (code >= 0x300) ? "Commit"
                          : (code >= 0x200) ? "Nbi 分块" : (code >= 0x100) ? "Init" : "?";
        snprintf(buf, sizeof(buf), "%s 失败（内嵌码 %#llx）", stage, (unsigned long long)code);
        return buf;
    }
    snprintf(buf, sizeof(buf), "未知状态 %#llx", (unsigned long long)st);
    return buf;
}

// AIV 引擎下发：attr 取值复刻 CANN 开源 acl_rt.h（SCHEM_MODE=1 / ENGINE_TYPE=3 / AIV=1 /
// TIMEOUT_US=8），并对齐 HCCL 950 AIV 生产下发（hicann/hcomm hccl_aiv_utils.cc 的三 attr 组合）
int LaunchPocKernel(PocDynApi &api, void *func, void *stream, void *hostArgs, size_t argsSize)
{
    // 三 attr 完全对齐 HCCL 950 AIV 生产下发（hccl_aiv_utils.cc::ExecuteKernelLaunchInner）：
    // SCHEM_MODE=1 + TIMEOUT_US（兜底防永久 hang）+ ENGINE_TYPE=AIV
    constexpr uint32_t POC_KERNEL_TIMEOUT_US = 60U * 1000U * 1000U;  // 60s
    PocLaunchKernelAttr attrs[3] {};
    attrs[0].id = POC_LAUNCH_ATTR_SCHEM_MODE;
    attrs[0].value.schemMode = 1;
    attrs[1].id = POC_LAUNCH_ATTR_TIMEOUT_US;
    attrs[1].value.timeoutUs.timeoutLow = POC_KERNEL_TIMEOUT_US;
    attrs[1].value.timeoutUs.timeoutHigh = 0;
    attrs[2].id = POC_LAUNCH_ATTR_ENGINE_TYPE;
    attrs[2].value.engineType = POC_ENGINE_TYPE_AIV;
    PocLaunchKernelCfg cfg {attrs, 3};
    return api.aclrtLaunchKernelWithHostArgs(func, 1, stream, &cfg, hostArgs, argsSize, nullptr, 0);
}

}  // namespace

int main(int argc, char **argv)
{
    PocOptions opt;
    if (ParseArgs(argc, argv, opt) != 0) {
        return 1;
    }
    const bool isServer = (opt.role == "server");
    const uint64_t salt = isServer ? 0xA5A5A5A5A5A5A5A5ULL : 0x5A5A5A5A5A5A5A5AULL;
    printf("[POC] role=%s verify=%s size=%llu chunk=%llu device=%d\n", opt.role.c_str(), opt.verify.c_str(),
           (unsigned long long)opt.size, (unsigned long long)opt.chunk, opt.device);

    PocDynApi api;
    const char *ascendHomeEnv = std::getenv("ASCEND_HOME_PATH");
    const std::string ascendHome = (ascendHomeEnv != nullptr && ascendHomeEnv[0] != '\0')
                                       ? std::string(ascendHomeEnv)
                                       : std::string("/usr/local/Ascend/ascend-toolkit/latest");
    if (api.Load(ascendHome) != 0) {
        return 1;
    }

    int exitCode = 1;
    bool aclInited = false;
    bool deviceSet = false;
    void *stream = nullptr;
    void *binHandle = nullptr;        // fatbin 句柄（仅 client 侧加载）
    std::vector<char> kernelBin;      // fatbin 文件内容，随 main 作用域存活
    void *devBuf = nullptr;
    void *statusBuf = nullptr;
    EndpointHandle endpoint = nullptr;
    HcommMemHandle memHandle = nullptr;
    void *memDesc = nullptr;
    uint32_t memDescLen = 0;
    bool memImported = false;
    std::vector<uint8_t> peerMemDesc;
    ChannelHandle channel = 0;
    int oobFd = -1;

    do {
        // ── 1. ACL 基础初始化 ──
        int32_t ret = api.aclInit(nullptr);
        if (ret != 0) {
            printf("[POC][ERROR] aclInit ret=%d\n", ret);
            break;
        }
        aclInited = true;
        ret = api.aclrtSetDevice(opt.device);
        if (ret != 0) {
            printf("[POC][ERROR] aclrtSetDevice(%d) ret=%d\n", opt.device, ret);
            break;
        }
        deviceSet = true;

        // ── 2. 本地设备信息 ──
        int32_t userId = -1;
        ret = api.aclrtGetDevice(&userId);
        if (ret != 0 || userId < 0) {
            printf("[POC][ERROR] aclrtGetDevice ret=%d\n", ret);
            break;
        }
        // 对齐 MemFabric urma_topo_manager.cpp SetDeviceIdentity：符号缺失/调用失败时 fallback 用 logicId
        int32_t phyId = userId;
        if (api.aclrtGetPhyDevIdByLogicDevId != nullptr) {
            ret = api.aclrtGetPhyDevIdByLogicDevId(userId, &phyId);
            if (ret != 0) {
                printf("[POC][WARN] aclrtGetPhyDevIdByLogicDevId ret=%d, fallback phyId=logicId=%d\n", ret, userId);
                phyId = userId;
            }
        } else {
            printf("[POC][WARN] aclrtGetPhyDevIdByLogicDevId unavailable, fallback phyId=logicId=%d\n", userId);
        }
        int64_t v = 0;
        if (api.rtGetDeviceInfo(static_cast<uint32_t>(userId), 0, POC_INFO_TYPE_SDID, &v) != 0) {
            printf("[POC][ERROR] rtGetDeviceInfo(SDID) failed\n");
            break;
        }
        const uint32_t sdid = static_cast<uint32_t>(v);
        if (api.rtGetDeviceInfo(static_cast<uint32_t>(userId), 0, POC_INFO_TYPE_SERVER_ID, &v) != 0) {
            printf("[POC][ERROR] rtGetDeviceInfo(SERVER_ID) failed\n");
            break;
        }
        const uint32_t serverId = static_cast<uint32_t>(v);
        if (api.rtGetDeviceInfo(static_cast<uint32_t>(userId), 0, POC_INFO_TYPE_SUPER_POD_ID, &v) != 0) {
            printf("[POC][ERROR] rtGetDeviceInfo(SUPER_POD_ID) failed\n");
            break;
        }
        const uint32_t superPodId = static_cast<uint32_t>(v);
        int64_t mainboardId = 0;
        (void)api.rtGetDeviceInfo(static_cast<uint32_t>(userId), 0, POC_INFO_TYPE_MAINBOARD_ID, &mainboardId);
        printf("[POC] userId=%d phyId=%d sdid=%u serverId=%u superPodId=%u mainboardId=%lld\n", userId, phyId, sdid,
               serverId, superPodId, static_cast<long long>(mainboardId));

        // ── 自检模式（--self-test）：不建链、不碰 Hcomm，仅验证 fatbin -> AIV 下发 -> GM 写回 ──
        // 用于切分故障域：自检 PASS 但 hcomm 内核不执行 => hcomm 内核设备侧符号问题；
        // 自检也 FAIL => fatbin 格式/下发链路本身问题
        if (opt.selfTest) {
            ret = api.aclrtMalloc(&statusBuf, 64, 0);
            if (ret != 0 || statusBuf == nullptr) {
                printf("[POC][ERROR] aclrtMalloc status ret=%d\n", ret);
                break;
            }
            (void)api.aclrtMemset(statusBuf, 64, 0, 64);
            ret = api.aclrtCreateStream(&stream);
            if (ret != 0 || stream == nullptr) {
                printf("[POC][ERROR] aclrtCreateStream ret=%d\n", ret);
                break;
            }
            const std::string kernelImage =
                opt.kernelImage.empty() ? DefaultKernelImagePath() : opt.kernelImage;
            void *func = nullptr;
            if (LoadPocKernel(api, kernelImage, "poc_selftest", kernelBin, binHandle, func) != 0) {
                break;
            }
            constexpr uint64_t POC_SELFTEST_MAGIC = 0x5154A5155154A515ULL;
            uint64_t selfArgs[2] = {reinterpret_cast<uint64_t>(statusBuf), POC_SELFTEST_MAGIC};
            printf("[POC] self-test launch args: status=%#llx magic=%#llx\n",
                   (unsigned long long)selfArgs[0], (unsigned long long)selfArgs[1]);
            const uint64_t t0 = PocNowUs();
            ret = LaunchPocKernel(api, func, stream, selfArgs, sizeof(selfArgs));
            if (ret != 0) {
                printf("[POC][ERROR] self-test aclrtLaunchKernelWithHostArgs ret=%d\n", ret);
                break;
            }
            ret = api.aclrtSynchronizeStream(stream);
            const uint64_t t1 = PocNowUs();
            if (ret != 0) {
                printf("[POC][ERROR] self-test SynchronizeStream ret=%d\n", ret);
                break;
            }
            uint64_t selfDump[8] = {0};
            ret = api.aclrtMemcpy(selfDump, sizeof(selfDump), statusBuf, sizeof(selfDump), 2 /*D2H*/);
            if (ret != 0) {
                printf("[POC][ERROR] self-test read status ret=%d\n", ret);
                break;
            }
            printf("[POC] self-test kernel done in %llu us, status[0]=%#llx\n",
                   (unsigned long long)(t1 - t0), (unsigned long long)selfDump[0]);
            if (selfDump[0] == POC_SELFTEST_MAGIC) {
                printf("[POC] SELF-TEST PASS: fatbin 加载 + AIV 下发 + GM 写回链路完好；\n"
                       "[POC] hcomm 内核不执行请查设备侧符号（build.sh 的 UNDEF 检查输出）\n");
                exitCode = 0;
            } else {
                printf("[POC][ERROR] SELF-TEST FAIL: kernel 未执行（status[0]=%#llx），\n"
                       "[POC] 问题在 fatbin 格式/下发链路本身，与 hcomm 无关\n",
                       (unsigned long long)selfDump[0]);
            }
            break;
        }

        // ── 3. EID ──
        std::array<uint8_t, POC_EID_LEN> eid {};
        if (PocGetEid(opt.eidHex, opt.rootinfo, static_cast<uint32_t>(phyId), eid) != 0) {
            break;
        }
        printf("[POC] local EID: %s\n", PocEidToString(eid).c_str());

        // ── 4. Endpoint（UBC_CTP + EID + DEVICE 位置） ──
        EndpointDesc localDesc;
        if (EndpointDescInit(&localDesc, 1) != 0) {
            printf("[POC][ERROR] EndpointDescInit failed\n");
            break;
        }
        localDesc.protocol = COMM_PROTOCOL_UBC_CTP;
        localDesc.commAddr.type = COMM_ADDR_TYPE_EID;
        memcpy(localDesc.commAddr.eid, eid.data(), POC_EID_LEN);
        localDesc.loc.locType = ENDPOINT_LOC_TYPE_DEVICE;
        localDesc.loc.device.devPhyId = static_cast<uint32_t>(phyId);
        localDesc.loc.device.superDevId = sdid;
        localDesc.loc.device.serverIdx = serverId;
        localDesc.loc.device.superPodIdx = superPodId;

        ret = api.HcommEndpointCreate(&localDesc, &endpoint);
        if (ret != 0 || endpoint == nullptr) {
            printf("[POC][ERROR] HcommEndpointCreate ret=%d\n", ret);
            break;
        }
        printf("[POC] HcommEndpointCreate ok, handle=%p\n", endpoint);

        // ── 5. 数据 buffer + 状态 buffer ──
        ret = api.aclrtMalloc(&devBuf, opt.size, 0 /*HUGE_FIRST*/);
        if (ret != 0 || devBuf == nullptr) {
            printf("[POC][ERROR] aclrtMalloc(%llu) ret=%d\n", (unsigned long long)opt.size, ret);
            break;
        }
        ret = api.aclrtMalloc(&statusBuf, 64, 0);
        if (ret != 0 || statusBuf == nullptr) {
            printf("[POC][ERROR] aclrtMalloc status ret=%d\n", ret);
            break;
        }
        (void)api.aclrtMemset(statusBuf, 64, 0, 64);
        ret = api.aclrtCreateStream(&stream);
        if (ret != 0 || stream == nullptr) {
            printf("[POC][ERROR] aclrtCreateStream ret=%d\n", ret);
            break;
        }

        // 预填 pattern：write 模式 client 作为数据源；read 模式 server 作为数据源
        const bool localIsDataSource = (opt.verify == "write") ? !isServer : isServer;
        if (localIsDataSource) {
            std::vector<uint64_t> host;
            FillPattern(host, opt.size / sizeof(uint64_t), salt);
            ret = api.aclrtMemcpy(devBuf, opt.size, host.data(), host.size() * sizeof(uint64_t), 1 /*H2D*/);
            if (ret != 0) {
                printf("[POC][ERROR] H2D pattern ret=%d\n", ret);
                break;
            }
        } else {
            (void)api.aclrtMemset(devBuf, opt.size, 0, opt.size);
        }

        // ── 6. 内存注册/导出 ──
        CommMem commMem {};
        commMem.type = COMM_MEM_TYPE_DEVICE;
        commMem.addr = devBuf;
        commMem.size = opt.size;
        ret = api.HcommMemReg(endpoint, "poc_buf", &commMem, &memHandle);
        if (ret != 0 || memHandle == nullptr) {
            printf("[POC][ERROR] HcommMemReg ret=%d\n", ret);
            break;
        }
        ret = api.HcommMemExport(endpoint, memHandle, &memDesc, &memDescLen);
        if (ret != 0 || memDesc == nullptr || memDescLen == 0 || memDescLen > POC_MAX_MEM_DESC) {
            printf("[POC][ERROR] HcommMemExport ret=%d descLen=%u\n", ret, memDescLen);
            break;
        }
        printf("[POC] MemReg+Export ok, descLen=%u, devBuf=%p\n", memDescLen, devBuf);

        // ── 7. TCP 带外交换 ──
        std::string tcpErr;
        if (isServer) {
            oobFd = PocTcp::ListenAndAccept(opt.oobPort, tcpErr);
        } else {
            oobFd = PocTcp::Connect(opt.peerIp, opt.oobPort, tcpErr);
        }
        if (oobFd < 0) {
            printf("[POC][ERROR] oob tcp failed: %s\n", tcpErr.c_str());
            break;
        }
        printf("[POC] oob tcp connected, fd=%d\n", oobFd);

        EndpointDesc peerDesc;
        uint16_t peerHcommPort = 0;
        if (ExchangePeerInfo(oobFd, isServer, localDesc, memDescLen, memDesc, opt.hcommPort, peerDesc, peerMemDesc,
                             peerHcommPort) != 0) {
            break;
        }
        printf("[POC] peer info ok, peerHcommPort=%u peerProtocol=%d peerAddrType=%d\n", peerHcommPort,
               static_cast<int>(peerDesc.protocol), static_cast<int>(peerDesc.commAddr.type));

        // ── 8. 导入对端内存 ──
        CommMem remoteMem {};
        ret = api.HcommMemImport(endpoint, peerMemDesc.data(), peerMemDesc.size(), &remoteMem);
        if (ret != 0) {
            printf("[POC][ERROR] HcommMemImport ret=%d\n", ret);
            break;
        }
        memImported = true;
        printf("[POC] MemImport ok, remote addr=%p size=%llu type=%d\n", remoteMem.addr,
               (unsigned long long)remoteMem.size, static_cast<int>(remoteMem.type));

        // ── 9. 创建 AIV 引擎通道 ──
        HcommChannelDesc chDesc;
        if (HcommChannelDescInit(&chDesc, 1) != 0) {
            printf("[POC][ERROR] HcommChannelDescInit failed\n");
            break;
        }
        chDesc.remoteEndpoint = peerDesc;
        chDesc.notifyNum = 1;
        chDesc.exchangeAllMems = true;  // 两端 endpoint 各只注册 1 块内存
        chDesc.role = isServer ? HCOMM_SOCKET_ROLE_SERVER : HCOMM_SOCKET_ROLE_CLIENT;
        chDesc.port = opt.hcommPort;  // server: 本地监听端口；client: 对端监听端口（两端配同一值）
        chDesc.qos = 0;
        chDesc.channelName = "poc_aiv_urma";

        ret = api.HcommChannelCreate(endpoint, COMM_ENGINE_AIV, &chDesc, 1, &channel);
        if (ret != 0 || channel == 0) {
            printf("[POC][ERROR] HcommChannelCreate(COMM_ENGINE_AIV) ret=%d\n", ret);
            break;
        }
        printf("[POC] HcommChannelCreate ok, channel=0x%llx, polling status ...\n",
               (unsigned long long)channel);

        // ── 10. 轮询通道就绪（HCOMM_CHANNEL_STATUS_READY = 0） ──
        bool ready = false;
        for (uint32_t waited = 0; waited < opt.statusTimeoutSec * 10U; waited++) {
            int32_t status = -1;
            ret = api.HcommChannelGetStatus(&channel, 1, &status);
            if (ret != 0) {
                printf("[POC][ERROR] HcommChannelGetStatus ret=%d\n", ret);
                break;
            }
            if (status == 0) {
                ready = true;
                break;
            }
            if (waited % 20U == 0U) {
                printf("[POC] channel status=%d (not ready yet) ...\n", status);
            }
            usleep(100 * 1000);
        }
        if (!ready) {
            printf("[POC][ERROR] channel not ready within %us\n", opt.statusTimeoutSec);
            break;
        }
        printf("[POC] channel READY\n");

        // ── 11. 数据面（client 侧 launch 内核；server 侧只做数据准备与校验） ──
        const bool iAmClient = !isServer;
        if (iAmClient) {
            // read 模式下等 server 先就绪 pattern
            if (opt.verify == "read") {
                char sig = 0;
                if (PocTcp::RecvAll(oobFd, &sig, 1) != 0 || sig != 'R') {
                    printf("[POC][ERROR] wait server-ready signal failed\n");
                    break;
                }
            }

            // 方案 B：channel READY 后加载 fatbin 并取 Function（仅 client 需要启动内核）
            const std::string kernelImage =
                opt.kernelImage.empty() ? DefaultKernelImagePath() : opt.kernelImage;
            const char *funcName = (opt.verify == "write") ? "poc_hcomm_write" : "poc_hcomm_read";
            void *func = nullptr;
            if (LoadPocKernel(api, kernelImage, funcName, kernelBin, binHandle, func) != 0) {
                if (opt.verify == "read") {
                    (void)NotifyReadResult(oobFd, false);
                }
                break;
            }

            PocKernelArgs args {};
            args.channel = channel;
            args.localAddr = reinterpret_cast<uint64_t>(devBuf);
            args.remoteAddr = reinterpret_cast<uint64_t>(remoteMem.addr);
            args.size = opt.size;
            args.chunkSize = opt.chunk;
            args.statusAddr = reinterpret_cast<uint64_t>(statusBuf);
            printf("[POC] launch args: channel=%#llx local=%#llx remote=%#llx size=%llu chunk=%llu status=%#llx\n",
                   (unsigned long long)args.channel, (unsigned long long)args.localAddr,
                   (unsigned long long)args.remoteAddr, (unsigned long long)args.size,
                   (unsigned long long)args.chunkSize, (unsigned long long)args.statusAddr);

            const uint64_t t0 = PocNowUs();
            ret = LaunchPocKernel(api, func, stream, &args, sizeof(args));
            if (ret != 0) {
                printf("[POC][ERROR] aclrtLaunchKernelWithHostArgs(%s) ret=%d\n", funcName, ret);
                if (opt.verify == "read") {
                    (void)NotifyReadResult(oobFd, false);
                }
                break;
            }
            ret = api.aclrtSynchronizeStream(stream);
            if (ret != 0) {
                printf("[POC][ERROR] SynchronizeStream ret=%d\n", ret);
                if (opt.verify == "read") {
                    (void)NotifyReadResult(oobFd, false);
                }
                break;
            }
            const uint64_t t1 = PocNowUs();

            // 状态区共 64B：[0]=状态/阶段标记，[1..4]=kernel 回显的实际收参（诊断）
            uint64_t statusDump[8] = {0};
            ret = api.aclrtMemcpy(statusDump, sizeof(statusDump), statusBuf, sizeof(statusDump), 2 /*D2H*/);
            if (ret != 0) {
                printf("[POC][ERROR] read kernel status ret=%d\n", ret);
                if (opt.verify == "read") {
                    (void)NotifyReadResult(oobFd, false);
                }
                break;
            }
            const uint64_t statusHost = statusDump[0];
            printf("[POC] kernel done in %llu us, status=%llu (%s)\n", (unsigned long long)(t1 - t0),
                   (unsigned long long)statusHost, DecodeKernelStatus(statusHost).c_str());
            printf("[POC] kernel echo args: size=%llu chunk=%llu channel=%#llx statusAddr=%#llx\n",
                   (unsigned long long)statusDump[1], (unsigned long long)statusDump[2],
                   (unsigned long long)statusDump[3], (unsigned long long)statusDump[4]);
            if (statusHost != POC_STATUS_OK) {
                printf("[POC][ERROR] kernel status not OK: 0x%llx (%s)\n", (unsigned long long)statusHost,
                       DecodeKernelStatus(statusHost).c_str());
                if (opt.verify == "read") {
                    (void)NotifyReadResult(oobFd, false);
                }
                break;
            }

            if (opt.verify == "read") {
                // 本地校验（read 模式：本地是校验方，salt 是 server 的）
                size_t badIdx = 0;
                const int chk = CheckBuffer(api, devBuf, opt.size, 0xA5A5A5A5A5A5A5A5ULL, badIdx);
                // 无论成功失败都通知server，保证远端buffer/channel在client完成读操作前保持存活
                if (NotifyReadResult(oobFd, chk == 0) != 0) {
                    break;
                }
                if (chk != 0) {
                    printf("[POC][RESULT][FAIL] read verify mismatch, chk=%d firstBadWord=%zu\n", chk, badIdx);
                    break;
                }
                printf("[POC][RESULT][PASS] read path verified (%llu bytes)\n", (unsigned long long)opt.size);
            } else {
                // 通知 server 校验
                const char sig = 'R';
                if (PocTcp::SendAll(oobFd, &sig, 1) != 0) {
                    printf("[POC][ERROR] notify server failed\n");
                    break;
                }
            }
        } else {
            // server 侧
            if (opt.verify == "write") {
                char sig = 0;
                printf("[POC] server waiting client write ...\n");
                if (PocTcp::RecvAll(oobFd, &sig, 1) != 0 || sig != 'R') {
                    printf("[POC][ERROR] wait client-done signal failed\n");
                    break;
                }
                size_t badIdx = 0;
                const int chk = CheckBuffer(api, devBuf, opt.size, 0x5A5A5A5A5A5A5A5AULL, badIdx);
                if (chk != 0) {
                    printf("[POC][RESULT][FAIL] write verify mismatch, chk=%d firstBadWord=%zu\n", chk, badIdx);
                    break;
                }
                printf("[POC][RESULT][PASS] write path verified (%llu bytes)\n", (unsigned long long)opt.size);
            } else {
                const char sig = 'R';
                if (PocTcp::SendAll(oobFd, &sig, 1) != 0) {
                    printf("[POC][ERROR] notify client failed\n");
                    break;
                }
                printf("[POC] server pattern ready, waiting client pull ...\n");

                // client完成ReadNbi/Drain/本地校验后返回D，收到前必须保持远端buffer/channel存活
                char done = 0;
                if (PocTcp::RecvAll(oobFd, &done, 1) != 0) {
                    printf("[POC][ERROR] wait client read-done signal failed\n");
                    break;
                }
                if (done != 'D') {
                    printf("[POC][ERROR] client read failed or returned unexpected signal: %c\n",
                           (done != 0) ? done : '?');
                    break;
                }
                printf("[POC] client pull done, server resources can be released\n");
            }
        }

        exitCode = 0;
    } while (false);

    // ── 清理 ──
    if (channel != 0) {
        (void)api.HcommChannelDestroy(&channel, 1);
    }
    if (memImported && !peerMemDesc.empty()) {
        (void)api.HcommMemUnimport(endpoint, peerMemDesc.data(), static_cast<uint32_t>(peerMemDesc.size()));
    }
    if (memHandle != nullptr) {
        (void)api.HcommMemUnreg(endpoint, memHandle);
    }
    if (endpoint != nullptr) {
        (void)api.HcommEndpointDestroy(endpoint);
    }
    if (binHandle != nullptr) {
        (void)api.aclrtBinaryUnLoad(binHandle);
    }
    if (stream != nullptr) {
        (void)api.aclrtDestroyStream(stream);
    }
    if (statusBuf != nullptr) {
        (void)api.aclrtFree(statusBuf);
    }
    if (devBuf != nullptr) {
        (void)api.aclrtFree(devBuf);
    }
    PocTcp::Close(oobFd);
    if (deviceSet) {
        (void)api.aclrtResetDevice(opt.device);
    }
    if (aclInited) {
        (void)api.aclFinalize();
    }
    printf("[POC] exit, code=%d\n", exitCode);
    return exitCode;
}
