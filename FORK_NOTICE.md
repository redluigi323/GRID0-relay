# GRID0 Relay fork notice

GRID0 Relay is a derivative of [switch-lan-play](https://github.com/spacemeowx2/switch-lan-play), based on upstream commit `1be20e1905f6ed2b2136f29f0c5aa66f2f8de04e`.

Upstream is licensed under GNU GPL version 3. This derivative is distributed under the same license; see [LICENSE.txt](LICENSE.txt). Its original copyright notices, bundled lwIP licensing files, and submodule licensing files are retained.

The intended change is to replace switch-lan-play's custom UDP relay client/server with native ZeroTier Ethernet transport while retaining its pcap, ARP, gateway and adapter-discovery implementation. It aims to interoperate with sys-zerotier. This is not affiliated with either upstream project.
