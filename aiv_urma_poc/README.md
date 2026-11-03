# AIV + URMA 直驱 PoC（MemFabric 阶段 0）

在 A5（Ascend 950PR/DT，CANN >= 9.1.0）双机上验证 **Host 纯 HCOMM 控制面 + AIV 内核直驱 URMA（UB_CTP）** 数据通路，
为 MemFabric xcopy 数据面引入 AIV 引擎（阶段 1）提供前置依据。

> **状态（2026-11-02）：方案 B 已在 A5 真机（Ascend950PR_9589 / CANN 9.1.0）回归全 PASS** ——
> READ 1MB（单 WQE 1MB、64KB×16、4KB×256 单 Commit 三种分块）与 WRITE 1MB/64KB 双向数据校验通过。
> 两个关键工程结论见 FAQ：
> ① AIV 核内裸指针标量 GM 写对 host 不可见，状态写回必须走 UB+DataCopy（MTE3）；
> ② kernel 参数须拍平为标量（bisheng 结构体参数指针降级），hostArgs 口径含 8B overflow 尾槽。

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
├── CMakeLists.txt            # 方案 B 工程：ascendc_fatbin_library（kernel）+ 纯 CXX 可执行（host）
├── common/poc_common.h       # host/kernel 共用参数布局
├── include/hcomm/            # hcomm_res.h / hcomm_res_defs.h（拷贝自 hcomm 仓，securec 换 shim）
├── kernel/
│   ├── poc_kernel_common.h   # 内核公共实现（传输循环/状态写回：UB 镜像 + DataCopy MTE3）
│   ├── poc_hcomm_write.cpp   # AIV 写内核（fatbin 设备侧编译单元，符号原名 poc_hcomm_write）
│   └── poc_hcomm_read.cpp    # AIV 读内核（符号原名 poc_hcomm_read）
├── host/
│   ├── poc_main.cpp          # 主流程（server/client 双角色；client 显式加载 fatbin 并 AIV 下发）
│   ├── poc_api.h/.cpp        # dlopen libascendcl/libhcomm（控制面 + 二进制加载/启动符号）
│   ├── poc_eid.h/.cpp        # EID 获取（--eid 或 /etc/hccl_rootinfo.json）
│   └── poc_tcp.h/.cpp        # 极简 TCP 带外通道
├── build.sh                  # 一键构建（cmake 全量 + install 落盘 fatbin）
├── run_server.sh / run_client.sh
└── README.md
```

## 前置条件

1. 两台 A5 服务器（UB 互联，UB_CTP 可达），相同 CANN（>= 9.1.0，含 `libhcomm.so`，且
   `tools/tikcpp/ascendc_kernel_cmake/ascendc.cmake` 存在——fatbin 编译模块）。
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
# 产物：build/poc/poc_aiv_urma（host 可执行）+ build/poc/poc_hcomm_kernel.o（fatbin 算子二进制）
# fatbin 目标 SOC 由 CMake 变量 SOC_VERSION 决定（build.sh 默认 ascend950pr_9599，
# 950DT/其他变体用 POC_SOC_VERSION=<值> ./build.sh 覆盖）
```

构建后建议先验收 fatbin 符号（方案 B 的关键不变量：符号保持原始名，无 `_0/_1` 后缀）：

