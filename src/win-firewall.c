#ifdef _WIN32

#include "win-firewall.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#define FW_RULE_NAME "GRID0 - block hotspot DHCP"

/* Runs netsh without flashing a console window. */
static void run_netsh(const char *args)
{
    char cmd[512];
    _snprintf(cmd, sizeof(cmd), "netsh advfirewall firewall %s", args);
    cmd[sizeof(cmd) - 1] = '\0';

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                       NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

void winfw_set_hotspot_dhcp_block(bool enable)
{
    run_netsh("delete rule name=\"" FW_RULE_NAME "\"");
    if (enable) {
        run_netsh("add rule name=\"" FW_RULE_NAME "\" dir=out protocol=udp localport=67 action=block");
    }
}

#endif /* _WIN32 */
