#include "lan-play.h"

#if defined(__APPLE__) || defined(__linux__)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <limits.h>
#ifdef __APPLE__
#include <libproc.h>
#else
#include <dirent.h>
#endif

#ifdef __linux__
/* Same intent as the macOS scan below: report a relay that an earlier launch
 * left running, rather than fighting it over the same adapters. */
static int report_running_relay(void)
{
    DIR *proc = opendir("/proc");
    if (!proc) return 0; /* Without /proc the lifetime lock still protects us. */
    struct dirent *entry;
    int found = 0;
    while (!found && (entry = readdir(proc))) {
        char *end = NULL;
        long pid = strtol(entry->d_name, &end, 10);
        if (!end || *end || pid <= 0 || pid == (long)getpid()) continue;
        char path[64], name[256];
        snprintf(path, sizeof(path), "/proc/%ld/comm", pid);
        FILE *comm = fopen(path, "r");
        if (!comm) continue;
        if (fgets(name, sizeof(name), comm)) {
            name[strcspn(name, "\n")] = '\0';
            if (!strcmp(name, "grid0-relay")) {
                eprintf("Another GRID0 Relay is already running (PID %ld).\n"
                        "Stop that relay before starting this one; two instances can interfere.\n", pid);
                found = 1;
            }
        }
        fclose(comm);
    }
    closedir(proc);
    return found ? -1 : 0;
}
#endif

/* Old GUI launches detached their privileged relay. Catch those legacy
 * processes as well as new instances protected by the lifetime lock below. */
