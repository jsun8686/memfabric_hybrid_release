/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT ANY KIND OF EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY,
 * OR FIT FOR A PARTICULAR PURPOSE. See the Mulan PSL v2 for more details.
 */

/*
 * hccn_sock_probe: standalone cross-node hccn socket connectivity probe.
 *
 * Mirrors the minimal libra.so call chain used by the product
 * (src/hybm/csrc/transport/device/device_qp_manager.cpp CreateLocalSocket/CreateServerSocket,
 *  src/hybm/csrc/transport/device/joinable_ranks_qp_manager.cpp CreateConnectionToServers/
 *  GetSocketFdByAddrs, device_rdma_transport_manager.cpp PrepareOpenDevice) without smem/hybm logic:
 *   common : [RaRdevGetHandle(reuse)] else TsdOpen -> RaInit(tolerated) -> RaRdevInitV2
 *            then verify device RoCE ip via RaGetIfNum/RaGetIfaddrs (eth<dev>)
 *   listen : RaSocketInit -> [RaSocketWhiteListAdd] -> RaSocketListenStart -> RaGetSockets(role=0)
 *   connect: RaSocketInit -> RaSocketBatchConnect                    -> RaGetSockets(role=1)
 *
 * Build (on the NPU node, inside the same container/env as the product):
 *   gcc -O2 -Wall -o hccn_probe hccn_sock_probe.c -ldl
 *
 * Usage:
 *   FAR node  (listener, npu0 RoCE .10):
 *     ./hccn_probe listen  -d 0 -i 192.168.100.10 -r 192.168.100.20 -p 10010 -a 192.168.100.20 -t 60
 *   NEAR node (connector, npu0 RoCE .20):
 *     ./hccn_probe connect -d 0 -i 192.168.100.20 -r 192.168.100.10 -p 10010 -t 15
 *   Same-node sanity check first (tool self-test):
 *     shell 1: ./hccn_probe listen  -d 0 -i 192.168.100.20 -r 192.168.100.21 -p 10010 -a 192.168.100.21 -t 30
 *     shell 2: ./hccn_probe connect -d 1 -i 192.168.100.21 -r 192.168.100.20 -p 10010 -t 15
 *
 * Verdict (exit code 0 = PASS, 1 = FAIL, 2 = usage):
 *   same-node PASS + cross-node FAIL(status stuck at 3/connecting or 2/timeout, fd empty)
 *     -> hccn socket layer blocked cross-node: switch/NPU fabric config issue.
 *   cross-node PASS -> socket layer fine; investigate upper layers (QP pairing / product logic).
 */

#include <arpa/inet.h>
#include <dlfcn.h>
#include <getopt.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#define HCCP_SOCK_CONN_TAG_SIZE 192U
#define HCCP_MAX_INTERFACE_NAME_LEN 256U

/* Byte-for-byte ABI copies of the structs in src/hybm/csrc/under_api/dl_hccp_def.h */
enum hccp_network_mode {
    NETWORK_PEER_ONLINE = 0,
    NETWORK_OFFLINE = 1,
    NETWORK_ONLINE = 2,
};

typedef union {
    struct in_addr addr;
    struct in6_addr addr6;
} hccp_ip_addr_t;

typedef struct {
    uint32_t phy_id;
    uint32_t nic_position;
    int hdc_type;
    bool enable_hdc_async;
} hccp_ra_init_cfg_t;

typedef struct {
    int mode;
    uint32_t notify_type;
    bool enabled_910a_lite;
    bool disabled_lite_thread;
    bool enabled_2mb_lite;
} hccp_rdev_init_t;

typedef struct {
    uint32_t phy_id;
    int family;
    hccp_ip_addr_t local_ip;
} hccp_rdev_t;

typedef struct {
    uint32_t phy_id;
    uint32_t nic_position;
    bool is_all;
} hccp_if_attr_t;

typedef struct {
    hccp_ip_addr_t ip;
    struct in_addr mask;
} hccp_ifaddr_info_t;

