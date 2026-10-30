# AIV + URMA 直驱 PoC（MemFabric 阶段 0）

在 A5（Ascend 950PR/DT，CANN >= 9.1.0）双机上验证 **Host 纯 HCOMM 控制面 + AIV 内核直驱 URMA（UB_CTP）** 数据通路，
为 MemFabric xcopy 数据面引入 AIV 引擎（阶段 1）提供前置依据。

## 验证目标

| # | 目标 | 判定 |
|---|------|------|
| 1 | `HcommChannelCreate(COMM_ENGINE_AIV)` + UBC_CTP + EID 端点建链成功 | 日志 `channel READY` |
| 2 | AIV 内核内 `AscendC::Hcomm<UBC_CTP>` Init/WriteNbi/Commit/Drain 全流程 | 内核 status=1（OK） |
| 3 | Drain 返回后数据可见性（write 与 read 双向） | `[RESULT][PASS]` |
| 4 | channel handle 直接传内核即可定位设备侧 ChannelEntity | 目标 2 的前置 |
| 5 | （延伸）UB_RTP（950 SuperPod PC16）是否可用 | 开源 hcomm/asc-devkit 均无 UB_RTP 实现与枚举，本 PoC 不覆盖，需上游确认 |

## 目录

```
aiv_urma_poc/
├── CMakeLists.txt            # ASC 一体化工程（find_package(ASC) + LANGUAGES ASC CXX）
├── common/poc_common.h       # host/kernel 共用参数布局
├── include/hcomm/            # hcomm_res.h / hcomm_res_defs.h（拷贝自 hcomm 仓，securec 换 shim）
├── kernel/
│   ├── poc_kernel_common.h   # 内核公共实现（传输循环/状态写回）
│   ├── poc_hcomm_write.cpp   # AIV 写内核 + LaunchPocWrite（<<<>>> 启动封装），ASC 编译单元
│   └── poc_hcomm_read.cpp    # AIV 读内核 + LaunchPocRead，ASC 编译单元
├── host/
│   ├── poc_main.cpp          # 主流程（server/client 双角色）
│   ├── poc_api.h/.cpp        # dlopen libascendcl/libhcomm（控制面符号）
│   ├── poc_eid.h/.cpp        # EID 获取（--eid 或 /etc/hccl_rootinfo.json）
│   └── poc_tcp.h/.cpp        # 极简 TCP 带外通道
├── build.sh                  # 一键构建（单次 cmake）
├── run_server.sh / run_client.sh
└── README.md
```

## 前置条件

1. 两台 A5 服务器（UB 互联，UB_CTP 可达），相同 CANN（>= 9.1.0，含 `libhcomm.so`，且
   `tools/tikcpp/ascendc_kernel_cmake/asc_modules/` 存在——ASC cmake 模块）。
2. 两台机器 `/etc/hccl_rootinfo.json` 存在且含本机 `device_id` 与 `addr_type:"EID"` 条目；
   若缺失，用 `--eid <32hex>` 手工指定本端 EID（两台各查各的）。
3. asc-devkit 仓（提供 `AscendC::Hcomm` 头）：
   ```bash
   git clone https://gitcode.com/cann/asc-devkit.git ~/asc-devkit
   # 或 GitHub: https://github.com/Ascend/asc-devkit
   ```
   检查 `~/asc-devkit/include/adv_api/hcomm/hcomm.h` 存在。
4. PoC TCP 带外端口（默认 26000）与 hcomm 通道 OOB 端口（默认 60001）未被占用/未被防火墙拦截。

## 构建（两台机器各执行一次）

```bash
source /usr/local/Ascend/cann/set_env.sh   # ASCEND_HOME_PATH 生效
cd aiv_urma_poc
./build.sh
# 产物：build/poc/poc_aiv_urma（单可执行，设备镜像内嵌）
```

## 运行

write 路径（client AIV 内核写 server）：

```bash
# 服务器 A：
./run_server.sh 0
# 服务器 B：
./run_client.sh <serverA_ip> 0
```

read 路径（client AIV 内核从 server 拉）：

```bash
# 服务器 A：
./build/poc/poc_aiv_urma --role server --verify read --device 0
# 服务器 B：
./build/poc/poc_aiv_urma --role client --peer-ip <serverA_ip> --verify read --device 0
```

成功标志：

```
[POC] channel READY
[POC] kernel done in <N> us, status=1
[POC][RESULT][PASS] write path verified (1048576 bytes)
```

