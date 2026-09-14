#pragma once
#include <pcap/pcap.h>
// The legacy SDK used an inline macro that breaks some C++ headers.
#undef inline
#ifdef _WIN32
#ifdef __cplusplus
extern "C" {
#endif
int zll_npcap_load(char *error, size_t size);
extern pcap_t * (*zll_pcap_create)(const char *name, char *err);
#define pcap_create zll_pcap_create
extern int (*zll_pcap_set_timeout)(pcap_t *p, int value);
#define pcap_set_timeout zll_pcap_set_timeout
extern int (*zll_pcap_set_snaplen)(pcap_t *p, int value);
#define pcap_set_snaplen zll_pcap_set_snaplen
extern int (*zll_pcap_set_buffer_size)(pcap_t *p, int value);
#define pcap_set_buffer_size zll_pcap_set_buffer_size
extern int (*zll_pcap_stats)(pcap_t *p, struct pcap_stat *stats);
#define pcap_stats zll_pcap_stats
extern int (*zll_pcap_set_promisc)(pcap_t *p, int value);
#define pcap_set_promisc zll_pcap_set_promisc
extern int (*zll_pcap_setmintocopy)(pcap_t *p, int value);
#define pcap_setmintocopy zll_pcap_setmintocopy
extern int (*zll_pcap_activate)(pcap_t *p);
#define pcap_activate zll_pcap_activate
extern int (*zll_pcap_datalink)(pcap_t *p);
#define pcap_datalink zll_pcap_datalink
extern char * (*zll_pcap_geterr)(pcap_t *p);
#define pcap_geterr zll_pcap_geterr
extern int (*zll_pcap_compile)(pcap_t *p, struct bpf_program *b, const char *filter, int optimize, bpf_u_int32 mask);
#define pcap_compile zll_pcap_compile
extern int (*zll_pcap_setfilter)(pcap_t *p, struct bpf_program *b);
#define pcap_setfilter zll_pcap_setfilter
extern void (*zll_pcap_freecode)(struct bpf_program *b);
#define pcap_freecode zll_pcap_freecode
extern void (*zll_pcap_close)(pcap_t *p);
#define pcap_close zll_pcap_close
extern int (*zll_pcap_findalldevs)(pcap_if_t **all, char *err);
#define pcap_findalldevs zll_pcap_findalldevs
extern void (*zll_pcap_freealldevs)(pcap_if_t *all);
#define pcap_freealldevs zll_pcap_freealldevs
extern int (*zll_pcap_sendpacket)(pcap_t *p, const u_char *data, int size);
#define pcap_sendpacket zll_pcap_sendpacket
extern int (*zll_pcap_setnonblock)(pcap_t *p, int value, char *err);
#define pcap_setnonblock zll_pcap_setnonblock
extern int (*zll_pcap_dispatch)(pcap_t *p, int count, pcap_handler cb, u_char *data);
#define pcap_dispatch zll_pcap_dispatch
extern pcap_t * (*zll_pcap_open_dead)(int type, int snaplen);
#define pcap_open_dead zll_pcap_open_dead
extern pcap_dumper_t * (*zll_pcap_dump_hopen)(pcap_t *p, intptr_t handle);
#define pcap_dump_hopen zll_pcap_dump_hopen
extern void (*zll_pcap_dump_close)(pcap_dumper_t *p);
#define pcap_dump_close zll_pcap_dump_close
extern void (*zll_pcap_dump)(u_char *p, const struct pcap_pkthdr *h, const u_char *packet);
#define pcap_dump zll_pcap_dump
extern int (*zll_pcap_dump_flush)(pcap_dumper_t *p);
#define pcap_dump_flush zll_pcap_dump_flush
extern const char * (*zll_pcap_lib_version)(void);
#define pcap_lib_version zll_pcap_lib_version
extern int (*zll_pcap_init)(unsigned int options, char *err);
#define pcap_init zll_pcap_init
#ifdef __cplusplus
}
#endif
#endif
