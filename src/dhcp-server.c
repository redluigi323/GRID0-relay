/* Minimal DHCP server for Automatic/DHCP Switch consoles.
 *
 * Listens on the Wi-Fi capture path (see dhcp_server_consume, called from
 * lan_play_pcap_handler) and answers only Nintendo-OUI clients with an
 * address from the ZeroTier subnet, plus the configured fake gateway.
 * Responses are injected with lan_play_dhcp_send, which transmits on the
 * local Wi-Fi interface.
 */
#include "lan-play.h"
#include "dhcp-server.h"
#include "nintendo_oui.h"

#define DHCP_OP_BOOTREQUEST 1
#define DHCP_OP_BOOTREPLY 2
#define DHCP_HTYPE_ETHER 1

#define DHCP_MSG_DISCOVER 1
#define DHCP_MSG_OFFER 2
#define DHCP_MSG_REQUEST 3
#define DHCP_MSG_ACK 5
#define DHCP_MSG_NAK 6
#define DHCP_MSG_RELEASE 7

#define DHCP_OPT_MSG_TYPE 53
#define DHCP_OPT_SERVER_ID 54
#define DHCP_OPT_REQ_IP 50
#define DHCP_OPT_LEASE_TIME 51
#define DHCP_OPT_MASK 1
#define DHCP_OPT_ROUTER 3
#define DHCP_OPT_DNS 6
#define DHCP_OPT_BROADCAST 28
#define DHCP_OPT_END 255

#define DHCP_MAGIC 0x63825363u
#define DHCP_MIN_LEN 300 /* BOOTP minimum payload */

/* Parsed from one client packet. */
struct dhcp_request {
    uint32_t xid;
    uint16_t flags;
    uint8_t chaddr[6];
    uint8_t msg_type;
    uint8_t requested_ip[4];
    bool has_requested_ip;
    uint8_t server_id[4];
    bool has_server_id;
    uint8_t ciaddr[4];
};

void dhcp_server_init(struct lan_play *lp)
{
    memset(&lp->dhcp_state, 0, sizeof(lp->dhcp_state));
}

static void dhcp_sweep_expired(struct dhcp_server_state *st, time_t now)
{
    for (int i = 0; i < DHCP_LEASE_COUNT; ++i) {
        if (st->leases[i].used && st->leases[i].expires <= now) {
            memset(&st->leases[i], 0, sizeof(st->leases[i]));
        }
    }
}

static struct dhcp_lease *dhcp_find_mac(struct dhcp_server_state *st, const uint8_t mac[6])
{
    for (int i = 0; i < DHCP_LEASE_COUNT; ++i) {
        if (st->leases[i].used && CMP_MAC(st->leases[i].mac, mac))
            return &st->leases[i];
    }
    return NULL;
}

static uint32_t ipv4_to_u32(const uint8_t ip[4])
{
    return ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
           ((uint32_t)ip[2] << 8) | ip[3];
}

static void u32_to_ipv4(uint32_t v, uint8_t ip[4])
{
    ip[0] = (v >> 24) & 0xff;
    ip[1] = (v >> 16) & 0xff;
    ip[2] = (v >> 8) & 0xff;
    ip[3] = v & 0xff;
}

/* Pick a free ZeroTier-subnet address for this MAC, preferring the top of
 * the subnet where manually configured consoles rarely live. */
static bool dhcp_pick_ip(struct lan_play *lp, const uint8_t mac[6], uint8_t out[4])
{
    struct dhcp_server_state *st = &lp->dhcp_state;
    const time_t now = time(NULL);
    struct dhcp_lease *own = dhcp_find_mac(st, mac);
    if (own && own->expires > now) {
        CPY_IPV4(out, own->ip);
        return true;
    }

    uint32_t net = ipv4_to_u32(lp->zerotier_ip) & ipv4_to_u32(lp->zerotier_netmask);
    uint32_t bcast = ipv4_to_u32(lp->zerotier_broadcast_ip);
    uint32_t gw = ipv4_to_u32(lp->packet_ctx.ip);
    uint32_t self = ipv4_to_u32(lp->zerotier_ip);

    for (uint32_t cand = bcast - 1; cand > net && (bcast - cand) <= 24; --cand) {
        uint8_t ip[4];
        u32_to_ipv4(cand, ip);
        if (cand == gw || cand == self) continue;
        if (lp->switch_seen && CMP_IPV4(ip, lp->switch_ip)) continue;
        if (arp_has_ip(&lp->packet_ctx, ip)) continue;
        bool taken = false;
        for (int i = 0; i < DHCP_LEASE_COUNT; ++i) {
            if (st->leases[i].used && st->leases[i].expires > now &&
                CMP_IPV4(st->leases[i].ip, ip)) {
                taken = true;
                break;
            }
        }
        if (taken) continue;
        CPY_IPV4(out, ip);
        return true;
    }
    return false;
}

