#include "lan-play.h"
#include "sha1.h"
#include "native-udp.h"
#include "nintendo_oui.h"

#define RETURN_ERR(lan_play, ...) \
    do { \
        snprintf((lan_play)->last_err, sizeof((lan_play)->last_err), __VA_ARGS__); \
        return -1; \
    } while (0)

struct lan_play real_lan_play;
uint8_t SEND_BUFFER[BUFFER_SIZE];
void lan_play_pcap_handler(uv_pcap_t *handle, const struct pcap_pkthdr *pkt_header, const u_char *packet, const uint8_t *mac);
void lan_play_zerotier_pcap_handler(uv_pcap_t *handle, const struct pcap_pkthdr *pkt_header, const u_char *packet, const uint8_t *mac);
static void switch_discovery_timer_cb(uv_timer_t *timer);
static void diagnostic_log_frame(struct lan_play *lan_play, const char *direction, const u_char *packet, int len);


static const char *capture_suffixes[5] = {
    "wifi-rx", "wifi-tx", "zerotier-rx", "zerotier-tx", "host"
};

static void flush_capture(struct lan_play *lp, int i)
{
    if (!lp->captures[i]) return;
    if (pcap_dump_flush(lp->captures[i]) != 0) {
        LLOG(LLOG_ERROR, "Capture write failed: %s", capture_suffixes[i]);
        pcap_dump_close(lp->captures[i]);
        lp->captures[i] = NULL;
    }
    lp->capture_pending[i] = 0;
}

static void close_captures(struct lan_play *lp)
{
    for (int i = 0; i < 5; ++i) {
        flush_capture(lp, i);
        if (lp->captures[i]) pcap_dump_close(lp->captures[i]);
        lp->captures[i] = NULL;
    }
    if (lp->capture_format) pcap_close(lp->capture_format);
    lp->capture_format = NULL;
}

static int open_captures(struct lan_play *lp)
{
    if (!options.capture_prefix) return 0;
    lp->capture_format = pcap_open_dead(DLT_EN10MB, 65535);
    if (!lp->capture_format) return -1;
    lp->capture_start_time = time(NULL);
    lp->capture_start_ns = uv_hrtime();
    memset(lp->capture_pending, 0, sizeof(lp->capture_pending));
    memset(lp->capture_last_flush_us, 0, sizeof(lp->capture_last_flush_us));
    for (int i = 0; i < 5; ++i) {
        char path[4096];
        int n = snprintf(path, sizeof(path), "%s-%s.pcap",
                         options.capture_prefix, capture_suffixes[i]);
        if (n < 0 || n >= (int)sizeof(path)) {
            close_captures(lp);
            return -1;
        }
        // Exclusive creation avoids overwriting an earlier reproduction.
#ifdef _WIN32
        wchar_t wide[4096];
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 4096)) {
            close_captures(lp); return -1;
        }
        HANDLE file = CreateFileW(wide, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) {
            LLOG(LLOG_ERROR, "Cannot create capture %s (Windows error %lu)", path, GetLastError());
            close_captures(lp); return -1;
        }
        // Pass an OS handle, never a FILE* across different C runtimes.
        lp->captures[i] = pcap_dump_hopen(lp->capture_format, (intptr_t)file);
        if (!lp->captures[i]) {
            CloseHandle(file); close_captures(lp); return -1;
        }
#else
        FILE *file = fopen(path, "wbx");
        if (!file) {
            LLOG(LLOG_ERROR, "Cannot create capture %s: %s", path, strerror(errno));
            close_captures(lp);
            return -1;
        }
        lp->captures[i] = pcap_dump_fopen(lp->capture_format, file);
        if (!lp->captures[i]) {
            fclose(file);
            close_captures(lp);
            return -1;
        }
#endif
        LLOG(LLOG_INFO, "Packet capture: %s (includes payloads)", path);
    }
    return 0;
}

static void capture_frame(struct lan_play *lp, const char *direction,
                          const u_char *packet, int len)
{
    int index = !strcmp(direction, "RX Wi-Fi") ? 0 :
                !strcmp(direction, "TX Wi-Fi") ? 1 :
                !strcmp(direction, "RX ZeroTier") ? 2 :
                !strcmp(direction, "TX ZeroTier") ? 3 : 4;
    if (len <= 0 || !lp->captures[index]) return;
    uint64_t elapsed_us = (uv_hrtime() - lp->capture_start_ns) / 1000;
    struct pcap_pkthdr h = {0};
    h.ts.tv_sec = lp->capture_start_time + elapsed_us / 1000000;
    h.ts.tv_usec = elapsed_us % 1000000;
    h.caplen = h.len = (bpf_u_int32)len;
    pcap_dump((u_char *)lp->captures[index], &h, packet);
    // Keep diagnostic disk writes off the per-packet path. Flush on activity
    // after 100 ms or 32 records, and flush every remaining record on shutdown.
    if (++lp->capture_pending[index] >= 32 ||
        elapsed_us - lp->capture_last_flush_us[index] >= 100000) {
        flush_capture(lp, index);
        lp->capture_last_flush_us[index] = elapsed_us;
    }
}

static bool is_nintendo_lan_broadcast(const uint8_t ip[4])
{
    return ip[0] == 10 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255;
}

static bool is_zerotier_destination(const struct lan_play *lan_play, const uint8_t ip[4])
{
    if (CMP_IPV4(ip, lan_play->zerotier_broadcast_ip)) return true;
    for (int i = 0; i < 4; ++i) {
        if ((ip[i] & lan_play->zerotier_netmask[i]) !=
            (lan_play->zerotier_ip[i] & lan_play->zerotier_netmask[i])) return false;
    }
    return true;
}

static bool ip_in_subnet(const uint8_t ip[4], const uint8_t net[4], const uint8_t mask[4])
{
    for (int i = 0; i < 4; ++i) {
        if ((ip[i] & mask[i]) != net[i]) return false;
    }
    return true;
}

uint16_t ipv4_header_checksum(const uint8_t *packet, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < len; i += 2) sum += ((uint16_t)packet[i] << 8) | packet[i + 1];
    if (len & 1) sum += (uint16_t)packet[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static void checksum_add_bytes(uint32_t *sum, const uint8_t *bytes, size_t len)
{
    while (len >= 2) {
        *sum += ((uint16_t)bytes[0] << 8) | bytes[1];
        bytes += 2;
        len -= 2;
    }
    if (len) *sum += (uint16_t)bytes[0] << 8;
}

uint16_t udp_checksum(const uint8_t *ip, size_t header_len, size_t total_len)
{
    const uint8_t *udp = ip + header_len;
    const size_t udp_len = total_len - header_len;
    uint32_t sum = 0;

    checksum_add_bytes(&sum, ip + IPV4_OFF_SRC, 8);
    sum += IPV4_PROTOCOL_UDP;
    sum += (uint16_t)udp_len;
    checksum_add_bytes(&sum, udp, udp_len);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);

    const uint16_t result = (uint16_t)~sum;
    return result ? result : 0xffff;
}

