# Hcomm AIV 与 AICPU 数据通路接口对照

- 版本：1.0（2026-11-03）
- 适用范围：Ascend 950PR/DT（A5，dav-3510），CANN >= 9.1.0（hcomm 组件 timestamp >= 2026-07）
- 信息来源：asc-comm 仓、asc-devkit 仓、hcomm 仓源码与官方示例（详见第 7 章），以及 aiv_urma_poc 阶段 0 实测结论
- 读者：MemFabric xcopy 数据面引擎选型与阶段 1（AIV 引擎引入）开发人员

## 1. 架构总览

| 维度 | AICPU 路径（MemFabric 现网） | AIV 路径（PoC 方案 B） |
|---|---|---|
| 执行体 | AICPU 常驻线程（`HcommThreadAlloc` 分配） | Vector Core（AIV 内核，fatbin 显式 launch） |
| host 控制面 | 同一套 `Hcomm*` C API（`hcomm_res.h`） | 同左（仅引擎参数不同） |
| 数据面 API | host 直接调 C 函数 `asccomm_*_on_thread` | 设备侧 C++ 模板 `AscendC::Hcomm<UBC_CTP>` |
| host 在数据路径上 | 是（每次读写均为 host 调用，含完成等待） | 否（launch 后数据面在内核内闭环） |
| 数据通路 | host 下发 → AICPU 线程搬运 → URMA | AIV 内核直接构造 WQE → URMA 硬件 |

## 2. Host 控制面对照（共用 `hcomm_res.h`，差异仅参数）

| 环节 | AICPU | AIV | 差异说明 |
|---|---|---|---|
| 端点创建 | `HcommEndpointCreate(EndpointDesc)` | 同左 | 无（协议/地址/EID 同构） |
| 内存注册 | `HcommMemReg` / `HcommMemUnreg` | 同左 | 无 |
| 内存导出导入 | `HcommMemExport` → OOB 交换 → `HcommMemImport` | 同左 | 无 |
| 线程资源 | `HcommThreadAlloc` / `HcommThreadFree` | 不需要 | AICPU 独有：占 AICPU 线程与 notify 槽位 |
| 通道创建 | `HcommChannelCreate`，引擎传 `COMM_ENGINE_AICPU` | 同左，引擎传 `COMM_ENGINE_AIV` | 仅枚举不同；`HcommChannelDesc` 同构 |
| 就绪等待 | `HcommChannelGetStatus` 轮询至 READY(0) | 同左 | 无 |
| 句柄语义 | `ChannelHandle` + `ThreadHandle` | 仅 `ChannelHandle` | AIV 句柄值 = 设备侧 ChannelEntity 指针，可直接进内核 |
| 通道销毁 | `HcommChannelDestroy` / `HcommEndpointDestroy` | 同左 | 无 |

`HcommChannelDesc` 两引擎共用字段：`remoteEndpoint / role / port / channelName / notifyNum /
exchangeAllMems / qos`。

## 3. 数据面 API 对照

### 3.1 初始化与上下文

| 操作 | AICPU | AIV |
|---|---|---|
| 初始化 | 线程即上下文，无对应调用 | `hcomm.Init(ws, 512)`：512B UB workspace（`TBuf<VECOUT>`，32B 对齐） |

### 3.2 读写与通知

| 操作 | AICPU（host C 函数） | AIV（内核 C++） |
|---|---|---|
| 写 | `asccomm_write_on_thread` | `hcomm.WriteNbi<commit>` |
| 读 | `asccomm_read_on_thread` | `hcomm.ReadNbi<commit>` |
| 写+远端通知 | `asccomm_write_with_notify_on_thread` | `hcomm.WriteWithNotifyNbi` |
| 归约读写 | `asccomm_write/read_reduce_on_thread` | 无直接对应（需内核自实现） |

统一签名形态：

```c
// AICPU：th 为 HcommThreadAlloc 返回的线程句柄
int32_t asccomm_write_on_thread(ThreadHandle th, ChannelHandle ch,
                                void* dst, const void* src, uint64_t len);
int32_t asccomm_read_on_thread(ThreadHandle th, ChannelHandle ch,
                               void* dst, const void* src, uint64_t len);
```

