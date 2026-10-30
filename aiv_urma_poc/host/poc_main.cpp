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
#include <string>
#include <unistd.h>
#include <vector>

#include "hcomm/hcomm_res.h"
#include "poc_api.h"
#include "poc_common.h"
#include "poc_eid.h"
#include "poc_tcp.h"

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
    std::string kernelPath = "./poc_aiv_hcomm_kernel.o";
    std::string verify = "write";           // write | read
    int engineType = -1;                    // <0 = 不设置 ENGINE_TYPE attr（依赖 meta section）
    uint32_t statusTimeoutSec = 120;
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
           "  --kernel-o PATH     AIV kernel object (default ./poc_aiv_hcomm_kernel.o)\n"
           "  --verify write|read write: client->server write path; read: client pull from server\n"
           "  --engine-type N     force launch attr ENGINE_TYPE value (default: not set)\n"
           "  --status-timeout S  channel ready wait timeout seconds (default %u)\n",
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
        } else if (k == "--kernel-o") {
            opt.kernelPath = next("--kernel-o");
        } else if (k == "--verify") {
            opt.verify = next("--verify");
        } else if (k == "--engine-type") {
            opt.engineType = atoi(next("--engine-type"));
        } else if (k == "--status-timeout") {
            opt.statusTimeoutSec = static_cast<uint32_t>(atoi(next("--status-timeout")));
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
            return 1;
        }
    }
    return 0;
}

int LaunchAivKernel(PocDynApi &api, aclrtFuncHandle func, void *stream, const PocKernelArgs &args,
                    int engineType)
{
    aclrtArgsHandle argsHandle = nullptr;
    int32_t ret = api.aclrtKernelArgsInit(func, &argsHandle);
    if (ret != 0) {
        printf("[POC][ERROR] KernelArgsInit ret=%d\n", ret);
        return ret;
    }
    aclrtParamHandle paramHandle = nullptr;
    PocKernelArgs argsCopy = args;
    ret = api.aclrtKernelArgsAppend(argsHandle, &argsCopy, sizeof(argsCopy), &paramHandle);
    if (ret != 0) {
        printf("[POC][ERROR] KernelArgsAppend ret=%d\n", ret);
        return ret;
    }
    ret = api.aclrtKernelArgsFinalize(argsHandle);
    if (ret != 0) {
        printf("[POC][ERROR] KernelArgsFinalize ret=%d\n", ret);
        return ret;
    }

    PocLaunchKernelAttr attrs[2] = {};
    size_t numAttrs = 0;
    if (engineType >= 0) {
        attrs[numAttrs].id = POC_ATTR_ENGINE_TYPE;
        attrs[numAttrs].value.engineType = static_cast<uint32_t>(engineType);
        numAttrs++;
    }
    attrs[numAttrs].id = POC_ATTR_TIMEOUT;
    attrs[numAttrs].value.timeout = 600;
    numAttrs++;

    PocLaunchKernelCfg cfg {};
    cfg.attrs = attrs;
    cfg.numAttrs = numAttrs;
    return api.aclrtLaunchKernelWithConfig(func, 1U, stream, &cfg, argsHandle, nullptr);
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
    aclrtBinHandle binHandle = nullptr;

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

            // AIV 镜像必须带 VECTOR_CORE magic 加载（对齐 asc-devkit RegisterAscendBinary 的 AIVEC 路径），
            // 否则 GetFunction 查不到内核名
            PocBinaryLoadOption loadOpt {};
            loadOpt.type = PocBinaryLoadOptionType::MAGIC;
            loadOpt.value.magic = POC_BINARY_MAGIC_ELF_VECTOR_CORE;
            PocBinaryLoadOptions loadOpts {&loadOpt, 1};
            ret = api.aclrtBinaryLoadFromFile(opt.kernelPath.c_str(), &loadOpts, &binHandle);
            if (ret != 0 || binHandle == nullptr) {
                printf("[POC][ERROR] aclrtBinaryLoadFromFile(%s) ret=%d\n", opt.kernelPath.c_str(), ret);
                break;
            }
            const char *funcName = (opt.verify == "write") ? "poc_hcomm_write" : "poc_hcomm_read";
            aclrtFuncHandle func = nullptr;
            ret = api.aclrtBinaryGetFunction(binHandle, funcName, &func);
            if (ret != 0 || func == nullptr) {
                printf("[POC][ERROR] aclrtBinaryGetFunction(%s) ret=%d\n", funcName, ret);
                break;
            }
            printf("[POC] kernel loaded: %s\n", funcName);

            PocKernelArgs args {};
            args.channel = channel;
            args.localAddr = reinterpret_cast<uint64_t>(devBuf);
            args.remoteAddr = reinterpret_cast<uint64_t>(remoteMem.addr);
            args.size = opt.size;
            args.chunkSize = opt.chunk;
            args.statusAddr = reinterpret_cast<uint64_t>(statusBuf);

            const uint64_t t0 = PocNowUs();
            ret = LaunchAivKernel(api, func, stream, args, opt.engineType);
            if (ret != 0) {
                printf("[POC][ERROR] launch kernel ret=%d\n", ret);
                break;
            }
            ret = api.aclrtSynchronizeStream(stream);
            if (ret != 0) {
                printf("[POC][ERROR] SynchronizeStream ret=%d\n", ret);
                break;
            }
            const uint64_t t1 = PocNowUs();

            uint64_t statusHost = 0;
            (void)api.aclrtMemcpy(&statusHost, sizeof(statusHost), statusBuf, sizeof(statusBuf), 2 /*D2H*/);
            printf("[POC] kernel done in %llu us, status=%llu\n", (unsigned long long)(t1 - t0),
                   (unsigned long long)statusHost);
            if (statusHost != POC_STATUS_OK) {
                printf("[POC][ERROR] kernel status not OK: 0x%llx\n", (unsigned long long)statusHost);
                break;
            }

            if (opt.verify == "read") {
                // 本地校验（read 模式：本地是校验方，salt 是 server 的）
                size_t badIdx = 0;
                const int chk = CheckBuffer(api, devBuf, opt.size, 0xA5A5A5A5A5A5A5A5ULL, badIdx);
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
