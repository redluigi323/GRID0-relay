/* Offline test of the actual relay functions. No adapters or privileges needed.
 * Include the implementation to exercise internal forwarding/diagnostic paths. */
#include "../src/lan-play.c"

struct cli_options options;
static uint8_t sent[2048];
static int sent_len, sends;
static uv_pcap_t *sent_handle;
static uint16_t reserved_port;
static int reserve_error;

/* Real socket ownership/draining is tested separately on loopback. */
int native_udp_guard_reserve(struct native_udp_guard *guard, uint16_t port)
{
    (void)guard;
    reserved_port = port;
    return reserve_error;
}

int uv_pcap_sendpacket(uv_pcap_t *h, const u_char *data, int size)
{
    assert(size <= sizeof(sent));
    memcpy(sent, data, size);
    sent_len = size; sent_handle = h; ++sends;
    return 0;
}
void packet_set_mac(struct packet_ctx *ctx, const uint8_t *mac) { ctx->mac = mac; }
void get_packet(struct packet_ctx *ctx, const struct pcap_pkthdr *h, const u_char *p)
{ (void)ctx; (void)h; (void)p; }
int send_ether(struct packet_ctx *ctx, const void *dst, uint16_t type, const struct payload *p)
{ (void)ctx; (void)dst; (void)type; (void)p; assert(!"Unexpected Wi-Fi ARP from overlay cache"); return -1; }

static void fixture(struct lan_play *lp)
{
    memset(lp, 0, sizeof(*lp));
    memcpy(lp->wifi_mac, "\x02\x01\x02\x03\x04\x05", 6);
    memcpy(lp->zerotier_mac, "\x02\x11\x12\x13\x14\x15", 6);
    memcpy(lp->switch_mac, "\x02\x21\x22\x23\x24\x25", 6);
    memcpy(lp->zerotier_ip, "\x0a\x93\x11\x9c", 4);
    memcpy(lp->switch_ip, lp->zerotier_ip, 4);
    memcpy(lp->zerotier_netmask, "\xff\xff\xff\x00", 4);
    memcpy(lp->zerotier_broadcast_ip, "\x0a\x93\x11\xff", 4);
    memcpy(lp->packet_ctx.ip, "\x0a\x93\x11\x01", 4);
    lp->switch_seen = true;
    lp->zerotier_neighbors.arp_ttl = 30;
    lp->pcap.data = lp->zerotier_pcap.data = lp;
}

static size_t datagram(uint8_t *frame, const uint8_t *srcmac, const uint8_t *src,
                       const uint8_t *dst, size_t payload_len)
{
    size_t size = 42 + payload_len;
    memset(frame, 0, size);
    memset(frame, 0xff, 6);
    memcpy(frame + 6, srcmac, 6);
    WRITE_NET16(frame, 12, ETHER_TYPE_IPV4);
    uint8_t *ip = frame + 14;
    ip[0] = 0x45; ip[8] = 64; ip[9] = 17;
    WRITE_NET16(ip, 2, 28 + payload_len);
    WRITE_NET16(ip, 20, 35000); WRITE_NET16(ip, 22, 35000);
    WRITE_NET16(ip, 24, 8 + payload_len);
    for (size_t i = 0; i < payload_len; ++i) ip[28 + i] = (uint8_t)(i * 7);
    assert(rewrite_ipv4_addresses(ip, size - 14, src, dst));
    return size;
}

