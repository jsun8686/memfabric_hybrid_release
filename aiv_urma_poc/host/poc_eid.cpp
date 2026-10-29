/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * AIV+URMA PoC is licensed under Mulan PSL v2.
 */

#include "poc_eid.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace {
int HexDigit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

int ParseHexEid(const std::string &hex, std::array<uint8_t, POC_EID_LEN> &eid)
{
    if (hex.size() != POC_EID_LEN * 2U) {
        printf("[POC][ERROR] EID hex length must be %u, got %zu\n", POC_EID_LEN * 2U, hex.size());
        return -1;
    }
    for (uint32_t i = 0; i < POC_EID_LEN; i++) {
        const int hi = HexDigit(hex[2U * i]);
        const int lo = HexDigit(hex[2U * i + 1U]);
        if (hi < 0 || lo < 0) {
            printf("[POC][ERROR] EID hex contains non-hex char at pos %u: %s\n", 2U * i, hex.c_str());
            return -1;
        }
        eid[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return 0;
}

std::string ReadFile(const std::string &path)
{
    std::ifstream f(path);
    if (!f.is_open()) {
        return "";
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
}  // namespace

std::string PocEidToString(const std::array<uint8_t, POC_EID_LEN> &eid)
{
    static const char *hexTab = "0123456789abcdef";
    std::string s;
    s.reserve(POC_EID_LEN * 2U);
    for (auto b : eid) {
        s.push_back(hexTab[b >> 4]);
        s.push_back(hexTab[b & 0xF]);
    }
    return s;
}

int PocGetEid(const std::string &eidHex, const std::string &rootinfoPath, uint32_t phyDeviceId,
              std::array<uint8_t, POC_EID_LEN> &eid)
{
    eid.fill(0);
    if (!eidHex.empty()) {
        return ParseHexEid(eidHex, eid);
    }

    // 自动模式：rootinfo 简化解析。结构（hccl_rootinfo.json）：
    //   ... "rank_list":[{ "device_id":N, "level_list":[{ "rank_addr_list":[
    //       {"addr_type":"EID","addr":"<32hex>", ...}]}]}]
    // 解析策略：按 "device_id" 切块，取值等于 phyDeviceId 的块内第一个 addr_type 为 EID 的 addr。
    const std::string text = ReadFile(rootinfoPath);
    if (text.empty()) {
        printf("[POC][ERROR] rootinfo not readable: %s (use --eid <32hex> to specify manually)\n",
               rootinfoPath.c_str());
        return -1;
    }

    const std::string devKey = "\"device_id\"";
    size_t pos = 0;
    while ((pos = text.find(devKey, pos)) != std::string::npos) {
        const size_t colon = text.find(':', pos + devKey.size());
        if (colon == std::string::npos) {
            break;
        }
        size_t v = colon + 1;
        while (v < text.size() && isspace(static_cast<unsigned char>(text[v]))) {
            v++;
        }
        uint64_t devId = 0;
        bool valid = false;
        while (v < text.size() && isdigit(static_cast<unsigned char>(text[v]))) {
            devId = devId * 10U + static_cast<uint64_t>(text[v] - '0');
            valid = true;
            v++;
        }
        if (!valid) {
            pos = colon;
            continue;
        }
        // 块范围：从当前 device_id 到下一个 device_id（或串尾）
        const size_t next = text.find(devKey, v);
        const size_t blockEnd = (next == std::string::npos) ? text.size() : next;
        if (devId == phyDeviceId) {
            const std::string block = text.substr(v, blockEnd - v);
            const size_t eidMark = block.find("\"EID\"");
            if (eidMark == std::string::npos) {
                printf("[POC][ERROR] device_id %u found in rootinfo but no EID addr in its block\n", phyDeviceId);
                return -1;
            }
            const size_t addrKey = block.find("\"addr\"", eidMark);
            if (addrKey == std::string::npos) {
                printf("[POC][ERROR] EID addr key not found after addr_type EID\n");
                return -1;
            }
            const size_t q1 = block.find('"', addrKey + 6);
            const size_t q2 = (q1 == std::string::npos) ? std::string::npos : block.find('"', q1 + 1);
            if (q1 == std::string::npos || q2 == std::string::npos) {
                printf("[POC][ERROR] malformed addr value near EID entry\n");
                return -1;
            }
            return ParseHexEid(block.substr(q1 + 1, q2 - q1 - 1), eid);
        }
        pos = blockEnd;
    }
    printf("[POC][ERROR] device_id %u not found in rootinfo %s (use --eid to override)\n", phyDeviceId,
           rootinfoPath.c_str());
    return -1;
}
