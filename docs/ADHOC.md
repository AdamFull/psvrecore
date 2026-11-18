# Ad hoc play over a local network

A PS Vita's ad hoc mode links consoles that are near each other directly, over a Wi-Fi network of their own (IBSS).
Phones and PCs cannot join that kind of network, so psvrecore does not try to: with the setting below on, a game's ad
hoc mode runs over an ordinary local network instead, between psvrecore instances. Those can be two phones on one Wi-Fi
network or on one phone's hotspot, a phone and a PC, or two PCs. A real Vita cannot join in.

The setting is off by default. Off, the console is offline as before: the network check dialog finds no network, and
no socket reaches anything beyond the loopback.

## The setting

In `settings.ini` (on Android: the settings screen, Network), or on the command line with `--set`:

```ini
[network]
; Ad hoc play over the local network.
adhoc = true
; The network interface it uses: empty for the local network (Wi-Fi or Ethernet), an interface's name (wlan0, Wi-Fi)
; or IPv4 address, or a loopback address (127.0.0.2) for copies on one machine, each with its own.
adhoc_interface =
```

Empty `adhoc_interface` picks the interface by itself: one that is up, IPv4, not the loopback and not a tunnel, VPN,
virtual adapter or mobile data. An interface with a default gateway counts first, then Wi-Fi or Ethernet. A
link-local address (169.254.x.x) is used only when nothing else is up. If the wrong one is chosen (a PC with several
networks), name it: its address or its name (`Wi-Fi`, `Ethernet` on Windows; `wlan0` on Linux and Android). Each
connection logs the interface used:
`SceNet: ad hoc connection made on Wi-Fi (192.168.1.20/24)`.

## How to play

1. Put both devices on the same network: the same Wi-Fi network, or one device's hotspot with the other joined to it
   (the hotspot's own device takes part too). Guest networks and networks with client isolation ("AP isolation")
   keep devices from reaching each other. Use a home network or a hotspot.
2. Turn `[network] adhoc` on in each copy. On a phone, open the settings (the on-screen menu button, or Back), then
   Network. It applies at the game's next start.
3. In the game, open its ad hoc mode. One player creates a game and the others find it in the game's list and join,
   as on the console.

The firewall must let the game's traffic in. On Windows, allow the program on private networks when Windows asks the
first time, or open UDP port 3658 (the console's ad hoc port) and any port the game uses beside it. On Linux, open the
same ports if a firewall runs. Phones need nothing.

Only one copy per device can play over a real network: the ad hoc port (3658) is bound once. To try two copies on one
PC, give each a loopback address of its own and a data folder of its own:

```
wipeout2048recomp --data run\a --set network.adhoc=true --set network.adhoc_interface=127.0.0.1
wipeout2048recomp --data run\b --set network.adhoc=true --set network.adhoc_interface=127.0.0.2
```

A broadcast from either then reaches 127.0.0.1 to 127.0.0.16.

## What the system libraries do

- **The network check dialog** (SceCommonDialog), in its ad hoc modes (ad hoc, and the PSP ad hoc create, join and
  connect modes, all one network here): the connection is made on the interface at once, and the dialog ends with OK
  at its next update, drawn or not. If no interface is up, the dialog answers as offline (the player backs out), and
  the log says why.
- **SceNetCtl**: the ad hoc state is "IP obtained" while the connection is up, `sceNetCtlAdhocGetInAddr` gives the
  interface's address, and the ad hoc callbacks are told of the connection (IPOBTAINED) and of
  `sceNetCtlAdhocDisconnect` (DISCONNECT_REQ_FINISHED). As on the console, they run in the thread that calls
  `sceNetCtlCheckCallback`. The infrastructure state stays disconnected. The ad hoc peer list is empty: peers are
  known only from the traffic they send.
- **SceNet**: sockets made while the connection is up are the host's own (`libs/runtime/src/hle/host_socket.cpp`:
  Winsock on Windows, BSD sockets elsewhere).
  - Datagram and stream sockets are UDP and TCP sockets. A P2P datagram socket (`SOCK_DGRAM_P2P`) is a UDP socket on
    its port (3658 when it names none), shared by every P2P socket bound to that port. Each datagram starts with a
    header of 8 bytes: `PV`, a version (1), a reserved byte, then the virtual ports it is from and to (network order).
    The datagrams that arrive are handed to the socket bound to their virtual port. A socket bound to virtual port 0
    takes the ones no other socket is bound to. A P2P stream socket is a TCP socket on its port; its virtual port is
    kept but not carried.
  - Reachable: the interface's own subnet, its broadcast address and the loopback. 255.255.255.255, and any address
    that reads as a broadcast of the console's own network (all ones under a /8, /16 or /24 of its address), become
    the subnet's broadcast address. Anything else has no route (`ENETUNREACH`), as when offline.
  - Waits (receive, accept, connect, epoll) poll the host's sockets in short slices, so an abort, a close or a time-out
    ends them and other guest threads run meanwhile. The socket's own time-outs and non-blocking mode are kept.
    Failures are translated to the console's errno values.
  - Options that reach the host: non-blocking, reuse (address, port), broadcast (always on for datagrams), keep-alive,
    buffer sizes, linger, `TCP_NODELAY`, TTL, TOS. Others are kept and read back.
  - `sceNetGetMacAddress` gives a locally administered address made from the interface's IPv4 address, so each copy
    has its own.
  - Sockets made before the connection, and raw sockets, stay offline. The resolver, HTTP, SSL and PSN stay offline.
- **Android**: the game's manifest needs `INTERNET`, `ACCESS_WIFI_STATE` and `CHANGE_WIFI_MULTICAST_STATE`. While
  the connection is up, the activity holds a Wi-Fi multicast lock (`PsvrecoreActivity.setMulticastLock`), without
  which the Wi-Fi chip may drop the broadcasts other players send.

Unit tests: `network_tests`, "ad hoc: ..." (the dialog, SceNetCtl's state, address and callbacks, UDP with
broadcasts, errno, time-outs and epoll, P2P sockets sharing a port by virtual port, TCP), over the loopback.

## Games

| Game | Ad hoc | Notes |
| --- | --- | --- |
| WipEout 2048 | See its README | Its ad hoc mode: the dialog, then P2P datagram sockets on port 3658 (virtual port 0 for its keep-alives, 3670 for the game). Games are announced by broadcast every 2 s |
