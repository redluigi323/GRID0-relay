# GRID0 Relay

Fork of Switch-lan-play, meant to be used in conjuction with ZeroTier as a modern replacement to the old lan play relay servers
resulting in much more stability across the board, way faster speeds, and also a way for unmodded switches to play online with either 
[sys-zerotier] (https://github.com/redluigi323/sys-zerotier) users (wonder who made that lol) or emulator users!

Currently has support for Windows pc's and ARM macs.


Theres both a cli you can use, and a Native QT gui with either macOS theming or Windows 11 theming.


simple usage guide:

1. Download the latest release from [releases] (https://github.com/redluigi323/GRID0-relay/releases).
2. Run it by either running GRID0Relay.exe(Windows) or GRID0 Relay.app(macOS) (you will be prompted for admin perms when necessary).
3. If you dont have Zerotier and Npcap installed, head to settings and press the button to install both and run their setups.
4. Join a network on ZeroTier's ui.
5. Setup the adapters (will be automatically chosen if both are detected)
6. Input the ip settings on your switch


Then just start the relay and play with your friends over lan!


# AI USAGE NOTICE

Straight up, AI was used in the making of this. To a point of admitting that without it, this and sys-zerotier wouldnt be a reality,
but both were tested in controlled environments (my own switch over atleast 13 days of testing or so ive counted) and the technologies used
are proven projects in other uses (This being a fork of switch-lan-play, ZeroTier for the networking, and open source vpn system, etc..).