typedef struct {
    int family;
    int scope_id;
    hccp_ifaddr_info_t ifaddr;
    char ifname[HCCP_MAX_INTERFACE_NAME_LEN];
} hccp_if_info_t;

typedef struct {
    void *handle;
    hccp_ip_addr_t remote_ip;
    uint32_t port;
    char tag[HCCP_SOCK_CONN_TAG_SIZE];
} hccp_conn_info_t;

typedef struct {
    void *handle;
    unsigned int port;
    unsigned int phase;
    unsigned int err;
} hccp_listen_info_t;

typedef struct {
    void *handle;
    void *fd;
    hccp_ip_addr_t remote_ip;
    int status;
    char tag[HCCP_SOCK_CONN_TAG_SIZE];
} hccp_sock_info_t;

typedef struct {
    hccp_ip_addr_t remote_ip;
    uint32_t conn_limit;
    char tag[HCCP_SOCK_CONN_TAG_SIZE];
} hccp_white_info_t;

typedef struct {
    void *handle;
    void *fd;
    int linger;
} hccp_close_info_t;

struct probe_api {
    int (*ra_init)(const hccp_ra_init_cfg_t *cfg);
    int (*rdev_init_v2)(hccp_rdev_init_t info, hccp_rdev_t rdev, void **handle);
    int (*rdev_get_handle)(uint32_t dev, void **handle);
    int (*get_ifnum)(const hccp_if_attr_t *attr, uint32_t *count);
    int (*get_ifaddrs)(const hccp_if_attr_t *attr, hccp_if_info_t infos[], uint32_t *count);
    uint32_t (*tsd_open)(uint32_t dev, uint32_t rank_count);
    int (*socket_init)(int mode, hccp_rdev_t rdev, void **handle);
    int (*socket_deinit)(void *handle);
    int (*listen_start)(hccp_listen_info_t info[], uint32_t num);
    int (*listen_stop)(hccp_listen_info_t info[], uint32_t num);
    int (*batch_connect)(hccp_conn_info_t conn[], uint32_t num);
    int (*batch_close)(hccp_close_info_t conn[], uint32_t num);
    int (*get_sockets)(uint32_t role, hccp_sock_info_t info[], uint32_t num, uint32_t *cnt);
    int (*white_list_add)(void *handle, const hccp_white_info_t list[], uint32_t num);
    int (*socket_send)(const void *fd, const void *buf, uint64_t len, uint64_t *done);
    int (*socket_recv)(const void *fd, void *buf, uint64_t len, uint64_t *done);
};

struct probe_opts {
    const char *lib_path;
    const char *mode;
    const char *local_ip;
    const char *remote_ip;
    const char *allow_ip;
    uint32_t dev_id;
    uint32_t port;
    uint32_t role;
    uint32_t rank_count;
    int timeout_sec;
    const char *tag;
    int do_echo;
};

static const char *g_prog = "hccn_probe";