## 关键实现说明（与调研结论的对应）

- **Host 控制面**：`HcommEndpointCreate(UBC_CTP+EID)` → `HcommMemReg/Export` → TCP 带外交换 →
  `HcommMemImport` → `HcommChannelCreate(COMM_ENGINE_AIV)` → `HcommChannelGetStatus` 轮询至
  `HCOMM_CHANNEL_STATUS_READY(0)`。`channelDesc` 必填 `role/port/channelName`（AIV 通道内部有 OOB
  socket 辅助建链，server 监听 `port`，client 连接），`exchangeAllMems=true`（两端各注册 1 块内存）。
- **AIV 数据面（ASC 一体化启动链）**：内核源文件以 ASC 语言编译（bisheng 一次编译完成
  host/device 分裂），host 经 `LaunchPocWrite/Read` 调用 `<<<1, 0, stream>>>(args)` 启动——
  设备镜像内嵌、类型标识与按 entry 注册均由编译器/运行时闭环处理（对齐官方 asc-comm 示例
  04_simt_urma / 01_hcomm_write_read_nbi）。早期"tikcpp ascendc_library + 裸 .o +
  aclrtBinaryLoad*/GetFunction"路径已废弃：该组合下按名查函数表不完整，必现
  `aclrtBinaryGetFunction` 107000。
- **内核实现**：`Hcomm<UBC_CTP>` 需要 512B UB workspace（`TBuf<VECOUT>` 分配，32B 对齐天然满足）；
  分块 `WriteNbi/ReadNbi<commit=false>` → `Commit` → `Drain`；内核用 `__vector__ __global__
  __aicore__` 修饰（ASC 路径自动生成类型标识，勿手写 `.ascend.meta` 段）。
- **启动后同步**：host `aclrtSynchronizeStream` + 内核写回 statusAddr 双保险。

## 常见问题排查

| 现象 | 处理 |
|------|------|
| `dlopen libhcomm.so failed` | 确认 CANN 安装含 hcomm 组件，或设 `LD_LIBRARY_PATH` 后重试；PoC 依次尝试 `<CANN>/lib64` 与 `<CANN>/runtime/lib64/hcomm` |
| `find_package(ASC)` 失败 | 确认 `$ASCEND_HOME_PATH/tools/tikcpp/ascendc_kernel_cmake/asc_modules/FindASC.cmake` 存在；确认 `set_env.sh` 已 source |
| 链接期 undefined `LaunchAscendKernel` 等 | `<<<>>>` 胶水符号在运行时库；按报错补链（`ascendc_runtime`/`acl_rt`/`runtime`） |
| `HcommEndpointCreate ret!=0` | EID/协议与组网不符：核对 rootinfo 中 EID 归属、两机是否 UB_CTP 互联（PoC 不覆盖 UB_RTP/超节点 PC16 组网） |
| `channel not ready` | 看 hcomm 日志（`ASCEND_GLOBAL_LOG_LEVEL=0` 前置运行）；核对两端 `--hcomm-port` 一致、server 侧端口可达；`channelName` 两端一致 |
| `kernel status=0x1xxx` | 低 12 位为内核内错误码段：0x1xx=Init 失败、0x2xx=WriteNbi/ReadNbi 失败（含偏移信息）、0x3xx=Commit 失败、0x4xx=Drain 失败/超时 |
| write 校验 FAIL | 降低 `--size/--chunk` 复测；对照 `--verify read` 是否同样失败以区分方向性问题 |

## 与 MemFabric 阶段 1 的衔接

PoC 通过后，MemFabric 侧改造点见调研计划：`DeviceUrmaTransportManager` 增加引擎子模式
（`HcommChannelCreate(COMM_ENGINE_AIV)`，免 `HcommThreadAlloc`），hybm ops 新增 AIV 版
batch transfer 内核（复用 `HybmBatchTransferParam` ABI，多核按 coreId 切分），完成语义改用
内核内 `Drain` + host stream sync。GVM 窗口内存（DVA）在 AIV 通道上的注册/导入验证也移至
阶段 1（需结合 MemFabric 内存池，PoC 用 `aclrtMalloc` HBM 已验证链路层）。

注意：MemFabric 正式集成时若维持 dlopen/dlsym 架构（不编译期链接 CANN 符号），需评估
`ascendc_fatbin_library`（纯设备 .o）+ dlsym 复刻 ASC 运行时（`RegisterAscendBinary`/
`LaunchAscendKernel`）的路径，见调研记录。
