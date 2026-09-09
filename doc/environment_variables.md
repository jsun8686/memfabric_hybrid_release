# 环境变量说明

本文档列出 MemFabric_Hybrid 使用的所有环境变量及其用途。

## 运行时环境变量

以下环境变量在程序运行时读取，用于控制运行时行为：

| 环境变量名 | 默认值 | 说明 |
|-----------|-------|------|
| `ASCEND_HOME_PATH` | 无（必填） | NPU环境下Ascend安装路径，用于加载CANN动态库。GPU环境无需设置。 |
| `CUDA_HOME` | 无（必填） | GPU环境下CUDA安装路径，用于加载CUDA动态库。NPU环境无需设置。 |
| `ASCEND_RT_VISIBLE_DEVICES` | 无 | 设备可见性控制，用于将物理设备ID映射为逻辑设备ID。格式如`0,1,2,3`。 |
| `HCOM_MAX_SLICE_SIZE` | NPU: 1GB<br>其他: 1MB | HCOM传输最大切片大小（字节），控制单次传输数据分片上限。适用于HOST_RDMA/HOST_TCP/HOST_URMA传输模式。 |
| `HCOM_RECV_DATA_SIZE` | NPU: 1MB+1KB<br>其他: 1MB+1024 | HCOM接收数据缓冲区大小（字节）。建议设置为`HCOM_MAX_SLICE_SIZE + 1024`。 |
| `HYBM_RDMA_SWAP_SPACE_SIZE` | NPU: 4GB<br>其他: 1GB | RDMA交换空间大小（字节），用于Host RDMA数据传输的中转内存。 |
| `HYBM_RDMA_FORCE_UNREGISTERED` | 0 | 强制RDMA跳过内存注册检查路径。设为非0值时，`BatchDataCopy`直接走未注册路径发起RDMA读写。 |
| `MEMFABRIC_HYBRID_EXTEND_LIB_PATH` | 无 | 扩展库路径，用于加载自定义的`libmf_hybm_copy_extend.so`库。 |
| `SHMEM_LOG_LEVEL` | 无 | Transfer模块日志级别，取值范围0-4（0:DEBUG, 1:INFO, 2:WARN, 3:ERROR, 4:OFF）。**仅Python接口下生效，bm/shm场景不生效。** |
| `ASCEND_MF_LOG_LEVEL` | 无 | MemFabric日志级别，优先级高于`SHMEM_LOG_LEVEL`。取值范围0-4。**仅Python接口下生效，bm/shm场景不生效。** |
| `ASCEND_MF_STORE_URL` | 无（必填） | MemFabric Store URL，用于Transfer Engine初始化时连接配置存储。格式如`tcp://ip:port`，当前实际只取协议前缀。 |
| `ACCLINK_CHECK_PERIOD_HOURS` | 24 | 控制路径SSL证书定期检查周期（小时），有效范围1-168。 |
| `ACCLINK_CERT_CHECK_AHEAD_DAYS` | 30 | 证书提前检查天数，在证书过期前多少天开始告警，有效范围1-365。 |
| `MF_CONFIG_STORE_PORT_START` | 9000 | Config Store可用端口范围起始值，与`MF_CONFIG_STORE_PORT_END`配合使用。TransferEngine在`session_id`未指定端口（如`ip`/`ip:0`）时自动选端口亦使用此范围。 |
| `MF_CONFIG_STORE_PORT_END` | 65535 | Config Store可用端口范围结束值。 |
| `MF_CONFIG_STORE_HEARTBEAT_TIMEOUT_S` | 60 | Config Store 服务端心跳超时（秒），范围 0～3600（1 小时）；0 表示永不超时。 |
| `MF_GROUP_JOIN_MAX_TIMEOUT` | 600 | Group Join最大超时时间（秒）。在`smem_trans`和`smem_bm`的Join/GroupUpdate流程中控制等待集群加入的超时上限。 |
| `MF_SMEM_GROUP_RETRY_TIME` | 10 | Group Update重试次数。在`smem_trans`和`smem_bm`的Update流程中控制`GroupUpdate`的重试次数，每次重试间隔1秒。 |
| `MF_QP_READY_CHECK_TIMEOUT_BASE` | 100 | Device RDMA QP（Queue Pair）就绪检查超时基础值（秒）。在Device RDMA传输模式下，等待所有rank的QP就绪的总超时基数，实际超时为基础值+每rank额外100ms。 |
| `HCCL_RDMA_TC` | 132 | Device RDMA Traffic Class（流量类别），必须为4的整数倍，有效范围0-255。用于NPU Device RDMA传输的QoS配置。 |
| `HCCL_RDMA_SL` | 4 | Device RDMA Service Level（服务等级），有效范围0-7。用于NPU Device RDMA传输的QoS配置。 |
| `MF_NPU_RDMA_SEND_CQ_DEPTH` | 16384 | Device RDMA 发送CQ（Completion Queue）深度，有效范围64-32768。 |
| `MF_NPU_RDMA_RECV_CQ_DEPTH` | 16384 | Device RDMA 接收CQ深度，有效范围64-32768。 |
| `MF_NPU_RDMA_MAX_SEND_WR` | 32767 | Device RDMA 发送WQ深度（max_send_wr），有效范围64-32767。 |
| `MF_NPU_RDMA_MAX_RECV_WR` | 32767 | Device RDMA 接收WQ深度（max_recv_wr），有效范围64-32767。 |

## 构建时环境变量

以下环境变量在编译构建时读取，用于控制构建行为：

| 环境变量名 | 默认值 | 说明 |
|-----------|-------|------|
| `MEMFABRIC_VERSION` | 无（必填） | 构建版本号，用于生成whl包版本。**仅构建Python whl包时需要，C++构建不需要。** |
| `BUILD_OPEN_ABI` | OFF | 是否使用开放ABI（`_GLIBCXX_USE_CXX11_ABI`）。ON表示使用C++11 ABI。 |
| `BUILD_MODE` | RELEASE | 构建模式。可选值：RELEASE、DEBUG、ASAN。 |
| `ENABLE_PTRACER` | ON | 是否启用ptracer性能打点工具。 |
| `XPU_TYPE` | NPU | 异构设备类型。可选值：NPU、GPU、NONE。 |
| `IS_MANYLINUX` | FALSE | 是否构建manylinux兼容的wheel包。设为ON时，使用auditwheel修复wheel包以兼容manylinux平台。**仅构建Python whl包时生效。** |

## 安装脚本环境变量

| 环境变量名 | 说明 |
|-----------|------|
| `ASCEND_TOOLKIT_HOME` | Ascend Toolkit安装路径，安装脚本检查用。 |

## 使用示例

### NPU环境运行前设置

```bash
export ASCEND_HOME_PATH=/usr/local/Ascend/ascend-toolkit
export ASCEND_MF_LOG_LEVEL=1
export HCOM_MAX_SLICE_SIZE=$((128*1024))
export HCOM_RECV_DATA_SIZE=$((128*1024+128))
export HYBM_RDMA_SWAP_SPACE_SIZE=$((128*1024*1024))
```

### GPU环境运行前设置

```bash
export CUDA_HOME=/usr/local/cuda
export ASCEND_MF_LOG_LEVEL=1
```

### 无卡环境（NONE）运行前设置

```bash
export ASCEND_MF_LOG_LEVEL=1
export HCOM_MAX_SLICE_SIZE=$((128*1024))
export HCOM_RECV_DATA_SIZE=$((128*1024+128))
export HYBM_RDMA_SWAP_SPACE_SIZE=$((128*1024*1024))
```