static int64_t now_ms(void)
{
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void msleep(uint32_t ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000U;
    ts.tv_nsec = (long)(ms % 1000U) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

static void log_ts(int64_t start_ms, const char *fmt, ...)
{
    va_list ap;
    printf("[%8.3fs] ", (double)(now_ms() - start_ms) / 1000.0);
    va_start(ap, fmt);
    (void)vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    (void)fflush(stdout);
}

static const char *status_name(int status)
{
    switch (status) {
        case 0:
            return "0:not-connected";
        case 1:
            return "1:connected";
        case 2:
            return "2:connect-timeout";
        case 3:
            return "3:connecting";
        default:
            return "?:unknown";
    }
}

static void *sym_or_alt(void *lib, const char *primary, const char *alt, const char **used)
{
    void *fn = dlsym(lib, primary);
    if (fn != NULL) {
        *used = primary;
        return fn;
    }
    fn = dlsym(lib, alt);
    if (fn != NULL) {
        *used = alt;
    }
    return fn;
}

static int load_api(const char *lib_path, struct probe_api *api, int64_t start_ms)
{
    void *lib = dlopen(lib_path, RTLD_NOW | RTLD_NODELETE);
    if (lib == NULL) {
        log_ts(start_ms, "ERROR dlopen(%s) failed: %s (source set_env.sh or set LD_LIBRARY_PATH)", lib_path, dlerror());
        return 1;
    }
    void *tsd = dlopen("libtsdclient.so", RTLD_NOW | RTLD_NODELETE);
    if (tsd == NULL) {
        const char *err = dlerror();
        log_ts(start_ms, "WARN dlopen(libtsdclient.so) failed: %s (TsdOpen unavailable, cold path will fail)",
               (err != NULL) ? err : "unknown");
    }
    const char *used = NULL;
    struct {
        void **slot;
        void *lib;
        const char *primary;
        const char *alt;
    } items[] = {
        {(void **)&api->ra_init, lib, "ra_init", "RaInit"},
        {(void **)&api->rdev_init_v2, lib, "ra_rdev_init_v2", "RaRdevInitV2"},
        {(void **)&api->rdev_get_handle, lib, "ra_rdev_get_handle", "RaRdevGetHandle"},
        {(void **)&api->get_ifnum, lib, "ra_get_ifnum", "RaGetIfnum"},
        {(void **)&api->get_ifaddrs, lib, "ra_get_ifaddrs", "RaGetIfaddrs"},
        {(void **)&api->socket_init, lib, "ra_socket_init", "RaSocketInit"},
        {(void **)&api->socket_deinit, lib, "ra_socket_deinit", "RaSocketDeinit"},
        {(void **)&api->listen_start, lib, "ra_socket_listen_start", "RaSocketListenStart"},
        {(void **)&api->listen_stop, lib, "ra_socket_listen_stop", "RaSocketListenStop"},
        {(void **)&api->batch_connect, lib, "ra_socket_batch_connect", "RaSocketBatchConnect"},
        {(void **)&api->batch_close, lib, "ra_socket_batch_close", "RaSocketBatchClose"},
        {(void **)&api->get_sockets, lib, "ra_get_sockets", "RaGetSockets"},
        {(void **)&api->white_list_add, lib, "ra_socket_white_list_add", "RaSocketWhiteListAdd"},
        {(void **)&api->socket_send, lib, "ra_socket_send", "RaSocketSend"},
        {(void **)&api->socket_recv, lib, "ra_socket_recv", "RaSocketRecv"},
        {(void **)&api->tsd_open, tsd, "TsdOpen", "TsdOpen"},
    };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        *items[i].slot = (items[i].lib != NULL) ? sym_or_alt(items[i].lib, items[i].primary, items[i].alt, &used) : NULL;
        log_ts(start_ms, "dlsym %-26s -> %s", items[i].primary,
               (*items[i].slot != NULL) ? used : "MISSING");
    }
    int required_missing = (api->ra_init == NULL) || (api->rdev_init_v2 == NULL) || (api->socket_init == NULL) ||
                           (api->listen_start == NULL) || (api->batch_connect == NULL) || (api->get_sockets == NULL);
    if (required_missing) {
        log_ts(start_ms, "ERROR required libra.so symbols missing, cannot continue");
        return 1;
    }
    return 0;
}

static void fill_tag(char dst[HCCP_SOCK_CONN_TAG_SIZE], const char *text)
{
    (void)memset(dst, 0, HCCP_SOCK_CONN_TAG_SIZE);
    size_t len = strlen(text);
    if (len >= HCCP_SOCK_CONN_TAG_SIZE) {
        len = HCCP_SOCK_CONN_TAG_SIZE - 1U;
    }
    (void)memcpy(dst, text, len);
}

static const char *hccn_ret_hint(int ret)
{
    return (ret == 128203) ? " [128203: invalid state/resource, usually missing RaInit/RaRdevInitV2 precondition]"
                           : "";
}

static int verify_device_ip(const struct probe_api *api, const struct probe_opts *o, int64_t start_ms)
{
    if (api->get_ifnum == NULL || api->get_ifaddrs == NULL) {
        log_ts(start_ms, "WARN get_ifnum/get_ifaddrs missing, skip device ip verification");
        return 0;
    }
    hccp_if_attr_t attr;
    (void)memset(&attr, 0, sizeof(attr));
    attr.phy_id = o->dev_id;
    attr.nic_position = (uint32_t)NETWORK_OFFLINE;
    attr.is_all = true;
    uint32_t count = 0;
    if (api->get_ifnum(&attr, &count) != 0 || count == 0U) {
        log_ts(start_ms, "WARN RaGetIfnum failed, skip device ip verification");
        return 0;
    }
    hccp_if_info_t infos[8];
    if (count > 8U) {
        count = 8U;
    }
    if (api->get_ifaddrs(&attr, infos, &count) != 0) {
        log_ts(start_ms, "WARN RaGetIfaddrs failed, skip device ip verification");
        return 0;
    }
    char want_name[16];
    (void)snprintf(want_name, sizeof(want_name), "eth%u", o->dev_id);
    struct in_addr want;
    want.s_addr = inet_addr(o->local_ip);
    int found = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (infos[i].family != AF_INET) {
            continue;
        }
        char ipstr[INET_ADDRSTRLEN] = {0};
        (void)inet_ntop(AF_INET, &infos[i].ifaddr.ip.addr, ipstr, sizeof(ipstr));
        log_ts(start_ms, "device interface: name=%s ip=%s", infos[i].ifname, ipstr);
        if (strcmp(infos[i].ifname, want_name) == 0) {
            found = 1;
            if (infos[i].ifaddr.ip.addr.s_addr != want.s_addr) {
                log_ts(start_ms, "WARN eth%u real ip %s != given -i %s", o->dev_id, ipstr, o->local_ip);
            } else {
                log_ts(start_ms, "device ip verified: eth%u == %s", o->dev_id, ipstr);
            }
        }
    }
    if (found == 0) {
        log_ts(start_ms, "WARN interface %s not found among %u entries, mapping unverified", want_name, count);
    }
    return 0;
}

