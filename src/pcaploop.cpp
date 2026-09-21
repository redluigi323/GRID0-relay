#include "config.h"
#include "pcaploop.h"
#include "helper.h"
#include "capture-name.h"
#include <base/llog.h>
#include <unordered_map>

#ifndef PCAPLOOP_USE_POLL
#if defined(_WIN32)
#define PCAPLOOP_USE_POLL 0
#else
#define PCAPLOOP_USE_POLL 1
#endif
#endif

typedef struct uv_pcap_interf_s uv_pcap_interf_t;
typedef void (*uv_pcap_interf_cb)(uv_pcap_interf_t *handle, const struct pcap_pkthdr *pkt_header, const u_char *packet);
typedef void (*uv_pcap_interf_close_cb)(uv_pcap_interf_t *handle);
static int uv_pcap_interf_init(uv_loop_t *loop, uv_pcap_interf_t *handle, uv_pcap_interf_cb cb, pcap_t *dev, uint8_t *mac);
static void uv_pcap_interf_close(uv_pcap_interf_t *handle, uv_close_cb cb);
static int uv_pcap_interf_sendpacket(uv_pcap_interf_t *handle, const u_char *data, int size);
static u_char EmptyMac[6] = {0,0,0,0,0,0};

struct uv_pcap_interf_s {
#if PCAPLOOP_USE_POLL
    int fd;
    uv_poll_t poll;
#else
    uv_timer_t capture_timer;
#endif
    pcap_t *dev;
    uv_pcap_interf_cb callback;
    uint8_t mac[6];
    uint8_t ipv4[4];
    uint8_t netmask[4];
    bool has_ipv4;
    uint64_t delivered;
    uint64_t send_failures;
    uint64_t read_failures;
    uint64_t full_batches;

    void *data;
};
struct uv_pcap_inner {
    std::unordered_map<uint64_t, uv_pcap_interf_t *> map;
    uv_pcap_interf_t *interfaces;
    int count;
};

static uint64_t mac2int(const uint8_t *mac)
{
    uint64_t r = 0;
    memcpy(&r, mac, 6);
    return r;
}

static void int2mac(const uint64_t i, uint8_t *mac)
{
    memcpy(mac, &i, 6);
}

static int mask_to_prefix(const uint8_t mask[4])
{
    int prefix = 0;
    for (int i = 0; i < 4; ++i) {
        uint8_t m = mask[i];
        while (m & 0x80) {
            prefix++;
            m <<= 1;
        }
        if (m) break; /* Non-contiguous mask; stop at the first gap. */
    }
    return prefix;
}

static int set_filter(pcap_t *dev, const uint8_t *mac, const char *match)
{
    char filter[384];
    static struct bpf_program bpf;

    // Include host-originated ICMP errors for diagnostics. The relay handlers
    // explicitly discard host frames, so these can never loop through the bridge.
    // ARP always passes: Switch discovery depends on ARP replies, which the
    // `net` primitive would otherwise drop (ARP is not IP).
    snprintf(filter, sizeof(filter), "(%s) and (not ether src %02x:%02x:%02x:%02x:%02x:%02x or icmp)", match,
        mac[0],
        mac[1],
        mac[2],
        mac[3],
        mac[4],
        mac[5]
    );
    // LLOG(LLOG_DEBUG, "filter: %s", filter);
    if (pcap_compile(dev, &bpf, filter, 1, 0) != 0) {
        LLOG(LLOG_ERROR, "pcap filter compile failed: %s", pcap_geterr(dev));
        return -1;
    }
    int result = pcap_setfilter(dev, &bpf);
    pcap_freecode(&bpf);
    if (result != 0) LLOG(LLOG_ERROR, "pcap filter install failed: %s", pcap_geterr(dev));
    return result;
}


