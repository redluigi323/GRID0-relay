#ifndef ZLL_NATIVE_UDP_H
#define ZLL_NATIVE_UDP_H

#include <stdint.h>
#include <uv.h>

/* Own the host's UDP endpoints while pcap forwards the console's packets.
 * Socket copies are drained, never forwarded a second time. */
struct native_udp_guard;
int native_udp_guard_create(uv_loop_t *loop, const uint8_t ip[4],
                            struct native_udp_guard **out);
int native_udp_guard_reserve(struct native_udp_guard *guard, uint16_t port);
uint64_t native_udp_guard_received(const struct native_udp_guard *guard);
void native_udp_guard_close(struct native_udp_guard *guard);

#endif