static bool rewrite_ipv4_addresses(uint8_t *ip, size_t captured_len,
                                   const uint8_t src[4], const uint8_t dst[4])
{
    if (captured_len < IPV4_HEADER_LEN) return false;
    const size_t header_len = (ip[IPV4_OFF_VER_LEN] & 0x0f) * 4;
    const size_t total_len = READ_NET16(ip, IPV4_OFF_TOTAL_LEN);
    if (header_len < IPV4_HEADER_LEN || header_len > captured_len || total_len < header_len || total_len > captured_len) return false;

    CPY_IPV4(ip + IPV4_OFF_SRC, src);
    CPY_IPV4(ip + IPV4_OFF_DST, dst);
    WRITE_NET16(ip, IPV4_OFF_CHECKSUM, 0);
    WRITE_NET16(ip, IPV4_OFF_CHECKSUM, ipv4_header_checksum(ip, header_len));

    /* Recalculate the UDP checksum after changing the pseudo-header. This is
     * accepted by every IPv4 receiver and matches native ZeroTier frames. */
    if (ip[IPV4_OFF_PROTOCOL] == IPV4_PROTOCOL_UDP && total_len >= header_len + 8) {
        WRITE_NET16(ip, header_len + 6, 0);
        WRITE_NET16(ip, header_len + 6, udp_checksum(ip, header_len, total_len));
    }
    return true;
}

static int lan_play_send_wifi_ipv4(struct lan_play *lan_play, const uint8_t *dst_mac,
                                   const uint8_t *ip, uint16_t ip_len)
{
    uint8_t frame[ETHER_MTU + ETHER_HEADER_LEN];
    if (ip_len < IPV4_HEADER_LEN || ip_len > ETHER_MTU) return -1;
    CPY_MAC(frame + ETHER_OFF_DST, dst_mac);
    CPY_MAC(frame + ETHER_OFF_SRC, lan_play->wifi_mac);
    WRITE_NET16(frame, ETHER_OFF_TYPE, ETHER_TYPE_IPV4);
    memcpy(frame + ETHER_HEADER_LEN, ip, ip_len);
    return lan_play_send_packet(lan_play, frame, ip_len + ETHER_HEADER_LEN);
}

int lan_play_dhcp_send(struct lan_play *lan_play, const uint8_t *dst_mac,
                       const uint8_t *ip, uint16_t ip_len)
{
    return lan_play_send_wifi_ipv4(lan_play, dst_mac, ip, ip_len);
}

static bool lan_play_relay_wifi_ipv4(struct lan_play *lan_play, const u_char *frame, uint16_t frame_len)
{
    const uint8_t *ip = frame + ETHER_HEADER_LEN;
    if (!lan_play->switch_seen || frame_len < ETHER_HEADER_LEN + IPV4_HEADER_LEN ||
        !CMP_IPV4(ip + IPV4_OFF_SRC, lan_play->switch_ip)) return false;

    const uint16_t ip_len = READ_NET16(ip, IPV4_OFF_TOTAL_LEN);
    const uint8_t *original_dst = ip + IPV4_OFF_DST;
    if (ip_len > ETHER_MTU || frame_len < ETHER_HEADER_LEN + ip_len ||
        (!is_nintendo_lan_broadcast(original_dst) && !is_zerotier_destination(lan_play, original_dst))) return false;

    uint8_t translated[ETHER_MTU];
    memcpy(translated, ip, ip_len);
    const uint8_t *dst = is_nintendo_lan_broadcast(original_dst)
        ? lan_play->zerotier_broadcast_ip : original_dst;
    if (!rewrite_ipv4_addresses(translated, ip_len, lan_play->zerotier_ip, dst)) return false;
    const size_t header_len = (ip[0] & 0x0f) * 4;
    if (!lan_play->warned_broadcast_mismatch && ip[IPV4_OFF_PROTOCOL] == IPV4_PROTOCOL_UDP &&
        ip_len >= header_len + 8 && !(READ_NET16(ip, 6) & 0x3fff) &&
        READ_NET16(ip, header_len + 2) == 35000 &&
        is_nintendo_lan_broadcast(original_dst) && !CMP_IPV4(original_dst, dst)) {
        lan_play->warned_broadcast_mismatch = true;
        LLOG(LLOG_WARNING, "LAN broadcast mismatch: Switch uses %u.%u.%u.%u, ZeroTier uses %u.%u.%u.%u. Set the Switch subnet mask to %u.%u.%u.%u and restart LAN mode; PIA authentication uses the broadcast address, which header translation cannot repair.",
            original_dst[0], original_dst[1], original_dst[2], original_dst[3],
            dst[0], dst[1], dst[2], dst[3], lan_play->zerotier_netmask[0], lan_play->zerotier_netmask[1],
            lan_play->zerotier_netmask[2], lan_play->zerotier_netmask[3]);
    }
    if (ip[IPV4_OFF_PROTOCOL] == IPV4_PROTOCOL_UDP &&
        !(READ_NET16(ip, IPV4_OFF_FLAGS_FRAG_OFFSET) & 0x1fff) && ip_len >= header_len + 8) {
        const uint16_t port = READ_NET16(ip, header_len);
        int ret = native_udp_guard_reserve(lan_play->udp_guard, port);
        if (ret != 0) {
            LLOG(LLOG_ERROR, "Cannot reserve host UDP/%u: %s; outgoing datagram dropped", port, uv_strerror(ret));
            return true; /* Handled: do not fall through to the legacy relay. */
        }
    }
    if (CMP_IPV4(dst, lan_play->zerotier_broadcast_ip))
        lan_play_send_zerotier_ipv4_broadcast(lan_play, translated, ip_len);
    else
        lan_play_send_zerotier_ipv4(lan_play, dst, translated, ip_len);
    return true;
}

static void probe_wifi_delivery(struct lan_play *lp)
{
    /* Fire once per run, on discovery or first game broadcast: a single ICMP
     * echo is cheap and confirms the Wi-Fi path before the user needs it. */
    if (lp->wifi_delivery_probe_sent) return;
    uint8_t ip[44] = {0};
    ip[0] = 0x45; ip[8] = 64; ip[9] = 1;
    WRITE_NET16(ip, 2, sizeof(ip));
    ip[20] = 8; /* Echo request from our fake gateway, only on the local LAN. */
    WRITE_NET16(ip, 24, 0x5a4c); WRITE_NET16(ip, 26, 1);
    memcpy(lp->wifi_delivery_probe_payload, "ZLLPROBE", 8);
    uint64_t nonce = uv_hrtime();
    memcpy(lp->wifi_delivery_probe_payload + 8, &nonce, sizeof(nonce));
    memcpy(ip + 28, lp->wifi_delivery_probe_payload, 16);
    WRITE_NET16(ip, 22, ipv4_header_checksum(ip + 20, 24));
    /* DHCP/Automatic consoles live on the Wi-Fi subnet, so probe from the
     * Wi-Fi address; the fake gateway address is off their subnet and the
     * reply would go to the real router instead of us. */
    const uint8_t *probe_src = lp->switch_dhcp ? lp->wifi_ip : lp->packet_ctx.ip;
    rewrite_ipv4_addresses(ip, sizeof(ip), probe_src, lp->switch_ip);
    CPY_IPV4(lp->wifi_delivery_probe_ip, lp->switch_ip);
    CPY_MAC(lp->wifi_delivery_probe_mac, lp->switch_mac);
    lp->wifi_delivery_probe_sent = true;
    LLOG(LLOG_INFO, "Testing local IPv4 delivery with one gateway-to-Switch ICMP echo; this is not game traffic");
    if (lan_play_send_wifi_ipv4(lp, lp->switch_mac, ip, sizeof(ip)) != 0)
        LLOG(LLOG_WARNING, "Local IPv4 delivery probe injection failed");
}

