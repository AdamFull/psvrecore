#include "host_socket.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cstring>

#include "core/foundation/threading/sync.h"
#include "psvrecore/runtime/host.hpp"

namespace psvrecore::runtime::hostnet {

namespace {

#if defined(_WIN32)
using Native = SOCKET;
Native native(Fd fd) { return static_cast<SOCKET>(fd); }
int last_error() { return WSAGetLastError(); }
// (Winsock's errors, as the console numbers them)
i32 translate(int w) {
    switch (w) {
        case WSAEINTR: return e::kIntr;
        case WSAEBADF: return e::kBadf;
        case WSAEACCES: return e::kAcces;
        case WSAEFAULT: return e::kFault;
        case WSAEINVAL: return e::kInval;
        case WSAEMFILE: return e::kMfile;
        case WSAEWOULDBLOCK: return e::kAgain;
        case WSAEINPROGRESS: return e::kInProgress;
        case WSAEALREADY: return e::kAlready;
        case WSAENOTSOCK: return e::kNotSock;
        case WSAEDESTADDRREQ: return e::kDestAddrReq;
        case WSAEMSGSIZE: return e::kMsgSize;
        case WSAENOPROTOOPT: return e::kNoProtoOpt;
        case WSAEPROTONOSUPPORT: return e::kProtoNoSupport;
        case WSAEOPNOTSUPP: return e::kOpNotSupp;
        case WSAEAFNOSUPPORT: return e::kAfNoSupport;
        case WSAEADDRINUSE: return e::kAddrInUse;
        case WSAEADDRNOTAVAIL: return e::kAddrNotAvail;
        case WSAENETDOWN: return e::kNetDown;
        case WSAENETUNREACH: return e::kNetUnreach;
        case WSAECONNABORTED: return e::kConnAborted;
        case WSAECONNRESET: return e::kConnReset;
        case WSAENOBUFS: return e::kNoBufs;
        case WSAEISCONN: return e::kIsConn;
        case WSAENOTCONN: return e::kNotConn;
        case WSAESHUTDOWN: return e::kPipe;
        case WSAETIMEDOUT: return e::kTimedOut;
        case WSAECONNREFUSED: return e::kConnRefused;
        case WSAEHOSTUNREACH: return e::kHostUnreach;
        default: return e::kInval;
    }
}
#else
using Native = int;
Native native(Fd fd) { return static_cast<int>(fd); }
int last_error() { return errno; }
i32 translate(int x) {
    switch (x) {
        case EINTR: return e::kIntr;
        case EBADF: return e::kBadf;
        case EACCES:
        case EPERM: return e::kAcces;
        case EFAULT: return e::kFault;
        case EINVAL: return e::kInval;
        case EMFILE:
        case ENFILE: return e::kMfile;
        case EPIPE: return e::kPipe;
        case EAGAIN: return e::kAgain;
#if EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK: return e::kAgain;
#endif
        case EINPROGRESS: return e::kInProgress;
        case EALREADY: return e::kAlready;
        case ENOTSOCK: return e::kNotSock;
        case EDESTADDRREQ: return e::kDestAddrReq;
        case EMSGSIZE: return e::kMsgSize;
        case ENOPROTOOPT: return e::kNoProtoOpt;
        case EPROTONOSUPPORT: return e::kProtoNoSupport;
        case EOPNOTSUPP: return e::kOpNotSupp;
        case EAFNOSUPPORT: return e::kAfNoSupport;
        case EADDRINUSE: return e::kAddrInUse;
        case EADDRNOTAVAIL: return e::kAddrNotAvail;
        case ENETDOWN: return e::kNetDown;
        case ENETUNREACH: return e::kNetUnreach;
        case ECONNABORTED: return e::kConnAborted;
        case ECONNRESET: return e::kConnReset;
        case ENOBUFS:
        case ENOMEM: return e::kNoBufs;
        case EISCONN: return e::kIsConn;
        case ENOTCONN: return e::kNotConn;
        case ETIMEDOUT: return e::kTimedOut;
        case ECONNREFUSED: return e::kConnRefused;
        case EHOSTUNREACH: return e::kHostUnreach;
        default: return e::kInval;
    }
}
#endif

i32 failed() { return -translate(last_error()); }

sockaddr_in to_native(Endpoint at) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(at.port);
    a.sin_addr.s_addr = htonl(at.addr);
    return a;
}
Endpoint from_native(const sockaddr_in& a) { return Endpoint{ntohl(a.sin_addr.s_addr), ntohs(a.sin_port)}; }

i32 set_int(Fd fd, int level, int name, int value) {
    return setsockopt(native(fd), level, name, reinterpret_cast<const char*>(&value), sizeof value) == 0 ? 0 : failed();
}
i32 get_int(Fd fd, int level, int name, i32* value) {
    int v = 0;
#if defined(_WIN32)
    int length = sizeof v;
#else
    socklen_t length = sizeof v;
#endif
    if (getsockopt(native(fd), level, name, reinterpret_cast<char*>(&v), &length) != 0) return failed();
    *value = v;
    return 0;
}

}  // namespace