static int rdev_init_v2(const struct probe_api *api, const struct probe_opts *o, int64_t start_ms)
{
    hccp_rdev_init_t info;
    (void)memset(&info, 0, sizeof(info));
    info.mode = NETWORK_OFFLINE;
    info.notify_type = 1U; /* NOTIFY, mirror product RaRdevInit */
    info.enabled_2mb_lite = true;
    hccp_rdev_t rdev;
    (void)memset(&rdev, 0, sizeof(rdev));
    rdev.phy_id = o->dev_id;
    rdev.family = AF_INET;
    rdev.local_ip.addr.s_addr = inet_addr(o->local_ip);
    void *rdma_handle = NULL;
    int ret = api->rdev_init_v2(info, rdev, &rdma_handle);
    if (ret != 0 || rdma_handle == NULL) {
        log_ts(start_ms, "ERROR RaRdevInitV2 failed: ret=%d dev=%u ip=%s%s", ret, o->dev_id, o->local_ip,
               hccn_ret_hint(ret));
        return 1;
    }
    log_ts(start_ms, "RaRdevInitV2 OK dev=%u ip=%s handle=%p", o->dev_id, o->local_ip, rdma_handle);
    return 0;
}

static int reuse_or_open_tsd(const struct probe_api *api, const struct probe_opts *o, int64_t start_ms, int *reused)
{
    void *rdma_handle = NULL;
    if (api->rdev_get_handle != NULL && api->rdev_get_handle(o->dev_id, &rdma_handle) == 0 && rdma_handle != NULL) {
        log_ts(start_ms, "RaRdevGetHandle OK dev=%u handle=%p (device already opened, reuse)", o->dev_id, rdma_handle);
        *reused = 1;
        return 0;
    }
    if (api->tsd_open == NULL) {
        log_ts(start_ms, "ERROR TsdOpen unavailable (libtsdclient.so not loaded) and device not opened by anyone");
        return 1;
    }
    uint32_t trc = api->tsd_open(o->dev_id, o->rank_count);
    if (trc != 0U) {
        log_ts(start_ms, "ERROR TsdOpen failed: ret=%u dev=%u rank_count=%u", trc, o->dev_id, o->rank_count);
        return 1;
    }
    log_ts(start_ms, "TsdOpen OK dev=%u rank_count=%u", o->dev_id, o->rank_count);
    *reused = 0;
    return 0;
}