static int acquire_relay_instance(void)
{
#ifdef __linux__
    if (report_running_relay() != 0) return -1;
#else
    int count = proc_listallpids(NULL, 0);
    if (count <= 0 || count > (INT_MAX / (int)sizeof(pid_t)) - 256) {
        eprintf("Cannot inspect running relays; refusing an unchecked duplicate start.\n");
        return -1;
    }
    count += 256;
    pid_t *pids = calloc(count, sizeof(*pids));
    if (!pids) return -1;
    int found = proc_listallpids(pids, count * sizeof(*pids));
    if (found <= 0 || found >= count) {
        free(pids);
        eprintf("Could not obtain a complete relay process list. Try again.\n");
        return -1;
    }
    for (int i = 0; i < found; ++i) {
        if (pids[i] <= 0 || pids[i] == getpid()) continue;
        char path[PROC_PIDPATHINFO_MAXSIZE];
        if (proc_pidpath(pids[i], path, sizeof(path)) <= 0) continue;
        const char *name = strrchr(path, '/');
        if (name && !strcmp(name + 1, "grid0-relay")) {
            eprintf("Another GRID0 Relay is already running (PID %d).\n"
                    "Stop that relay before starting this one; two instances can interfere.\n", pids[i]);
            free(pids);
            return -1;
        }
    }
    free(pids);
#endif

    /* Do not unlink this file on exit: its inode is the shared lock. The OS
     * releases flock on process exit, including a crash or forced stop. */
    static int lock_fd = -1;
    lock_fd = open("/var/run/grid0-relay.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock_fd < 0) {
        eprintf("Cannot open relay instance lock: %s. Start the relay with administrator privileges.\n", strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(lock_fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != 0 || st.st_nlink != 1) {
        eprintf("Relay instance lock is not a regular root-owned file.\n");
        close(lock_fd); lock_fd = -1;
        return -1;
    }
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        eprintf("Another GRID0 Relay holds the instance lock.\n");
        close(lock_fd); lock_fd = -1;
        return -1;
    }
    return 0;
}
#endif

// command-line options
struct cli_options options;

OPTIONS_DEF(socks5_server_addr);
OPTIONS_DEF(relay_server_addr);
OPTIONS_DEF(zerotier_if);
uv_signal_t signal_int;
#ifdef _WIN32
#include <shellapi.h>
static const char *stop_event_name;
static DWORD parent_pid;
static HANDLE stop_event, parent_process, instance_mutex;
static uv_timer_t control_timer;
void lan_play_signal_cb(uv_signal_t *signal, int signum);
static void windows_control_cb(uv_timer_t *timer)
{
    if ((stop_event && WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) ||
        (parent_process && WaitForSingleObject(parent_process, 0) == WAIT_OBJECT_0))
        lan_play_signal_cb(&signal_int, SIGINT);
}
#endif

int list_interfaces(pcap_if_t *alldevs)
{
    int i = 0;
    pcap_if_t *d;
    for (d = alldevs; d; d = d->next) {
        printf("%d. %s", ++i, d->name);
        if (d->description) {
            printf(" (%s)", d->description);
        } else {
            printf(" (No description available)");
        }
        if (d->addresses) {
            struct pcap_addr *taddr;
            struct sockaddr_in *sin;
            char  revIP[100];
            bool  first = true;
            for (taddr = d->addresses; taddr; taddr = taddr->next)
            {
                sin = (struct sockaddr_in *)taddr->addr;
                if (sin && sin->sin_family == AF_INET) {
                    strncpy(revIP, inet_ntoa(sin->sin_addr), sizeof(revIP));
                    if (first) {
                        printf("\n\tIP: [");
                        first = false;
                    } else {
                        putchar(',');
                    }
                    printf("%s", revIP);
                }
            }
            if (!first) {
                putchar(']');
            }
        }
        putchar('\n');
    }
    return i;
}

int parse_arguments(int argc, char **argv)
{
    #define CHECK_PARAM() if (1 >= argc - i) { \
        eprintf("%s: requires an argument\n", arg); \
        return -1; \
    }
    if (argc <= 0) {
        return -1;
    }

    options.help = 0;
    options.version = 0;

    options.broadcast = false;
    options.pmtu = 0;
    options.fake_internet = false;
    options.list_if = false;
    options.diagnostics = false;
    options.status_events = false;
    options.discover_switch = true;

    options.netif = NULL;
    options.zerotier_if = NULL;
    options.subnet = NULL;
    options.gateway_ip = NULL;
    options.netif_ipaddr = NULL;
    options.netif_netmask = NULL;

    options.relay_server_addr = NULL;
    options.relay_username = NULL;
    options.relay_password = NULL;
    options.relay_password_file = NULL;

    options.socks5_server_addr = NULL;
    options.socks5_username = NULL;
    options.socks5_password = NULL;
    options.socks5_password_file = NULL;
    options.rpc = NULL;
    options.rpc_token = NULL;
    options.rpc_protocol = NULL;

    int i;
    for (i = 1; i < argc; i++) {
        char *arg = argv[i];

#ifdef _WIN32
        if (!strcmp(arg, "--stop-event")) {
            CHECK_PARAM(); stop_event_name = argv[++i]; continue;
        }
        if (!strcmp(arg, "--parent-pid")) {
            CHECK_PARAM(); char *end = NULL;
            unsigned long value = strtoul(argv[++i], &end, 10);
            if (!value || *end) return -1;
            parent_pid = (DWORD)value; continue;
        }
#endif
        if (!strcmp(arg, "--help")) {
            options.help = 1;
        } else if (!strcmp(arg, "--version")) {
            options.version = 1;
        } else if (!strcmp(arg, "--netif")) {
            CHECK_PARAM();
            options.netif = strdup(argv[i + 1]);
            i++;
        } else if (!strcmp(arg, "--zerotier-if")) {
            CHECK_PARAM();
            options.zerotier_if = strdup(argv[i + 1]);
            i++;
        } else if (!strcmp(arg, "--subnet")) {
            CHECK_PARAM();
            options.subnet = strdup(argv[i + 1]);
            i++;
        } else if (!strcmp(arg, "--gateway")) {
            CHECK_PARAM();
            options.gateway_ip = strdup(argv[i + 1]);
            i++;
        // } else if (!strcmp(arg, "--netif-netmask")) {
        //     CHECK_PARAM();
        //     options.netif_netmask = argv[i + 1];
        //     i++;
        } else if (!strcmp(arg, "--relay-server-addr")) {
            CHECK_PARAM();
            options.relay_server_addr = argv[i + 1];
            i++;
        } else if (!strcmp(arg, "--username")) {
            CHECK_PARAM();
            options.relay_username = argv[i + 1];
            i++;
        } else if (!strcmp(arg, "--password")) {
            CHECK_PARAM();
            options.relay_password = argv[i + 1];
            i++;
        } else if (!strcmp(arg, "--password-file")) {
            CHECK_PARAM();
            options.relay_password_file = argv[i + 1];
            i++;
        } else if (!strcmp(arg, "--socks5-server-addr")) {
            CHECK_PARAM();
            options.socks5_server_addr = argv[i + 1];
            i++;
        // } else if (!strcmp(arg, "--socks5-username")) {
        //     CHECK_PARAM();
        //     options.socks5_username = argv[i + 1];
        //     i++;
        // } else if (!strcmp(arg, "--socks5-password")) {
        //     CHECK_PARAM();
        //     options.socks5_password = argv[i + 1];
        //     i++;
        // } else if (!strcmp(arg, "--socks5-password-file")) {
        //     CHECK_PARAM();
        //     options.socks5_password_file = argv[i + 1];
        //     i++;
        } else if (!strcmp(arg, "--list-if")) {
            options.list_if = true;
        } else if (!strcmp(arg, "--diagnostics")) {
            options.diagnostics = true;
        } else if (!strcmp(arg, "--status-events")) {
            options.status_events = true;
        } else if (!strcmp(arg, "--capture-prefix")) {
            CHECK_PARAM();
            options.capture_prefix = argv[++i];
            options.diagnostics = true;
        } else if (!strcmp(arg, "--no-discover-switch")) {
            options.discover_switch = false;
        } else if (!strcmp(arg, "--broadcast")) {
            options.broadcast = true;
            options.relay_server_addr = "255.255.255.255:11451";
        } else if (!strcmp(arg, "--pmtu")) {
            CHECK_PARAM();
            options.pmtu = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(arg, "--fake-internet")) {
            options.fake_internet = true;
        } else if (!strcmp(arg, "--set-ionbf")) {
            setvbuf(stdout, NULL, _IONBF, 0);
            setvbuf(stderr, NULL, _IONBF, 0);
        } else if (!strcmp(arg, "--rpc")) {
            CHECK_PARAM();
            options.rpc = argv[i + 1];
            i++;
        } else if (!strcmp(arg, "--rpc-token")) {
            CHECK_PARAM();
            options.rpc_token = argv[i + 1];
            i++;
        } else if (!strcmp(arg, "--rpc-protocol")) {
            CHECK_PARAM();
            options.rpc_protocol = argv[i + 1];
            i++;
        } else {
            LLOG(LLOG_WARNING, "unknown paramter: %s", arg);
        }
    }

    if (options.help || options.version || options.list_if || options.rpc) {
        return 0;
    }
    // The fork's normal path will use the native ZeroTier transport. Keep
    // this option only for explicitly requested legacy upstream diagnostics.
    if (options.socks5_username) {
        if (!options.socks5_password && !options.socks5_password_file) {
            eprintf("username given but password not given\n");
            return -1;
        }

        if (options.socks5_password && options.socks5_password_file) {
            eprintf("--password and --password-file cannot both be given\n");
            return -1;
        }
    }
    if (options.relay_username) {
        if (!options.relay_password && !options.relay_password_file) {
            eprintf("username given but password not given\n");
            return -1;
        }

        if (options.relay_password && options.relay_password_file) {
            eprintf("--password and --password-file cannot both be given\n");
            return -1;
        }
    }

    return 0;
}