bool available() {
#if defined(_WIN32)
    static const bool started = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return started;
#else
    return true;
#endif
}

i32 open(bool stream, Fd* out) {
    if (!available()) return -e::kNetDown;
    const Native s = ::socket(AF_INET, stream ? SOCK_STREAM : SOCK_DGRAM, stream ? IPPROTO_TCP : IPPROTO_UDP);
#if defined(_WIN32)
    if (s == INVALID_SOCKET) return failed();
    u_long on = 1;
    if (ioctlsocket(s, FIONBIO, &on) != 0) {
        const i32 r = failed();
        closesocket(s);
        return r;
    }
    if (!stream) {
        // (an ICMP port unreachable for a datagram sent would fail the next receive with WSAECONNRESET: not on BSD)
        BOOL off = FALSE;
        DWORD bytes = 0;
        WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, nullptr, 0, &bytes, nullptr, nullptr);
    }
#else
    if (s < 0) return failed();
    const int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) != 0) {
        const i32 r = failed();
        ::close(s);
        return r;
    }
#if defined(SO_NOSIGPIPE)
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
#endif
    *out = static_cast<Fd>(s);
    if (!stream) set_int(*out, SOL_SOCKET, SO_BROADCAST, 1);
    return 0;
}

void close(Fd fd) {
    if (fd == kNoFd) return;
#if defined(_WIN32)
    closesocket(native(fd));
#else
    ::close(native(fd));
#endif
}