static struct dhcp_lease *dhcp_claim(struct lan_play *lp, const uint8_t mac[6],
                                     const uint8_t ip[4], bool offered)
{
    struct dhcp_server_state *st = &lp->dhcp_state;
    const time_t now = time(NULL);
    struct dhcp_lease *lease = dhcp_find_mac(st, mac);
    if (!lease) {
        for (int i = 0; i < DHCP_LEASE_COUNT; ++i) {
            if (!st->leases[i].used) {
                lease = &st->leases[i];
                break;
            }
        }
        if (!lease) return NULL;
    }
    CPY_MAC(lease->mac, mac);
    CPY_IPV4(lease->ip, ip);
    lease->offered = offered;
    lease->expires = now + (offered ? DHCP_OFFER_TIME : DHCP_LEASE_TIME);
    lease->used = true;
    return lease;
}

/* Serialize one DHCP reply option; returns the new write offset. */
static size_t dhcp_opt(uint8_t *p, size_t off, uint8_t code, const void *data, size_t len)
{
    p[off++] = code;
    p[off++] = (uint8_t)len;
    memcpy(p + off, data, len);
    return off + len;
}

static uint16_t dhcp_ip_id = 0;

static int dhcp_send_reply(struct lan_play *lp, const uint8_t dst_mac[6],
                           const struct dhcp_request *req, uint8_t msg_type,
                           const uint8_t yiaddr[4])
{
    /* Ethernet is added by lan_play_dhcp_send; build the IPv4 packet here. */
    uint8_t ip[ETHER_MTU];
    const size_t dhcp_len = DHCP_MIN_LEN;
    const size_t udp_len = UDP_OFF_END + dhcp_len;
    const size_t ip_len = IPV4_HEADER_LEN + udp_len;
    if (ip_len > sizeof(ip)) return -1;
    memset(ip, 0, ip_len);

    uint8_t *dhcp = ip + IPV4_HEADER_LEN + UDP_OFF_END;
    dhcp[0] = DHCP_OP_BOOTREPLY;
    dhcp[1] = DHCP_HTYPE_ETHER;
    dhcp[2] = 6;
    dhcp[3] = 0;
    WRITE_NET16(dhcp, 4, (uint16_t)(req->xid >> 16));
    WRITE_NET16(dhcp, 6, (uint16_t)(req->xid & 0xffff));
    WRITE_NET16(dhcp, 10, req->flags);
    if (yiaddr) CPY_IPV4(dhcp + 16, yiaddr);
    /* siaddr: the server identifier the client unicasts to. The Wi-Fi
     * address is directly reachable on the local LAN. */
    CPY_IPV4(dhcp + 20, lp->wifi_ip);
    CPY_MAC(dhcp + 28, dst_mac);
    WRITE_NET16(dhcp, 236, (uint16_t)(DHCP_MAGIC >> 16));
    WRITE_NET16(dhcp, 238, (uint16_t)(DHCP_MAGIC & 0xffff));

    size_t off = 240;
    off = dhcp_opt(dhcp, off, DHCP_OPT_MSG_TYPE, &msg_type, 1);
    if (msg_type != DHCP_MSG_NAK) {
        off = dhcp_opt(dhcp, off, DHCP_OPT_SERVER_ID, lp->wifi_ip, 4);
        uint8_t lease_be[4];
        uint32_t lease = DHCP_LEASE_TIME;
        lease_be[0] = (lease >> 24) & 0xff;
        lease_be[1] = (lease >> 16) & 0xff;
        lease_be[2] = (lease >> 8) & 0xff;
        lease_be[3] = lease & 0xff;
        off = dhcp_opt(dhcp, off, DHCP_OPT_LEASE_TIME, lease_be, 4);
        /* The Switch must believe it lives on the ZeroTier subnet: mask,
         * router and broadcast all come from the overlay network. */
        off = dhcp_opt(dhcp, off, DHCP_OPT_MASK, lp->zerotier_netmask, 4);
        off = dhcp_opt(dhcp, off, DHCP_OPT_ROUTER, lp->packet_ctx.ip, 4);
        off = dhcp_opt(dhcp, off, DHCP_OPT_DNS, lp->packet_ctx.ip, 4);
        off = dhcp_opt(dhcp, off, DHCP_OPT_BROADCAST, lp->zerotier_broadcast_ip, 4);
    }
    dhcp[off++] = DHCP_OPT_END;

    ip[IPV4_OFF_VER_LEN] = 0x45;
    WRITE_NET16(ip, IPV4_OFF_TOTAL_LEN, (uint16_t)ip_len);
    WRITE_NET16(ip, IPV4_OFF_ID, ++dhcp_ip_id);
    ip[IPV4_OFF_TTL] = 64;
    ip[IPV4_OFF_PROTOCOL] = IPV4_PROTOCOL_UDP;
    CPY_IPV4(ip + IPV4_OFF_SRC, lp->wifi_ip);
    memset(ip + IPV4_OFF_DST, 0xff, 4);
    WRITE_NET16(ip, IPV4_OFF_CHECKSUM, 0);
    WRITE_NET16(ip, IPV4_OFF_CHECKSUM, ipv4_header_checksum(ip, IPV4_HEADER_LEN));

    uint8_t *udp = ip + IPV4_HEADER_LEN;
    WRITE_NET16(udp, UDP_OFF_SRCPORT, 67);
    WRITE_NET16(udp, UDP_OFF_DSTPORT, 68);
    WRITE_NET16(udp, UDP_OFF_LENGTH, (uint16_t)udp_len);
    WRITE_NET16(udp, UDP_OFF_CHECKSUM, 0);
    WRITE_NET16(udp, UDP_OFF_CHECKSUM, udp_checksum(ip, IPV4_HEADER_LEN, ip_len));

    static const uint8_t bcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    return lan_play_dhcp_send(lp, bcast_mac, ip, (uint16_t)ip_len);
}