void print_help(const char *name)
{
    printf(
        "Usage:\n"
        "    %s\n"
        "        [--help]\n"
        "        [--version]\n"
        "        [--broadcast]\n"
        "        [--fake-internet]\n"
        "        [--netif <interface>] default: all\n"
        "        --zerotier-if <interface>\n"
        "        [--subnet <IPv4/CIDR>] default: " SUBNET_NET "/24\n"
        "        [--gateway <IPv4>] default: " SERVER_IP "\n"
        // "        [--netif-netmask <ipnetmask>] default: 255.255.0.0\n"
        "        [--relay-server-addr <addr>]\n"
        "        [--username <username>]\n"
        "        [--password <password>]\n"
        "        [--password-file <password-file>]\n"
        "        [--list-if]\n"
        "        [--diagnostics] print ARP/IPv4 relay traffic\n"
        "        [--status-events] print console detection without packet diagnostics\n"
        "        [--capture-prefix <path>] save five packet traces, including game payloads\n"
        "        [--no-discover-switch] disable automatic local Switch ARP discovery\n"
        "        [--pmtu <pmtu>]\n"
        "        [--socks5-server-addr <addr>]\n"
        "        [--rpc <address>]\n"
        "        [--rpc-token <token>]\n"
        "        [--rpc-protocol <rpc protocl>]\n"
        // "        [--socks5-username <username>]\n"
        // "        [--socks5-password <password>]\n"
        // "        [--socks5-password-file <file>]\n"
        "Address format is a.b.c.d:port (IPv4).\n"
        "RPC protocol could be tcp, ws. Default to ws.\n",
        name
    );
}

void walk_cb(uv_handle_t* handle, void* arg)
{
    if (!uv_is_closing(handle)) {
        uv_close(handle, NULL);
    }
    // LLOG(LLOG_DEBUG, "walk %d %p", handle->type, handle->data);
}

void lan_play_signal_cb(uv_signal_t *signal, int signum)
{
    if (uv_is_closing((uv_handle_t *)&signal_int)) return;
    struct lan_play *lan_play = signal->data;
    eprintf("stopping signum: %d\n", signum);

    int ret = lan_play_close(lan_play);
    if (ret) {
        LLOG(LLOG_ERROR, "lan_play_close %d", ret);
    }

    ret = uv_signal_stop(&signal_int);
    if (ret) {
        LLOG(LLOG_ERROR, "uv_signal_stop(signal_int) %d", ret);
    }

    uv_close((uv_handle_t *)&signal_int, NULL);

    uv_walk(lan_play->loop, walk_cb, lan_play);
}