static void uv_pcap_callback(uv_pcap_interf_t *h, const struct pcap_pkthdr *pkt_header, const u_char *packet) {
    uv_pcap_t *handle = (uv_pcap_t *)h->data;
    ++h->delivered;
    if (pkt_header->caplen < 14) return;
    auto key = mac2int(packet + 0);
    handle->inner->map[key] = h;
    handle->cb(handle, pkt_header, packet, h->mac);
}
int uv_pcap_sendpacket(uv_pcap_t *handle, const u_char *data, int size)
{
    auto inner = handle->inner;
    auto key = mac2int(data + 6);
    const auto &map = inner->map;

    auto search = map.find(key);

    if (search != map.end()) {
        auto item = search->second;
        int ret = uv_pcap_interf_sendpacket(item, data, size);
        if (ret != 0) {
            LLOG(LLOG_DEBUG, "uv_pcap_interf_sendpacket failed %d", ret);
        }
        return ret;
    } else {
        int result = 0;
        for (int i = 0; i < inner->count; i++) {
            int ret = uv_pcap_interf_sendpacket(&inner->interfaces[i], data, size);
            if (ret != 0) {
                LLOG(LLOG_DEBUG, "uv_pcap_interf_sendpacket failed %d", ret);
                result = ret;
            }
        }
        // LLOG(LLOG_DEBUG, "cache not hit %llu", key);
        return result;
    }
}