```cpp
// AIV：内核内 C++ 模板，channel 为 HcommChannelCreate 返回值直传
hcomm.template WriteNbi<false>(ch, dst, src, len);   // false = 攒 WQE 不立即提交
hcomm.template ReadNbi<false>(ch, dst, src, len);
hcomm.WriteWithNotifyNbi(ch, dst, src, len, notifyAddr, notifyVal);
```

### 3.3 批量与提交

| 操作 | AICPU | AIV |
|---|---|---|
| 异构批量 | `asccomm_batch_transfer_on_thread`（描述符数组） | 逐条 + `Commit`；或 BatchHandle 接口 |
| 延迟提交 | `asccomm_batch_mode_start/end` | `WriteNbi<false>` 攒 WQE，末次 `Commit` |

- AICPU 批量：单次提交 `AsccommBatchTransferDesc[]`，transferType 覆盖 WRITE / WRITE_REDUCE /
  WRITE(_REDUCE)_WITH_NOTIFY / READ(_REDUCE) / NOTIFY_RECORD 共 7 种。
- AIV BatchHandle：`MakeBatchHandle` → 逐任务 `WriteNbi/ReadNbi/WriteWithNotifyNbi(batchHandle)` →
  `BatchCommit`；每任务占 64B workspace 槽（WriteWithNotify 128B），受通道任务容量约束。

### 3.4 完成等待与同步

| 操作 | AICPU | AIV |
|---|---|---|
| 完成等待 | `asccomm_channel_drain_on_thread`（host 阻塞） | `hcomm.Drain(ch)`（内核内阻塞等 CQE） |
| 排序/栅栏 | `asccomm_channel_fence_on_thread` | WQE config 的 `fence/odr` 位（默认强序 + fence） |
| 通知等待 | thread / aclrt / channel 三族 notify API | 无 host 等待原语，完成感知见下方注 |

注：

- `Drain` 在 UBC_CTP 下超时返回 0xFF，完成记录异常返回 `status<<8 | substatus`（正值）。
- AIV 的 host 完成感知 = `aclrtSynchronizeStream` + 内核写回 status；状态回写必须经 UB +
  `DataCopy`(MTE3) 通道，volatile 直写 GM 对 host 不可见（PoC 实测教训）。

### 3.5 各自独有能力

| AICPU 独有 | AIV 独有 |
|---|---|
| 归约读写（write/read_reduce，op + dataType 参数化） | 原子操作 `AtomicFAA<T>` / `AtomicCAS<T>`（仅 UBC_CTP） |
| thread / aclrt 两级 notify 等待 | 通道锁 `hcomm.Lock/Unlock(ch)`（共享通道保护，不可重入） |
| 超时配置：`asccomm_set_notify_wait_timeout` 等 | 多通道批量 `MakeMultiChannelHandle` + `GetHandleRef` |
| 本地拷贝/归约：`asccomm_local_copy/reduce_on_thread` | 本地拷贝由内核原生 DataCopy 承担 |

## 4. 程序加载与启动对照

| 维度 | AICPU | AIV（方案 B 实测链路） |
|---|---|---|
| 内核形态 | 无自定义内核；自定义 AICPU 内核走 json + CPU_KERNEL_MODE 加载 | fatbin `.o`：`ascendc_fatbin_library` 产物，原名符号 + 自动 AIV 类型段 |
| 加载 | — | `aclrtBinaryLoadFromFile(path, LAZY_LOAD=1)` → `GetFunction("原名")` |
| 启动 | — | `aclrtLaunchKernelWithHostArgs`（三 attr，见下） |
| 参数约定 | — | args 扁平标量 + 8B overflow 尾槽（工具链 `__ascendc_args` ABI） |

AIV 加载与启动链（对齐 HCCL 生产路径 `hccl_aiv_utils.cc:329/425-447`）：

```c
// 加载：LAZY（首次 launch 才真正绑定）
aclrtBinaryLoadFromFile(path, {ACL_RT_BINARY_LOAD_OPT_LAZY_LOAD, 1}, &binHandle);
aclrtBinaryGetFunction(binHandle, "poc_hcomm_write", &func);

// 启动：三 attr = SCHEM_MODE + TIMEOUT_US + ENGINE_TYPE(AIV)
aclrtLaunchKernelAttr attrs[3] = {
    {ATTR_SCHEM_MODE,   {.schemMode = 1}},
    {ATTR_TIMEOUT_US,   {.timeoutUs = {timeout, 0}}},
    {ATTR_ENGINE_TYPE,  {.engineType = ACL_RT_ENGINE_TYPE_AIV}},
};
aclrtLaunchKernelWithHostArgs(func, blockDim, stream, &cfg, args, argsSize, nullptr, 0);
```

