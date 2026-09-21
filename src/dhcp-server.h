#ifndef _DHCP_SERVER_H_
#define _DHCP_SERVER_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* Minimal DHCP server for Automatic/DHCP Switch consoles.
 *
 * The stock Switch only learns its network from manual entry or DHCP. When
 * the relay runs this server on the local Wi-Fi interface, a Switch left on
 * Automatic receives a ZeroTier-subnet address, so its broadcast address
 * matches the overlay network and Splatoon's PIA authentication succeeds.
 *
 * Safety: the server only answers clients whose MAC carries a Nintendo
 * vendor prefix (see nintendo_oui.h), so the household router's other
 * clients can never receive a poisoned lease. It is off unless the
 * --dhcp flag is passed.
 */

struct lan_play;

#define DHCP_LEASE_COUNT 8
#define DHCP_LEASE_TIME 3600 /* seconds */
#define DHCP_OFFER_TIME 120  /* seconds a DISCOVER offer is held */

struct dhcp_lease {
    uint8_t mac[6];
    uint8_t ip[4];
    time_t expires;
    bool offered; /* true while holding an unanswered OFFER */
    bool used;
};

struct dhcp_server_state {
    struct dhcp_lease leases[DHCP_LEASE_COUNT];
    uint64_t offers_sent;
    uint64_t acks_sent;
    uint64_t naks_sent;
    uint64_t ignored_non_nintendo;
    /* Times a Nintendo console took another DHCP server's offer (home
     * router, Windows' own hotspot DHCP). The desktop UI uses the matching
     * GRID0_DHCP lost event to prompt a Wi-Fi toggle retry. */
    uint64_t lost_races;
};

/* Inspect one captured Wi-Fi frame. Returns true when the frame was a DHCP
 * client packet and has been handled (the caller must not relay it). */
bool dhcp_server_consume(struct lan_play *lp, const uint8_t *frame, size_t len);

/* Reset lease state; called once from lan_play_init. */
void dhcp_server_init(struct lan_play *lp);

#endif // _DHCP_SERVER_H_
