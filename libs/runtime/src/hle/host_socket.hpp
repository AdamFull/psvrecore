#pragma once

// The host's own IPv4 sockets, for SceNet's ad hoc mode (net.cpp): Winsock on Windows, BSD sockets elsewhere. Every
// socket is non-blocking (a guest's wait is a poll, so it can be aborted, and lets other guest threads run); failures
// are negative errno values in the console's numbering (BSD's: EAGAIN 35, EINPROGRESS 36, ...), whatever the host's.
// Addresses and ports are in host byte order here (192.168.1.2 is 0xC0A80102).

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore::runtime::hostnet {

using Fd = i64;
inline constexpr Fd kNoFd = -1;

struct Endpoint {
    u32 addr = 0;  // (host order)
    u16 port = 0;
};

// The console's errno values (SCE_NET_E*), as returned negated.
namespace e {
inline constexpr i32 kIntr = 4, kBadf = 9, kAcces = 13, kFault = 14, kInval = 22, kMfile = 24, kPipe = 32,
                     kAgain = 35, kInProgress = 36, kAlready = 37, kNotSock = 38, kDestAddrReq = 39, kMsgSize = 40,
                     kNoProtoOpt = 42, kProtoNoSupport = 43, kOpNotSupp = 45, kAfNoSupport = 47, kAddrInUse = 48,
                     kAddrNotAvail = 49, kNetDown = 50, kNetUnreach = 51, kConnAborted = 53, kConnReset = 54,
                     kNoBufs = 55, kIsConn = 56, kNotConn = 57, kTimedOut = 60, kConnRefused = 61, kHostUnreach = 65;
}

// Whether the host's sockets can be used (Winsock started; once, on first use).
bool available();

// A socket: UDP or TCP; non-blocking, and (UDP) with broadcasts allowed and, on Windows, no "connection reset" from
// an ICMP port unreachable reported by a later receive.
i32 open(bool stream, Fd* out);
void close(Fd fd);
i32 bind(Fd fd, Endpoint at);
// (non-blocking: -kInProgress while it connects; then connect_status says how it went)
i32 connect(Fd fd, Endpoint to);
// A connection under way: 0 made, -kInProgress still under way, or -errno why it failed.
i32 connect_status(Fd fd);
i32 listen(Fd fd, i32 backlog);
i32 accept(Fd fd, Fd* out, Endpoint* from);
// Bytes sent or received, or -errno. `to` null: the connected peer. A stream's end is 0 bytes received.
i32 send(Fd fd, const void* data, u32 length, const Endpoint* to);
i32 receive(Fd fd, void* data, u32 length, Endpoint* from, bool peek);
i32 shutdown(Fd fd, i32 how);  // (0 read, 1 write, 2 both)
i32 local_name(Fd fd, Endpoint* out);
i32 peer_name(Fd fd, Endpoint* out);
i32 pending_error(Fd fd);  // (SO_ERROR, as a positive errno, 0 if none)

enum class Option : u8 { ReuseAddr, ReusePort, Broadcast, KeepAlive, SendBuffer, ReceiveBuffer, NoDelay, Ttl, Tos,
                         Linger };
// (Linger: the value is the seconds, or -1 for off)
i32 set_option(Fd fd, Option option, i32 value);
i32 get_option(Fd fd, Option option, i32* value);

struct PollItem {
    Fd fd = kNoFd;
    bool want_read = false, want_write = false;
    bool readable = false, writable = false, error = false, hangup = false;  // (out)
};
// Waits up to `timeout_ms` (0: not at all) for any item to be ready; the number ready, or -errno.
i32 poll(PollItem* items, u32 count, i32 timeout_ms);

// The host's IPv4 interfaces that are up.
struct Interface {
    nx::string name;         // (wlan0; on Windows the adapter's friendly name, "Wi-Fi")
    u32 addr = 0, mask = 0;  // (host order)
    bool loopback = false;
    bool gateway = false;    // (it has a default gateway: the network the host uses; Windows only)
    bool wireless = false, wired = false, virtual_adapter = false;
};
nx::vector<Interface> interfaces();

}  // namespace psvrecore::runtime::hostnet