```bash
readelf -sW build/poc/poc_hcomm_kernel.o | grep poc_hcomm
# 期望：FUNC GLOBAL DEFAULT poc_hcomm_write / poc_hcomm_read
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
- **AIV 数据面（方案 B：fatbin 显式加载，CANN 官方"非混合编程"路径）**：内核经
  `ascendc_fatbin_library`（bisheng）编译为独立算子二进制 `poc_hcomm_kernel.o`；
  client 侧运行时 `aclrtCreateBinary` → `aclrtBinaryLoad` → `aclrtBinaryGetFunction`
  （**按原始名** `poc_hcomm_write`/`poc_hcomm_read` 查询）→ `aclrtLaunchKernelWithHostArgs`，
  launch attr 携带 `ACL_RT_ENGINE_TYPE_AIV`（对齐官方 HCCL AIV 样例与 AddKernelInvocationAcl
  样例）。fatbin 路径保留原始符号名——这正是它与旧路径的本质区别：旧路径把
  `ascendc_library` 的中间产物 `_merge_obj_dir/device_aiv.o` 当正式二进制加载，而
  ascendc_library 的 auto-gen 包装会把符号改写成 `poc_hcomm_write_0`/`poc_hcomm_read_1`
  （已用 readelf 实证），host 按原名查询必现 `aclrtBinaryGetFunction` 107000。
  更早的"裸 .o + 手写 `.ascend.meta` + MAGIC 选项"组合同样废弃：meta 段只给同名符号
  附加类型信息，符号名不匹配时无法完成注册。
- **内核实现**：`Hcomm<UBC_CTP>` 需要 512B UB workspace（`TBuf<VECOUT>` 分配，32B 对齐天然满足）；
  分块 `WriteNbi/ReadNbi<commit=false>` → `Commit` → `Drain`；内核声明为朴素的
  `__global__ __aicore__`（对齐官方 add_custom 样例）。**不要**在声明上携带
  `__attribute__((aiv))` 核型属性：auto-gen 包装会把 `__global__` 重定义为 `inline`
  （原函数降级为普通设备函数并改名 `_origin`），真正的 kernel 是工具链生成的 wrapper，
  属性挂在非 kernel 函数上必报 "needs to appear with kernel func"。
- **启动后同步**：host `aclrtSynchronizeStream` + 内核写回 statusAddr 双保险。

## 常见问题排查

| 现象 | 处理 |
|------|------|
| `dlopen libhcomm.so failed` | 确认 CANN 安装含 hcomm 组件，或设 `LD_LIBRARY_PATH` 后重试；PoC 依次尝试 `<CANN>/lib64` 与 `<CANN>/runtime/lib64/hcomm` |
| `ascendc.cmake not found` | 确认 `$ASCEND_HOME_PATH/tools/tikcpp/ascendc_kernel_cmake/ascendc.cmake` 存在；确认 `set_env.sh` 已 source |
| `SOC_VERSION value not set.` | legacy ascendc_kernel_cmake 只认 CMake 变量 `SOC_VERSION`（不认 `ASCEND_COMPUTE_UNIT`）：build.sh 已默认传 `ascend950pr_9599`；若手工 cmake 需加 `-DSOC_VERSION=<值>` |
| `SOC_VERSION ... does not support` | 取值与 host_config.cmake 的 `ascend950_list` 不符：`grep -n ascend950_list $ASCEND_HOME_PATH/tools/tikcpp/ascendc_kernel_cmake/legacy_modules/host_config.cmake`，或 `ls $ASCEND_HOME_PATH/compiler/data/platform_config/ \| grep -i 950` 对照本机已装平台配置，再用 `POC_SOC_VERSION=<值> ./build.sh` 覆盖（950DT 机器必须改） |
| `... does not exist, please check the setting of ASCEND_CANN_PACKAGE_PATH` | host_config.cmake 以 **CMake 变量**方式引用它（光 export 环境变量无效）：build.sh 已默认 `-DASCEND_CANN_PACKAGE_PATH=$ASCEND_HOME_PATH`；若手工 cmake 需自行加该 `-D`，且路径必须真实存在 |
| `__attribute__((aic)), __attribute__((aiv)), bisheng::core_ratio attribute needs to appear with kernel func` | 核型属性挂在了**非 kernel** 函数上：auto-gen 会把 `__global__` 重定义为 `inline`，源码里的原 kernel 已退化为普通设备函数，不能带 `__attribute__((aiv))`/`__vector__`。kernel 声明用朴素的 `__global__ __aicore__` 即可，核型分工由 aic/aiv 子构建承担 |
| `host_stub_util.FuncNameNotFound`（extract_host_stub.py） | kernel 签名里有解析器不认的元素（如属性插在 `__aicore__` 与返回类型之间）：签名正则要求 `[aicore]` 后紧跟"返回类型 函数名(参数)"，保持朴素的 `__global__ __aicore__ void name(args)` 形式 |
| `fatbin output not found` | `cmake --install` 未落盘：检查 `build/poc/fatbin/` 目录；或手工 `find build/poc -name 'poc_hcomm_kernel.o'` 后用 `--kernel-image` 指定 |
| `aclrtBinaryGetFunction(xxx) ret=107000` | fatbin 中没有该名字的 GLOBAL FUNC 符号：`readelf -sW build/poc/poc_hcomm_kernel.o \| grep poc_hcomm`，若符号带 `_0/_1` 后缀说明误用了 ascendc_library 的中间产物而非本方案的 fatbin |
| `HcommEndpointCreate ret!=0` | EID/协议与组网不符：核对 rootinfo 中 EID 归属、两机是否 UB_CTP 互联（PoC 不覆盖 UB_RTP/超节点 PC16 组网） |
| `channel not ready` | 看 hcomm 日志（`ASCEND_GLOBAL_LOG_LEVEL=0` 前置运行）；核对两端 `--hcomm-port` 一致、server 侧端口可达；`channelName` 两端一致 |
| `kernel status=0x1xxx` | 低 12 位为内核内错误码段：0x1xx=Init 失败、0x2xx=WriteNbi/ReadNbi 失败（含偏移信息）、0x3xx=Commit 失败、0x4xx=Drain 失败/超时 |
| `kernel done ..., status=0` | **根因已结案（见下行）**：AIV 核内裸指针标量 GM store 对 host D2H 不可见，kernel 其实执行了但写回读不到。内核按阶段经 UB+DataCopy 回写非 0 标记（0xE1=已入内核、0xE2=Init 成功、0xE3=Nbi 全部入队、0xE4=Commit 成功），host 日志会直接打印阶段解码 |
| write 校验 FAIL | 降低 `--size/--chunk` 复测；对照 `--verify read` 是否同样失败以区分方向性问题；**先 `npu-smi` 确认两卡空闲**——卡被他者占用时 Commit/Drain 可能返回失败（如 0x3ff），属环境噪声而非链路故障 |
| kernel 注册成功但完全不执行（阶段标记全 0） | **已结案（A5 实测，WRITE/READ 三配置回归全 PASS）**：真根因是 **AIV 核内裸指针标量 GM store（`*(volatile uint64_t*)addr = v`）对 host D2H 恒不可见**——任务实际正常下发并完成（slog：LoadArgsWithCopy 成功、SQE 下发 drvRet=0、task completed），但 host 永远读不到写回值，追加 c310 收尾 `pipe_barrier/dsb/dci` 也不能使其可见（实验证伪）。**修法：状态统一走 UB 镜像 + `DataCopy` UB→GM（MTE3）写回**——与官方 fatbin 样例 `6_memory_loaded_vector_add` 的唯一写回路径一致（同机实测 PASS）；DataCopy 最小粒度 32B，故状态页 64B 整页刷出，标量写 UB 与 MTE3 读 UB 之间需 `SetFlag/WaitFlag<HardEvent::S_MTE3>` 硬同步。排查中实验排除的因素：argsSize 尾槽口径、launch attrs、加载路径（file-lazy / mem-eager 均可）。kernel 参数拍平为标量仍保留（bisheng 会把 by-value 结构体降级为指针传递，拍平后 `__CCE_KernelArgSize` NOTE 与 hostArgs 口径一致） |
| 单 WQE 长度上限 | URMA 协议上限**严格小于 256 MiB**（hicann/ops-transformer 生产代码：`256MiB-512` 分块，512B 对齐）；A5 实测单 WQE 1MB 直接通过。历史上一次"1MB Commit 失败 0x3ff"为卡被占用的环境性失败，与长度无关 |

## 与 MemFabric 阶段 1 的衔接

PoC 通过后，MemFabric 侧改造点见调研计划：`DeviceUrmaTransportManager` 增加引擎子模式
（`HcommChannelCreate(COMM_ENGINE_AIV)`，免 `HcommThreadAlloc`），hybm ops 新增 AIV 版
batch transfer 内核（复用 `HybmBatchTransferParam` ABI，多核按 coreId 切分），完成语义改用
内核内 `Drain` + host stream sync。GVM 窗口内存（DVA）在 AIV 通道上的注册/导入验证也移至
阶段 1（需结合 MemFabric 内存池，PoC 用 `aclrtMalloc` HBM 已验证链路层）。

注意：MemFabric 正式集成维持 dlopen/dlsym 架构（不编译期链接 CANN 符号）已无阻碍——
本 PoC 的 fatbin 显式加载路径（`aclrtCreateBinary`/`aclrtBinaryLoad`/`aclrtBinaryGetFunction`/
`aclrtLaunchKernelWithHostArgs`）全部经 dlsym 获取，launch attr 结构体布局复刻自 CANN
开源 `acl_rt.h`，可直接平移到 `load_kernel.cpp` 体系。ASC 一体化（`<<<>>>` 内嵌镜像）
版本保留在 release/1.2 主干作为已验证基线，本方案在其之上只替换构建与启动链。
