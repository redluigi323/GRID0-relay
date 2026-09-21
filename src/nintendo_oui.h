#ifndef _NINTENDO_OUI_H_
#define _NINTENDO_OUI_H_

/* Nintendo vendor-prefix (OUI) detection.
 *
 * Used only as a confidence signal when learning a local Switch candidate:
 * a matching prefix strongly suggests the device is a Nintendo console, but
 * a non-match must NOT disqualify a candidate (a docked Switch behind a
 * third-party USB Ethernet adapter, or a future OUI, would not match).
 *
 * Seeded from public OUI databases. Verify against your own captures and
 * extend freely; keeping this list accurate beats keeping it long.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

static const uint8_t NINTENDO_OUIS[][3] = {
    {0x00, 0x1F, 0x32}, /* Nintendo Co., Ltd. */
    {0x04, 0x03, 0xD6}, /* Nintendo Co., Ltd. (commonly seen on Switch) */
    {0x2C, 0x10, 0xC1}, /* Nintendo Co., Ltd. */
    {0x58, 0xB9, 0x61}, /* Nintendo Co., Ltd. */
    {0x7C, 0xBB, 0x8A}, /* Nintendo Co., Ltd. */
    {0x98, 0xB6, 0xE9}, /* Nintendo Co., Ltd. */
    {0xCC, 0xFB, 0x65}, /* Nintendo Co., Ltd. */
    {0xE0, 0x0C, 0x7F}, /* Nintendo Co., Ltd. */
};

static inline bool is_nintendo_mac(const uint8_t mac[6])
{
    size_t i;
    for (i = 0; i < sizeof(NINTENDO_OUIS) / sizeof(NINTENDO_OUIS[0]); i++) {
        if (memcmp(mac, NINTENDO_OUIS[i], 3) == 0) return true;
    }
    return false;
}

#endif /* _NINTENDO_OUI_H_ */
