/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * AIV+URMA PoC is licensed under Mulan PSL v2.
 */

#ifndef POC_TCP_H
#define POC_TCP_H

#include <cstdint>
#include <string>

// PoC 专用极简阻塞 TCP 带外通道：交换 EndpointDesc / memDesc / 端口等小消息。
// server: ListenAndAccept(port) -> fd；client: Connect(ip, port) -> fd。
class PocTcp {
public:
    static int ListenAndAccept(uint16_t port, std::string &err);
    static int Connect(const std::string &ip, uint16_t port, std::string &err);
    static int SendAll(int fd, const void *buf, size_t len);
    static int RecvAll(int fd, void *buf, size_t len);
    static void Close(int fd);
};

#endif  // POC_TCP_H
