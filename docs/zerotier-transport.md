# ZeroTier transport migration

This fork replaces switch-lan-play's `lan-client.c` custom UDP relay protocol, not its Wi-Fi-facing gateway code.

The native ZeroTier transport has four obligations:

1. Capture Ethernet ARP and IPv4 frames from the selected ZeroTier interface.
2. For a stock Switch source address, emit Ethernet frames with the relay's own ZeroTier MAC as source and the Switch IP in ARP/IPv4. This avoids foreign-MAC bridging permission.
3. Learn remote ZeroTier MACs from ARP, then inject inbound IPv4 onto Wi-Fi with the relay's Wi-Fi MAC as source and the Switch MAC as destination.
4. Keep switch-lan-play's gateway path local: packets for the fake gateway feed lwIP/proxy handling, while remote gaming-subnet packets use ZeroTier.

The existing UDP fragmentation, keepalive, server authentication and public relay code will be removed from the normal path. The fake gateway must remain explicit and separately tested: ARP alone does not provide NAT, DNS, or the connection-check behavior that lwIP currently supplies.

The first native path now owns `zerotier_pcap` beside the Wi-Fi pcap handle. `lan_client_send_ipv4` sends raw IPv4 Ethernet frames to that handle after ARP learning, and ZeroTier capture reuses the established ARP cache to return frames through the Wi-Fi handle. The pcap filter accepts ARP and IPv4 so `--subnet` and `--gateway` can be selected at runtime. This has only compile-time validation; live ZeroTier and Switch tests are still required before it can be treated as interoperable.