static bool consume_wifi_delivery_reply(struct lan_play *lp, const uint8_t *frame, size_t len)
{
    if (!lp->wifi_delivery_probe_sent || len < 14 + 44 ||
        READ_NET16(frame, 12) != ETHER_TYPE_IPV4 ||
        !CMP_MAC(frame + ETHER_OFF_SRC, lp->wifi_delivery_probe_mac) ||
        !CMP_MAC(frame + ETHER_OFF_DST, lp->wifi_mac)) return false;
    const uint8_t *ip = frame + 14;
    size_t hlen = (ip[0] & 15) * 4;
    size_t total = READ_NET16(ip, 2);
    if (ip[0] >> 4 != 4 || hlen < 20 || total != hlen + 24 || total > len - 14 ||
        ip[9] != 1 || (READ_NET16(ip, 6) & 0x3fff) ||
        !CMP_IPV4(ip + 12, lp->wifi_delivery_probe_ip) ||
        !CMP_IPV4(ip + 16, lp->switch_dhcp ? lp->wifi_ip : lp->packet_ctx.ip)) return false;
    const uint8_t *icmp = ip + hlen;
    if (icmp[0] || icmp[1] || READ_NET16(icmp, 4) != 0x5a4c || READ_NET16(icmp, 6) != 1 ||
        memcmp(icmp + 8, lp->wifi_delivery_probe_payload, 16) ||
        ipv4_header_checksum(ip, hlen) || ipv4_header_checksum(icmp, 24)) return false;
    if (!lp->wifi_delivery_probe_replied)
        LLOG(LLOG_INFO, "Local IPv4 delivery confirmed: matching echo reply from the detected Switch MAC (game acceptance remains unverified)");
    lp->wifi_delivery_probe_replied = true;
    return true; /* This locally generated diagnostic must not enter ZeroTier. */
}

static bool lan_play_relay_zerotier_ipv4(struct lan_play *lan_play, const u_char *frame, uint16_t frame_len)
{
    const uint8_t *ip = frame + ETHER_HEADER_LEN;
    if (!lan_play->switch_seen || frame_len < ETHER_HEADER_LEN + IPV4_HEADER_LEN) return false;

    const uint16_t ip_len = READ_NET16(ip, IPV4_OFF_TOTAL_LEN);
    const uint8_t *original_dst = ip + IPV4_OFF_DST;
    const bool broadcast = CMP_IPV4(original_dst, lan_play->zerotier_broadcast_ip);
    if (ip_len > ETHER_MTU || frame_len < ETHER_HEADER_LEN + ip_len ||
        (!broadcast && !CMP_IPV4(original_dst, lan_play->zerotier_ip))) return false;

    uint8_t translated[ETHER_MTU];
    uint8_t wifi_broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    memcpy(translated, ip, ip_len);
    /* The console must use the overlay's subnet mask: PIA includes that
     * broadcast address in its authenticated challenge nonce. Preserve it. */
    const uint8_t *dst = broadcast ? lan_play->zerotier_broadcast_ip : lan_play->switch_ip;
    if (!rewrite_ipv4_addresses(translated, ip_len, ip + IPV4_OFF_SRC, dst)) return false;
    int ret = lan_play_send_wifi_ipv4(lan_play, broadcast ? wifi_broadcast : lan_play->switch_mac, translated, ip_len);
    const size_t header_len = (ip[0] & 15) * 4;
    if (ret == 0 && !broadcast && ip[9] == IPV4_PROTOCOL_UDP &&
        !(READ_NET16(ip, 6) & 0x3fff) && ip_len >= header_len + 8 &&
        READ_NET16(ip, header_len + 2) == 35000) probe_wifi_delivery(lan_play);
    return true;
}

static void diagnostic_log_frame(struct lan_play *lan_play, const char *direction, const u_char *packet, int len)
{
    capture_frame(lan_play, direction, packet, len);
    if (!options.diagnostics || len < ETHER_HEADER_LEN) return;

    uint16_t type = READ_NET16(packet, ETHER_OFF_TYPE);
    if (type == ETHER_TYPE_ARP && len >= ETHER_HEADER_LEN + ARP_LEN) {
        const u_char *arp = packet + ETHER_HEADER_LEN;
        LLOG(LLOG_INFO, "%s ARP op=%u %02x:%02x:%02x:%02x:%02x:%02x/%u.%u.%u.%u -> %u.%u.%u.%u",
            direction, READ_NET16(arp, ARP_OFF_OPCODE),
            packet[6], packet[7], packet[8], packet[9], packet[10], packet[11],
            arp[ARP_OFF_SENDER_IP], arp[ARP_OFF_SENDER_IP + 1], arp[ARP_OFF_SENDER_IP + 2], arp[ARP_OFF_SENDER_IP + 3],
            arp[ARP_OFF_TARGET_IP], arp[ARP_OFF_TARGET_IP + 1], arp[ARP_OFF_TARGET_IP + 2], arp[ARP_OFF_TARGET_IP + 3]);
    } else if (type == ETHER_TYPE_IPV4 && len >= ETHER_HEADER_LEN + IPV4_HEADER_LEN) {
        const u_char *ip = packet + ETHER_HEADER_LEN;
        const uint8_t header_len = (ip[IPV4_OFF_VER_LEN] & 0x0f) * 4;
        const size_t total_len = READ_NET16(ip, IPV4_OFF_TOTAL_LEN);
        if ((ip[0] >> 4) != 4 || header_len < 20 ||
            total_len < header_len || total_len > (size_t)len - ETHER_HEADER_LEN) {
            LLOG(LLOG_WARNING, "%s malformed/truncated IPv4 frame (%d bytes)", direction, len);
            return;
        }
        const bool fragment = (READ_NET16(ip, IPV4_OFF_FLAGS_FRAG_OFFSET) & 0x3fff) != 0;
        if (ip[IPV4_OFF_PROTOCOL] == IPV4_PROTOCOL_UDP && !fragment && total_len >= header_len + 8) {
            const size_t udp_len = READ_NET16(ip, header_len + 4);
            if (udp_len < 8 || udp_len > total_len - header_len) {
                LLOG(LLOG_WARNING, "%s invalid UDP length", direction);
                return;
            }
            uint32_t hash = 2166136261u;
            const uint8_t *payload = ip + header_len + 8;
            for (size_t i = 0; i < udp_len - 8; ++i) hash = (hash ^ payload[i]) * 16777619u;
            LLOG(LLOG_INFO, "%s IPv4 UDP %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u (%d frame bytes)",
                direction,
                ip[IPV4_OFF_SRC], ip[IPV4_OFF_SRC + 1], ip[IPV4_OFF_SRC + 2], ip[IPV4_OFF_SRC + 3],
                READ_NET16(ip, header_len),
                ip[IPV4_OFF_DST], ip[IPV4_OFF_DST + 1], ip[IPV4_OFF_DST + 2], ip[IPV4_OFF_DST + 3],
                READ_NET16(ip, header_len + 2), len);
            LLOG(LLOG_INFO, "  UDP payload=%u hash=%08x Ethernet dst=%02x:%02x:%02x:%02x:%02x:%02x",
                (unsigned)(udp_len - 8), hash, packet[0], packet[1], packet[2], packet[3], packet[4], packet[5]);
        } else if (ip[IPV4_OFF_PROTOCOL] == IPV4_PROTOCOL_ICMP && !fragment && total_len >= header_len + 8) {
            const uint8_t *icmp = ip + header_len;
            LLOG(LLOG_INFO, "%s ICMP type=%u code=%u %u.%u.%u.%u -> %u.%u.%u.%u",
                direction, icmp[0], icmp[1], ip[12], ip[13], ip[14], ip[15],
                ip[16], ip[17], ip[18], ip[19]);
            if (icmp[0] == 3 || icmp[0] == 11) {
                const uint8_t *quoted = icmp + 8;
                size_t available = total_len - header_len - 8;
                if (available >= 20 && (quoted[0] >> 4) == 4) {
                    size_t qhl = (quoted[0] & 15) * 4;
                    if (qhl >= 20 && available >= qhl + 4 && quoted[9] == 17) {
                        LLOG(LLOG_WARNING, "  ICMP quotes UDP %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u",
                            quoted[12], quoted[13], quoted[14], quoted[15], READ_NET16(quoted, qhl),
                            quoted[16], quoted[17], quoted[18], quoted[19], READ_NET16(quoted, qhl + 2));
                    }
                }
            }
        } else {
            LLOG(LLOG_INFO, "%s IPv4 proto=%u %u.%u.%u.%u -> %u.%u.%u.%u (%d bytes)",
                direction, ip[IPV4_OFF_PROTOCOL],
                ip[IPV4_OFF_SRC], ip[IPV4_OFF_SRC + 1], ip[IPV4_OFF_SRC + 2], ip[IPV4_OFF_SRC + 3],
                ip[IPV4_OFF_DST], ip[IPV4_OFF_DST + 1], ip[IPV4_OFF_DST + 2], ip[IPV4_OFF_DST + 3], len);
        }
    }
}

