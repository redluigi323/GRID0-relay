// Local mock for launcher lifecycle tests. Never opens a network adapter.
#include <windows.h>
#include <cstdio>
#include <cstring>
int main(int argc, char **argv) {
    if (GetEnvironmentVariableA("ZLL_TEST_FAIL", nullptr, 0)) {
        fprintf(stderr, "[ERROR]: Mock missing Npcap installation\n"); return 2;
    }
    const char *name = nullptr;
    for (int i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], "--stop-event")) name = argv[i + 1];
    HANDLE event = name ? OpenEventA(SYNCHRONIZE, FALSE, name) : nullptr;
    if (!event) return 3;
    fprintf(stderr, "Relay started (PID %lu)\n", GetCurrentProcessId());
    fprintf(stderr, "Detected local Switch candidate: 10.42.9.123 (02:00:00:00:00:01)\n");
    fflush(stderr);
    DWORD result = WaitForSingleObject(event, 10000);
    CloseHandle(event);
    fprintf(stderr, "Mock relay stopped\n");
    return result == WAIT_OBJECT_0 ? 0 : 4;
}