static int init_rdev(const struct probe_api *api, const struct probe_opts *o, int64_t start_ms)
{
    int reused = 0;
    if (reuse_or_open_tsd(api, o, start_ms, &reused) != 0) {
        return 1;
    }
    if (reused) {
        return verify_device_ip(api, o, start_ms);
    }
    hccp_ra_init_cfg_t cfg;
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.phy_id = o->dev_id;
    cfg.nic_position = (uint32_t)NETWORK_OFFLINE;
    cfg.hdc_type = 6; /* HDC_SERVICE_TYPE_RDMA, mirror product RaInit */
    int ret = api->ra_init(&cfg);
    if (ret != 0) {
        log_ts(start_ms, "WARN RaInit failed: ret=%d, try RaRdevGetHandle fallback%s", ret, hccn_ret_hint(ret));
        void *handle = NULL;
        if (api->rdev_get_handle != NULL && api->rdev_get_handle(o->dev_id, &handle) == 0 && handle != NULL) {
            log_ts(start_ms, "RaRdevGetHandle OK after RaInit-warn handle=%p", handle);
            return verify_device_ip(api, o, start_ms);
        }
        log_ts(start_ms, "ERROR no usable rdma handle: device not initialized by anyone; run a product process once "
                         "on this node or check driver state");
        return 1;
    }
    log_ts(start_ms, "RaInit OK dev=%u", o->dev_id);
    if (rdev_init_v2(api, o, start_ms) != 0) {
        return 1;
    }
    return verify_device_ip(api, o, start_ms);
}

static int init_socket(const struct probe_api *api, const struct probe_opts *o, void **sock, int64_t start_ms)
{
    hccp_rdev_t rdev;
    (void)memset(&rdev, 0, sizeof(rdev));
    rdev.phy_id = o->dev_id;
    rdev.family = AF_INET;
    rdev.local_ip.addr.s_addr = inet_addr(o->local_ip);
    int ret = api->socket_init(NETWORK_OFFLINE, rdev, sock);
    if (ret != 0 || *sock == NULL) {
        log_ts(start_ms, "ERROR RaSocketInit failed: ret=%d dev=%u ip=%s", ret, o->dev_id, o->local_ip);
        return 1;
    }
    log_ts(start_ms, "RaSocketInit OK dev=%u ip=%s handle=%p", o->dev_id, o->local_ip, *sock);
    return 0;
}

static void fill_query(hccp_sock_info_t *q, void *sock, const char *remote_ip, const char *tag)
{
    (void)memset(q, 0, sizeof(*q));
    q->handle = sock;
    q->remote_ip.addr.s_addr = inet_addr(remote_ip);
    q->status = 0;
    fill_tag(q->tag, tag);
}