static void diagnostic_count_rx(struct lan_play *lan_play, bool zerotier, const u_char *packet, int len)
{
    if (len < ETHER_HEADER_LEN) return;
    switch (READ_NET16(packet, ETHER_OFF_TYPE)) {
        case ETHER_TYPE_ARP:
            if (zerotier) lan_play->zerotier_arp_rx++; else lan_play->wifi_arp_rx++;
            break;
        case ETHER_TYPE_IPV4:
            if (zerotier) lan_play->zerotier_ipv4_rx++; else lan_play->wifi_ipv4_rx++;
            break;
    }
}

static int lan_play_send_wifi_arp_probe(struct lan_play *lan_play, const uint8_t target_ip[4],
                                          const uint8_t sender_ip[4])
{
    uint8_t frame[ETHER_HEADER_LEN + ARP_LEN] = {0};
    memset(frame + ETHER_OFF_DST, 0xff, 6);
    CPY_MAC(frame + ETHER_OFF_SRC, lan_play->wifi_mac);
    WRITE_NET16(frame, ETHER_OFF_TYPE, ETHER_TYPE_ARP);
    WRITE_NET16(frame, ETHER_HEADER_LEN + ARP_OFF_HARDWARE, ARP_HARDTYPE_ETHER);
    WRITE_NET16(frame, ETHER_HEADER_LEN + ARP_OFF_PROTOCOL, ETHER_TYPE_IPV4);
    WRITE_NET8(frame, ETHER_HEADER_LEN + ARP_OFF_HARDWARE_SIZE, 6);
    WRITE_NET8(frame, ETHER_HEADER_LEN + ARP_OFF_PROTOCOL_SIZE, 4);
    WRITE_NET16(frame, ETHER_HEADER_LEN + ARP_OFF_OPCODE, ARP_OPCODE_REQUEST);
    CPY_MAC(frame + ETHER_HEADER_LEN + ARP_OFF_SENDER_MAC, lan_play->wifi_mac);
    CPY_IPV4(frame + ETHER_HEADER_LEN + ARP_OFF_SENDER_IP, sender_ip);
    CPY_IPV4(frame + ETHER_HEADER_LEN + ARP_OFF_TARGET_IP, target_ip);

    lan_play->wifi_probe_tx++;
    int ret = uv_pcap_sendpacket(&lan_play->pcap, frame, sizeof(frame));
    if (ret != 0) LLOG(LLOG_ERROR, "Switch discovery ARP probe failed %d", ret);
    return ret;
}

static void switch_discovery_timer_cb(uv_timer_t *timer)
{
    struct lan_play *lan_play = timer->data;
    if (lan_play->switch_seen) {
        uv_timer_stop(timer);
        return;
    }
    if (lan_play->switch_discovery_pause_ticks) {
        lan_play->switch_discovery_pause_ticks--;
        return;
    }

    /* Sweep the Wi-Fi subnet first (finds DHCP/Automatic consoles), then the
     * ZeroTier subnet (finds manually configured consoles), then pause. */
    uint8_t target_ip[4];
    const uint8_t *sender_ip = lan_play->packet_ctx.ip;
    bool wifi_phase = lan_play->switch_discovery_wifi_phase && lan_play->wifi_subnet_known;
    if (wifi_phase) {
        /* Probe our own /24; that is where DHCP peers live. */
        CPY_IPV4(target_ip, lan_play->wifi_ip);
        sender_ip = lan_play->wifi_ip;
    } else {
        CPY_IPV4(target_ip, lan_play->packet_ctx.subnet_net);
    }
    target_ip[3] = lan_play->switch_discovery_host;
    if (wifi_phase && target_ip[3] == lan_play->wifi_ip[3]) {
        /* Never probe ourselves. */
    } else {
        lan_play_send_wifi_arp_probe(lan_play, target_ip, sender_ip);
    }

    lan_play->switch_discovery_host++;
    if (lan_play->switch_discovery_host == 0 || lan_play->switch_discovery_host == 255) {
        lan_play->switch_discovery_host = 2;
        if (wifi_phase) {
            lan_play->switch_discovery_wifi_phase = false; /* Wi-Fi sweep done; now the ZeroTier subnet. */
        } else {
            lan_play->switch_discovery_wifi_phase = lan_play->wifi_subnet_known; /* Start over with Wi-Fi. */
            lan_play->switch_discovery_pause_ticks = 600; // 30 seconds at 50ms per tick
            if (options.diagnostics) LLOG(LLOG_INFO, "No Switch response yet; retrying ARP discovery in 30 seconds");
        }
    }
}

int init_pcap(struct lan_play *lan_play, char *netif, const char *subnet)
{
    int ret = uv_pcap_init(lan_play->loop, &lan_play->pcap, lan_play_pcap_handler, netif, subnet);
    if (ret != 0) {
        RETURN_ERR(lan_play, "Local capture: %s", lan_play->pcap.last_error);
    };

    return 0;
}
int init_zerotier_pcap(struct lan_play *lan_play, char *netif, const char *subnet) {
    int ret = uv_pcap_init(lan_play->loop, &lan_play->zerotier_pcap, lan_play_zerotier_pcap_handler, netif, subnet);
    if (ret != 0) RETURN_ERR(lan_play, "ZeroTier capture: %s", lan_play->zerotier_pcap.last_error);
    lan_play->zerotier_pcap.data = lan_play; return 0;
}

int lan_play_send_packet(struct lan_play *lan_play, void *data, int size)
{
    lan_play->wifi_tx++;
    diagnostic_log_frame(lan_play, "TX Wi-Fi", data, size);
    int ret = uv_pcap_sendpacket(&lan_play->pcap, data, size);
    if (ret != 0) {
        LLOG(LLOG_ERROR, "uv_pcap_sendpacket %d", ret);
    }
    return ret;
}

