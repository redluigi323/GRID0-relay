#include "pcap.h"
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
pcap_t * (*zll_pcap_create)(const char *name, char *err);
int (*zll_pcap_set_timeout)(pcap_t *p, int value);
int (*zll_pcap_set_snaplen)(pcap_t *p, int value);
int (*zll_pcap_set_promisc)(pcap_t *p, int value);
int (*zll_pcap_setmintocopy)(pcap_t *p, int value);
int (*zll_pcap_activate)(pcap_t *p);
int (*zll_pcap_datalink)(pcap_t *p);
char * (*zll_pcap_geterr)(pcap_t *p);
int (*zll_pcap_compile)(pcap_t *p, struct bpf_program *b, const char *filter, int optimize, bpf_u_int32 mask);
int (*zll_pcap_setfilter)(pcap_t *p, struct bpf_program *b);
void (*zll_pcap_freecode)(struct bpf_program *b);
void (*zll_pcap_close)(pcap_t *p);
int (*zll_pcap_findalldevs)(pcap_if_t **all, char *err);
void (*zll_pcap_freealldevs)(pcap_if_t *all);
int (*zll_pcap_sendpacket)(pcap_t *p, const u_char *data, int size);
int (*zll_pcap_setnonblock)(pcap_t *p, int value, char *err);
int (*zll_pcap_dispatch)(pcap_t *p, int count, pcap_handler cb, u_char *data);
pcap_t * (*zll_pcap_open_dead)(int type, int snaplen);
pcap_dumper_t * (*zll_pcap_dump_hopen)(pcap_t *p, intptr_t handle);
void (*zll_pcap_dump_close)(pcap_dumper_t *p);
void (*zll_pcap_dump)(u_char *p, const struct pcap_pkthdr *h, const u_char *packet);
int (*zll_pcap_dump_flush)(pcap_dumper_t *p);
const char * (*zll_pcap_lib_version)(void);
int (*zll_pcap_init)(unsigned int options, char *err);

int zll_npcap_load(char *error, size_t size)
{
    static HMODULE module;
    if (module) return 0;
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (!n || n + 17 >= MAX_PATH) {
        snprintf(error, size, "Cannot locate the Windows system directory.");
        return -1;
    }
    wcscat(path, L"\\Npcap\\wpcap.dll");
    // Exclude the working directory and old WinPcap DLLs from the search.
    HMODULE candidate = LoadLibraryExW(path, NULL,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!candidate) {
        snprintf(error, size, "Npcap could not be loaded (Windows error %lu). Install or repair Npcap from https://npcap.com, then reopen GRID0 Relay.", GetLastError());
        return -1;
    }
#define RESOLVE(name) do { \
    *(FARPROC *)(&zll_##name) = GetProcAddress(candidate, #name); \
    if (!zll_##name) { \
        snprintf(error, size, "Npcap is missing %s. Update Npcap from https://npcap.com.", #name); \
        FreeLibrary(candidate); return -1; \
    } \
} while (0)
    RESOLVE(pcap_create);
    RESOLVE(pcap_set_timeout);
    RESOLVE(pcap_set_snaplen);
    RESOLVE(pcap_set_promisc);
    RESOLVE(pcap_setmintocopy);
    RESOLVE(pcap_activate);
    RESOLVE(pcap_datalink);
    RESOLVE(pcap_geterr);
    RESOLVE(pcap_compile);
    RESOLVE(pcap_setfilter);
    RESOLVE(pcap_freecode);
    RESOLVE(pcap_close);
    RESOLVE(pcap_findalldevs);
    RESOLVE(pcap_freealldevs);
    RESOLVE(pcap_sendpacket);
    RESOLVE(pcap_setnonblock);
    RESOLVE(pcap_dispatch);
    RESOLVE(pcap_open_dead);
    RESOLVE(pcap_dump_hopen);
    RESOLVE(pcap_dump_close);
    RESOLVE(pcap_dump);
    RESOLVE(pcap_dump_flush);
    RESOLVE(pcap_lib_version);
    RESOLVE(pcap_init);
#undef RESOLVE
    char init_error[PCAP_ERRBUF_SIZE];
    if (pcap_init(PCAP_CHAR_ENC_UTF_8, init_error) != 0) {
        snprintf(error, size, "Npcap initialization failed: %s", init_error);
        FreeLibrary(candidate); return -1;
    }
    module = candidate; // Keep loaded while any capture handle exists.
    return 0;
}
