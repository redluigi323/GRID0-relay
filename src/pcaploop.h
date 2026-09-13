#ifndef _PCAPLOOP_H_
#define _PCAPLOOP_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <uv.h>
#include "pcap.h"

typedef struct uv_pcap_s uv_pcap_t;
typedef void (*uv_pcap_cb)(uv_pcap_t *handle, const struct pcap_pkthdr *pkt_header, const u_char *packet, const uint8_t *mac);

struct uv_pcap_inner;
struct uv_pcap_s {
    struct uv_pcap_inner *inner;
    uv_pcap_cb cb;
    char last_error[512];

    void *data;
};

int uv_pcap_init(uv_loop_t *loop, uv_pcap_t *handle, uv_pcap_cb cb, char *netif, const char *subnet);
void uv_pcap_close(uv_pcap_t *handle);
int uv_pcap_sendpacket(uv_pcap_t *handle, const u_char *data, int size);
int uv_pcap_get_mac(const uv_pcap_t *handle, uint8_t mac[6]);
int uv_pcap_get_ipv4(const uv_pcap_t *handle, uint8_t ip[4], uint8_t netmask[4]);

#ifdef __cplusplus
}
#endif

#endif
