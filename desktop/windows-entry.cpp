#include <windows.h>
#include <cstdlib>

// Do not call `main`: Qt headers can rename the application's main to qMain,
// leaving MinGW's fallback main -> WinMain stub. Calling it loops forever.
extern int grid0RelayMain(int, char **);
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return grid0RelayMain(__argc, __argv);
}