int main(int argc, char **argv)
{
    struct lan_play lp;
    fixture(&lp);
    uint8_t frame[1600], payload[620];
    const uint8_t peer[4] = {10,147,17,26};
    const uint8_t peer_mac[6] = {2,0x31,0x32,0x33,0x34,0x35};
    const uint8_t broadcast[4] = {10,255,255,255};
    struct pcap_pkthdr h = {0};

    // Incoming discovery learns an overlay-only neighbor and forwards once,
    // without injecting an unsolicited ARP reply.
    h.caplen = h.len = datagram(frame, peer_mac, peer, lp.zerotier_broadcast_ip, 325);
    sends = 0;
    lan_play_zerotier_pcap_handler(&lp.zerotier_pcap, &h, frame, lp.zerotier_mac);
    assert(sends == 1 && sent_handle == &lp.pcap);
    assert(!memcmp(sent + 30, lp.zerotier_broadcast_ip, 4));
    assert(!memcmp(sent + 14, frame + 14, h.caplen - 14));
    assert(arp_has_ip(&lp.zerotier_neighbors, peer));
    assert(!arp_has_ip(&lp.packet_ctx, peer));

    // Stock Switch's 620-byte browse payload leaves as Ethernet UNICAST,
    // exactly intact, with valid transport and IP checksums.
    h.caplen = h.len = datagram(frame, lp.switch_mac, lp.switch_ip, peer, 620);
    memcpy(payload, frame + 42, 620);
    sends = 0;
    assert(lan_play_relay_wifi_ipv4(&lp, frame, h.caplen));
    assert(sends == 1 && sent_handle == &lp.zerotier_pcap && sent_len == 662);
    assert(reserved_port == 35000);
    assert(!memcmp(sent, peer_mac, 6));
    assert(!memcmp(sent + 42, payload, 620));
    assert(ipv4_header_checksum(sent + 14, 20) == 0);
    assert(udp_checksum(sent + 14, 20, 648) == 0xffff);

    // Both even and odd UDP payload lengths, plus round trip identity.
    for (size_t n = 0; n <= 620; ++n) {
        size_t len = datagram(frame, lp.switch_mac, lp.switch_ip, broadcast, n);
        assert(lan_play_relay_wifi_ipv4(&lp, frame, len));
        assert(ipv4_header_checksum(sent + 14, 20) == 0);
        assert(udp_checksum(sent + 14, 20, len - 14) == 0xffff);
        assert(!memcmp(frame + 42, sent + 42, n));
    }
    assert(lp.warned_broadcast_mismatch);
    // Correctly configured /24 discovery keeps the complete IPv4 packet intact.
    lp.warned_broadcast_mismatch = false;
    size_t matching_len = datagram(frame, lp.switch_mac, lp.switch_ip, lp.zerotier_broadcast_ip, 325);
    assert(lan_play_relay_wifi_ipv4(&lp, frame, matching_len));
    assert(!lp.warned_broadcast_mismatch);
    assert(!memcmp(frame + 14, sent + 14, matching_len - 14));

    // Unknown peers must not cause a null-context ARP send or lose the packet.
    const uint8_t unknown[4] = {10,147,17,99};
    size_t len = datagram(frame, lp.switch_mac, lp.switch_ip, unknown, 620);
    sends = 0;
    assert(lan_play_relay_wifi_ipv4(&lp, frame, len));
    assert(sends == 2 && sent_len == 662 && sent[0] == 255);

    // A new game source port is reserved before transmission; a conflict
    // consumes/drops the frame instead of falling through to legacy transport.
    WRITE_NET16(frame, 34, 49154);
    sends = 0; reserve_error = UV_EADDRINUSE;
    assert(lan_play_relay_wifi_ipv4(&lp, frame, len));
    assert(reserved_port == 49154 && sends == 0);
    reserve_error = 0;
    assert(lan_play_relay_wifi_ipv4(&lp, frame, len));
    assert(sends == 2);

    // A frame emitted by the host must never be relayed or learned as a Switch.
    h.caplen = h.len = datagram(frame, lp.zerotier_mac, lp.zerotier_ip, peer, 8);
    sends = 0;
    lan_play_zerotier_pcap_handler(&lp.zerotier_pcap, &h, frame, lp.zerotier_mac);
    assert(sends == 0);
    memcpy(frame + 6, lp.wifi_mac, 6);
    lan_play_pcap_handler(&lp.pcap, &h, frame, lp.wifi_mac);
    assert(sends == 0 && !memcmp(lp.switch_mac, "\x02\x21\x22\x23\x24\x25", 6));

    // Diagnostic unicast delivery probe follows a browse reply only once.
    options.diagnostics = true;
    h.caplen = h.len = datagram(frame, peer_mac, peer, lp.zerotier_ip, 620);
    sends = 0;
    assert(lan_play_relay_zerotier_ipv4(&lp, frame, h.caplen));
    assert(sends == 2 && sent_len == 58 && sent[34] == 8);
    assert(ipv4_header_checksum(sent + 14, 20) == 0);
    assert(ipv4_header_checksum(sent + 34, 24) == 0);
    uint8_t echo[58]; memcpy(echo, sent, sizeof(echo));
    sends = 0;
    assert(lan_play_relay_zerotier_ipv4(&lp, frame, h.caplen));
    assert(sends == 1); // No repeated probe or extra game packet.
    memcpy(echo, lp.wifi_mac, 6); memcpy(echo + 6, lp.switch_mac, 6);
    echo[34] = 0; WRITE_NET16(echo, 36, 0);
    WRITE_NET16(echo, 36, ipv4_header_checksum(echo + 34, 24));
    assert(rewrite_ipv4_addresses(echo + 14, 44, lp.switch_ip, lp.packet_ctx.ip));
    echo[57] ^= 1;
    assert(!consume_wifi_delivery_reply(&lp, echo, sizeof(echo)));
    echo[57] ^= 1;
    assert(!consume_wifi_delivery_reply(&lp, echo, 40));
    h.caplen = h.len = sizeof(echo); sends = 0;
    lan_play_pcap_handler(&lp.pcap, &h, echo, lp.wifi_mac);
    assert(lp.wifi_delivery_probe_replied && sends == 0);

    // Host ICMP port-unreachable quotes the game tuple but is never forwarded.
    options.diagnostics = true;
    memset(frame, 0, 70);
    memcpy(frame + 6, lp.zerotier_mac, 6);
    WRITE_NET16(frame, 12, ETHER_TYPE_IPV4);
    frame[14] = 0x45; frame[23] = 1;
    WRITE_NET16(frame, 16, 56);
    memcpy(frame + 26, lp.zerotier_ip, 4); memcpy(frame + 30, peer, 4);
    frame[34] = 3; frame[35] = 3;
    frame[42] = 0x45; frame[51] = 17;
    memcpy(frame + 54, peer, 4); memcpy(frame + 58, lp.zerotier_ip, 4);
    WRITE_NET16(frame, 62, 35000); WRITE_NET16(frame, 64, 35000);
    h.caplen = h.len = 70;
    sends = 0;
    lan_play_zerotier_pcap_handler(&lp.zerotier_pcap, &h, frame, lp.zerotier_mac);
    assert(sends == 0);
    // Invalid IHL/UDP lengths must not make diagnostics inspect payload memory.
    frame[14] = 0x4f;
    diagnostic_log_frame(&lp, "RX ZeroTier", frame, 34);
    frame[14] = 0x45; frame[23] = 17;
    WRITE_NET16(frame, 16, 28); WRITE_NET16(frame, 38, 65535);
    diagnostic_log_frame(&lp, "RX ZeroTier", frame, 42);
    options.diagnostics = false;

    // Write a synthetic capture for independent analysis with Python/pcap.
    if (argc == 2) {
        options.capture_prefix = argv[1];
        assert(open_captures(&lp) == 0);
        len = datagram(frame, lp.switch_mac, lp.switch_ip, peer, 620);
        diagnostic_log_frame(&lp, "RX Wi-Fi", frame, len);
        assert(lan_play_relay_wifi_ipv4(&lp, frame, len));
        close_captures(&lp);
        // Exclusive filenames reject accidental overwrite.
        assert(open_captures(&lp) != 0);
    }
    puts("PASS: forwarding, neighbor isolation, payload preservation, checksums, host-loop prevention, captures");
    return 0;
}