static int poll_connected(const struct probe_api *api, void *sock, const struct probe_opts *o, hccp_sock_info_t *out,
                          int64_t start_ms)
{
    int last_status = -1;
    int64_t deadline = now_ms() + (int64_t)o->timeout_sec * 1000L;
    while (now_ms() < deadline) {
        hccp_sock_info_t q;
        fill_query(&q, sock, o->remote_ip, o->tag);
        uint32_t cnt = 0;
        int ret = api->get_sockets(o->role, &q, 1U, &cnt);
        if (ret != 0) {
            log_ts(start_ms, "ERROR RaGetSockets failed: ret=%d role=%u remote=%s", ret, o->role, o->remote_ip);
            return 1;
        }
        if (q.status != last_status) {
            log_ts(start_ms, "status -> %s (fd=%p cnt=%u)", status_name(q.status), q.fd, cnt);
            last_status = q.status;
        }
        if (cnt > 0 && q.status == 1 && q.fd != NULL) {
            *out = q;
            return 0;
        }
        if (q.status == 2) {
            log_ts(start_ms, "hccn reports connect-timeout (status=2), stop waiting");
            return 1;
        }
        msleep(100U);
    }
    log_ts(start_ms, "probe timeout after %ds, last status=%s", o->timeout_sec, status_name(last_status));
    return 1;
}

static int echo_round(const struct probe_api *api, void *fd, int am_connector, int64_t start_ms)
{
    char buf[4] = {0};
    uint64_t done = 0;
    if (am_connector) {
        (void)memcpy(buf, "PING", 4);
        int ret = api->socket_send(fd, buf, 4U, &done);
        log_ts(start_ms, "send PING ret=%d done=%llu", ret, (unsigned long long)done);
        if (ret != 0 || done != 4U) {
            return 1;
        }
        (void)memset(buf, 0, sizeof(buf));
        ret = api->socket_recv(fd, buf, 4U, &done);
        log_ts(start_ms, "recv ret=%d done=%llu data=%.4s", ret, (unsigned long long)done, buf);
        return (ret == 0 && done == 4U) ? 0 : 1;
    }
    int ret = api->socket_recv(fd, buf, 4U, &done);
    log_ts(start_ms, "recv ret=%d done=%llu data=%.4s", ret, (unsigned long long)done, buf);
    if (ret != 0 || done != 4U) {
        return 1;
    }
    (void)memcpy(buf, "PONG", 4);
    ret = api->socket_send(fd, buf, 4U, &done);
    log_ts(start_ms, "send PONG ret=%d done=%llu", ret, (unsigned long long)done);
    return (ret == 0 && done == 4U) ? 0 : 1;
}

static int add_whitelist(const struct probe_api *api, void *sock, const struct probe_opts *o, int64_t start_ms)
{
    if (api->white_list_add == NULL) {
        log_ts(start_ms, "WARN white_list_add symbol missing, skip whitelist");
        return 0;
    }
    hccp_white_info_t w;
    (void)memset(&w, 0, sizeof(w));
    w.remote_ip.addr.s_addr = inet_addr(o->allow_ip);
    w.conn_limit = 8U;
    fill_tag(w.tag, o->tag);
    int ret = api->white_list_add(sock, &w, 1U);
    if (ret != 0) {
        log_ts(start_ms, "ERROR RaSocketWhiteListAdd failed: ret=%d ip=%s%s", ret, o->allow_ip, hccn_ret_hint(ret));
        return 1;
    }
    log_ts(start_ms, "RaSocketWhiteListAdd OK ip=%s tag=%s", o->allow_ip, o->tag);
    return 0;
}