int lan_play_close(struct lan_play *lan_play)
{
    int ret;

    if (options.discover_switch) uv_timer_stop(&lan_play->switch_discovery_timer);
    if (options.diagnostics) LLOG(LLOG_INFO, "Host UDP socket copies drained: %llu (not additional forwarding)",
        (unsigned long long)native_udp_guard_received(lan_play->udp_guard));
    if (lan_play->wifi_delivery_probe_sent && !lan_play->wifi_delivery_probe_replied)
        LLOG(LLOG_WARNING, "No matching local IPv4 probe reply observed; Wi-Fi delivery remains unconfirmed (the console may ignore ICMP)");
    native_udp_guard_close(lan_play->udp_guard);
    lan_play->udp_guard = NULL;
    if (options.diagnostics) {
        LLOG(LLOG_INFO, "Local return-path conflicts ignored: %llu; MAC learned from %s",
            (unsigned long long)lan_play->switch_mac_conflicts,
            !lan_play->switch_seen ? "no candidate" :
            lan_play->switch_mac_confirmed ? "ARP/broadcast" : "provisional unicast");
        LLOG(LLOG_INFO, "Traffic summary: Wi-Fi RX ARP=%llu IPv4=%llu TX=%llu probes=%llu; ZeroTier RX ARP=%llu IPv4=%llu TX=%llu",
            (unsigned long long)lan_play->wifi_arp_rx, (unsigned long long)lan_play->wifi_ipv4_rx, (unsigned long long)lan_play->wifi_tx, (unsigned long long)lan_play->wifi_probe_tx,
            (unsigned long long)lan_play->zerotier_arp_rx, (unsigned long long)lan_play->zerotier_ipv4_rx, (unsigned long long)lan_play->zerotier_tx);
    }
    close_captures(lan_play);
    uv_pcap_close(&lan_play->pcap);
    uv_pcap_close(&lan_play->zerotier_pcap);
    ret = packet_close(&lan_play->packet_ctx);
    if (ret != 0) return ret;

    if (options.relay_server_addr) {
        ret = lan_client_close(lan_play);
        if (ret != 0) return ret;
    }

    ret = gateway_close(lan_play->gateway);
    if (ret != 0) return ret;

    return 0;
}

/* Unicast may arrive with an AP/proxy Ethernet source. Use it provisionally,
 * then prefer an ARP claim or subnet broadcast; never chase each unicast MAC.
 * This is delivery evidence, not proof of a device's manufacturer/identity. */
static bool learn_local_switch(struct lan_play *lp, const uint8_t *frame, size_t len)
{
    static const uint8_t all[6] = {255,255,255,255,255,255};
    static const uint8_t zero[6] = {0};
    if (len < ETHER_HEADER_LEN || (frame[6] & 1) || CMP_MAC(frame + 6, zero)) return false;
    const uint8_t *body = frame + ETHER_HEADER_LEN, *ip = NULL;
    bool direct = false;
    uint16_t type = READ_NET16(frame, ETHER_OFF_TYPE);
    if (type == ETHER_TYPE_ARP) {
        if (len < ETHER_HEADER_LEN + ARP_LEN || READ_NET16(body, ARP_OFF_HARDWARE) != 1 ||
            READ_NET16(body, ARP_OFF_PROTOCOL) != ETHER_TYPE_IPV4 || body[4] != 6 || body[5] != 4 ||
            (READ_NET16(body, ARP_OFF_OPCODE) != ARP_OPCODE_REQUEST &&
             READ_NET16(body, ARP_OFF_OPCODE) != ARP_OPCODE_REPLY) ||
            !CMP_MAC(body + ARP_OFF_SENDER_MAC, frame + 6)) return false;
        ip = body + ARP_OFF_SENDER_IP;
        direct = true;
    } else if (type == ETHER_TYPE_IPV4) {
        if (len < ETHER_HEADER_LEN + IPV4_HEADER_LEN || (body[0] >> 4) != 4) return false;
        size_t header = (body[0] & 15) * 4, total = READ_NET16(body, IPV4_OFF_TOTAL_LEN);
        if (header < IPV4_HEADER_LEN || total < header || total > len - ETHER_HEADER_LEN) return false;
        ip = body + IPV4_OFF_SRC;
        direct = CMP_MAC(frame, all) &&
            (CMP_IPV4(body + IPV4_OFF_DST, lp->zerotier_broadcast_ip) ||
             is_nintendo_lan_broadcast(body + IPV4_OFF_DST));
    } else return false;
    const bool zt_dest = is_zerotier_destination(lp, ip);
    const bool wifi_dest = lp->wifi_subnet_known &&
        ip_in_subnet(ip, lp->wifi_subnet, lp->wifi_netmask);
    if ((!zt_dest && !wifi_dest) || CMP_IPV4(ip, lp->zerotier_broadcast_ip) ||
        CMP_IPV4(ip, lp->packet_ctx.ip)) return false;
    if (wifi_dest && (CMP_IPV4(ip, lp->wifi_broadcast) || CMP_IPV4(ip, lp->wifi_subnet))) return false;
    bool network = true;
    for (int i = 0; i < 4; ++i) if (ip[i] & (uint8_t)~lp->zerotier_netmask[i]) network = false;
    if (network) return false;
    /* DHCP candidates on the Wi-Fi subnet must carry a Nintendo OUI, otherwise
     * the first chatty device (usually the router) would claim the single
     * Switch slot and stop discovery. Manually configured ZeroTier-subnet
     * consoles keep the previous behavior. */
    const bool dhcp = wifi_dest && !zt_dest;
    if (dhcp && !is_nintendo_mac(frame + 6)) return false;
    if (lp->switch_seen && !CMP_IPV4(ip, lp->switch_ip)) return false;
    bool changed = !lp->switch_seen || !CMP_MAC(frame + 6, lp->switch_mac);
    if (lp->switch_seen && changed && (lp->switch_mac_confirmed || !direct)) {
        if (++lp->switch_mac_conflicts == 1)
            LLOG(LLOG_WARNING, "Local Switch IP %u.%u.%u.%u also arrived from MAC %02x:%02x:%02x:%02x:%02x:%02x; keeping return MAC %02x:%02x:%02x:%02x:%02x:%02x. Check for an AP/proxy or duplicate IP; restart the relay after changing consoles.",
                ip[0], ip[1], ip[2], ip[3], frame[6], frame[7], frame[8], frame[9], frame[10], frame[11],
                lp->switch_mac[0], lp->switch_mac[1], lp->switch_mac[2], lp->switch_mac[3], lp->switch_mac[4], lp->switch_mac[5]);
        return true; /* Keep forwarding this IP's datagrams; do not pin its ingress path. */
    }
    CPY_IPV4(lp->switch_ip, ip);
    CPY_MAC(lp->switch_mac, frame + 6);
    lp->switch_seen = true;
    lp->switch_mac_confirmed |= direct;
    if (changed) {
        /* Pre-seed the local ARP cache so inbound delivery and proxy ARP
         * resolve immediately instead of waiting for the next ARP exchange. */
        arp_set(&lp->packet_ctx, frame + 6, ip);
        lp->switch_dhcp = dhcp;
        if (is_nintendo_mac(frame + 6)) {
            lp->switch_nintendo_oui = true;
            LLOG(LLOG_INFO, "Switch candidate carries a Nintendo vendor prefix; treating as console");
        }
        if (dhcp)
            LLOG(LLOG_INFO, "Switch candidate uses a DHCP/Automatic address on the Wi-Fi subnet");
        /* Confirm the Wi-Fi path right away instead of waiting for game traffic. */
        probe_wifi_delivery(lp);
    }
    if (changed && (options.diagnostics || options.status_events))
        LLOG(LLOG_INFO, "Detected local Switch candidate: %u.%u.%u.%u (%02x:%02x:%02x:%02x:%02x:%02x)%s",
            ip[0], ip[1], ip[2], ip[3], frame[6], frame[7], frame[8], frame[9], frame[10], frame[11],
            dhcp ? " [DHCP/Automatic]" : "");
    if (changed && options.diagnostics && !CMP_IPV4(ip, lp->zerotier_ip))
        LLOG(LLOG_WARNING, "For Splatoon with sys-zerotier, configure the stock Switch address as the managed ZeroTier address %u.%u.%u.%u",
            lp->zerotier_ip[0], lp->zerotier_ip[1], lp->zerotier_ip[2], lp->zerotier_ip[3]);
    return true;
}