static bool dhcp_parse(const uint8_t *frame, size_t len, struct dhcp_request *req)
{
    memset(req, 0, sizeof(*req));
    if (len < ETHER_HEADER_LEN + IPV4_HEADER_LEN + UDP_OFF_END + 240) return false;
    if (READ_NET16(frame, ETHER_OFF_TYPE) != ETHER_TYPE_IPV4) return false;
    const uint8_t *ip = frame + ETHER_HEADER_LEN;
    const size_t hlen = (ip[IPV4_OFF_VER_LEN] & 0x0f) * 4;
    if ((ip[IPV4_OFF_VER_LEN] >> 4) != 4 || hlen < IPV4_HEADER_LEN) return false;
    const size_t total = READ_NET16(ip, IPV4_OFF_TOTAL_LEN);
    if (total < hlen + UDP_OFF_END || total > len - ETHER_HEADER_LEN) return false;
    if (ip[IPV4_OFF_PROTOCOL] != IPV4_PROTOCOL_UDP) return false;
    const uint8_t *udp = ip + hlen;
    if (READ_NET16(udp, UDP_OFF_DSTPORT) != 67) return false;
    const size_t ulen = READ_NET16(udp, UDP_OFF_LENGTH);
    if (ulen < UDP_OFF_END + 240 || hlen + ulen > total) return false;

    const uint8_t *dhcp = udp + UDP_OFF_END;
    if (dhcp[0] != DHCP_OP_BOOTREQUEST || dhcp[1] != DHCP_HTYPE_ETHER || dhcp[2] != 6)
        return false;
    if (READ_NET16(dhcp, 236) != (uint16_t)(DHCP_MAGIC >> 16) ||
        READ_NET16(dhcp, 238) != (uint16_t)(DHCP_MAGIC & 0xffff))
        return false;

    req->xid = ((uint32_t)READ_NET16(dhcp, 4) << 16) | READ_NET16(dhcp, 6);
    req->flags = READ_NET16(dhcp, 10);
    CPY_IPV4(req->ciaddr, dhcp + 12);
    CPY_MAC(req->chaddr, dhcp + 28);

    size_t off = 240;
    const size_t end = hlen + ulen - UDP_OFF_END;
    while (off + 1 < end) {
        uint8_t code = dhcp[off];
        if (code == DHCP_OPT_END) break;
        if (code == 0) {
            ++off;
            continue;
        }
        if (off + 2 > end) break;
        uint8_t olen = dhcp[off + 1];
        if (off + 2 + olen > end) break;
        const uint8_t *data = dhcp + off + 2;
        if (code == DHCP_OPT_MSG_TYPE && olen == 1) req->msg_type = data[0];
        else if (code == DHCP_OPT_REQ_IP && olen == 4) {
            CPY_IPV4(req->requested_ip, data);
            req->has_requested_ip = true;
        } else if (code == DHCP_OPT_SERVER_ID && olen == 4) {
            CPY_IPV4(req->server_id, data);
            req->has_server_id = true;
        }
        off += 2 + olen;
    }
    return req->msg_type != 0;
}