static int run_listen(const struct probe_api *api, const struct probe_opts *o, int64_t start_ms)
{
    void *sock = NULL;
    if (init_socket(api, o, &sock, start_ms) != 0) {
        return 1;
    }
    if (o->allow_ip != NULL && add_whitelist(api, sock, o, start_ms) != 0) {
        (void)api->socket_deinit(sock);
        return 1;
    }
    hccp_listen_info_t li;
    uint32_t tried = 0;
    int ret;
    do {
        (void)memset(&li, 0, sizeof(li));
        li.handle = sock;
        li.port = o->port + tried;
        ret = api->listen_start(&li, 1U);
        if (ret == 0) {
            break;
        }
        log_ts(start_ms, "listen_start port=%u failed: ret=%d err=%u, trying next port", li.port, ret, li.err);
        tried++;
    } while (tried < 128U);
    if (ret != 0) {
        log_ts(start_ms, "ERROR RaSocketListenStart failed after %u ports from %u", tried, o->port);
        (void)api->socket_deinit(sock);
        return 1;
    }
    log_ts(start_ms, "RaSocketListenStart OK port=%u (asked=%u)", li.port, o->port);

    hccp_sock_info_t conn;
    if (poll_connected(api, sock, o, &conn, start_ms) != 0) {
        log_ts(start_ms, "VERDICT: FAIL no socket established within %ds (listen side)", o->timeout_sec);
        (void)api->listen_stop(&li, 1U);
        (void)api->socket_deinit(sock);
        return 1;
    }
    log_ts(start_ms, "VERDICT: PASS socket established, fd=%p remote pending echo", conn.fd);
    int rc = 0;
    if (o->do_echo) {
        rc = echo_round(api, conn.fd, 0, start_ms);
        log_ts(start_ms, "echo(recv->send) %s", (rc == 0) ? "OK" : "FAILED");
    }
    hccp_close_info_t ci;
    (void)memset(&ci, 0, sizeof(ci));
    ci.handle = sock;
    ci.fd = conn.fd;
    ci.linger = 0;
    if (api->batch_close != NULL) {
        (void)api->batch_close(&ci, 1U);
    }
    (void)api->listen_stop(&li, 1U);
    (void)api->socket_deinit(sock);
    return rc;
}

static int run_connect(const struct probe_api *api, const struct probe_opts *o, int64_t start_ms)
{
    void *sock = NULL;
    if (init_socket(api, o, &sock, start_ms) != 0) {
        return 1;
    }
    hccp_conn_info_t ci;
    (void)memset(&ci, 0, sizeof(ci));
    ci.handle = sock;
    ci.remote_ip.addr.s_addr = inet_addr(o->remote_ip);
    ci.port = o->port;
    fill_tag(ci.tag, o->tag);
    int ret = api->batch_connect(&ci, 1U);
    if (ret != 0) {
        log_ts(start_ms, "ERROR RaSocketBatchConnect failed immediately: ret=%d remote=%s:%u%s", ret, o->remote_ip,
               o->port, hccn_ret_hint(ret));
        (void)api->socket_deinit(sock);
        return 1;
    }
    log_ts(start_ms, "RaSocketBatchConnect submitted remote=%s:%u tag=%s", o->remote_ip, o->port, o->tag);

    hccp_sock_info_t conn;
    if (poll_connected(api, sock, o, &conn, start_ms) != 0) {
        log_ts(start_ms, "VERDICT: FAIL socket not established within %ds (connect side)", o->timeout_sec);
        (void)api->socket_deinit(sock);
        return 1;
    }
    log_ts(start_ms, "VERDICT: PASS socket established, fd=%p", conn.fd);
    int rc = 0;
    if (o->do_echo) {
        rc = echo_round(api, conn.fd, 1, start_ms);
        log_ts(start_ms, "echo(send->recv) %s", (rc == 0) ? "OK" : "FAILED");
    }
    hccp_close_info_t cl;
    (void)memset(&cl, 0, sizeof(cl));
    cl.handle = sock;
    cl.fd = conn.fd;
    cl.linger = 0;
    if (api->batch_close != NULL) {
        (void)api->batch_close(&cl, 1U);
    }
    (void)api->socket_deinit(sock);
    return rc;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: %s <listen|connect> [-d dev_id] -i local_ip -r remote_ip [-p port] [-t timeout_sec]\n"
            "       [-a allow_ip] [-n rank_count] [--role 0|1] [--tag TAG] [--lib PATH] [--echo]\n"
            "  listen : bind NPU RoCE ip and wait for one incoming hccn socket (role default 0)\n"
            "  connect: connect out to remote NPU RoCE ip (role default 1)\n"
            "  -d NPU device id (phyId, default 0)   -i local RoCE ip bound by RaSocketInit\n"
            "  -r remote ip, required in BOTH modes (listen side: expected client ip used by the query)\n"
            "  -p port (default 10010, away from product 10005)\n"
            "  -a whitelist the given source ip on listen side (recommended, mirrors product)\n"
            "  -n rank count passed to TsdOpen (default 2)\n"
            "  -t probe timeout seconds (listen default 60, connect default 15)\n"
            "  --role override RaGetSockets role (0=server side, 1=client side)\n"
            "  --tag  hccn tag (default TRANS_TAG, mirror FillHccpTag; try BM_TAG if needed)\n"
            "  --lib  libra.so path override (default libra.so from LD_LIBRARY_PATH)\n"
            "  --echo after establishment, exchange 4 bytes via ra_socket_send/recv (may block)\n",
            g_prog);
}

