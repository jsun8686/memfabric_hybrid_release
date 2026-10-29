/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * AIV+URMA PoC is licensed under Mulan PSL v2.
 */

#include "poc_tcp.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

int PocTcp::ListenAndAccept(uint16_t port, std::string &err)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        err = "socket failed: " + std::string(strerror(errno));
        return -1;
    }
    int reuse = 1;
    (void)setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(lfd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        err = "bind failed: " + std::string(strerror(errno));
        (void)close(lfd);
        return -1;
    }
    if (listen(lfd, 1) != 0) {
        err = "listen failed: " + std::string(strerror(errno));
        (void)close(lfd);
        return -1;
    }
    printf("[POC] oob tcp listening on port %u, waiting for peer ...\n", port);
    int fd = accept(lfd, nullptr, nullptr);
    (void)close(lfd);
    if (fd < 0) {
        err = "accept failed: " + std::string(strerror(errno));
        return -1;
    }
    int nodelay = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    return fd;
}

int PocTcp::Connect(const std::string &ip, uint16_t port, std::string &err)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        err = "socket failed: " + std::string(strerror(errno));
        return -1;
    }
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        err = "invalid peer ip: " + ip;
        (void)close(fd);
        return -1;
    }
    if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        err = "connect " + ip + ":" + std::to_string(port) + " failed: " + strerror(errno);
        (void)close(fd);
        return -1;
    }
    int nodelay = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    return fd;
}

int PocTcp::SendAll(int fd, const void *buf, size_t len)
{
    const auto *p = static_cast<const uint8_t *>(buf);
    size_t done = 0;
    while (done < len) {
        const ssize_t n = send(fd, p + done, len - done, 0);
        if (n <= 0) {
            return -1;
        }
        done += static_cast<size_t>(n);
    }
    return 0;
}

int PocTcp::RecvAll(int fd, void *buf, size_t len)
{
    auto *p = static_cast<uint8_t *>(buf);
    size_t done = 0;
    while (done < len) {
        const ssize_t n = recv(fd, p + done, len - done, 0);
        if (n <= 0) {
            return -1;
        }
        done += static_cast<size_t>(n);
    }
    return 0;
}

void PocTcp::Close(int fd)
{
    if (fd >= 0) {
        (void)close(fd);
    }
}
