# macOS smoke test

This is a packet-path smoke test, not a claim that a particular game has been validated. It needs a Mac joined to the same ZeroTier network as the sys-zerotier Switch and a stock Switch connected to the Mac's Wi-Fi network.

1. Build the program, then list capture interfaces:

   ```sh
   ./build/src/grid0-relay --list-if
   ```

   Choose the Wi-Fi interface used by the stock Switch (commonly `en0`) and the Ethernet-like ZeroTier adapter. The interface list includes IPv4 addresses, which makes the ZeroTier adapter identifiable when it owns an address in the selected game subnet.

2. Pick one private `/24` game subnet. The default is `10.147.17.0/24`; reserve `10.147.17.1` as the relay's fake gateway. For Splatoon interoperability with sys-zerotier, set the stock Switch's manual address to the selected ZeroTier adapter's managed address, such as `10.147.17.156`, with mask `255.255.255.0`. The address is deliberately shared across the Mac's separate Wi-Fi and ZeroTier interfaces.

3. Start the relay with both capture interfaces named:

   ```sh
   sudo ./build/src/grid0-relay --netif en0 --zerotier-if ZEROTIER_INTERFACE
   ```

   The capture permission is the reason for `sudo`. This command does not enable Internet Sharing, modify routes, create a bridge, or change ZeroTier's Ethernet-bridging authorization.

4. Put the stock Switch in the same manual game subnet and start the relay. By default, it searches the `/24` once with ARP probes and learns the Switch address without changing its static configuration. In diagnostic mode, wait for `Detected local Switch candidate` before starting the lobby test. A sys-zerotier Switch on the ZeroTier network must use that same game subnet for LAN play.

5. Copy the managed IP and exact subnet mask printed in `Switch manual settings` to the Switch, along with the fake gateway. Reconnect and restart LAN mode after changes. For the default `/24`, discovery must use `10.147.17.255` on both interfaces. An outgoing `10.255.255.255` browse request indicates incompatible broadcast configuration; changing only its IP header does not fix the authenticated PIA payload. Diagnostics must show corresponding Wi-Fi RX and ZeroTier TX entries. Capture both sides if discovery still fails; packet transmission alone does not prove game acceptance.

For another range, select both values explicitly, for example `--subnet 10.147.23.0/24 --gateway 10.147.23.1`, then use addresses from that same range on both Switches.