int uv_pcap_get_mac(const uv_pcap_t *handle, uint8_t mac[6])
{
    if (!handle || !handle->inner || handle->inner->count != 1) {
        LLOG(LLOG_ERROR, "pcap MAC lookup failed: handle=%p inner=%p count=%d",
            handle,
            handle ? handle->inner : NULL,
            handle && handle->inner ? handle->inner->count : -1);
        return -1;
    }
    CPY_MAC(mac, handle->inner->interfaces[0].mac);
    LLOG(LLOG_DEBUG, "pcap adapter MAC %02x:%02x:%02x:%02x:%02x:%02x",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}

int uv_pcap_get_ipv4(const uv_pcap_t *handle, uint8_t ip[4], uint8_t netmask[4])
{
    if (!handle || !handle->inner || handle->inner->count != 1 ||
        !handle->inner->interfaces[0].has_ipv4) {
        LLOG(LLOG_ERROR, "pcap IPv4 lookup failed");
        return -1;
    }
    memcpy(ip, handle->inner->interfaces[0].ipv4, 4);
    memcpy(netmask, handle->inner->interfaces[0].netmask, 4);
    LLOG(LLOG_DEBUG, "pcap adapter IPv4 %u.%u.%u.%u/%u.%u.%u.%u",
        ip[0], ip[1], ip[2], ip[3], netmask[0], netmask[1], netmask[2], netmask[3]);
    return 0;
}

int uv_pcap_open_live(
    uv_loop_t *loop,
    uv_pcap_interf_t *interf,
    pcap_if_t *d,
    char err_buf[PCAP_ERRBUF_SIZE],
    const char *subnet
) {
    int ret;
    int datalink;
    pcap_t *dev;
    int rc;
    const char *stage = "pcap_create";
    err_buf[0] = 0;

    dev = pcap_create(d->name, err_buf);
    if (!dev) {
        LLOG(LLOG_DEBUG, "open %s fail: pcap_create", d->name);
        return -1;
    }
    stage = "pcap_set_timeout";
    if (pcap_set_timeout(dev, 0)) {
        LLOG(LLOG_DEBUG, "open %s fail: pcap_set_timeout", d->name);
        goto fail;
    }
    // if (pcap_set_immediate_mode(dev, 1)) {
    //     LLOG(LLOG_DEBUG, "open %s fail: pcap_set_immediate_mode", d->name);
    //     goto fail;
    // }
    stage = "pcap_set_snaplen";
    if (pcap_set_snaplen(dev, 65535)) {
        LLOG(LLOG_DEBUG, "open %s fail: pcap_set_snaplen", d->name);
        goto fail;
    }
#ifdef _WIN32
    // Absorb short scheduling/disk stalls without changing immediate delivery.
    stage = "pcap_set_buffer_size";
    if (pcap_set_buffer_size(dev, 4 * 1024 * 1024) != 0) goto fail;
#endif
    stage = "pcap_set_promisc";
    if (pcap_set_promisc(dev, 1)) {
        LLOG(LLOG_DEBUG, "open %s fail: pcap_set_promisc", d->name);
        goto fail;
    }

    stage = "pcap_activate";
    rc = pcap_activate(dev);
    if (rc > 0) {
        // Positive codes mean an activated handle with a warning, not failure.
        LLOG(LLOG_WARNING, "Capture warning on %s (%d): %s", d->name, rc, pcap_geterr(dev));
    } else if (rc < 0) {
        snprintf(err_buf, PCAP_ERRBUF_SIZE, "pcap_activate (%d): %s", rc, pcap_geterr(dev));
        goto fail;
    }

    datalink = pcap_datalink(dev);
    if (datalink != DLT_EN10MB) {
        snprintf(err_buf, PCAP_ERRBUF_SIZE, "Unsupported capture link type %d; Ethernet-compatible capture is required", datalink);
        goto fail;
    }

    uint8_t mac[6];
    stage = "adapter MAC lookup";
    if (get_mac_address(d, dev, mac) != 0) {
        LLOG(LLOG_DEBUG, "open %s fail: get mac", d->name);
        goto fail;
    }
    if (memcmp(EmptyMac, mac, 6) == 0) {
        snprintf(err_buf, PCAP_ERRBUF_SIZE, "Adapter has an all-zero MAC address");
        goto fail;
    }

    interf->has_ipv4 = false;
    for (pcap_addr_t *address = d->addresses; address; address = address->next) {
        if (!address->addr || address->addr->sa_family != AF_INET) continue;
        const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)address->addr;
        memcpy(interf->ipv4, &ipv4->sin_addr, 4);
        if (address->netmask) {
            const struct sockaddr_in *mask = (const struct sockaddr_in *)address->netmask;
            memcpy(interf->netmask, &mask->sin_addr, 4);
        } else {
            memset(interf->netmask, 0, 4);
        }
        interf->has_ipv4 = true;
        break;
    }

    stage = "capture filter";
    {
        /* The interface's own subnet must pass so DHCP/Automatic consoles on
         * the local Wi-Fi are visible, and ARP must always pass because
         * Switch discovery depends on ARP replies (`net` alone drops them).
         * UDP ports 67/68 must pass so the DHCP server sees client DISCOVER
         * and REQUEST packets, which come from 0.0.0.0 and match no `net`. */
        char match[256];
        if (interf->has_ipv4 &&
            (interf->netmask[0] | interf->netmask[1] | interf->netmask[2] | interf->netmask[3])) {
            uint8_t subnet[4];
            for (int i = 0; i < 4; ++i) subnet[i] = interf->ipv4[i] & interf->netmask[i];
            snprintf(match, sizeof(match), "net %s or net %u.%u.%u.%u/%d or arp or udp port 67 or udp port 68", subnet,
                subnet[0], subnet[1], subnet[2], subnet[3], mask_to_prefix(interf->netmask));
        } else {
            snprintf(match, sizeof(match), "net %s or arp or udp port 67 or udp port 68", subnet);
        }
        if (set_filter(dev, mac, match) != 0) goto fail;
    }

    stage = "immediate capture mode";
    if (set_immediate_mode(dev) == -1) {
        LLOG(LLOG_DEBUG, "open %s fail: set_immediate_mode %s", d->name, strerror(errno));
        goto fail;
    }

    stage = "capture event loop";
    ret = uv_pcap_interf_init(loop, interf, uv_pcap_callback, dev, mac);
    if (ret) {
        LLOG(LLOG_DEBUG, "open %s fail: pcap init", d->name);
        goto fail;
    }
    LLOG(LLOG_DEBUG, "open %s ok", d->name);
    return 0;
fail:
    if (!err_buf[0]) snprintf(err_buf, PCAP_ERRBUF_SIZE, "%s: %s", stage, pcap_geterr(dev)[0] ? pcap_geterr(dev) : "initialization failed");
    LLOG(LLOG_ERROR, "Capture %s: %s", d->name, err_buf);
    pcap_close(dev);
    return -1;
}