i32 bind(Fd fd, Endpoint at) {
    const sockaddr_in a = to_native(at);
    return ::bind(native(fd), reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0 ? 0 : failed();
}

i32 connect(Fd fd, Endpoint to) {
    const sockaddr_in a = to_native(to);
    if (::connect(native(fd), reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0) return 0;
    const i32 r = failed();
    // (Winsock says "would block" where BSD says "in progress")
    return r == -e::kAgain ? -e::kInProgress : r;
}

i32 connect_status(Fd fd) {
#if defined(_WIN32)
    // (a failed connection is reported in the exception set, and only there, by select)
    fd_set w, x;
    FD_ZERO(&w);
    FD_ZERO(&x);
    FD_SET(native(fd), &w);
    FD_SET(native(fd), &x);
    timeval zero{};
    if (select(0, nullptr, &w, &x, &zero) < 0) return failed();
    if (FD_ISSET(native(fd), &x)) {
        const i32 error = pending_error(fd);
        return error ? -error : -e::kConnRefused;
    }
    return FD_ISSET(native(fd), &w) ? 0 : -e::kInProgress;
#else
    pollfd p{};
    p.fd = native(fd);
    p.events = POLLOUT;
    const int n = ::poll(&p, 1, 0);
    if (n < 0) return failed();
    if (n == 0 || !(p.revents & (POLLOUT | POLLERR | POLLHUP))) return -e::kInProgress;
    const i32 error = pending_error(fd);
    return error ? -error : 0;
#endif
}

i32 listen(Fd fd, i32 backlog) { return ::listen(native(fd), backlog > 0 ? backlog : 5) == 0 ? 0 : failed(); }

i32 accept(Fd fd, Fd* out, Endpoint* from) {
    sockaddr_in a{};
#if defined(_WIN32)
    int length = sizeof a;
#else
    socklen_t length = sizeof a;
#endif
    const Native s = ::accept(native(fd), reinterpret_cast<sockaddr*>(&a), &length);
#if defined(_WIN32)
    if (s == INVALID_SOCKET) return failed();
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    if (s < 0) return failed();
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    *out = static_cast<Fd>(s);
    if (from) *from = from_native(a);
    return 0;
}

i32 send(Fd fd, const void* data, u32 length, const Endpoint* to) {
    host::touch_for_os(data, length);  // (guest memory's deferred pages written first)
#if defined(MSG_NOSIGNAL)
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    long long n;
    if (to) {
        const sockaddr_in a = to_native(*to);
        n = ::sendto(native(fd), static_cast<const char*>(data), static_cast<int>(length), flags,
                     reinterpret_cast<const sockaddr*>(&a), sizeof a);
    } else {
        n = ::send(native(fd), static_cast<const char*>(data), static_cast<int>(length), flags);
    }
    return n < 0 ? failed() : static_cast<i32>(n);
}

i32 receive(Fd fd, void* data, u32 length, Endpoint* from, bool peek) {
    sockaddr_in a{};
#if defined(_WIN32)
    int alength = sizeof a;
#else
    socklen_t alength = sizeof a;
#endif
    const long long n = ::recvfrom(native(fd), static_cast<char*>(data), static_cast<int>(length), peek ? MSG_PEEK : 0,
                                   reinterpret_cast<sockaddr*>(&a), &alength);
    if (n < 0) {
#if defined(_WIN32)
        // (a datagram longer than the buffer: BSD truncates it and says nothing; Winsock fills it and says EMSGSIZE)
        if (last_error() == WSAEMSGSIZE) {
            if (from) *from = from_native(a);
            return static_cast<i32>(length);
        }
#endif
        return failed();
    }
    if (from) *from = from_native(a);
    return static_cast<i32>(n);
}

i32 shutdown(Fd fd, i32 how) {
#if defined(_WIN32)
    const int h = how == 0 ? SD_RECEIVE : how == 1 ? SD_SEND : SD_BOTH;
#else
    const int h = how == 0 ? SHUT_RD : how == 1 ? SHUT_WR : SHUT_RDWR;
#endif
    return ::shutdown(native(fd), h) == 0 ? 0 : failed();
}

i32 local_name(Fd fd, Endpoint* out) {
    sockaddr_in a{};
#if defined(_WIN32)
    int length = sizeof a;
#else
    socklen_t length = sizeof a;
#endif
    if (getsockname(native(fd), reinterpret_cast<sockaddr*>(&a), &length) != 0) return failed();
    *out = from_native(a);
    return 0;
}
i32 peer_name(Fd fd, Endpoint* out) {
    sockaddr_in a{};
#if defined(_WIN32)
    int length = sizeof a;
#else
    socklen_t length = sizeof a;
#endif
    if (getpeername(native(fd), reinterpret_cast<sockaddr*>(&a), &length) != 0) return failed();
    *out = from_native(a);
    return 0;
}
i32 pending_error(Fd fd) {
    i32 v = 0;
    if (get_int(fd, SOL_SOCKET, SO_ERROR, &v) < 0) return 0;
    return v ? translate(v) : 0;
}

i32 set_option(Fd fd, Option option, i32 value) {
    switch (option) {
        case Option::ReuseAddr: return set_int(fd, SOL_SOCKET, SO_REUSEADDR, value ? 1 : 0);
        case Option::ReusePort:
#if defined(SO_REUSEPORT)
            return set_int(fd, SOL_SOCKET, SO_REUSEPORT, value ? 1 : 0);
#else
            return set_int(fd, SOL_SOCKET, SO_REUSEADDR, value ? 1 : 0);
#endif
        case Option::Broadcast: return set_int(fd, SOL_SOCKET, SO_BROADCAST, value ? 1 : 0);
        case Option::KeepAlive: return set_int(fd, SOL_SOCKET, SO_KEEPALIVE, value ? 1 : 0);
        case Option::SendBuffer: return set_int(fd, SOL_SOCKET, SO_SNDBUF, value);
        case Option::ReceiveBuffer: return set_int(fd, SOL_SOCKET, SO_RCVBUF, value);
        case Option::NoDelay: return set_int(fd, IPPROTO_TCP, TCP_NODELAY, value ? 1 : 0);
        case Option::Ttl: return set_int(fd, IPPROTO_IP, IP_TTL, value);
        case Option::Tos: return set_int(fd, IPPROTO_IP, IP_TOS, value);
        case Option::Linger: {
            linger l{};
            l.l_onoff = value >= 0 ? 1 : 0;
            l.l_linger = static_cast<decltype(l.l_linger)>(value >= 0 ? value : 0);
            return setsockopt(native(fd), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof l) == 0
                       ? 0
                       : failed();
        }
    }
    return -e::kNoProtoOpt;
}
i32 get_option(Fd fd, Option option, i32* value) {
    switch (option) {
        case Option::SendBuffer: return get_int(fd, SOL_SOCKET, SO_SNDBUF, value);
        case Option::ReceiveBuffer: return get_int(fd, SOL_SOCKET, SO_RCVBUF, value);
        case Option::Ttl: return get_int(fd, IPPROTO_IP, IP_TTL, value);
        default: return -e::kNoProtoOpt;
    }
}

i32 poll(PollItem* items, u32 count, i32 timeout_ms) {
    if (count == 0) return 0;
#if defined(_WIN32)
    WSAPOLLFD fds[64];
#else
    pollfd fds[64];
#endif
    if (count > 64) count = 64;
    for (u32 i = 0; i < count; ++i) {
        fds[i] = {};
        fds[i].fd = native(items[i].fd);
        fds[i].events = static_cast<short>((items[i].want_read ? POLLIN : 0) | (items[i].want_write ? POLLOUT : 0));
    }
#if defined(_WIN32)
    const int n = WSAPoll(fds, count, timeout_ms);
#else
    const int n = ::poll(fds, count, timeout_ms);
#endif
    if (n < 0) {
        const i32 r = failed();
        return r == -e::kIntr ? 0 : r;
    }
    i32 ready = 0;
    for (u32 i = 0; i < count; ++i) {
        const short r = fds[i].revents;
        items[i].readable = (r & (POLLIN | POLLHUP)) != 0;
        items[i].writable = (r & POLLOUT) != 0;
        items[i].error = (r & (POLLERR | POLLNVAL)) != 0;
        items[i].hangup = (r & POLLHUP) != 0;
        if (items[i].readable || items[i].writable || items[i].error) ++ready;
    }
    return ready;
}

nx::vector<Interface> interfaces() {
    nx::vector<Interface> out;
#if defined(_WIN32)
    if (!available()) return out;
    ULONG size = 16 * 1024;
    nx::vector<u8> buffer;
    ULONG r = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 4 && r == ERROR_BUFFER_OVERFLOW; ++tries) {
        buffer.resize(size);
        r = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST,
                                 nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (r != NO_ERROR) return out;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        char name[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name, sizeof name - 1, nullptr, nullptr);
        char description[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, a->Description, -1, description, sizeof description - 1, nullptr, nullptr);
        const nx::string_view d(description), n(name);
        auto has = [](nx::string_view s, const char* what) { return s.find(what) != nx::string_view::npos; };
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            Interface i;
            i.name = name;
            i.addr = ntohl(reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr);
            const u32 prefix = u->OnLinkPrefixLength;
            i.mask = prefix == 0 ? 0 : prefix >= 32 ? 0xFFFFFFFFu : ~((1u << (32 - prefix)) - 1);
            i.loopback = a->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
            i.gateway = a->FirstGatewayAddress != nullptr;
            i.wireless = a->IfType == IF_TYPE_IEEE80211;
            i.wired = a->IfType == IF_TYPE_ETHERNET_CSMACD;
            i.virtual_adapter = has(d, "Virtual") || has(d, "Hyper-V") || has(d, "VMware") || has(d, "VirtualBox") ||
                                has(d, "TAP-") || has(d, "Bluetooth") || has(n, "vEthernet") || has(d, "WireGuard");
            out.push_back(std::move(i));
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !(a->ifa_flags & IFF_UP)) continue;
        Interface i;
        i.name = a->ifa_name ? a->ifa_name : "";
        i.addr = ntohl(reinterpret_cast<const sockaddr_in*>(a->ifa_addr)->sin_addr.s_addr);
        i.mask = a->ifa_netmask ? ntohl(reinterpret_cast<const sockaddr_in*>(a->ifa_netmask)->sin_addr.s_addr) : 0;
        i.loopback = (a->ifa_flags & IFF_LOOPBACK) != 0;
        const nx::string_view n = i.name.view();
        i.wireless = n.starts_with("wlan") || n.starts_with("swlan") || n.starts_with("ap") || n.starts_with("wl") ||
                     n.starts_with("softap");
        i.wired = n.starts_with("eth") || n.starts_with("en");
        // (cellular data, tunnels and the like: not a local network to play over)
        i.virtual_adapter = n.starts_with("rmnet") || n.starts_with("ccmni") || n.starts_with("tun") ||
                            n.starts_with("dummy") || n.starts_with("v4-") || n.starts_with("ppp") ||
                            n.starts_with("docker") || n.starts_with("veth") || n.starts_with("virbr") ||
                            n.starts_with("br-") || n.starts_with("wg");
        out.push_back(std::move(i));
    }
    freeifaddrs(list);
#endif
    return out;
}

}  // namespace psvrecore::runtime::hostnet