void lan_play_pcap_handler(uv_pcap_t *handle, const struct pcap_pkthdr *pkt_header, const u_char *packet, const uint8_t *mac)
{
    struct lan_play *lan_play = handle->data;
    if (pkt_header->caplen < ETHER_HEADER_LEN) return;
    if (CMP_MAC(packet + ETHER_OFF_SRC, mac)) {
        diagnostic_log_frame(lan_play, "HOST Wi-Fi", packet, pkt_header->caplen);
        return;
    }
    lan_play->ingress_zerotier = false;
    diagnostic_count_rx(lan_play, false, packet, pkt_header->caplen);
    diagnostic_log_frame(lan_play, "RX Wi-Fi", packet, pkt_header->caplen);
    if (consume_wifi_delivery_reply(lan_play, packet, pkt_header->caplen)) return;
    /* DHCP client traffic is answered locally when --dhcp is on; it must
     * never reach discovery or the ZeroTier relay path. */
    if (dhcp_server_consume(lan_play, packet, pkt_header->caplen)) return;
    if (!learn_local_switch(lan_play, packet, pkt_header->caplen)) return;
    packet_set_mac(&lan_play->packet_ctx, mac);
    if (pkt_header->caplen >= ETHER_HEADER_LEN + IPV4_HEADER_LEN &&
        READ_NET16(packet, ETHER_OFF_TYPE) == ETHER_TYPE_IPV4 &&
        lan_play_relay_wifi_ipv4(lan_play, packet, pkt_header->caplen)) return;
    get_packet(&lan_play->packet_ctx, pkt_header, packet);
}
void lan_play_zerotier_pcap_handler(uv_pcap_t *handle, const struct pcap_pkthdr *pkt_header, const u_char *packet, const uint8_t *mac) {
    struct lan_play *lan_play = handle->data;
    if (pkt_header->caplen < ETHER_HEADER_LEN) return;
    if (CMP_MAC(packet + ETHER_OFF_SRC, mac)) {
        diagnostic_log_frame(lan_play, "HOST ZeroTier", packet, pkt_header->caplen);
        return;
    }
    // Keep overlay neighbors separate from the Wi-Fi ARP table. Learning from
    // discovery IPv4 as well as ARP lets the first browse reply use unicast.
    const uint8_t *peer_ip = NULL;
    const uint8_t *body = packet + ETHER_HEADER_LEN;
    uint16_t type = READ_NET16(packet, ETHER_OFF_TYPE);
    if (type == ETHER_TYPE_IPV4 && pkt_header->caplen >= ETHER_HEADER_LEN + IPV4_HEADER_LEN &&
        (body[0] >> 4) == 4) peer_ip = body + IPV4_OFF_SRC;
    if (type == ETHER_TYPE_ARP && pkt_header->caplen >= ETHER_HEADER_LEN + ARP_LEN &&
        READ_NET16(body, ARP_OFF_HARDWARE) == 1 &&
        READ_NET16(body, ARP_OFF_PROTOCOL) == ETHER_TYPE_IPV4 &&
        body[4] == 6 && body[5] == 4 &&
        CMP_MAC(body + ARP_OFF_SENDER_MAC, packet + ETHER_OFF_SRC))
        peer_ip = body + ARP_OFF_SENDER_IP;
    if (peer_ip && is_zerotier_destination(lan_play, peer_ip) &&
        !CMP_IPV4(peer_ip, lan_play->zerotier_ip) &&
        !CMP_IPV4(peer_ip, lan_play->zerotier_broadcast_ip) && !(packet[ETHER_OFF_SRC] & 1))
        arp_set(&lan_play->zerotier_neighbors, packet + ETHER_OFF_SRC, peer_ip);
    lan_play->ingress_zerotier = true;
    diagnostic_count_rx(lan_play, true, packet, pkt_header->caplen);
    diagnostic_log_frame(lan_play, "RX ZeroTier", packet, pkt_header->caplen);
    packet_set_mac(&lan_play->packet_ctx, mac);
    if (pkt_header->caplen >= ETHER_HEADER_LEN + IPV4_HEADER_LEN &&
        READ_NET16(packet, ETHER_OFF_TYPE) == ETHER_TYPE_IPV4 &&
        lan_play_relay_zerotier_ipv4(lan_play, packet, pkt_header->caplen)) {
        lan_play->ingress_zerotier = false;
        return;
    }
    get_packet(&lan_play->packet_ctx, pkt_header, packet);
    lan_play->ingress_zerotier = false;
}

static int parse_ipv4(const char *text, uint8_t out[4])
{
    return inet_pton(AF_INET, text, out) == 1 ? 0 : -1;
}

static int parse_subnet(const char *text, uint8_t net[4], uint8_t mask[4])
{
    char address[IP_STR_LEN];
    char *slash;
    char *end;
    long prefix;

    if (strlen(text) >= sizeof(address)) return -1;
    strcpy(address, text);
    slash = strchr(address, '/');
    if (!slash) return -1;
    *slash++ = '\0';
    errno = 0;
    prefix = strtol(slash, &end, 10);
    if (errno || *end || prefix < 0 || prefix > 32 || parse_ipv4(address, net) != 0) return -1;
    for (int i = 0; i < 4; ++i) {
        int remaining = (int)prefix - i * 8;
        mask[i] = remaining >= 8 ? 0xff : remaining <= 0 ? 0 : (uint8_t)(0xff << (8 - remaining));
        net[i] &= mask[i];
    }
    return 0;
}