int uv_pcap_init(uv_loop_t *loop, uv_pcap_t *handle, uv_pcap_cb cb, char *netif, const char *subnet)
{
    handle->last_error[0] = 0;
    handle->cb = cb;
    handle->inner = new uv_pcap_inner{};
    auto inner = handle->inner;
    pcap_if_t *alldevs;
    char err_buf[PCAP_ERRBUF_SIZE];
    if (pcap_findalldevs(&alldevs, err_buf)) {
        snprintf(handle->last_error, sizeof(handle->last_error), "Npcap adapter enumeration: %s", err_buf);
        return -1;
    }
    int i = 0;
    pcap_if_t *d;

    for (d = alldevs; d; d = d->next) {
        i++;
        if (netif != NULL) {
            int netif_index = atoi(netif);
            if (i == netif_index) {
                netif = strdup(d->name);
            }
        }
    }
    if (i == 0) {
        snprintf(handle->last_error, sizeof(handle->last_error), "No capture adapters found. Check Npcap installation and connected adapters.");
        pcap_freealldevs(alldevs);
        return -1;
    }
    inner->interfaces = new uv_pcap_interf_t[i]{};
    i = 0;
    if (netif) {
        for (d = alldevs; d; d = d->next) {
#ifdef _WIN32
            bool matches = capture_names_equal(d->name, netif, 1);
#else
            bool matches = capture_names_equal(d->name, netif, 0);
#endif
            if (matches) {
                printf("found interface: %s\n", d->name);

                int ret = uv_pcap_open_live(loop, &inner->interfaces[i], d, err_buf, subnet);
                if (ret) {
                    snprintf(handle->last_error, sizeof(handle->last_error), "%s: %s", d->name, err_buf);
                    break;
                }
                inner->interfaces[i].data = handle;
                i++;
                break;
            }
        }
    } else {
        for (d = alldevs; d; d = d->next) {
            int ret = uv_pcap_open_live(loop, &inner->interfaces[i], d, err_buf, subnet);
            if (ret) {
                continue;
            }
            inner->interfaces[i].data = handle;
            i++;
        }
    }
    inner->count = i;
    pcap_freealldevs(alldevs);
    if (inner->count == 0) {
        if (!handle->last_error[0]) snprintf(handle->last_error, sizeof(handle->last_error),
            "Capture adapter not found: %s. Refresh adapters in Settings.", netif ? netif : "(any)");
        LLOG(LLOG_ERROR, "%s", handle->last_error);
        return -1;
    }
    printf("pcap loop start\n");
    return 0;
}
void uv_pcap_close(uv_pcap_t *handle)
{
    auto inner = handle->inner;
    inner->map.clear();
    for (int i = 0; i < inner->count; i++) {
        auto *h = &inner->interfaces[i];
        struct pcap_stat stats = {};
        LLOG(LLOG_INFO, "Capture health MAC %02x:%02x:%02x:%02x:%02x:%02x: delivered=%llu read-errors=%llu injection-errors=%llu full-batches=%llu",
            h->mac[0], h->mac[1], h->mac[2], h->mac[3], h->mac[4], h->mac[5],
            (unsigned long long)h->delivered, (unsigned long long)h->read_failures,
            (unsigned long long)h->send_failures, (unsigned long long)h->full_batches);
        if (pcap_stats(h->dev, &stats) == 0)
            LLOG(LLOG_INFO, "Capture driver counters: received=%u buffer-drops=%u interface-drops=%u (platform-dependent; zero can mean unavailable, not proof of delivery)",
                stats.ps_recv, stats.ps_drop, stats.ps_ifdrop);
        else
            LLOG(LLOG_INFO, "Capture driver counters unavailable: %s", pcap_geterr(h->dev));
        uv_pcap_interf_close(&inner->interfaces[i], NULL);
    }
    printf("pcap loop stop\n");
}

static int uv_pcap_interf_sendpacket(uv_pcap_interf_t *handle, const u_char *data, int size)
{
    u_char old[6];
    u_char *d = (u_char *)data;
    CPY_MAC(old, d + 6);
    CPY_MAC(d + 6, handle->mac);
    int ret = pcap_sendpacket(handle->dev, data, size);
    if (ret != 0) ++handle->send_failures;
    CPY_MAC(d + 6, old);

    return ret;
}


