#pragma once
#include <string.h>

/* Windows capture paths contain case-insensitive adapter GUIDs. Unix names
 * remain case-sensitive; never choose a different adapter as a fallback. */
static inline int capture_names_equal(const char *a, const char *b, int windows)
{
    if (!a || !b) return 0;
    if (!windows) return strcmp(a, b) == 0;
    while (*a && *b) {
        unsigned char x = (unsigned char)*a++, y = (unsigned char)*b++;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return 0;
    }
    return *a == *b;
}