static void dhcp_log_mac_ip(const char *what, const uint8_t mac[6], const uint8_t ip[4])
{
    LLOG(LLOG_INFO, "DHCP: %s %u.%u.%u.%u for %02x:%02x:%02x:%02x:%02x:%02x",
         what, ip[0], ip[1], ip[2], ip[3],
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* Machine-readable event for the desktop UI, which scans relay output for
 * lines containing "GRID0_DHCP <kind> <ip> <mac>". Substring matching is
 * deliberate: the log prefix format is owned by base/llog.h. */
static void dhcp_event(const char *kind, const uint8_t mac[6], const uint8_t ip[4])
{
    LLOG(LLOG_INFO, "GRID0_DHCP %s %u.%u.%u.%u %02x:%02x:%02x:%02x:%02x:%02x",
         kind, ip[0], ip[1], ip[2], ip[3],
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* True when the address lives on the ZeroTier subnet we serve. Anything
 * else came from another DHCP server (home router, Windows hotspot DHCP). */
static bool dhcp_in_our_subnet(struct lan_play *lp, const uint8_t ip[4])
{
    const uint32_t mask = ipv4_to_u32(lp->zerotier_netmask);
    const uint32_t net = ipv4_to_u32(lp->zerotier_ip) & mask;
    return (ipv4_to_u32(ip) & mask) == net;
}

bool dhcp_server_consume(struct lan_play *lp, const uint8_t *frame, size_t len)
{
    if (!options.dhcp_server || !lp->wifi_subnet_known) return false;

    struct dhcp_request req;
    if (!dhcp_parse(frame, len, &req)) return false;

    /* A DHCP client packet is always consumed while the server runs: it must
     * never be relayed into ZeroTier or mistaken for game traffic. */
    dhcp_sweep_expired(&lp->dhcp_state, time(NULL));

    const uint8_t *mac = frame + ETHER_OFF_SRC;
    if (!is_nintendo_mac(mac)) {
        /* Not a console: swallow the packet so only the household router
         * answers it, and never hand it a lease. */
        lp->dhcp_state.ignored_non_nintendo++;
        return true;
    }

    if (req.msg_type == DHCP_MSG_DISCOVER) {
        uint8_t ip[4];
        if (!dhcp_pick_ip(lp, mac, ip)) {
            LLOG(LLOG_WARNING, "DHCP: address pool exhausted; DISCOVER ignored");
            return true;
        }
        if (!dhcp_claim(lp, mac, ip, true)) {
            LLOG(LLOG_WARNING, "DHCP: no free lease slot; DISCOVER ignored");
            return true;
        }
        if (dhcp_send_reply(lp, mac, &req, DHCP_MSG_OFFER, ip) == 0) {
            lp->dhcp_state.offers_sent++;
            dhcp_log_mac_ip("offered", mac, ip);
            dhcp_event("offered", mac, ip);
        } else {
            LLOG(LLOG_WARNING, "DHCP: OFFER injection failed");
        }
        return true;
    }

    if (req.msg_type == DHCP_MSG_REQUEST) {
        struct dhcp_lease *lease = dhcp_find_mac(&lp->dhcp_state, mac);
        const time_t now = time(NULL);
        const uint8_t *want = req.has_requested_ip ? req.requested_ip : req.ciaddr;
        const bool has_want = req.has_requested_ip || !CMP_IPV4(req.ciaddr, "\0\0\0\0");
        if (lease && lease->expires > now && has_want && CMP_IPV4(want, lease->ip)) {
            /* Our client, our lease: the race is won. */
            dhcp_claim(lp, mac, lease->ip, false);
            if (dhcp_send_reply(lp, mac, &req, DHCP_MSG_ACK, lease->ip) == 0) {
                lp->dhcp_state.acks_sent++;
                dhcp_log_mac_ip("assigned", mac, lease->ip);
                dhcp_event("assigned", mac, lease->ip);
            } else {
                LLOG(LLOG_WARNING, "DHCP: ACK injection failed");
            }
            return true;
        }
        if (has_want && dhcp_in_our_subnet(lp, want)) {
            /* The console asked us for a ZeroTier-subnet address that is not
             * its lease: NAK so it restarts cleanly with DISCOVER. */
            uint8_t zero[4] = {0, 0, 0, 0};
            if (dhcp_send_reply(lp, mac, &req, DHCP_MSG_NAK, zero) == 0)
                lp->dhcp_state.naks_sent++;
            return true;
        }
        if (has_want) {
            /* The console took another DHCP server's offer (home router,
             * Windows' own hotspot DHCP). Stay silent: never NAK someone
             * else's client. The UI uses the event to suggest a retry. */
            lp->dhcp_state.lost_races++;
            dhcp_event("lost", mac, want);
            LLOG(LLOG_INFO, "DHCP: console took %u.%u.%u.%u from another server; toggle its Wi-Fi to retry",
                 want[0], want[1], want[2], want[3]);
        }
        return true;
    }

    if (req.msg_type == DHCP_MSG_RELEASE) {
        struct dhcp_lease *lease = dhcp_find_mac(&lp->dhcp_state, mac);
        if (lease) memset(lease, 0, sizeof(*lease));
        return true;
    }

    return true;
}