#if PCAPLOOP_USE_POLL

static void poll_handler(uv_poll_t *handle, int status, int events);

static int uv_pcap_interf_init(uv_loop_t *loop, uv_pcap_interf_t *handle, uv_pcap_interf_cb cb, pcap_t *dev, uint8_t *mac)
{
    int ret;
    char errbuf[PCAP_ERRBUF_SIZE];
    if (pcap_setnonblock(dev, 1, errbuf) == -1) {
        LLOG(LLOG_ERROR, "setnonblock %s", errbuf);
    }
    handle->poll.data = handle;
    handle->fd = pcap_get_selectable_fd(dev);
    handle->dev = dev;
    handle->callback = cb;
    CPY_MAC(handle->mac, mac);
    ret = uv_poll_init(loop, &handle->poll, handle->fd);
    if (ret) return ret;
    ret = uv_poll_start(&handle->poll, UV_READABLE, poll_handler);
    if (ret) return ret;
    return 0;
}

static void uv_pcap_interf_close(uv_pcap_interf_t *handle, uv_close_cb cb)
{
    int ret = uv_poll_stop(&handle->poll);
    if (ret) {
        LLOG(LLOG_ERROR, "uv_poll_stop %d", ret);
    }
}

static void poll_callback(u_char *data, const struct pcap_pkthdr *pkt_header, const u_char *packet)
{
    uv_pcap_interf_t *handle = (uv_pcap_interf_t *)data;

    handle->callback(handle, pkt_header, packet);
}

static void poll_handler(uv_poll_t *poll, int status, int events)
{
    uv_pcap_interf_t *handle = (uv_pcap_interf_t *)poll->data;
    int count;

    if (events & UV_READABLE) {
        do {
            count = pcap_dispatch(handle->dev, 1, poll_callback, (u_char *)handle);
        } while (count > 0);
        if (count == PCAP_ERROR) {
            ++handle->read_failures;
            LLOG(LLOG_ERROR, "Capture read failed: %s", pcap_geterr(handle->dev));
            uv_poll_stop(poll);
        }
    }
}

#else

static void timer_packet(u_char *data, const struct pcap_pkthdr *header, const u_char *packet)
{
    auto *h = reinterpret_cast<uv_pcap_interf_t *>(data);
    h->callback(h, header, packet);
}
static void capture_timer_cb(uv_timer_t *timer)
{
    auto *h = static_cast<uv_pcap_interf_t *>(timer->data);
    // Bounded batches leave room for the other adapter and shutdown timers.
    int ret = pcap_dispatch(h->dev, 128, timer_packet, reinterpret_cast<u_char *>(h));
    if (ret == 128) ++h->full_batches;
    if (ret < 0) {
        ++h->read_failures;
        LLOG(LLOG_ERROR, "Npcap capture failed: %s", pcap_geterr(h->dev));
        uv_timer_stop(timer);
    }
}
static int uv_pcap_interf_init(uv_loop_t *loop, uv_pcap_interf_t *h,
                              uv_pcap_interf_cb cb, pcap_t *dev, uint8_t *mac)
{
    char err[PCAP_ERRBUF_SIZE];
    if (pcap_setnonblock(dev, 1, err) != 0) {
        LLOG(LLOG_ERROR, "Npcap nonblocking capture failed: %s", err);
        return -1;
    }
    h->dev = dev; h->callback = cb; CPY_MAC(h->mac, mac);
    int ret = uv_timer_init(loop, &h->capture_timer);
    if (ret) return ret;
    h->capture_timer.data = h;
    return uv_timer_start(&h->capture_timer, capture_timer_cb, 0, 2);
}
static void uv_pcap_interf_close(uv_pcap_interf_t *h, uv_close_cb cb)
{
    uv_timer_stop(&h->capture_timer);
    uv_close(reinterpret_cast<uv_handle_t *>(&h->capture_timer), cb);
    pcap_close(h->dev); h->dev = nullptr;
}

#endif