void print_version()
{
    printf("GRID0 Relay " LANPLAY_VERSION "\n");
}

void list_netif()
{
    pcap_if_t *alldevs;
    char err_buf[PCAP_ERRBUF_SIZE];

    if (pcap_findalldevs(&alldevs, err_buf)) {
        fprintf(stderr, "Error pcap_findalldevs: %s\n", err_buf);
        exit(1);
    }

    list_interfaces(alldevs);

    pcap_freealldevs(alldevs);
}

int old_main()
{
    struct lan_play *lan_play = &real_lan_play;
    int ret;

    lan_play->loop = uv_default_loop();

    if (options.version) {
        print_version();
        return 0;
    }

    if (options.list_if) {
        list_netif();
        return 0;
    }

    if (options.zerotier_if == NULL || options.netif == NULL) {
        eprintf("Native mode requires --netif WIFI_INTERFACE and --zerotier-if ZEROTIER_INTERFACE.\n");
        return 2;
    }

#if defined(__APPLE__) || defined(__linux__)
    if (acquire_relay_instance() != 0) return 2;
#elif defined(_WIN32)
    instance_mutex = CreateMutexW(NULL, FALSE, L"Local\\Grid0Relay.NativeRelay");
    if (!instance_mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        eprintf("Another GRID0 Relay relay is running, or its instance lock is unavailable.\n");
        return 2;
    }
    if (stop_event_name) {
        stop_event = OpenEventA(SYNCHRONIZE, FALSE, stop_event_name);
        if (!stop_event) { eprintf("Cannot open the desktop stop event.\n"); return 2; }
    }
    if (parent_pid) {
        parent_process = OpenProcess(SYNCHRONIZE, FALSE, parent_pid);
        if (!parent_process) { eprintf("The desktop process is unavailable.\n"); return 2; }
    }
#endif

    RT_ASSERT(uv_signal_init(lan_play->loop, &signal_int) == 0);
    RT_ASSERT(uv_signal_start(&signal_int, lan_play_signal_cb, SIGINT) == 0);
    signal_int.data = lan_play;

    if (options.netif == NULL) {
        printf("Interface not specified, opening all interfaces\n");
    } else {
        printf("Opening single interface\n");
    }
    ret = lan_play_init(lan_play);
    if (ret != 0) {
        eprintf("Failed to start native relay (status %d): %s\n", ret, lan_play->last_err[0] ? lan_play->last_err : "unknown initialization error");
        return ret;
    }

#ifdef _WIN32
    if (stop_event || parent_process) {
        RT_ASSERT(uv_timer_init(lan_play->loop, &control_timer) == 0);
        RT_ASSERT(uv_timer_start(&control_timer, windows_control_cb, 0, 100) == 0);
    }
#endif
    eprintf("Relay started (PID %d)\n", (int)getpid());
    ret = uv_run(lan_play->loop, UV_RUN_DEFAULT);
    if (ret) {
        LLOG(LLOG_ERROR, "uv_run %d", ret);
    }

    LLOG(LLOG_DEBUG, "lan_play exit %d", ret);

    return ret;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
#ifdef _WIN32
    // QProcess sends Unicode arguments. Preserve non-ASCII report paths.
    LPWSTR *wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wide) return 1;
    argv = calloc(argc + 1, sizeof(char *));
    if (!argv) { LocalFree(wide); return 1; }
    for (int i = 0; i < argc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, NULL, 0, NULL, NULL);
        argv[i] = malloc(n);
        if (!argv[i]) { LocalFree(wide); return 1; }
        WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, argv[i], n, NULL, NULL);
    }
    LocalFree(wide); // UTF-8 argv remains valid for the process lifetime.
#endif
    if (parse_arguments(argc, argv) != 0) {
        LLOG(LLOG_ERROR, "Failed to parse arguments");
        print_help(argv[0]);
        return 1;
    }
    if (options.help) {
        print_version();
        print_help(argv[0]);
        return 0;
    }
#ifdef _WIN32
    if (!options.version) {
        char error[1024];
        if (zll_npcap_load(error, sizeof(error)) != 0) {
            eprintf("[ERROR]: %s\n", error); return 2;
        }
        eprintf("Capture runtime: %s\n", pcap_lib_version());
    }
#endif
    if (options.rpc) {
        return rpc_main(options.rpc, options.rpc_token, options.rpc_protocol);
    } else {
        return old_main();
    }
}
