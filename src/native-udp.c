#include "native-udp.h"
#include <stdlib.h>
#include <string.h>
#include <base/llog.h>

#define MAX_UDP_PORTS 64

struct native_udp_port {
    uv_udp_t socket;
    struct native_udp_guard *owner;
    struct native_udp_port *next;
    uint16_t port;
    char buffer[2048]; /* Discarding a truncated socket copy is intentional. */
};

struct native_udp_guard {
    uv_loop_t *loop;
    struct sockaddr_in address;
    struct native_udp_port *ports;
    unsigned count;
    uint64_t received;
};

static void free_port(uv_handle_t *handle)
{
    free(handle->data);
}

static void alloc_datagram(uv_handle_t *handle, size_t suggested, uv_buf_t *buf)
{
    (void)suggested;
    struct native_udp_port *port = handle->data;
    *buf = uv_buf_init(port->buffer, sizeof(port->buffer));
}

static void drain_datagram(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
                           const struct sockaddr *address, unsigned flags)
{
    (void)buf;
    (void)flags;
    struct native_udp_port *port = handle->data;
    if (nread < 0) {
        LLOG(LLOG_WARNING, "Host UDP/%u receive error: %s", port->port, uv_strerror((int)nread));
    } else if (address) {
        /* Includes empty datagrams. pcap is the only forwarding path. */
        port->owner->received++;
    }
}

int native_udp_guard_create(uv_loop_t *loop, const uint8_t ip[4],
                            struct native_udp_guard **out)
{
    *out = NULL;
    struct native_udp_guard *guard = calloc(1, sizeof(*guard));
    if (!guard) return UV_ENOMEM;
    guard->loop = loop;
    /* Bind only the managed IPv4 address, never all host interfaces. */
    uv_ip4_addr("0.0.0.0", 0, &guard->address);
    memcpy(&guard->address.sin_addr, ip, 4);
    *out = guard;
    return 0;
}

int native_udp_guard_reserve(struct native_udp_guard *guard, uint16_t port)
{
    if (!guard || !port) return UV_EINVAL;
    for (struct native_udp_port *p = guard->ports; p; p = p->next)
        if (p->port == port) return 0;
    if (guard->count >= MAX_UDP_PORTS) return UV_ENOSPC;

    struct native_udp_port *p = calloc(1, sizeof(*p));
    if (!p) return UV_ENOMEM;
    p->owner = guard;
    p->port = port;
    int ret = uv_udp_init(guard->loop, &p->socket);
    if (ret != 0) { free(p); return ret; }
    p->socket.data = p;
    struct sockaddr_in address = guard->address;
    address.sin_port = htons(port);
    /* Do not share a port with another application. Surface the conflict. */
    ret = uv_udp_bind(&p->socket, (const struct sockaddr *)&address, 0);
    if (ret == 0) ret = uv_udp_recv_start(&p->socket, alloc_datagram, drain_datagram);
    if (ret != 0) {
        uv_close((uv_handle_t *)&p->socket, free_port);
        return ret;
    }
    p->next = guard->ports;
    guard->ports = p;
    guard->count++;
    char ip[16];
    uv_ip4_name(&address, ip, sizeof(ip));
    LLOG(LLOG_INFO, "Host UDP endpoint ready: %s:%u (pcap handles forwarding)", ip, port);
    return 0;
}

uint64_t native_udp_guard_received(const struct native_udp_guard *guard)
{
    return guard ? guard->received : 0;
}

void native_udp_guard_close(struct native_udp_guard *guard)
{
    if (!guard) return;
    struct native_udp_port *p = guard->ports;
    while (p) {
        struct native_udp_port *next = p->next;
        uv_udp_recv_stop(&p->socket);
        /* No receive callback runs after close; free_port only owns the node. */
        uv_close((uv_handle_t *)&p->socket, free_port);
        p = next;
    }
    free(guard);
}