kernel 声明用朴素 `__global__ __aicore__`（核型由 aic/aiv 子构建分担，勿加核型属性，
否则与 auto-gen 宏化冲突报"attribute needs to appear with kernel func"）。

## 5. 端到端时序对照

```text
AICPU（MemFabric 现网）                    AIV（方案 B，回归全 PASS）
─────────────────────────                  ─────────────────────────────
HcommEndpointCreate(UBC_CTP + EID)         HcommEndpointCreate(UBC_CTP + EID)
HcommMemReg → Export → OOB → Import        HcommMemReg → Export → OOB → Import
HcommThreadAlloc(COMM_ENGINE_AICPU)        （免线程分配）
HcommChannelCreate(COMM_ENGINE_AICPU)      HcommChannelCreate(COMM_ENGINE_AIV)
HcommChannelGetStatus → READY              HcommChannelGetStatus → READY
loop:                                      LoadFromFile(LAZY) → GetFunction
  asccomm_write/read_on_thread(...)        LaunchKernelWithHostArgs(AIV 三 attr)
  asccomm_channel_drain_on_thread            └ 内核: Init(512B) → Nbi×N(false)
  （每次传输均有 host 往返）                        → Commit → Drain → status(MTE3)
                                           SynchronizeStream → 数据校验
HcommThreadFree                            （免）
HcommChannelDestroy / EndpointDestroy      HcommChannelDestroy / EndpointDestroy
```

## 6. 关键差异与集成结论

1. 控制面 90% 同构：Endpoint/MemReg/Import/ChannelCreate 为同一套 dlsym API，仅 `COMM_ENGINE_*`
   与是否 `HcommThreadAlloc` 之差，`DeviceUrmaTransportManager` 增加引擎子模式的改动成本低。
2. 数据面完全不同：AICPU 为 host 逐次 C 调用（host 在数据路径上）；AIV 为一次 launch 后内核内
   闭环（host 不在数据路径上），这是 AIV 低时延、高并发能力的本质来源。
3. 完成语义：AICPU 在 host 侧 `drain_on_thread` 阻塞；AIV 在内核 `Drain` + host `SynchronizeStream`，
   状态回写必须走 MTE3 通道（实测教训：volatile 直写 GM 对 host 不可见）。
4. 批量能力两侧各有原生方案：AICPU 用 `AsccommBatchTransferDesc` 数组聚合异构操作；AIV 用
   BatchHandle（受 64B/任务 workspace 与通道任务容量约束，中间任务 cqe=0、末任务 cqe=1）。
5. 已知限制与上游问题（阶段 0 记录）：
   - asc-devkit `hcomm_aiv_urma.h` 的 `PollCq` 等待循环为"先判断后拷贝"（依赖 UB 残留值），
     同仓 RoCE 实现（`hcomm_aiv_roce.h`）为正确的"先拷贝后判断"结构；该差异曾导致多块 read
     偶发假成功/超时，建议向上游反馈。
   - read 完成依赖 CQE，write 为推送语义不依赖 host 等 CQE，两侧稳定性特征不同。

## 7. 参考文件清单

| 内容 | 位置 |
|---|---|
| host 资源层 C API（两引擎共用） | hcomm 包 `pkg_inc/hcomm/hcomm_res.h`；PoC vendor 副本 `include/hcomm/` |
| AICPU 数据面 C API | asc-comm `include/aicpu/hcomm/asccomm_primitives.h` 与 `asccomm_res_defs.h` |
| AIV 数据面 C++ API 与实现 | asc-comm `include/aicore/hcomm/hcomm.h`；asc-devkit `impl/adv_api/detail/hcomm/`（实现见第 6 章） |
| AIV 官方示例 | asc-comm `examples/aicore/hcomm/` 下 01（读写）、02（批量）、04（SIMT/原子） |
| AIV launch 生产参照 | hcomm 仓 `src/legacy/ascend950/framework/aiv/aiv_ins/hccl_aiv_utils.cc` |
| PoC 实测代码 | 本仓 `aiv_urma_poc/`：host dlsym 控制面 + fatbin 内核 + 显式 launch 链（A5 回归全 PASS） |
