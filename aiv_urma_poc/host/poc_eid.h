/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * AIV+URMA PoC is licensed under Mulan PSL v2.
 */

#ifndef POC_EID_H
#define POC_EID_H

#include <array>
#include <cstdint>
#include <string>

constexpr uint32_t POC_EID_LEN = 16;

// EID 获取（两级）：
// 1. hex 字符串（32 个 hex 字符，--eid 指定，格式同 rootinfo 的 addr 字段）
// 2. 自动解析 rootinfo（默认 /etc/hccl_rootinfo.json）：在 "device_id":<phyId> 的 rank 块内
//    找第一个 "addr_type":"EID" 的 "addr" 字段（简化解析，PoC 专用）
int PocGetEid(const std::string &eidHex, const std::string &rootinfoPath, uint32_t phyDeviceId,
              std::array<uint8_t, POC_EID_LEN> &eid);

std::string PocEidToString(const std::array<uint8_t, POC_EID_LEN> &eid);

#endif  // POC_EID_H
