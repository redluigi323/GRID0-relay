/* Actual host sockets, confined to loopback; no console, pcap or root needed. */
#include "../src/native-udp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void pump(uv_loop_t *loop, struct native_udp_guard *guard, uint64_t count)
{
    uint64_t deadline = uv_hrtime() + 1000000000ULL;
    while (native_udp_guard_received(guard) < count && uv_hrtime() < deadline) {
        uv_run(loop, UV_RUN_NOWAIT);
        usleep(1000);
    }
    assert(native_udp_guard_received(guard) == count);
}

int main(void)
{
    uv_loop_t loop;
    assert(uv_loop_init(&loop) == 0);
    const uint8_t local[4] = {127,0,0,1};
    struct native_udp_guard *guard, *conflict;
    assert(native_udp_guard_create(&loop, local, &guard) == 0);
    assert(native_udp_guard_create(&loop, local, &conflict) == 0);

    // Let the OS choose a currently unused test port.
    uv_udp_t picker;
    struct sockaddr_in address;
    assert(uv_udp_init(&loop, &picker) == 0);
    assert(uv_ip4_addr("127.0.0.1", 0, &address) == 0);
    assert(uv_udp_bind(&picker, (struct sockaddr *)&address, 0) == 0);
    int length = sizeof(address);
    assert(uv_udp_getsockname(&picker, (struct sockaddr *)&address, &length) == 0);
    uint16_t port = ntohs(address.sin_port);
    uv_close((uv_handle_t *)&picker, NULL);
    uv_run(&loop, UV_RUN_DEFAULT);

    assert(native_udp_guard_reserve(guard, 0) == UV_EINVAL);
    assert(native_udp_guard_reserve(guard, port) == 0);
    assert(native_udp_guard_reserve(guard, port) == 0);
    assert(native_udp_guard_reserve(conflict, port) == UV_EADDRINUSE);
    native_udp_guard_close(conflict); // Also exercise pending failed-bind close.

    uv_udp_t sender;
    assert(uv_udp_init(&loop, &sender) == 0);
    char payload[4096] = {0};
    const unsigned sizes[] = {620, 0, sizeof(payload)};
    for (unsigned i = 0; i < 3; ++i) {
        uv_buf_t buffer = uv_buf_init(payload, sizes[i]);
        assert(uv_udp_try_send(&sender, &buffer, 1, (struct sockaddr *)&address) == (int)sizes[i]);
        pump(&loop, guard, i + 1);
    }
    uv_close((uv_handle_t *)&sender, NULL);
    native_udp_guard_close(guard);
    uv_run(&loop, UV_RUN_DEFAULT);

    // Closing releases the endpoint for the next relay run.
    assert(native_udp_guard_create(&loop, local, &guard) == 0);
    assert(native_udp_guard_reserve(guard, port) == 0);
    native_udp_guard_close(guard);
    uv_run(&loop, UV_RUN_DEFAULT);
    assert(uv_loop_close(&loop) == 0);
    puts("PASS: real UDP bind, conflict, idempotence, receive/drain, empty/large datagrams, cleanup/rebind");
    return 0;
}