int lan_play_init(struct lan_play *lan_play)
{
    int ret = 0;
    uint8_t ip[4];
    uint8_t subnet_net[4];
    uint8_t subnet_mask[4];
    char subnet_filter[IP_STR_LEN + 4];

    arp_list_init(lan_play->zerotier_neighbors.arp_list);
    lan_play->zerotier_neighbors.arp_ttl = 30;
    dhcp_server_init(lan_play);
    lan_play->broadcast = options.broadcast;
    lan_play->pmtu = options.pmtu;

    if (options.relay_server_addr) {
        if (parse_addr(options.relay_server_addr, &lan_play->server_addr) != 0) {
            RETURN_ERR(lan_play, "Failed to parse and get ip address. --relay-server-addr: %s", options.relay_server_addr);
        }
    }
    lan_play->username = options.relay_username;
    if (options.relay_password) {
        SHA1_CTX hashctx;
        SHA1Init(&hashctx);
        SHA1Update(&hashctx, (const unsigned char *)options.relay_password, strlen(options.relay_password));
        SHA1Final(lan_play->key, &hashctx);
    }

    if (options.subnet) {
        if (parse_subnet(options.subnet, subnet_net, subnet_mask) != 0) {
            RETURN_ERR(lan_play, "Invalid --subnet value: %s", options.subnet);
        }
        snprintf(subnet_filter, sizeof(subnet_filter), "%s", options.subnet);
    } else {
        CPY_IPV4(subnet_net, str2ip(SUBNET_NET));
        CPY_IPV4(subnet_mask, str2ip(SUBNET_MASK));
        snprintf(subnet_filter, sizeof(subnet_filter), "%s/24", SUBNET_NET);
    }
    if (options.gateway_ip) {
        if (parse_ipv4(options.gateway_ip, ip) != 0) {
            RETURN_ERR(lan_play, "Invalid --gateway value: %s", options.gateway_ip);
        }
    } else {
        CPY_IPV4(ip, str2ip(SERVER_IP));
    }
    for (int i = 0; i < 4; ++i) {
        if ((ip[i] & subnet_mask[i]) != subnet_net[i]) {
            RETURN_ERR(lan_play, "The fake gateway must be inside --subnet");
        }
    }
    eprintf("native init: opening Wi-Fi capture\n");
    ret = init_pcap(lan_play, options.netif, subnet_filter);
    if (ret != 0) return ret;
    eprintf("native init: opening ZeroTier capture\n");
    ret = init_zerotier_pcap(lan_play, options.zerotier_if, subnet_filter);
    if (ret != 0) return ret;
    eprintf("native init: reading Wi-Fi adapter MAC\n");
    ret = uv_pcap_get_mac(&lan_play->pcap, lan_play->wifi_mac);
    if (ret != 0) {
        RETURN_ERR(lan_play, "Could not obtain the Wi-Fi adapter MAC address");
    }
    eprintf("native init: reading Wi-Fi adapter IPv4\n");
    if (uv_pcap_get_ipv4(&lan_play->pcap, lan_play->wifi_ip, lan_play->wifi_netmask) == 0) {
        int i;
        for (i = 0; i < 4; ++i) lan_play->wifi_subnet[i] = lan_play->wifi_ip[i] & lan_play->wifi_netmask[i];
        for (i = 0; i < 4; ++i) lan_play->wifi_broadcast[i] = lan_play->wifi_subnet[i] | (uint8_t)~lan_play->wifi_netmask[i];
        lan_play->wifi_subnet_known = true;
        LLOG(LLOG_INFO, "Wi-Fi subnet: %u.%u.%u.%u/%u.%u.%u.%u (DHCP/Automatic consoles will be discovered here)",
            lan_play->wifi_subnet[0], lan_play->wifi_subnet[1], lan_play->wifi_subnet[2], lan_play->wifi_subnet[3],
            lan_play->wifi_netmask[0], lan_play->wifi_netmask[1], lan_play->wifi_netmask[2], lan_play->wifi_netmask[3]);
    } else {
        LLOG(LLOG_WARNING, "Could not obtain an IPv4 address from the Wi-Fi adapter; DHCP/Automatic Switch discovery is disabled");
    }
    eprintf("native init: reading ZeroTier adapter MAC\n");
    ret = uv_pcap_get_mac(&lan_play->zerotier_pcap, lan_play->zerotier_mac);
    if (ret != 0) {
        RETURN_ERR(lan_play, "Could not obtain the ZeroTier adapter MAC address");
    }
    ret = uv_pcap_get_ipv4(&lan_play->zerotier_pcap, lan_play->zerotier_ip, lan_play->zerotier_netmask);
    if (ret != 0) {
        RETURN_ERR(lan_play, "Could not obtain an IPv4 address from the ZeroTier adapter");
    }
    for (int i = 0; i < 4; ++i) {
        lan_play->zerotier_broadcast_ip[i] = lan_play->zerotier_ip[i] | (uint8_t)~lan_play->zerotier_netmask[i];
        if ((lan_play->zerotier_ip[i] & subnet_mask[i]) != subnet_net[i]) {
            RETURN_ERR(lan_play, "The ZeroTier adapter IPv4 must belong to --subnet");
        }
    }
    LLOG(LLOG_INFO, "Switch manual settings: IP %u.%u.%u.%u; subnet mask %u.%u.%u.%u; gateway %u.%u.%u.%u",
        lan_play->zerotier_ip[0], lan_play->zerotier_ip[1], lan_play->zerotier_ip[2], lan_play->zerotier_ip[3],
        lan_play->zerotier_netmask[0], lan_play->zerotier_netmask[1], lan_play->zerotier_netmask[2], lan_play->zerotier_netmask[3],
        ip[0], ip[1], ip[2], ip[3]);
    LLOG(LLOG_DEBUG, "native relay ready: Wi-Fi %02x:%02x:%02x:%02x:%02x:%02x, ZeroTier %02x:%02x:%02x:%02x:%02x:%02x (%u.%u.%u.%u)",
        lan_play->wifi_mac[0], lan_play->wifi_mac[1], lan_play->wifi_mac[2], lan_play->wifi_mac[3], lan_play->wifi_mac[4], lan_play->wifi_mac[5],
        lan_play->zerotier_mac[0], lan_play->zerotier_mac[1], lan_play->zerotier_mac[2], lan_play->zerotier_mac[3], lan_play->zerotier_mac[4], lan_play->zerotier_mac[5],
        lan_play->zerotier_ip[0], lan_play->zerotier_ip[1], lan_play->zerotier_ip[2], lan_play->zerotier_ip[3]);
    if (lan_play->wifi_mac[0] == 0 && lan_play->wifi_mac[1] == 0 && lan_play->wifi_mac[2] == 0 &&
        lan_play->wifi_mac[3] == 0 && lan_play->wifi_mac[4] == 0 && lan_play->wifi_mac[5] == 0) {
        RETURN_ERR(lan_play, "The Wi-Fi adapter reported an all-zero MAC address");
    }
    if (lan_play->zerotier_mac[0] == 0 && lan_play->zerotier_mac[1] == 0 && lan_play->zerotier_mac[2] == 0 &&
        lan_play->zerotier_mac[3] == 0 && lan_play->zerotier_mac[4] == 0 && lan_play->zerotier_mac[5] == 0) {
        RETURN_ERR(lan_play, "The ZeroTier adapter reported an all-zero MAC address");
    }
    LLOG(LLOG_DEBUG, "packet init buffer %p", SEND_BUFFER);
    eprintf("native init: starting packet processor\n");
    ret = packet_init(
        &lan_play->packet_ctx,
        lan_play,
        SEND_BUFFER,
        sizeof(SEND_BUFFER),
        ip,
        subnet_net,
        subnet_mask,
        30
    );
    if (ret != 0) RETURN_ERR(lan_play, "Packet processor initialization failed: %d", ret);

    struct slp_addr_in proxy_server;
    struct slp_addr_in *proxy_server_ptr = NULL;
    if (options.socks5_server_addr) {
        proxy_server_ptr = &proxy_server;
        if (parse_addr(options.socks5_server_addr, proxy_server_ptr) != 0) {
            RETURN_ERR(lan_play, "Failed to parse and get ip address. --socks5-server-addr: %s", options.socks5_server_addr);
        }
    }
    eprintf("native init: starting local gateway\n");
    ret = gateway_init(
        &lan_play->gateway,
        &lan_play->packet_ctx,
        options.fake_internet,
        proxy_server_ptr,
        options.socks5_username,
        options.socks5_password
    );
    if (ret != 0) RETURN_ERR(lan_play, "Local gateway initialization failed: %d", ret);

    lan_play->pcap.data = lan_play;
    ret = native_udp_guard_create(lan_play->loop, lan_play->zerotier_ip, &lan_play->udp_guard);
    if (ret != 0) RETURN_ERR(lan_play, "Could not initialize host UDP endpoints: %s", uv_strerror(ret));
    /* Reserve browse and known session ports before peers can reply, including
     * ports on which the game may listen before its first outgoing datagram. */
    const uint16_t game_ports[] = {35000, 49152, 49153, 49154, 49155};
    for (size_t i = 0; i < sizeof(game_ports) / sizeof(game_ports[0]); ++i) {
        ret = native_udp_guard_reserve(lan_play->udp_guard, game_ports[i]);
        if (ret != 0) {
            native_udp_guard_close(lan_play->udp_guard);
            lan_play->udp_guard = NULL;
            RETURN_ERR(lan_play, "Cannot reserve ZeroTier UDP/%u: %s. Check for another application using this port.",
                game_ports[i], uv_strerror(ret));
        }
    }
    if (open_captures(lan_play) != 0) RETURN_ERR(lan_play, "Could not open packet captures; use a new writable prefix");
    if (options.discover_switch) {
        lan_play->switch_discovery_host = 2;
        lan_play->switch_discovery_pause_ticks = 0;
        lan_play->switch_discovery_wifi_phase = lan_play->wifi_subnet_known;
        ret = uv_timer_init(lan_play->loop, &lan_play->switch_discovery_timer);
        if (ret != 0) RETURN_ERR(lan_play, "Could not initialize Switch discovery timer: %d", ret);
        lan_play->switch_discovery_timer.data = lan_play;
        ret = uv_timer_start(&lan_play->switch_discovery_timer, switch_discovery_timer_cb, 50, 50);
        if (ret != 0) RETURN_ERR(lan_play, "Could not start Switch discovery timer: %d", ret);
        if (options.diagnostics) {
            if (lan_play->wifi_subnet_known)
                LLOG(LLOG_INFO, "Searching %u.%u.%u.2-254 (Wi-Fi/DHCP) then %u.%u.%u.2-254 (ZeroTier/manual) for a local Switch using ARP probes",
                    lan_play->wifi_ip[0], lan_play->wifi_ip[1], lan_play->wifi_ip[2],
                    lan_play->packet_ctx.subnet_net[0], lan_play->packet_ctx.subnet_net[1], lan_play->packet_ctx.subnet_net[2]);
            else
                LLOG(LLOG_INFO, "Searching %u.%u.%u.2-254 for a local Switch using ARP probes",
                    lan_play->packet_ctx.subnet_net[0], lan_play->packet_ctx.subnet_net[1], lan_play->packet_ctx.subnet_net[2]);
        }
    }

    return ret;
}

