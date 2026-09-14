// Exercise the real adapter dispatch/error path without opening a live NIC.
#include "../src/pcaploop.cpp"
#include <cassert>

static unsigned callbacks;
static void received(uv_pcap_t *, const pcap_pkthdr *, const u_char *, const uint8_t *)
{
    ++callbacks;
}

int main()
{
    uv_pcap_t capture{};
    uv_pcap_inner inner{};
    uv_pcap_interf_t adapter{};
    capture.inner = &inner; capture.cb = received;
    inner.interfaces = &adapter; inner.count = 1;
    adapter.data = &capture;
    adapter.dev = pcap_open_dead(DLT_EN10MB, 65535);
    assert(adapter.dev);
    const uint8_t host[6] = {2, 1, 2, 3, 4, 5};
    memcpy(adapter.mac, host, 6);
    uint8_t frame[60] = {};
    memcpy(frame, host, 6);
    memset(frame + 6, 2, 6);
    pcap_pkthdr header{};
    header.caplen = header.len = sizeof(frame);
    for (int i = 0; i < 1024; ++i) uv_pcap_callback(&adapter, &header, frame);
    assert(adapter.delivered == 1024 && callbacks == 1024 && inner.map.size() == 1);
    header.caplen = 8;
    uv_pcap_callback(&adapter, &header, frame);
    assert(adapter.delivered == 1025 && callbacks == 1024);
    uint8_t original[sizeof(frame)]; memcpy(original, frame, sizeof(frame));
    // A dead capture rejects injection. Count that failure, retain caller bytes.
    assert(uv_pcap_sendpacket(&capture, frame, sizeof(frame)) != 0);
    assert(adapter.send_failures == 1 && !memcmp(original, frame, sizeof(frame)));
    memcpy(frame + 6, host, 6); memcpy(original, frame, sizeof(frame));
    assert(uv_pcap_sendpacket(&capture, frame, sizeof(frame)) != 0);
    assert(adapter.send_failures == 2 && !memcmp(original, frame, sizeof(frame)));
    pcap_close(adapter.dev);
    puts("PASS: capture delivery accounting, truncated frames, injection errors, source MAC restoration");
}