static int parse_opts(int argc, char *argv[], struct probe_opts *o)
{
    (void)memset(o, 0, sizeof(*o));
    o->lib_path = "libra.so";
    o->dev_id = 0;
    o->port = 10010U;
    o->timeout_sec = -1;
    o->role = UINT32_MAX;
    o->rank_count = 2U;
    o->tag = "TRANS_TAG";
    static const struct option longOpts[] = {
        {"role", required_argument, NULL, 'R'}, {"tag", required_argument, NULL, 'T'},
        {"lib", required_argument, NULL, 'L'},  {"echo", no_argument, NULL, 'e'},
        {NULL, 0, NULL, 0},
    };
    int ch;
    while ((ch = getopt_long(argc, argv, "d:i:r:p:t:a:n:h", longOpts, NULL)) != -1) {
        switch (ch) {
            case 'd':
                o->dev_id = (uint32_t)strtoul(optarg, NULL, 0);
                break;
            case 'i':
                o->local_ip = optarg;
                break;
            case 'r':
                o->remote_ip = optarg;
                break;
            case 'p':
                o->port = (uint32_t)strtoul(optarg, NULL, 0);
                break;
            case 't':
                o->timeout_sec = (int)strtol(optarg, NULL, 0);
                break;
            case 'a':
                o->allow_ip = optarg;
                break;
            case 'n':
                o->rank_count = (uint32_t)strtoul(optarg, NULL, 0);
                break;
            case 'R':
                o->role = (uint32_t)strtoul(optarg, NULL, 0);
                break;
            case 'T':
                o->tag = optarg;
                break;
            case 'L':
                o->lib_path = optarg;
                break;
            case 'e':
                o->do_echo = 1;
                break;
            case 'h':
            default:
                usage();
                return 2;
        }
    }
    if (optind >= argc || o->local_ip == NULL || o->remote_ip == NULL) {
        usage();
        return 2;
    }
    o->mode = argv[optind];
    int is_listen = (strcmp(o->mode, "listen") == 0);
    int is_connect = (strcmp(o->mode, "connect") == 0);
    if (!is_listen && !is_connect) {
        usage();
        return 2;
    }
    if (o->timeout_sec < 0) {
        o->timeout_sec = is_listen ? 60 : 15;
    }
    if (o->role == UINT32_MAX) {
        o->role = is_listen ? 0U : 1U;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    struct probe_opts o;
    int prc = parse_opts(argc, argv, &o);
    if (prc != 0) {
        return prc;
    }
    int is_listen = (strcmp(o.mode, "listen") == 0);

    int64_t start_ms = now_ms();
    printf("%s mode=%s dev=%u local=%s remote=%s port=%u role=%u tag=%s lib=%s timeout=%ds\n", g_prog, o.mode,
           o.dev_id, o.local_ip, o.remote_ip, o.port, o.role, o.tag, o.lib_path, o.timeout_sec);
    struct probe_api api;
    (void)memset(&api, 0, sizeof(api));
    if (load_api(o.lib_path, &api, start_ms) != 0) {
        return 1;
    }
    if (init_rdev(&api, &o, start_ms) != 0) {
        return 1;
    }
    int rc = is_listen ? run_listen(&api, &o, start_ms) : run_connect(&api, &o, start_ms);
    log_ts(start_ms, "exit rc=%d", rc);
    return rc;
}