int lan_play_send_zerotier_arp(struct lan_play *lan_play, const uint8_t *sender_ip, const uint8_t *target_ip) {
    uint8_t frame[42] = {0}; memset(frame, 0xff, 6); CPY_MAC(frame + ETHER_OFF_SRC, lan_play->zerotier_mac); WRITE_NET16(frame, 12, ETHER_TYPE_ARP);
    WRITE_NET16(frame, 14, ARP_HARDTYPE_ETHER); WRITE_NET16(frame, 16, ETHER_TYPE_IPV4);
    frame[18] = 6; frame[19] = 4; WRITE_NET16(frame, 20, ARP_OPCODE_REQUEST);
    CPY_MAC(frame + ETHER_HEADER_LEN + ARP_OFF_SENDER_MAC, lan_play->zerotier_mac);
    CPY_IPV4(frame + ETHER_HEADER_LEN + ARP_OFF_SENDER_IP, sender_ip);
    CPY_IPV4(frame + ETHER_HEADER_LEN + ARP_OFF_TARGET_IP, target_ip);
    lan_play->zerotier_tx++;
    diagnostic_log_frame(lan_play, "TX ZeroTier", frame, sizeof(frame));
    int ret = uv_pcap_sendpacket(&lan_play->zerotier_pcap, frame, sizeof(frame));
    if (ret != 0) LLOG(LLOG_ERROR, "ZeroTier ARP send failed %d", ret);
    return ret;
}
int lan_play_send_zerotier_arp_reply(struct lan_play *lan_play, const uint8_t *target_mac, const uint8_t *target_ip, const uint8_t *sender_ip) {
    uint8_t frame[42] = {0};
    CPY_MAC(frame + ETHER_OFF_DST, target_mac); CPY_MAC(frame + ETHER_OFF_SRC, lan_play->zerotier_mac); WRITE_NET16(frame, 12, ETHER_TYPE_ARP);
    WRITE_NET16(frame, 14, ARP_HARDTYPE_ETHER); WRITE_NET16(frame, 16, ETHER_TYPE_IPV4);
    frame[18] = 6; frame[19] = 4; WRITE_NET16(frame, 20, ARP_OPCODE_REPLY);
    CPY_MAC(frame + ETHER_HEADER_LEN + ARP_OFF_SENDER_MAC, lan_play->zerotier_mac);
    CPY_IPV4(frame + ETHER_HEADER_LEN + ARP_OFF_SENDER_IP, target_ip);
    CPY_MAC(frame + ETHER_HEADER_LEN + ARP_OFF_TARGET_MAC, target_mac);
    CPY_IPV4(frame + ETHER_HEADER_LEN + ARP_OFF_TARGET_IP, sender_ip);
    lan_play->zerotier_tx++;
    diagnostic_log_frame(lan_play, "TX ZeroTier", frame, sizeof(frame));
    int ret = uv_pcap_sendpacket(&lan_play->zerotier_pcap, frame, sizeof(frame));
    if (ret != 0) LLOG(LLOG_ERROR, "ZeroTier ARP reply send failed %d", ret);
    return ret;
}
int lan_play_send_zerotier_ipv4(struct lan_play *lan_play, const void *dst_ip, const void *packet, uint16_t len) {
    uint8_t dst[6], frame[ETHER_MTU + ETHER_HEADER_LEN];
    if (len < IPV4_HEADER_LEN || len > ETHER_MTU) return -1;
    if (!arp_has_ip(&lan_play->zerotier_neighbors, dst_ip) ||
        !arp_get_mac_by_ip(&lan_play->zerotier_neighbors, dst, dst_ip)) {
        lan_play_send_zerotier_arp(lan_play, lan_play->zerotier_ip, dst_ip);
        // Keep the first datagram while resolution is pending; later packets
        // use the peer's learned Ethernet destination.
        return lan_play_send_zerotier_ipv4_broadcast(lan_play, packet, len);
    }
    CPY_MAC(frame + ETHER_OFF_DST, dst); CPY_MAC(frame + ETHER_OFF_SRC, lan_play->zerotier_mac); WRITE_NET16(frame, 12, ETHER_TYPE_IPV4); memcpy(frame + ETHER_HEADER_LEN, packet, len);
    lan_play->zerotier_tx++;
    diagnostic_log_frame(lan_play, "TX ZeroTier", frame, len + ETHER_HEADER_LEN);
    int ret = uv_pcap_sendpacket(&lan_play->zerotier_pcap, frame, len + ETHER_HEADER_LEN);
    if (ret != 0) LLOG(LLOG_ERROR, "ZeroTier IPv4 send failed %d", ret);
    return ret;
}

int lan_play_send_zerotier_ipv4_broadcast(struct lan_play *lan_play, const void *packet, uint16_t len)
{
    uint8_t frame[ETHER_MTU + ETHER_HEADER_LEN];
    if (len < IPV4_HEADER_LEN || len > ETHER_MTU) return -1;

    memset(frame + ETHER_OFF_DST, 0xff, 6);
    CPY_MAC(frame + ETHER_OFF_SRC, lan_play->zerotier_mac);
    WRITE_NET16(frame, ETHER_OFF_TYPE, ETHER_TYPE_IPV4);
    memcpy(frame + ETHER_HEADER_LEN, packet, len);

    lan_play->zerotier_tx++;
    diagnostic_log_frame(lan_play, "TX ZeroTier", frame, len + ETHER_HEADER_LEN);
    int ret = uv_pcap_sendpacket(&lan_play->zerotier_pcap, frame, len + ETHER_HEADER_LEN);
    if (ret != 0) LLOG(LLOG_ERROR, "ZeroTier IPv4 broadcast send failed %d", ret);
    return ret;
}

int lan_play_gateway_send_packet(struct packet_ctx *packet_ctx, const void *data, uint16_t len)
{
    struct payload part;
    uint8_t dst_mac[6];
    const uint8_t *dst = (uint8_t *)data + IPV4_OFF_DST;

    if (!arp_get_mac_by_ip(packet_ctx, dst_mac, dst)) {
        return false;
    }

    part.ptr = data;
    part.len = len;
    part.next = NULL;

    return send_ether(
        packet_ctx,
        dst_mac,
        ETHER_TYPE_IPV4,
        &part
    );
}
