// The network stack (SceNet) of a console that is offline: no interface is up but the loopback, so sockets, epolls
// and resolvers are made and given back, options are kept and read back, a socket binds; whatever would reach another
// host fails as the stack fails it then: a stream connects to nothing (ENETUNREACH; the loopback: ECONNREFUSED), a
// datagram sent off the machine finds no route (ENETUNREACH; to the loopback it is sent, and nobody receives it), a
// name has no DNS server to ask (RESOLVER_ENODNS). Nothing ever arrives, so a receive, an accept or an epoll wait
// waits: not at all when the socket does not block (EAGAIN), as long as its time-out says (then EAGAIN, or no events),
// or until the socket or epoll is aborted (EINTR) or closed. Failures also set the thread's sce_net_errno.
//
// Ad hoc mode (Runtime::Config::network.adhoc; off by default): the network check dialog's ad hoc connection is made
// on a host interface (adhoc_connect), and the sockets made while it is up are the host's own (host_socket.hpp),
// reaching that interface's network, as the console's ad hoc network reaches the consoles near it:
// - datagram and stream sockets are UDP and TCP sockets; a P2P datagram socket (SOCK_DGRAM_P2P) is a UDP socket on
//   its port (3658 by default) shared by the P2P sockets bound to it, each datagram led by a header of 8 bytes: "PV",
//   a version (1), a reserved byte, the virtual ports it is from and to (network order), by which the datagrams that
//   arrive are given to their sockets (one bound to virtual port 0 takes those for a virtual port nobody is bound
//   to: WipEout 2048's keep-alives). A P2P stream socket is a TCP socket on its port (its virtual port kept, not
//   carried);
// - what is reachable: the interface's own network (its subnet), its broadcast address, which 255.255.255.255 and
//   any address that reads as a broadcast of the console's own network (all ones under /8, /16, /24) become, and the
//   loopback; anything else has no route (ENETUNREACH), as offline. On a loopback interface (instances on one
//   machine, 127.0.0.N each) a broadcast goes to 127.0.0.1 to 127.0.0.16;
// - a wait is a poll of the host's socket, in slices, so an abort, a close or a time-out ends it, and other guest
//   threads run meanwhile; errno values and options are translated (non-blocking, the time-outs, reuse, broadcast,
//   keep-alive, buffers, linger, TCP_NODELAY, TTL and TOS reach the host; the others are kept and read back);
// - epolls wait on their sockets' host descriptors together.
// Sockets made before the connection, and raw sockets, stay offline; nothing else changes (SceNetCtl's
// infrastructure state, the resolver, PSN).
//
// Addresses are SceNetSockaddrIn: length, family (2), port (network order), address (network order), a virtual
// port, padding (16 bytes). Byte order and address text (InetPton/Ntop) need no network and work.

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>

#include "hle_common.hpp"
#include "host_socket.hpp"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/strings/format.h"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceNet = 0x6BF8B2A2;
constexpr i32 kAfInet = 2;
constexpr i32 kSockStream = 1, kSockDgram = 2, kSockRaw = 3, kSockDgramP2P = 6, kSockStreamP2P = 10;
constexpr i32 kSolSocket = 0xFFFF, kSoReuseAddr = 0x4, kSoKeepAlive = 0x8, kSoBroadcast = 0x20, kSoLinger = 0x80,
              kSoReusePort = 0x200, kSoSndBuf = 0x1001, kSoRcvBuf = 0x1002, kSoSndTimeo = 0x1005,
              kSoRcvTimeo = 0x1006, kSoError = 0x1007, kSoType = 0x1008, kSoNbio = 0x1100;
constexpr i32 kIpProtoIp = 0, kIpProtoTcp = 6, kIpTos = 3, kIpTtl = 4, kTcpNoDelay = 1;
constexpr i32 kMsgPeek = 0x2, kMsgWaitAll = 0x40, kMsgDontWait = 0x80;
constexpr u32 kEpollIn = 0x1, kEpollOut = 0x2, kEpollErr = 0x8, kEpollHup = 0x10, kEpollCtlAdd = 1, kEpollCtlMod = 2,
              kEpollCtlDel = 3;
constexpr u16 kAdhocPort = 3658;  // (SCE_NET_ADHOC_PORT: a P2P socket's UDP port when it names none)

// SCE_NET_ERROR_E*: 0x80410100 + the BSD errno.
constexpr i32 net_error(u32 errno_value) { return err::code(0x80410100u + errno_value); }
constexpr i32 kEintr = net_error(4), kEbadf = net_error(9), kEfault = net_error(14), kEinval = net_error(22),
              kEagain = net_error(35), kEinprogress = net_error(36), kEdestaddrreq = net_error(39),
              kEprotonosupport = net_error(43), kEafnosupport = net_error(47), kEnetunreach = net_error(51),
              kEisconn = net_error(56), kEnotconn = net_error(57), kEconnrefused = net_error(61),
              kEnotinit = err::code(0x804101C8), kResolverEnodns = err::code(0x804101E1);
// (a host failure, -errno, as the console's error)
i32 host_error(i32 r) { return r < 0 ? net_error(static_cast<u32>(-r)) : r; }

struct SockaddrIn {
    u8 len, family;
    u16 port;  // (network order)
    u32 addr;  // (network order)
    u16 vport;  // (network order)
    u8 zero[6];
};
static_assert(sizeof(SockaddrIn) == 16);

struct EpollEvent {
    u32 events, reserved;
    u32 system[4];
    u64 data;
};
static_assert(sizeof(EpollEvent) == 32);

// A host socket: closed when the last of its users (the socket, a P2P port, a wait in progress) lets it go.
struct HostFd {
    hostnet::Fd fd = hostnet::kNoFd;
    explicit HostFd(hostnet::Fd f) : fd(f) {}
    ~HostFd() { hostnet::close(fd); }
    HostFd(const HostFd&) = delete;
    HostFd& operator=(const HostFd&) = delete;
};
using HostRef = std::shared_ptr<HostFd>;

// A datagram that came for a P2P socket: who from (its virtual port too), and what.
struct Packet {
    SockaddrIn from;
    nx::vector<u8> data;
};
constexpr usize kP2pQueue = 256;  // (datagrams a P2P socket keeps unread; more are dropped, as a full buffer drops them)

struct Socket {
    i32 type = 0;
    bool nbio = false, aborted = false, listening = false, bound = false;
    u32 rcvtimeo = 0, sndtimeo = 0;  // (µs; 0: none)
    SockaddrIn local{};
    nx::hash_map<u64, u32> options;  // (other options: (level << 32 | name) -> the value's first word)
    // Ad hoc: the host's socket (a P2P datagram socket's: its port's, once bound), the peer a datagram socket was
    // connected to, and a P2P socket's datagrams.
    bool adhoc = false, connected = false;
    HostRef host;
    SockaddrIn peer{};
    nx::vector<Packet> queue;
};
struct Registered {
    i32 id;
    u32 events;
    u64 data;
};
struct Epoll {
    bool aborted = false;
    nx::vector<Registered> registered;
};
struct Resolver {
    i32 error = 0;
    bool aborted = false;
};

// The ad hoc connection: the host interface it is on (host order), up or not.
struct Link {
    bool up = false, loopback = false;
    u32 addr = 0, mask = 0;
    nx::string name;
};

struct Net {
    nx::mutex lock;
    bool initialized = false;
    i32 memory_size = 0;
    i32 next_id = 1;
    nx::hash_map<i32, Socket> sockets;
    nx::hash_map<i32, Epoll> epolls;
    nx::hash_map<i32, Resolver> resolvers;
    nx::mutex errno_lock;  // (its own: errors are set with the lock above held)
    nx::hash_map<i32, u32> errno_at;  // (each thread's sce_net_errno, in guest memory)
    Link link;
    nx::hash_map<u16, HostRef> p2p_ports;  // (UDP port (host order) -> the socket its P2P sockets share)
    nx::vector<u8> datagram;               // (a datagram read or written through a P2P port)
};
Net& net() {
    static Net n;
    return n;
}
using Held = nx::scoped_lock<nx::mutex>;

// ---- addresses: the guest's (network order, in its sockaddr) and the host's (host order)

u32 host_addr(u32 network_order) { return __builtin_bswap32(network_order); }
u16 host_port(u16 network_order) { return __builtin_bswap16(network_order); }
hostnet::Endpoint endpoint(const SockaddrIn& a) { return {host_addr(a.addr), host_port(a.port)}; }
SockaddrIn sockaddr(hostnet::Endpoint e, u16 vport_host = 0) {
    SockaddrIn a{};
    a.len = sizeof(SockaddrIn);
    a.family = kAfInet;
    a.port = __builtin_bswap16(e.port);
    a.addr = __builtin_bswap32(e.addr);
    a.vport = __builtin_bswap16(vport_host);
    return a;
}
nx::string text(u32 host_order) {
    return nx::format("{}.{}.{}.{}", host_order >> 24, (host_order >> 16) & 0xFF, (host_order >> 8) & 0xFF,
                      host_order & 0xFF);
}
bool is_loopback(u32 host_order) { return (host_order >> 24) == 127; }

// The host destinations of a guest one: one, several (a broadcast on the loopback link), or none: -ENETUNREACH.
constexpr u32 kLoopbackPeers = 16;
i32 destinations(const Link& l, u32 to, hostnet::Endpoint* out, u16 port) {
    if (is_loopback(to) && !(l.up && l.loopback)) {
        out[0] = {to, port};
        return 1;
    }
    if (!l.up) return -hostnet::e::kNetUnreach;
    bool broadcast = to == 0xFFFFFFFFu;
    for (const u32 m : {l.mask, 0xFF000000u, 0xFFFF0000u, 0xFFFFFF00u})
        if (m && (to & m) == (l.addr & m) && (to | m) == 0xFFFFFFFFu) broadcast = true;
    if (broadcast) {
        if (!l.loopback) {
            out[0] = {l.addr | ~l.mask, port};
            return 1;
        }
        for (u32 i = 0; i < kLoopbackPeers; ++i) out[i] = {0x7F000001u + i, port};
        return static_cast<i32>(kLoopbackPeers);
    }
    if ((to & l.mask) == (l.addr & l.mask)) {
        out[0] = {to, port};
        return 1;
    }
    return -hostnet::e::kNetUnreach;
}

// (the calling thread's errno word: made on first use)
u32 errno_word(Net& n) {
    const i32 thread = kernel().current_thread_id();
    if (auto it = n.errno_at.find(thread); it != n.errno_at.end()) return it->second;
    const u32 at = runtime().allocate_system(4, "sce_net_errno");
    n.errno_at[thread] = at;
    return at;
}
// A result: errors also go to the thread's errno (the low byte of a SCE_NET_ERROR_E* code).
i32 result(i32 r) {
    if (r < 0 && (static_cast<u32>(r) & 0xFFFFFF00u) == 0x80410100u) {
        Net& n = net();
        Held held(n.errno_lock);
        if (const u32 at = errno_word(n)) psv_st32(at, static_cast<u32>(r) & 0xFF);
    }
    return r;
}

u32 sceNetErrnoLoc() {
    Net& n = net();
    Held held(n.errno_lock);
    return errno_word(n);
}

i32 sceNetInit(Ptr<const u32> param) {
    Net& n = net();
    Held held(n.lock);
    if (n.initialized) return net_error(16);  // (EBUSY)
    n.initialized = true;
    n.memory_size = param && readable(param.address, 12) ? static_cast<i32>(param.get()[1]) : 0;
    nx::logi(logs::hle, "SceNet: started ({})", adhoc_enabled() ? "offline until an ad hoc connection is made"
                                                                : "offline: only the loopback");
    return 0;
}
i32 sceNetTerm() {
    Net& n = net();
    Held held(n.lock);
    if (!n.initialized) return kEnotinit;
    n.initialized = false;
    n.sockets.clear();
    n.epolls.clear();
    n.resolvers.clear();
    n.p2p_ports.clear();
    return 0;
}

// Waits for what never comes: until `timeout_us` passes (0: forever; -1: not at all), or `aborted` says so, or the
// object is gone (`exists`). Returns 0 when the time ran out, kEintr when aborted, kEbadf when gone.
template <class Aborted, class Exists>
i32 wait_for_nothing(i64 timeout_us, Aborted&& aborted, Exists&& exists) {
    const u64 start = kernel().clock_ns();
    while (true) {
        {
            Held held(net().lock);
            if (!exists()) return kEbadf;
            if (aborted()) return kEintr;
        }
        if (timeout_us < 0) return 0;
        u64 step = 10'000;  // (µs)
        if (timeout_us > 0) {
            const u64 waited = (kernel().clock_ns() - start) / 1000;
            if (waited >= static_cast<u64>(timeout_us)) return 0;
            step = nx::min<u64>(step, static_cast<u64>(timeout_us) - waited);
        }
        kernel().delay_thread(static_cast<u32>(step), false);
    }
}

i32 new_id(Net& n) { return n.next_id++; }

Socket* socket_of(Net& n, i32 s) {
    auto it = n.sockets.find(s);
    return it == n.sockets.end() ? nullptr : &it->second;
}
bool stream(const Socket& s) { return s.type == kSockStream || s.type == kSockStreamP2P; }
bool p2p_datagram(const Socket& s) { return s.type == kSockDgramP2P; }
bool loopback(u32 network_order_addr) { return (network_order_addr & 0xFF) == 127; }
u16 vport_of(const Socket& s) { return host_port(s.local.vport); }
u16 port_of(const Socket& s) { return host_port(s.local.port); }

// An option the host is given: which, and its value (what the guest set).
bool host_option(i32 level, i32 name, hostnet::Option* out) {
    using O = hostnet::Option;
    if (level == kSolSocket) {
        switch (name) {
            case kSoReuseAddr: *out = O::ReuseAddr; return true;
            case kSoReusePort: *out = O::ReusePort; return true;
            case kSoBroadcast: *out = O::Broadcast; return true;
            case kSoKeepAlive: *out = O::KeepAlive; return true;
            case kSoSndBuf: *out = O::SendBuffer; return true;
            case kSoRcvBuf: *out = O::ReceiveBuffer; return true;
            case kSoLinger: *out = O::Linger; return true;
            default: return false;
        }
    }
    if (level == kIpProtoTcp && name == kTcpNoDelay) return *out = O::NoDelay, true;
    if (level == kIpProtoIp && name == kIpTtl) return *out = O::Ttl, true;
    if (level == kIpProtoIp && name == kIpTos) return *out = O::Tos, true;
    return false;
}
void apply_option(const Socket& k, hostnet::Fd fd, i32 level, i32 name, i32 value) {
    hostnet::Option o{};
    if (!host_option(level, name, &o)) return;
    // (the host's datagram sockets always allow broadcasts: the console's ad hoc ones need not ask)
    if (o == hostnet::Option::Broadcast && !stream(k)) value = 1;
    if (const i32 r = hostnet::set_option(fd, o, value); r < 0)
        nx::logd(logs::hle, "SceNet: option {}:{:#x} = {} not taken by the host (errno {})", level, name, value, -r);
}
// (the options kept, given to a host socket made after they were set)
void apply_options(const Socket& k, hostnet::Fd fd) {
    for (const auto& [key, value] : k.options)
        apply_option(k, fd, static_cast<i32>(key >> 32), static_cast<i32>(key & 0xFFFFFFFFu), static_cast<i32>(value));
}
u64 option_key(i32 level, i32 name) { return (static_cast<u64>(static_cast<u32>(level)) << 32) | static_cast<u32>(name); }

i32 sceNetSocket(Ptr<const char> name, i32 domain, i32 type, i32 /*protocol*/) {
    Net& n = net();
    Held held(n.lock);
    if (!n.initialized) return kEnotinit;
    if (domain != kAfInet) return kEafnosupport;
    if (type != kSockStream && type != kSockDgram && type != kSockRaw && type != kSockDgramP2P && type != kSockStreamP2P)
        return kEprotonosupport;
    HostRef host;
    const bool adhoc = n.link.up && type != kSockRaw;
    if (adhoc && type != kSockDgramP2P) {
        hostnet::Fd fd = hostnet::kNoFd;
        if (const i32 r = hostnet::open(type == kSockStream || type == kSockStreamP2P, &fd); r < 0) return host_error(r);
        host = std::make_shared<HostFd>(fd);
    }
    const i32 id = new_id(n);
    Socket& k = n.sockets[id];
    k.type = type;
    k.local.len = sizeof(SockaddrIn);
    k.local.family = kAfInet;
    k.adhoc = adhoc;
    k.host = std::move(host);
    if (adhoc) nx::logd(logs::hle, "SceNet: socket {} '{}' (type {}): the host's", id, str(name, 32), type);
    return id;
}

// (the P2P port's host socket, made on first use: bound to the link's address (a loopback link) or any)
i32 p2p_port(Net& n, const Socket& k, u16 port, HostRef* out) {
    if (auto it = n.p2p_ports.find(port); it != n.p2p_ports.end()) {
        *out = it->second;
        return 0;
    }
    hostnet::Fd fd = hostnet::kNoFd;
    if (const i32 r = hostnet::open(false, &fd); r < 0) return r;
    HostRef ref = std::make_shared<HostFd>(fd);
    apply_options(k, fd);
    if (const i32 r = hostnet::bind(fd, {n.link.loopback ? n.link.addr : 0u, port}); r < 0) return r;
    n.p2p_ports[port] = ref;
    nx::logi(logs::hle, "SceNet: P2P port {} open on {}", port, n.link.loopback ? text(n.link.addr) : nx::string("any address"));
    *out = std::move(ref);
    return 0;
}
// (a P2P socket gone: its port closed once none uses it)
void release_p2p_port(Net& n, u16 port) {
    for (const auto& [id, s] : n.sockets)
        if (s.adhoc && p2p_datagram(s) && s.bound && port_of(s) == port) return;
    n.p2p_ports.erase(port);
}

i32 sceNetSocketClose(i32 s) {
    Net& n = net();
    Held held(n.lock);
    Socket* k = socket_of(n, s);
    if (!k) return result(kEbadf);
    const bool p2p = k->adhoc && p2p_datagram(*k) && k->bound;
    const u16 port = port_of(*k);
    n.sockets.erase(s);
    if (p2p) release_p2p_port(n, port);
    return 0;
}
i32 sceNetSocketAbort(i32 s, i32 /*flags*/) {
    Net& n = net();
    Held held(n.lock);
    Socket* k = socket_of(n, s);
    if (!k) return result(kEbadf);
    k->aborted = true;
    return 0;
}

i32 read_address(Ptr<const SockaddrIn> addr, u32 length, SockaddrIn* out) {
    if (!addr || length < sizeof(SockaddrIn) || !readable(addr.address, sizeof(SockaddrIn))) return kEinval;
    *out = *addr;
    if (out->family != kAfInet) return kEafnosupport;
    return 0;
}

// An ad hoc socket bound: the host's at the link's address (a loopback link: each instance has its own) or any
// address (a network's broadcasts reach only a socket bound so, on most hosts); a P2P datagram socket to its port's
// shared socket, by its virtual port.
i32 bind_adhoc(Net& n, Socket& k, SockaddrIn a, bool unnamed = false) {
    if (p2p_datagram(k)) {
        u16 port = host_port(a.port);
        if (!port) port = kAdhocPort;
        u16 vport = host_port(a.vport);
        for (const auto& [id, s] : n.sockets)
            if (&s != &k && s.adhoc && p2p_datagram(s) && s.bound && port_of(s) == port && vport_of(s) == vport)
                return net_error(48);  // (EADDRINUSE)
        if (!vport && unnamed) {  // (a virtual port of its own)
            for (vport = 49152; vport; ++vport) {
                bool used = false;
                for (const auto& [id, s] : n.sockets) used |= s.adhoc && p2p_datagram(s) && s.bound && port_of(s) == port && vport_of(s) == vport;
                if (!used) break;
            }
        }
        if (const i32 r = p2p_port(n, k, port, &k.host); r < 0) return host_error(r);
        k.bound = true;
        k.local = a;
        k.local.len = sizeof(SockaddrIn);
        k.local.port = __builtin_bswap16(port);
        k.local.vport = __builtin_bswap16(vport);
        nx::logi(logs::hle, "SceNet: P2P socket bound to port {}, virtual port {}", port, vport);
        return 0;
    }
    const u32 at = n.link.loopback ? n.link.addr : 0u;
    if (const i32 r = hostnet::bind(k.host->fd, {at, host_port(a.port)}); r < 0) return host_error(r);
    hostnet::Endpoint bound{};
    hostnet::local_name(k.host->fd, &bound);
    k.bound = true;
    k.local = a;
    k.local.len = sizeof(SockaddrIn);
    k.local.port = __builtin_bswap16(bound.port);
    nx::logi(logs::hle, "SceNet: {} socket bound to {}:{} (on the host {}:{})", stream(k) ? "stream" : "datagram",
             text(host_addr(a.addr)), bound.port, text(at), bound.port);
    return 0;
}
// (an ad hoc socket used unbound: on a loopback link bound first to the link's own address, which the host would
// not choose; a P2P socket to the default port)
i32 autobind(Net& n, Socket& k) {
    if (k.bound) return 0;
    if (!p2p_datagram(k) && !n.link.loopback) return 0;
    SockaddrIn a = sockaddr({0, 0});
    return bind_adhoc(n, k, a, true);
}

i32 sceNetBind(i32 s, Ptr<const SockaddrIn> addr, u32 length) {
    SockaddrIn a{};
    if (const i32 r = read_address(addr, length, &a); r < 0) return result(r);
    Net& n = net();
    Held held(n.lock);
    Socket* k = socket_of(n, s);
    if (!k) return result(kEbadf);
    if (k->bound) return result(kEinval);
    if (k->adhoc) return result(bind_adhoc(n, *k, a));
    k->bound = true;
    k->local = a;
    k->local.len = sizeof(SockaddrIn);
    if (!a.port) k->local.port = __builtin_bswap16(static_cast<u16>(49152 + s % 16384));  // (an ephemeral port)
    return 0;
}

// ---- waits on ad hoc sockets: tries again whenever the host's socket is ready

constexpr i32 kSliceMs = 20, kP2pSliceMs = 4;  // (a P2P port's socket is read for all its sockets: polled more often)

// Repeats `attempt` (under the lock: -kEagain to wait) until it gives anything else, waiting meanwhile for the
// socket's host descriptor to be readable (or writable), up to `timeout_us` (0: forever; -1: not at all), or until it
// is aborted (EINTR) or closed (EBADF). The time running out is EAGAIN.
template <class Attempt>
i32 host_wait(i32 s, bool write, i64 timeout_us, Attempt&& attempt) {
    Net& n = net();
    const u64 start = kernel().clock_ns();
    while (true) {
        kernel().checkpoint();  // (the thread ended here at shutdown; its CPU given up when the scheduler took it)
        HostRef host;
        bool p2p = false;
        {
            Held held(n.lock);
            Socket* k = socket_of(n, s);
            if (!k) return kEbadf;
            if (k->aborted) {
                k->aborted = false;
                return kEintr;
            }
            if (const i32 r = attempt(*k); r != kEagain) return r;
            if (timeout_us < 0) return kEagain;
            host = k->host;
            p2p = p2p_datagram(*k);
        }
        i32 slice = p2p ? kP2pSliceMs : kSliceMs;
        if (timeout_us > 0) {
            const u64 waited = (kernel().clock_ns() - start) / 1000;
            if (waited >= static_cast<u64>(timeout_us)) return kEagain;
            slice = static_cast<i32>(nx::min<u64>(static_cast<u64>(slice), (static_cast<u64>(timeout_us) - waited + 999) / 1000));
        }
        if (!host) {  // (a P2P socket not bound yet: nothing can come)
            kernel().delay_thread(static_cast<u32>(slice) * 1000, false);
            continue;
        }
        hostnet::PollItem item;
        item.fd = host->fd;
        item.want_read = !write;
        item.want_write = write;
        Kernel::Blocking blocking(kernel());
        hostnet::poll(&item, 1, slice);
    }
}
i64 receive_timeout(const Socket& k, bool dont_wait) { return k.nbio || dont_wait ? -1 : static_cast<i64>(k.rcvtimeo); }
i64 send_timeout(const Socket& k, bool dont_wait) { return k.nbio || dont_wait ? -1 : static_cast<i64>(k.sndtimeo); }

// The datagrams waiting on a P2P port, given to the sockets bound to their virtual ports (a datagram for none is
// dropped, as the console drops it).
void pump_p2p(Net& n, u16 port) {
    auto it = n.p2p_ports.find(port);
    if (it == n.p2p_ports.end()) return;
    const HostRef host = it->second;
    n.datagram.resize(65536);
    for (u32 i = 0; i < 256; ++i) {
        hostnet::Endpoint from{};
        const i32 got = hostnet::receive(host->fd, n.datagram.data(), static_cast<u32>(n.datagram.size()), &from, false);
        if (got < 0) break;
        const u8* d = n.datagram.data();
        if (got < 8 || d[0] != 'P' || d[1] != 'V' || d[2] != 1) continue;
        const u16 from_vport = static_cast<u16>(d[4] << 8 | d[5]), to_vport = static_cast<u16>(d[6] << 8 | d[7]);
        nx::logd(logs::hle, "SceNet: P2P {} bytes from {}:{} virtual port {} to virtual port {}", got - 8, text(from.addr),
                 from.port, from_vport, to_vport);
        // (the socket bound to its virtual port, else one bound to virtual port 0, which takes any)
        Socket* to = nullptr;
        for (auto& [id, k] : n.sockets) {
            if (!k.adhoc || !p2p_datagram(k) || !k.bound || port_of(k) != port) continue;
            if (vport_of(k) == to_vport) {
                to = &k;
                break;
            }
            if (vport_of(k) == 0 && !to) to = &k;
        }
        if (!to) continue;
        if (to->connected && (to->peer.addr != __builtin_bswap32(from.addr) || host_port(to->peer.vport) != from_vport))
            continue;  // (a connected socket takes only its peer's)
        if (to->queue.size() >= kP2pQueue) continue;
        Packet p;
        p.from = sockaddr(from, from_vport);
        p.data.resize(static_cast<usize>(got - 8));
        if (got > 8) std::memcpy(p.data.data(), d + 8, static_cast<usize>(got - 8));
        to->queue.push_back(std::move(p));
    }
}

// One try at receiving on an ad hoc socket: what came (its length; a stream's end: 0), or -kEagain.
i32 try_receive(Net& n, Socket& k, u8* buffer, u32 length, bool peek, SockaddrIn* from) {
    if (p2p_datagram(k)) {
        if (!k.bound) return kEagain;
        pump_p2p(n, port_of(k));
        if (k.queue.empty()) return kEagain;
        Packet& p = k.queue.front();
        const u32 size = static_cast<u32>(p.data.size());
        const u32 copied = nx::min(size, length);
        if (copied) std::memcpy(buffer, p.data.data(), copied);
        if (from) *from = p.from;
        if (!peek) k.queue.erase(k.queue.begin());
        return static_cast<i32>(copied);
    }
    hostnet::Endpoint e{};
    const i32 got = hostnet::receive(k.host->fd, buffer, length, &e, peek);
    if (got < 0) return host_error(got);
    if (from) *from = stream(k) ? k.peer : sockaddr(e);
    return got;
}

// A receive on an ad hoc socket into guest memory (`buffer` checked by the caller).
i32 receive_adhoc(i32 s, u8* buffer, u32 length, i32 flags, SockaddrIn* from) {
    Net& n = net();
    i64 timeout = 0;
    bool is_stream = false;
    {
        Held held(n.lock);
        Socket* k = socket_of(n, s);
        if (!k) return kEbadf;
        if (stream(*k) && !k->connected) return kEnotconn;
        timeout = receive_timeout(*k, flags & kMsgDontWait);
        is_stream = stream(*k);
    }
    const bool peek = flags & kMsgPeek;
    if (!is_stream || !(flags & kMsgWaitAll) || peek)
        return host_wait(s, false, timeout, [&](Socket& k) { return try_receive(n, k, buffer, length, peek, from); });
    // (all of it: until the length, the stream's end, a failure or the time-out)
    u32 total = 0;
    while (total < length) {
        const i32 r = host_wait(s, false, timeout, [&](Socket& k) { return try_receive(n, k, buffer + total, length - total, false, from); });
        if (r < 0) return total ? static_cast<i32>(total) : r;
        if (r == 0) break;
        total += static_cast<u32>(r);
    }
    return static_cast<i32>(total);
}

// A send on an ad hoc socket: to `to` (null: its peer).
i32 send_adhoc(i32 s, const u8* message, u32 length, i32 flags, const SockaddrIn* to) {
    Net& n = net();
    i64 timeout = 0;
    {
        Held held(n.lock);
        Socket* k = socket_of(n, s);
        if (!k) return kEbadf;
        if (stream(*k)) {
            if (!k->connected) return kEnotconn;
        } else if (!to && !k->connected) {
            return kEdestaddrreq;
        } else if (to && k->connected) {
            return kEisconn;
        }
        timeout = send_timeout(*k, flags & kMsgDontWait);
        if (const i32 r = autobind(n, *k); r < 0) return r;
    }
    const SockaddrIn* dest = to;
    u32 total = 0;
    const i32 r = host_wait(s, true, timeout, [&](Socket& k) -> i32 {
        if (stream(k)) {
            const i32 sent = hostnet::send(k.host->fd, message + total, length - total, nullptr);
            if (sent < 0) return host_error(sent);
            total += static_cast<u32>(sent);
            return total < length && !k.nbio && !(flags & kMsgDontWait) ? kEagain : static_cast<i32>(total);
        }
        const SockaddrIn& d = dest ? *dest : k.peer;
        hostnet::Endpoint targets[kLoopbackPeers];
        const u16 port = p2p_datagram(k) && !d.port ? kAdhocPort : host_port(d.port);
        const i32 count = destinations(n.link, host_addr(d.addr), targets, port);
        if (count < 0) return host_error(count);
        const u8* data = message;
        u32 size = length;
        if (p2p_datagram(k)) {
            n.datagram.resize(8 + length);
            u8* h = n.datagram.data();
            const u16 from_vport = vport_of(k), to_vport = host_port(d.vport);
            h[0] = 'P', h[1] = 'V', h[2] = 1, h[3] = 0;
            h[4] = static_cast<u8>(from_vport >> 8), h[5] = static_cast<u8>(from_vport);
            h[6] = static_cast<u8>(to_vport >> 8), h[7] = static_cast<u8>(to_vport);
            if (length) std::memcpy(h + 8, message, length);
            data = h, size = 8 + length;
        } else if (k.connected && !dest) {
            const i32 sent = hostnet::send(k.host->fd, data, size, nullptr);
            return sent < 0 ? host_error(sent) : static_cast<i32>(length);
        }
        i32 last = 0;
        bool any = false;
        nx::logd(logs::hle, "SceNet: {} bytes to {}:{}{} ({} host address{})", length, text(host_addr(d.addr)), port,
                 p2p_datagram(k) ? nx::format(" virtual port {}", host_port(d.vport)) : nx::string(), count,
                 count > 1 ? "es" : "");
        for (i32 i = 0; i < count; ++i) {
            const i32 sent = hostnet::send(k.host->fd, data, size, &targets[i]);
            if (sent >= 0) any = true;
            else last = sent;
        }
        // (a broadcast on the loopback link reaches whoever listens: the addresses nobody has refuse it, unseen)
        if (any || count > 1) return static_cast<i32>(length);
        return last == -hostnet::e::kAgain || last == -hostnet::e::kNoBufs ? kEagain : host_error(last);
    });
    if (r == kEagain && total) return static_cast<i32>(total);
    return r;
}

i32 sceNetConnect(i32 s, Ptr<const SockaddrIn> addr, u32 length) {
    SockaddrIn a{};
    if (const i32 r = read_address(addr, length, &a); r < 0) return result(r);
    Net& n = net();
    i64 timeout = 0;
    {
        Held held(n.lock);
        Socket* k = socket_of(n, s);
        if (!k) return result(kEbadf);
        if (!k->adhoc) {
            // (a stream reaches nobody; a datagram socket's peer would be set, but it could never send to it: no route)
            return result(loopback(a.addr) && stream(*k) ? kEconnrefused : kEnetunreach);
        }
        if (k->connected && stream(*k)) return result(kEisconn);
        hostnet::Endpoint targets[kLoopbackPeers];
        const u16 port = p2p_datagram(*k) && !a.port ? kAdhocPort : host_port(a.port);
        const i32 count = destinations(n.link, host_addr(a.addr), targets, port);
        if (count < 0) return result(host_error(count));
        if (const i32 r = autobind(n, *k); r < 0) return result(r);
        if (p2p_datagram(*k)) {  // (its port's socket is shared: the peer is only remembered)
            k->peer = a;
            k->connected = true;
            return 0;
        }
        const i32 r = hostnet::connect(k->host->fd, targets[0]);
        if (r == 0 || !stream(*k)) {
            if (r == 0) k->peer = sockaddr(targets[0]), k->connected = true;
            return result(host_error(r));
        }
        if (r != -hostnet::e::kInProgress) return result(host_error(r));
        k->peer = sockaddr(targets[0]);
        if (k->nbio) {
            k->connected = true;  // (as far as the guest can tell: a send or receive says how it went)
            return result(kEinprogress);
        }
        timeout = k->sndtimeo;
    }
    // (blocking: until the host's socket is connected, or says why not)
    const i32 r = host_wait(s, true, timeout, [&](Socket& k) -> i32 {
        const i32 status = hostnet::connect_status(k.host->fd);
        if (status == -hostnet::e::kInProgress) return kEagain;
        if (status < 0) return host_error(status);
        k.connected = true;
        return 0;
    });
    if (r == 0) nx::logi(logs::hle, "SceNet: stream {} connected to {}:{}", s, text(host_addr(a.addr)), host_port(a.port));
    return result(r == kEagain ? net_error(60) : r);  // (the time-out: ETIMEDOUT)
}

i32 sceNetListen(i32 s, i32 backlog) {
    Net& n = net();
    Held held(n.lock);
    Socket* k = socket_of(n, s);
    if (!k) return result(kEbadf);
    if (!stream(*k)) return result(net_error(45));  // (EOPNOTSUPP)
    if (k->adhoc) {
        if (const i32 r = autobind(n, *k); r < 0) return result(r);
        if (const i32 r = hostnet::listen(k->host->fd, backlog); r < 0) return result(host_error(r));
    }
    k->listening = true;
    return 0;
}

// A wait on a socket for what never arrives (its own time-out, or none when it does not block).
i32 socket_wait(i32 s, bool dont_wait) {
    i64 timeout = 0;
    {
        Net& n = net();
        Held held(n.lock);
        Socket* k = socket_of(n, s);
        if (!k) return kEbadf;
        if (k->nbio || dont_wait) return kEagain;
        if (k->rcvtimeo) timeout = k->rcvtimeo;
    }
    Net& n = net();
    const i32 r = wait_for_nothing(
        timeout, [&] { return n.sockets.contains(s) && n.sockets[s].aborted; }, [&] { return n.sockets.contains(s); });
    if (r == kEintr) {
        Held held(n.lock);
        if (Socket* k = socket_of(n, s)) k->aborted = false;
    }
    return r == 0 ? kEagain : r;
}

bool is_adhoc(i32 s) {
    Net& n = net();
    Held held(n.lock);
    const Socket* k = socket_of(n, s);
    return k && k->adhoc;
}

i32 sceNetAccept(i32 s, Ptr<SockaddrIn> addr, Ptr<u32> length) {
    i64 timeout = 0;
    {
        Net& n = net();
        Held held(n.lock);
        Socket* k = socket_of(n, s);
        if (!k) return result(kEbadf);
        if (!k->listening) return result(kEinval);
        if (!k->adhoc) timeout = -2;
        else timeout = receive_timeout(*k, false);
    }
    if (timeout == -2) return result(socket_wait(s, false));
    Net& n = net();
    i32 accepted = 0;
    SockaddrIn from{};
    const i32 r = host_wait(s, false, timeout, [&](Socket& k) -> i32 {
        hostnet::Fd fd = hostnet::kNoFd;
        hostnet::Endpoint peer{};
        if (const i32 got = hostnet::accept(k.host->fd, &fd, &peer); got < 0) return host_error(got);
        const i32 id = new_id(n);
        Socket c;
        c.type = k.type;
        c.adhoc = true;
        c.bound = c.connected = true;
        c.host = std::make_shared<HostFd>(fd);
        c.local = k.local;
        c.peer = from = sockaddr(peer);
        n.sockets[id] = std::move(c);
        accepted = id;
        return id;
    });
    if (r < 0) return result(r);
    nx::logi(logs::hle, "SceNet: stream {} accepted from {}:{}", accepted, text(host_addr(from.addr)), host_port(from.port));
    if (addr && length && writable(length.address, 4) && *length >= sizeof(SockaddrIn) &&
        writable(addr.address, sizeof(SockaddrIn))) {
        *addr = from;
        *length = sizeof(SockaddrIn);
    }
    return r;
}

i32 sceNetRecvfrom(i32 s, Ptr<u8> buffer, u32 length, i32 flags, Ptr<SockaddrIn> from, Ptr<u32> from_length) {
    if (is_adhoc(s)) {
        if (length && (!buffer || !writable(buffer.address, length))) return result(kEfault);
        SockaddrIn f{};
        const i32 r = receive_adhoc(s, length ? buffer.get() : nullptr, length, flags, &f);
        if (r >= 0 && from && from_length && writable(from_length.address, 4) && *from_length >= sizeof(SockaddrIn) &&
            writable(from.address, sizeof(SockaddrIn))) {
            *from = f;
            *from_length = sizeof(SockaddrIn);
        }
        return result(r);
    }
    {
        Net& n = net();
        Held held(n.lock);
        Socket* k = socket_of(n, s);
        if (!k) return result(kEbadf);
        if (stream(*k)) return result(kEnotconn);
    }
    return result(socket_wait(s, flags & kMsgDontWait));
}
i32 sceNetRecv(i32 s, Ptr<u8> buffer, u32 length, i32 flags) {
    return sceNetRecvfrom(s, buffer, length, flags, Ptr<SockaddrIn>(0), Ptr<u32>(0));
}

i32 sceNetSend(i32 s, Ptr<const u8> message, u32 length, i32 flags) {
    if (is_adhoc(s)) {
        if (length && (!message || !readable(message.address, length))) return result(kEfault);
        return result(send_adhoc(s, length ? message.get() : nullptr, length, flags, nullptr));
    }
    Net& n = net();
    Held held(n.lock);
    Socket* k = socket_of(n, s);
    if (!k) return result(kEbadf);
    return result(stream(*k) ? kEnotconn : kEdestaddrreq);  // (never connected)
}
i32 send_to(i32 s, Ptr<const u8> message, u32 length, i32 flags, Ptr<const SockaddrIn> to, u32 to_length) {
    if (is_adhoc(s)) {
        if (length && (!message || !readable(message.address, length))) return kEfault;
        SockaddrIn a{};
        if (to) {
            if (const i32 r = read_address(to, to_length, &a); r < 0) return r;
        }
        return send_adhoc(s, length ? message.get() : nullptr, length, flags, to ? &a : nullptr);
    }
    Net& n = net();
    Held held(n.lock);
    Socket* k = socket_of(n, s);
    if (!k) return kEbadf;
    if (stream(*k)) return kEnotconn;
    if (!to) return kEdestaddrreq;
    SockaddrIn a{};
    if (const i32 r = read_address(to, to_length, &a); r < 0) return r;
    return loopback(a.addr) ? static_cast<i32>(length) : kEnetunreach;
}
i32 sceNetSendto(i32 s, Ptr<const u8> message, u32 length, i32 flags, Ptr<const SockaddrIn> to, u32 to_length) {
    return result(send_to(s, message, length, flags, to, to_length));
}
// SceNetMsghdr: the destination, its length, the vectors (base, length pairs) and their count, control data, flags.
// (ad hoc: the vectors gathered into one datagram, or written in turn on a stream)
i32 sceNetSendmsg(i32 s, Ptr<const u32> message, i32 flags) {
    if (!message || !readable(message.address, 28)) return result(kEfault);
    const u32* m = message.get();
    nx::vector<u8> gathered;
    u32 length = 0;
    for (u32 i = 0; i < m[3] && i < 1024; ++i) {
        if (!readable(m[2] + 8 * i, 8)) continue;
        const u32 base = psv_ld32(m[2] + 8 * i), size = psv_ld32(m[2] + 8 * i + 4);
        length += size;
        if (size && readable(base, size)) {
            const usize at = gathered.size();
            gathered.resize(at + size);
            std::memcpy(gathered.data() + at, memory().host(base), size);
        }
    }
    if (is_adhoc(s)) {
        SockaddrIn a{};
        if (m[0]) {
            if (const i32 r = read_address(Ptr<const SockaddrIn>(m[0]), m[1], &a); r < 0) return result(r);
        }
        return result(send_adhoc(s, gathered.data(), static_cast<u32>(gathered.size()), flags, m[0] ? &a : nullptr));
    }
    return result(send_to(s, Ptr<const u8>(0), length, flags, Ptr<const SockaddrIn>(m[0]), m[1]));
}

i32 sceNetShutdown(i32 s, i32 how) {
    Net& n = net();
    Held held(n.lock);
    Socket* k = socket_of(n, s);
    if (!k) return result(kEbadf);
    if (how < 0 || how > 2) return result(kEinval);
    if (k->adhoc && k->connected && stream(*k)) return result(host_error(hostnet::shutdown(k->host->fd, how)));
    return result(kEnotconn);
}

i32 sceNetSetsockopt(i32 s, i32 level, i32 name, Ptr<const u8> value, u32 length) {
    Net& n = net();
    Held held(n.lock);
    Socket* k = socket_of(n, s);
    if (!k) return result(kEbadf);
    if (!value || length < 4 || !readable(value.address, 4)) return result(kEinval);
    u32 v = psv_ld32(value.address);
    // (SceNetLinger: on or off, then the seconds)
    if (level == kSolSocket && name == kSoLinger) {
        if (length < 8 || !readable(value.address, 8)) return result(kEinval);
        v = v ? psv_ld32(value.address + 4) : 0xFFFFFFFFu;
    }
    if (level == kSolSocket && name == kSoNbio) k->nbio = v != 0;
    else if (level == kSolSocket && name == kSoRcvTimeo) k->rcvtimeo = v;
    else if (level == kSolSocket && name == kSoSndTimeo) k->sndtimeo = v;
    else k->options[option_key(level, name)] = v;
    if (k->adhoc && k->host) apply_option(*k, k->host->fd, level, name, static_cast<i32>(v));
    return 0;
}
i32 sceNetGetsockopt(i32 s, i32 level, i32 name, Ptr<u8> value, Ptr<u32> length) {
    Net& n = net();
    Held held(n.lock);
    Socket* k = socket_of(n, s);
    if (!k) return result(kEbadf);
    if (!value || !length || !writable(length.address, 4) || *length < 4 || !writable(value.address, 4))
        return result(kEinval);
    u32 v = 0;
    if (level == kSolSocket && name == kSoNbio) v = k->nbio;
    else if (level == kSolSocket && name == kSoRcvTimeo) v = k->rcvtimeo;
    else if (level == kSolSocket && name == kSoSndTimeo) v = k->sndtimeo;
    else if (level == kSolSocket && name == kSoError) v = k->adhoc && k->host ? static_cast<u32>(hostnet::pending_error(k->host->fd)) : 0;
    else if (level == kSolSocket && name == kSoType) v = static_cast<u32>(k->type);
    else if (auto it = k->options.find(option_key(level, name)); it != k->options.end()) v = it->second;
    else if (k->adhoc && k->host && level == kSolSocket && (name == kSoRcvBuf || name == kSoSndBuf)) {
        i32 host_value = 0;
        if (hostnet::get_option(k->host->fd, name == kSoRcvBuf ? hostnet::Option::ReceiveBuffer : hostnet::Option::SendBuffer,
                                &host_value) == 0)
            v = static_cast<u32>(host_value);
    }
    if (level == kSolSocket && name == kSoLinger) {
        if (*length < 8 || !writable(value.address, 8)) return result(kEinval);
        auto it = k->options.find(option_key(level, name));
        const bool on = it != k->options.end() && it->second != 0xFFFFFFFFu;
        psv_st32(value.address, on ? 1 : 0);
        psv_st32(value.address + 4, on ? it->second : 0);
        *length = 8;
        return 0;
    }
    psv_st32(value.address, v);
    *length = 4;
    return 0;
}
i32 sceNetGetsockname(i32 s, Ptr<SockaddrIn> addr, Ptr<u32> length) {
    Net& n = net();
    Held held(n.lock);
    Socket* k = socket_of(n, s);
    if (!k) return result(kEbadf);
    if (!addr || !length || !writable(length.address, 4) || *length < sizeof(SockaddrIn) ||
        !writable(addr.address, sizeof(SockaddrIn)))
        return result(kEinval);
    SockaddrIn local = k->local;
    // (an ad hoc socket bound by the host (it connected or sent unbound): the port it chose)
    if (k->adhoc && k->host && !p2p_datagram(*k)) {
        hostnet::Endpoint e{};
        if (hostnet::local_name(k->host->fd, &e) == 0) local.port = __builtin_bswap16(e.port);
    }
    *addr = local;
    *length = sizeof(SockaddrIn);
    return 0;
}
i32 sceNetGetpeername(i32 s, Ptr<SockaddrIn> addr, Ptr<u32> length) {
    Net& n = net();
    Held held(n.lock);
    Socket* k = socket_of(n, s);
    if (!k) return result(kEbadf);
    if (!k->adhoc || !k->connected) return result(kEnotconn);
    if (!addr || !length || !writable(length.address, 4) || *length < sizeof(SockaddrIn) ||
        !writable(addr.address, sizeof(SockaddrIn)))
        return result(kEinval);
    *addr = k->peer;
    *length = sizeof(SockaddrIn);
    return 0;
}
// (SceNetSockInfo records of the sockets asked about: none is reported)
i32 sceNetGetSockInfo(i32 /*s*/, Ptr<u8> /*info*/, i32 /*count*/, i32 /*flags*/) { return 0; }

// ---- epoll

i32 sceNetEpollCreate(Ptr<const char> /*name*/, i32 /*flags*/) {
    Net& n = net();
    Held held(n.lock);
    if (!n.initialized) return kEnotinit;
    const i32 id = new_id(n);
    n.epolls[id] = Epoll{};
    return id;
}
i32 sceNetEpollDestroy(i32 e) {
    Net& n = net();
    Held held(n.lock);
    return result(n.epolls.erase(e) ? 0 : kEbadf);
}
i32 sceNetEpollAbort(i32 e, i32 /*flags*/) {
    Net& n = net();
    Held held(n.lock);
    auto it = n.epolls.find(e);
    if (it == n.epolls.end()) return result(kEbadf);
    it->second.aborted = true;
    return 0;
}
i32 sceNetEpollControl(i32 e, i32 op, i32 id, Ptr<const EpollEvent> event) {
    Net& n = net();
    Held held(n.lock);
    auto it = n.epolls.find(e);
    if (it == n.epolls.end()) return result(kEbadf);
    if (!n.sockets.contains(id) && !n.resolvers.contains(id)) return result(kEbadf);
    auto& list = it->second.registered;
    auto at = std::find_if(list.begin(), list.end(), [&](const Registered& r) { return r.id == id; });
    if (op == kEpollCtlDel) {
        if (at == list.end()) return result(net_error(2));  // (ENOENT)
        list.erase(at);
        return 0;
    }
    if (op != kEpollCtlAdd && op != kEpollCtlMod) return result(kEinval);
    if (!event || !readable(event.address, sizeof(EpollEvent))) return result(kEinval);
    if (op == kEpollCtlAdd && at != list.end()) return result(net_error(17));  // (EEXIST)
    if (op == kEpollCtlMod && at == list.end()) return result(net_error(2));
    const Registered r{id, event->events, event->data};
    if (at == list.end()) list.push_back(r);
    else *at = r;
    return 0;
}
// What is ready. Offline: a datagram socket can always be written to (asked for with EPOLLOUT); nothing ever arrives.
// Ad hoc: as the host's sockets are (a P2P socket: readable when a datagram waits for it); the descriptors to wait on
// for the rest go to `waits` (each once).
i32 ready_events(Net& n, i32 e, EpollEvent* out, i32 max, nx::vector<HostRef>* waits) {
    i32 count = 0;
    nx::vector<hostnet::PollItem> items;
    nx::vector<const Registered*> polled;
    auto add = [&](const Registered& r, u32 events) {
        if (!events || count >= max) return;
        EpollEvent& ev = out[count++];
        std::memset(&ev, 0, sizeof ev);
        ev.events = events;
        ev.data = r.data;
    };
    for (const Registered& r : n.epolls[e].registered) {
        auto s = n.sockets.find(r.id);
        if (s == n.sockets.end()) continue;
        Socket& k = s->second;
        if (!k.adhoc) {
            if (!stream(k) && (r.events & kEpollOut)) add(r, kEpollOut);
            continue;
        }
        if (p2p_datagram(k)) {
            if (k.bound) pump_p2p(n, port_of(k));
            add(r, ((r.events & kEpollIn) && !k.queue.empty() ? kEpollIn : 0) | (r.events & kEpollOut));
            if (k.bound && k.host && waits) waits->push_back(k.host);
            continue;
        }
        hostnet::PollItem item;
        item.fd = k.host->fd;
        item.want_read = (r.events & kEpollIn) != 0;
        item.want_write = (r.events & kEpollOut) != 0;
        items.push_back(item);
        polled.push_back(&r);
    }
    if (!items.empty()) {
        hostnet::poll(items.data(), static_cast<u32>(items.size()), 0);
        for (usize i = 0; i < items.size(); ++i) {
            const hostnet::PollItem& it = items[i];
            add(*polled[i], (it.readable && it.want_read ? kEpollIn : 0) | (it.writable && it.want_write ? kEpollOut : 0) |
                                (it.error ? kEpollErr : 0) | (it.hangup ? kEpollHup : 0));
            if (waits) waits->push_back(n.sockets[polled[i]->id].host);
        }
    }
    return count;
}
i32 epoll_wait(i32 e, Ptr<EpollEvent> events, i32 max, i32 timeout) {
    Net& n = net();
    bool adhoc = false;
    {
        Held held(n.lock);
        if (!n.epolls.contains(e)) return result(kEbadf);
        if (max <= 0 || !events || !writable(events.address, sizeof(EpollEvent) * static_cast<u32>(max)))
            return result(kEinval);
        if (const i32 ready = ready_events(n, e, events.get(), max, nullptr)) return ready;
        for (const Registered& r : n.epolls[e].registered)
            if (auto s = n.sockets.find(r.id); s != n.sockets.end() && s->second.adhoc) adhoc = true;
    }
    if (!adhoc) {
        const i32 r = wait_for_nothing(timeout < 0 ? 0 : timeout == 0 ? -1 : timeout,
                                       [&] { return n.epolls.contains(e) && n.epolls[e].aborted; },
                                       [&] { return n.epolls.contains(e); });
        if (r == kEintr) {
            Held held(n.lock);
            if (n.epolls.contains(e)) n.epolls[e].aborted = false;
        }
        return result(r);
    }
    // (ad hoc: the host's descriptors polled together, in slices, until something is ready)
    if (timeout == 0) return 0;
    const u64 start = kernel().clock_ns();
    while (true) {
        kernel().checkpoint();
        nx::vector<HostRef> waits;
        bool p2p = false;
        {
            Held held(n.lock);
            if (!n.epolls.contains(e)) return result(kEbadf);
            if (n.epolls[e].aborted) {
                n.epolls[e].aborted = false;
                return result(kEintr);
            }
            if (const i32 ready = ready_events(n, e, events.get(), max, &waits)) return ready;
            for (const Registered& r : n.epolls[e].registered)
                if (auto s = n.sockets.find(r.id); s != n.sockets.end() && p2p_datagram(s->second)) p2p = true;
        }
        i32 slice = p2p ? kP2pSliceMs : kSliceMs;
        if (timeout > 0) {
            const u64 waited = (kernel().clock_ns() - start) / 1000;
            if (waited >= static_cast<u64>(timeout)) return 0;
            slice = static_cast<i32>(nx::min<u64>(static_cast<u64>(slice), (static_cast<u64>(timeout) - waited + 999) / 1000));
        }
        nx::vector<hostnet::PollItem> items;
        for (const HostRef& h : waits) {
            bool seen = false;
            for (const hostnet::PollItem& it : items) seen |= it.fd == h->fd;
            if (seen) continue;
            hostnet::PollItem item;
            item.fd = h->fd;
            item.want_read = true;
            items.push_back(item);
        }
        if (items.empty()) {
            kernel().delay_thread(static_cast<u32>(slice) * 1000, false);
            continue;
        }
        Kernel::Blocking blocking(kernel());
        hostnet::poll(items.data(), static_cast<u32>(items.size()), slice);
    }
}
i32 sceNetEpollWait(i32 e, Ptr<EpollEvent> events, i32 max, i32 timeout) { return epoll_wait(e, events, max, timeout); }

// ---- the resolver: no DNS server; numbers and "localhost" need none

i32 sceNetResolverCreate(Ptr<const char> /*name*/, Ptr<const void> /*param*/, i32 /*flags*/) {
    Net& n = net();
    Held held(n.lock);
    if (!n.initialized) return kEnotinit;
    const i32 id = new_id(n);
    n.resolvers[id] = Resolver{};
    return id;
}
i32 sceNetResolverDestroy(i32 r) {
    Net& n = net();
    Held held(n.lock);
    return result(n.resolvers.erase(r) ? 0 : kEbadf);
}
i32 sceNetResolverAbort(i32 r, i32 /*flags*/) {
    Net& n = net();
    Held held(n.lock);
    auto it = n.resolvers.find(r);
    if (it == n.resolvers.end()) return result(kEbadf);
    it->second.aborted = true;
    return 0;
}
i32 sceNetInetPton(i32 af, Ptr<const char> text, Ptr<u8> out);
i32 resolver_done(i32 r, i32 error) {
    Net& n = net();
    Held held(n.lock);
    auto it = n.resolvers.find(r);
    if (it == n.resolvers.end()) return kEbadf;
    it->second.error = error;
    return error;
}
i32 sceNetResolverStartNtoa(i32 r, Ptr<const char> host, Ptr<u32> addr, i32 /*timeout*/, i32 /*retry*/, i32 /*flags*/) {
    {
        Net& n = net();
        Held held(n.lock);
        if (!n.resolvers.contains(r)) return result(kEbadf);
    }
    if (!host || !addr || !writable(addr.address, 4)) return result(resolver_done(r, kEinval));
    const nx::string name = str(host, 256);
    if (name == "localhost") {
        *addr = 0x0100007Fu;  // (127.0.0.1, network order)
        return resolver_done(r, 0);
    }
    if (sceNetInetPton(kAfInet, host, Ptr<u8>(addr.address)) == 1) return resolver_done(r, 0);
    nx::logd(logs::hle, "SceNet: '{}' not resolved (no DNS server: offline)", name);
    return result(resolver_done(r, kResolverEnodns));
}
i32 sceNetResolverStartAton(i32 r, Ptr<const u32> /*addr*/, Ptr<char> /*host*/, i32 /*length*/, i32 /*timeout*/,
                            i32 /*retry*/, i32 /*flags*/) {
    return result(resolver_done(r, kResolverEnodns));
}
i32 sceNetResolverGetError(i32 r, Ptr<i32> error) {
    Net& n = net();
    Held held(n.lock);
    auto it = n.resolvers.find(r);
    if (it == n.resolvers.end()) return result(kEbadf);
    if (!error || !writable(error.address, 4)) return result(kEinval);
    *error = it->second.error;
    return 0;
}

// ---- information: the stack's own

i32 sceNetShowIfconfig() { return 0; }
i32 sceNetShowRoute() { return 0; }
i32 sceNetShowNetstat() { return 0; }
// (the wireless interface's address: one of the host's own making, locally administered; in ad hoc mode made from
// the interface's IPv4 address, so each instance has its own)
bool choose_link(Link* out, nx::string* why);
i32 sceNetGetMacAddress(Ptr<u8> out, i32 /*flags*/) {
    if (!out || !writable(out.address, 6)) return result(kEinval);
    u8 mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    Link l;
    nx::string why;
    if (adhoc_enabled() && choose_link(&l, &why))
        for (u32 i = 0; i < 4; ++i) mac[2 + i] = static_cast<u8>(l.addr >> (24 - 8 * i));
    std::memcpy(out.get(), mac, 6);
    return 0;
}
// SceNetStatisticsInfo: the kernel's free memory (least ever), packets queued, the library's free memory (least ever).
i32 sceNetGetStatisticsInfo(Ptr<i32> out, i32 /*flags*/) {
    if (!out || !writable(out.address, 24)) return result(kEinval);
    Net& n = net();
    Held held(n.lock);
    const i32 info[6] = {0x100000, 0x100000, 0, 0, n.memory_size, n.memory_size};
    std::memcpy(out.get(), info, sizeof info);
    return 0;
}

// Byte order (the guest's is little-endian, the network's big-endian) and addresses as text.
// The rest of SceNet:
// - sceNetRecvmsg: a receive into the message's vectors (none ever arrives offline: as sceNetRecv).
// - sceNetEpollWaitCB: sceNetEpollWait (callbacks are not run while it waits).
// - DNS: the cache cleared and servers set, to no effect (nothing is resolved but numbers and the loopback name).
// - Packet dumps (a debugging capture): made and destroyed; reading one gives nothing, aborting ends a read.
// - sceNetGetSockIdInfo: the socket IDs in use, as bits of an fd set (1024 bits).
// - sceNetEtherNtostr/Strton: a MAC address to "xx:xx:xx:xx:xx:xx" and back.
i32 sceNetRecvmsg(i32 s, Ptr<const u32> message, i32 flags) {
    if (!message || !readable(message.address, 28)) return result(kEfault);
    if (!is_adhoc(s)) return sceNetRecv(s, Ptr<u8>(0), 0, flags);
    // (ad hoc: received whole, then spread over the vectors; the sender's address to the name, if asked)
    const u32 m = message.address;
    const u32 iov = psv_ld32(m + 8), count = psv_ld32(m + 12);
    u32 length = 0;
    for (u32 i = 0; i < count && i < 1024; ++i)
        if (readable(iov + 8 * i, 8)) length += psv_ld32(iov + 8 * i + 4);
    nx::vector<u8> buffer(length ? length : 1);
    SockaddrIn from{};
    const i32 r = receive_adhoc(s, buffer.data(), length, flags, &from);
    if (r < 0) return result(r);
    u32 done = 0;
    for (u32 i = 0; i < count && i < 1024 && done < static_cast<u32>(r); ++i) {
        if (!readable(iov + 8 * i, 8)) continue;
        const u32 base = psv_ld32(iov + 8 * i), size = nx::min(psv_ld32(iov + 8 * i + 4), static_cast<u32>(r) - done);
        if (size && writable(base, size)) std::memcpy(memory().host(base), buffer.data() + done, size);
        done += size;
    }
    if (const u32 name = psv_ld32(m); name && psv_ld32(m + 4) >= sizeof(SockaddrIn) && writable(name, sizeof(SockaddrIn))) {
        std::memcpy(memory().host(name), &from, sizeof from);
        psv_st32(m + 4, sizeof(SockaddrIn));
    }
    return r;
}
i32 sceNetEpollWaitCB(i32 e, Ptr<EpollEvent> events, i32 max, i32 timeout) { return epoll_wait(e, events, max, timeout); }
i32 sceNetClearDnsCache(i32) { return 0; }
i32 sceNetSetDnsInfo(Ptr<const u8> info, i32) { return info ? 0 : result(kEinval); }
i32 sceNetDumpCreate(Ptr<const char> /*name*/, i32 length, i32) {
    if (length < 0) return result(kEinval);
    Net& n = net();
    Held held(n.lock);
    return new_id(n);
}
i32 sceNetDumpRead(i32 id, Ptr<u8>, i32 length, Ptr<i32> flags) {
    if (id <= 0 || length < 0) return result(kEinval);
    if (flags && writable(flags.address, 4)) *flags = 0;
    return 0;
}
i32 sceNetDumpAbort(i32 id, i32) { return id > 0 ? 0 : result(kEinval); }
i32 sceNetDumpDestroy(i32 id) { return id > 0 ? 0 : result(kEinval); }
i32 sceNetGetSockIdInfo(Ptr<u8> fds, i32, i32) {
    if (!fds || !writable(fds.address, 128)) return result(kEinval);
    std::memset(fds.get(), 0, 128);
    Net& n = net();
    Held held(n.lock);
    for (const auto& [id, socket] : n.sockets)
        if (id >= 0 && id < 1024) fds.get()[id / 8] |= static_cast<u8>(1u << (id % 8));
    return 0;
}
i32 sceNetEtherNtostr(Ptr<const u8> mac, Ptr<char> out, u32 length) {
    if (!mac || !out || length < 18 || !readable(mac.address, 6) || !writable(out.address, 18)) return result(kEinval);
    const u8* m = mac.get();
    std::snprintf(out.get(), 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return 0;
}
i32 sceNetEtherStrton(Ptr<const char> text, Ptr<u8> mac) {
    if (!text || !mac || !readable(text.address, 17) || !writable(mac.address, 6)) return result(kEinval);
    u8 out[6];
    const char* t = text.get();
    for (u32 i = 0; i < 6; ++i) {
        u32 v = 0;
        for (u32 k = 0; k < 2; ++k) {
            const char c = t[3 * i + k];
            const u32 d = c >= '0' && c <= '9' ? static_cast<u32>(c - '0')
                          : c >= 'a' && c <= 'f' ? static_cast<u32>(c - 'a' + 10)
                          : c >= 'A' && c <= 'F' ? static_cast<u32>(c - 'A' + 10) : 99;
            if (d > 15) return result(kEinval);
            v = v * 16 + d;
        }
        if (i < 5 && t[3 * i + 2] != ':' && t[3 * i + 2] != '-') return result(kEinval);
        out[i] = static_cast<u8>(v);
    }
    std::memcpy(mac.get(), out, 6);
    return 0;
}

u32 sceNetHtonl(u32 v) { return __builtin_bswap32(v); }
u32 sceNetNtohl(u32 v) { return __builtin_bswap32(v); }
u32 sceNetHtons(u32 v) { return __builtin_bswap16(static_cast<u16>(v)); }
u32 sceNetNtohs(u32 v) { return __builtin_bswap16(static_cast<u16>(v)); }
u64 sceNetHtonll(u64 v) { return __builtin_bswap64(v); }
u64 sceNetNtohll(u64 v) { return __builtin_bswap64(v); }
// IPv4 only: "a.b.c.d" into 4 bytes in network order; 1 if parsed, 0 if not.
i32 sceNetInetPton(i32 af, Ptr<const char> text, Ptr<u8> out) {
    if (af != kAfInet) return kEafnosupport;
    if (!text || !out || !writable(out.address, 4)) return err::kFault;
    const nx::string t = str(text, 16);
    u32 part[4] = {}, n = 0, digits = 0;
    for (const char c : t) {
        if (c >= '0' && c <= '9') {
            part[n] = part[n] * 10 + static_cast<u32>(c - '0');
            if (++digits > 3 || part[n] > 255) return 0;
        } else if (c == '.' && digits && n < 3) {
            ++n, digits = 0;
        } else {
            return 0;
        }
    }
    if (n != 3 || !digits) return 0;
    for (u32 k = 0; k < 4; ++k) out.get()[k] = static_cast<u8>(part[k]);
    return 1;
}
u32 sceNetInetNtop(i32 af, Ptr<const u8> address, Ptr<char> out, u32 size) {
    if (af != kAfInet || !address || !out) return 0;
    const u8* a = address.get();
    const nx::string t = nx::format("{}.{}.{}.{}", a[0], a[1], a[2], a[3]);
    if (size < t.size() + 1 || !writable(out.address, size)) return 0;
    std::memcpy(out.get(), t.c_str(), t.size() + 1);
    return out.address;
}

const Function kNet[] = {
    HLE_FUNCTION(0xEB03E265, sceNetInit),
    HLE_FUNCTION(0xEA3CC286, sceNetTerm),
    HLE_FUNCTION(0xE37F34AA, sceNetErrnoLoc),
    HLE_FUNCTION(0xF084FCE3, sceNetSocket),
    HLE_FUNCTION(0x29822B4D, sceNetSocketClose),
    HLE_FUNCTION(0x891C1B9B, sceNetSocketAbort),
    HLE_FUNCTION(0x1296A94B, sceNetBind),
    HLE_FUNCTION(0x11E5B6F6, sceNetConnect),
    HLE_FUNCTION(0x7A8DA094, sceNetListen),
    HLE_FUNCTION(0x1ADF9BB1, sceNetAccept),
    HLE_FUNCTION(0x023643B7, sceNetRecv),
    HLE_FUNCTION(0xB226138B, sceNetRecvfrom),
    HLE_FUNCTION(0xE3DD8CD9, sceNetSend),
    HLE_FUNCTION(0x52DB31D5, sceNetSendto),
    HLE_FUNCTION(0x99C579AE, sceNetSendmsg),
    HLE_FUNCTION(0x69E50BB5, sceNetShutdown),
    HLE_FUNCTION(0x065505CA, sceNetSetsockopt),
    HLE_FUNCTION(0xBA652062, sceNetGetsockopt),
    HLE_FUNCTION(0x1C66A6DB, sceNetGetsockname),
    HLE_FUNCTION(0x2348D353, sceNetGetpeername),
    HLE_FUNCTION(0xB1AF6840, sceNetGetSockInfo),
    HLE_FUNCTION(0xF9D102AE, sceNetEpollCreate),
    HLE_FUNCTION(0x7915CAF3, sceNetEpollDestroy),
    HLE_FUNCTION(0x93FCC4E8, sceNetEpollAbort),
    HLE_FUNCTION(0x4C8764AC, sceNetEpollControl),
    HLE_FUNCTION(0x45CE337D, sceNetEpollWait),
    HLE_FUNCTION(0x6DA29319, sceNetResolverCreate),
    HLE_FUNCTION(0x3559F098, sceNetResolverDestroy),
    HLE_FUNCTION(0x38EBBD57, sceNetResolverAbort),
    HLE_FUNCTION(0x1EB11857, sceNetResolverStartNtoa),
    HLE_FUNCTION(0x0424AE26, sceNetResolverStartAton),
    HLE_FUNCTION(0x874EF500, sceNetResolverGetError),
    HLE_FUNCTION(0x658B903B, sceNetShowIfconfig),
    HLE_FUNCTION(0x6AB3B74B, sceNetShowRoute),
    HLE_FUNCTION(0x338EDC2E, sceNetShowNetstat),
    HLE_FUNCTION(0x06C05518, sceNetGetMacAddress),
    HLE_FUNCTION(0xA86F8FE5, sceNetGetStatisticsInfo),
    HLE_FUNCTION(0x4C30B03C, sceNetHtonl),
    HLE_FUNCTION(0xD2EAA645, sceNetNtohl),
    HLE_FUNCTION(0x9FA3207B, sceNetHtons),
    HLE_FUNCTION(0x07845128, sceNetNtohs),
    HLE_FUNCTION(0x12C19209, sceNetHtonll),
    HLE_FUNCTION(0xFB3336A6, sceNetNtohll),
    HLE_FUNCTION(0xD5EEB048, sceNetInetPton),
    HLE_FUNCTION(0x98839B74, sceNetInetNtop),
    HLE_FUNCTION(0xDE94C6FE, sceNetRecvmsg),
    HLE_FUNCTION(0x92D3E767, sceNetEpollWaitCB),
    HLE_FUNCTION(0xFEC1166D, sceNetClearDnsCache),
    HLE_FUNCTION(0xD62EF218, sceNetSetDnsInfo),
    HLE_FUNCTION(0xAFF9FA4D, sceNetDumpCreate),
    HLE_FUNCTION(0x04042925, sceNetDumpRead),
    HLE_FUNCTION(0x3B24E75F, sceNetDumpAbort),
    HLE_FUNCTION(0x82DDCF63, sceNetDumpDestroy),
    HLE_FUNCTION(0x138CF1D6, sceNetGetSockIdInfo),
    HLE_FUNCTION(0x84334EB2, sceNetEtherNtostr),
    HLE_FUNCTION(0xEEC6D75F, sceNetEtherStrton),
};

// ---- RUDP (SceLibRudp): reliable sessions over the game's own UDP sockets. Contexts and polls are made, bound and
// given back; a session started never comes up (nothing answers), so nothing is ever readable or writable, and no
// event handler is ever called.

constexpr u32 kSceLibRudp = 0x32910D11;
constexpr i32 rudp_error(u32 n) { return err::code(0x80770000u + n); }
constexpr i32 kRudpNotInitialized = rudp_error(1), kRudpAlreadyInitialized = rudp_error(2),
              kRudpInvalidContext = rudp_error(3), kRudpInvalidArgument = rudp_error(4),
              kRudpNotAcceptable = rudp_error(17), kRudpCancelled = rudp_error(20), kRudpWouldBlock = rudp_error(22),
              kRudpInvalidPoll = rudp_error(31);

struct Rudp {
    bool initialized = false;
    i32 next_id = 1;
    nx::hash_map<i32, bool> contexts, polls;  // (a poll: whether it was cancelled)
};
Rudp& rudp() {
    static Rudp r;
    return r;
}
i32 rudp_ready() { return rudp().initialized ? 0 : kRudpNotInitialized; }
i32 rudp_context(i32 ctx) {
    if (!rudp().initialized) return kRudpNotInitialized;
    return rudp().contexts.contains(ctx) ? 0 : kRudpInvalidContext;
}

i32 sceRudpInit(Ptr<void> /*pool*/, i32 /*size*/) {
    Held held(net().lock);
    if (rudp().initialized) return kRudpAlreadyInitialized;
    rudp().initialized = true;
    return 0;
}
i32 sceRudpEnd() {
    Held held(net().lock);
    if (!rudp().initialized) return kRudpNotInitialized;
    rudp() = Rudp{};
    return 0;
}
i32 sceRudpEnableInternalIOThread(u32 /*stack*/, u32 /*priority*/) {
    Held held(net().lock);
    return rudp_ready();
}
i32 sceRudpSetEventHandler(u32 /*handler*/, u32 /*arg*/) {
    Held held(net().lock);
    return rudp_ready();
}
i32 sceRudpProcessEvents(u64 /*timeout*/) {
    Held held(net().lock);
    return rudp_ready();  // (no events)
}
i32 sceRudpCreateContext(u32 /*handler*/, u32 /*arg*/, Ptr<i32> id) {
    Held held(net().lock);
    if (const i32 r = rudp_ready(); r < 0) return r;
    if (!id || !writable(id.address, 4)) return kRudpInvalidArgument;
    Rudp& r = rudp();
    *id = r.next_id++;
    r.contexts[*id] = true;
    return 0;
}
i32 sceRudpTerminate(i32 ctx) {
    Held held(net().lock);
    if (const i32 r = rudp_context(ctx); r < 0) return r;
    rudp().contexts.erase(ctx);
    return 0;
}
i32 sceRudpBind(i32 ctx, i32 /*socket*/, u32 /*vport*/, u32 /*mux*/) {
    Held held(net().lock);
    return rudp_context(ctx);
}
// (a session asked for: it never comes up)
i32 sceRudpActivate(i32 ctx, Ptr<const SockaddrIn> to, u32 /*length*/) {
    Held held(net().lock);
    if (const i32 r = rudp_context(ctx); r < 0) return r;
    return to ? 0 : kRudpInvalidArgument;
}
i32 sceRudpInitiate(i32 ctx, Ptr<const SockaddrIn> to, u32 length, u32 /*vport*/) { return sceRudpActivate(ctx, to, length); }
i32 sceRudpRead(i32 ctx, Ptr<u8>, u32, u32, Ptr<void>) {
    Held held(net().lock);
    if (const i32 r = rudp_context(ctx); r < 0) return r;
    return kRudpWouldBlock;
}
i32 sceRudpWrite(i32 ctx, Ptr<const u8>, u32, u32) {
    Held held(net().lock);
    if (const i32 r = rudp_context(ctx); r < 0) return r;
    return kRudpNotAcceptable;  // (no session)
}
i32 sceRudpGetSizeReadable(i32 ctx) {
    Held held(net().lock);
    const i32 r = rudp_context(ctx);
    return r < 0 ? r : 0;
}
i32 sceRudpGetSizeWritable(i32 ctx) { return sceRudpGetSizeReadable(ctx); }
i32 sceRudpFlush(i32 ctx) {
    Held held(net().lock);
    return rudp_context(ctx);
}
// (a packet the game received for RUDP: none arrives offline; one given anyway is dropped)
i32 sceRudpNetReceived(i32 /*socket*/, Ptr<const u8>, u32, Ptr<const SockaddrIn>, u32) {
    Held held(net().lock);
    return rudp_ready();
}
i32 sceRudpSetOption(i32 ctx, i32 /*option*/, Ptr<const void> value, u32 /*length*/) {
    Held held(net().lock);
    if (const i32 r = rudp_context(ctx); r < 0) return r;
    return value ? 0 : kRudpInvalidArgument;
}
i32 zero_out(Ptr<u8> out, u32 length) {
    if (!out || !writable(out.address, length)) return kRudpInvalidArgument;
    std::memset(out.get(), 0, length);
    return 0;
}
i32 sceRudpGetOption(i32 ctx, i32 /*option*/, Ptr<u8> value, u32 length) {
    Held held(net().lock);
    if (const i32 r = rudp_context(ctx); r < 0) return r;
    return zero_out(value, length);
}
// (a context's state and counts, and the library's: all zero, the state "idle")
i32 sceRudpGetContextStatus(i32 ctx, Ptr<u8> status, u32 size) {
    Held held(net().lock);
    if (const i32 r = rudp_context(ctx); r < 0) return r;
    return zero_out(status, size);
}
i32 sceRudpGetStatus(Ptr<u8> status, u32 size) {
    Held held(net().lock);
    if (const i32 r = rudp_ready(); r < 0) return r;
    return zero_out(status, size);
}
i32 sceRudpPollCreate(u32 /*size*/) {
    Held held(net().lock);
    if (const i32 r = rudp_ready(); r < 0) return r;
    Rudp& r = rudp();
    const i32 id = r.next_id++;
    r.polls[id] = false;
    return id;
}
i32 sceRudpPollDestroy(i32 poll) {
    Held held(net().lock);
    if (const i32 r = rudp_ready(); r < 0) return r;
    return rudp().polls.erase(poll) ? 0 : kRudpInvalidPoll;
}
i32 sceRudpPollControl(i32 poll, i32 /*op*/, i32 ctx, u32 /*events*/) {
    Held held(net().lock);
    if (const i32 r = rudp_context(ctx); r < 0) return r;
    return rudp().polls.contains(poll) ? 0 : kRudpInvalidPoll;
}
// (nothing becomes readable or writable: the time-out passes (µs; 0: none, so it returns at once), or the poll is
// destroyed)
i32 sceRudpPollWait(i32 poll, Ptr<void>, u32, u64 timeout) {
    {
        Held held(net().lock);
        if (const i32 r = rudp_ready(); r < 0) return r;
        if (!rudp().polls.contains(poll)) return kRudpInvalidPoll;
    }
    const i64 t = timeout == 0 ? -1 : timeout > 0x7FFFFFFFull ? 0 : static_cast<i64>(timeout);
    const i32 r = wait_for_nothing(
        t, [&] { return rudp().polls.contains(poll) && rudp().polls[poll]; }, [&] { return rudp().polls.contains(poll); });
    Held held(net().lock);
    if (rudp().polls.contains(poll)) rudp().polls[poll] = false;
    return r == kEbadf ? kRudpInvalidPoll : r == kEintr ? kRudpCancelled : 0;
}

// The rest of RUDP: the segment size kept (a library-wide setting); a context's ends are unknown until it connects (none
// does offline: INVALID_CONTEXT unless it exists, then nothing to read); polls cancelled; the I/O thread's second form.
u16 g_rudp_mss = 1400;
i32 sceRudpGetMaxSegmentSize(Ptr<u16> mss) {
    if (!mss || !writable(mss.address, 2)) return kRudpInvalidArgument;
    *mss = g_rudp_mss;
    return 0;
}
i32 sceRudpSetMaxSegmentSize(u32 mss) {
    if ((mss & 0xFFFF) == 0) return kRudpInvalidArgument;
    g_rudp_mss = static_cast<u16>(mss);
    return 0;
}
i32 sceRudpEnableInternalIOThread2(u32 stack, u32 priority, u64) { return sceRudpEnableInternalIOThread(stack, priority); }
i32 sceRudpGetNumberOfPacketsToRead(i32 ctx) {
    Held held(net().lock);
    if (const i32 r = rudp_context(ctx); r < 0) return r;
    return 0;
}
i32 rudp_not_connected(i32 ctx) {
    Held held(net().lock);
    if (const i32 r = rudp_context(ctx); r < 0) return r;
    return kRudpNotAcceptable;
}
i32 sceRudpGetLocalInfo(i32 ctx, u32, u32, u32, u32, u32) { return rudp_not_connected(ctx); }
i32 sceRudpGetRemoteInfo(i32 ctx, u32, u32, u32) { return rudp_not_connected(ctx); }
i32 sceRudpPollCancel(i32 poll) {
    Held held(net().lock);
    if (!rudp().initialized) return kRudpNotInitialized;
    auto it = rudp().polls.find(poll);
    if (it == rudp().polls.end()) return kRudpInvalidPoll;
    it->second = true;
    return 0;
}

const Function kRudp[] = {
    HLE_FUNCTION(0x22C9AA29, sceRudpInit),
    HLE_FUNCTION(0x3EA0961E, sceRudpEnd),
    HLE_FUNCTION(0x3A495E69, sceRudpEnableInternalIOThread),
    HLE_FUNCTION(0x396A24D6, sceRudpSetEventHandler),
    HLE_FUNCTION(0x24E36E39, sceRudpProcessEvents),
    HLE_FUNCTION(0x772C07E8, sceRudpCreateContext),
    HLE_FUNCTION(0x552B2D3E, sceRudpTerminate),
    HLE_FUNCTION(0x8B766108, sceRudpBind),
    HLE_FUNCTION(0xF1151D91, sceRudpActivate),
    HLE_FUNCTION(0x46B635BD, sceRudpInitiate),
    HLE_FUNCTION(0x30A9438E, sceRudpRead),
    HLE_FUNCTION(0x69C9DE15, sceRudpWrite),
    HLE_FUNCTION(0x6B74CA22, sceRudpGetSizeReadable),
    HLE_FUNCTION(0xADE196DC, sceRudpGetSizeWritable),
    HLE_FUNCTION(0x8D45BA52, sceRudpFlush),
    HLE_FUNCTION(0x2C1E736E, sceRudpNetReceived),
    HLE_FUNCTION(0xBD87A5CA, sceRudpSetOption),
    HLE_FUNCTION(0xE68005E3, sceRudpGetOption),
    HLE_FUNCTION(0xD620B557, sceRudpGetContextStatus),
    HLE_FUNCTION(0xD77928C5, sceRudpGetStatus),
    HLE_FUNCTION(0x943D96EC, sceRudpPollCreate),
    HLE_FUNCTION(0xC9ACBDF0, sceRudpPollDestroy),
    HLE_FUNCTION(0x18668BD8, sceRudpPollControl),
    HLE_FUNCTION(0x55918C7C, sceRudpPollWait),
    HLE_FUNCTION(0x68A52861, sceRudpGetMaxSegmentSize),
    HLE_FUNCTION(0xE1DD9645, sceRudpSetMaxSegmentSize),
    HLE_FUNCTION(0x87EC465E, sceRudpEnableInternalIOThread2),
    HLE_FUNCTION(0x5D0460CD, sceRudpGetNumberOfPacketsToRead),
    HLE_FUNCTION(0x4DC43692, sceRudpGetLocalInfo),
    HLE_FUNCTION(0x104E4D3B, sceRudpGetRemoteInfo),
    HLE_FUNCTION(0xA9B92EFE, sceRudpPollCancel),
};

// ---- Ad hoc matching (SceNetAdhocMatching) and the PSP-style ad hoc control (ScePspnetAdhoc): no other console is
// near, so matching starts and runs without ever finding anyone (its handler is never called), and every target
// is unknown.

constexpr u32 kSceNetAdhocMatching = 0x00962A4A, kScePspnetAdhoc = 0x19C6AFDC;
constexpr i32 kMatchingInvalidMode = err::code(0x80413101), kMatchingInvalidArg = err::code(0x80413106),
              kMatchingInvalidId = err::code(0x80413107), kMatchingIsRunning = err::code(0x8041310A),
              kMatchingNotRunning = err::code(0x8041310B), kMatchingUnknownTarget = err::code(0x8041310C),
              kMatchingAlreadyInitialized = err::code(0x80413112), kMatchingNotInitialized = err::code(0x80413113);
struct Matching {
    bool initialized = false;
    i32 next_id = 1;
    nx::hash_map<i32, bool> contexts;  // (whether it runs)
};
Matching& matching() {
    static Matching m;
    return m;
}
i32 sceNetAdhocMatchingInit(u32 /*pool_size*/, Ptr<void> /*pool*/) {
    Held held(net().lock);
    if (matching().initialized) return kMatchingAlreadyInitialized;
    matching().initialized = true;
    return 0;
}
i32 sceNetAdhocMatchingTerm() {
    Held held(net().lock);
    if (!matching().initialized) return kMatchingNotInitialized;
    matching() = Matching{};
    return 0;
}
i32 sceNetAdhocMatchingCreate(i32 mode, i32 /*max*/, u32 /*port*/, i32 /*rx_length*/, u32, u32, i32, u32, u32 /*handler*/) {
    Held held(net().lock);
    Matching& m = matching();
    if (!m.initialized) return kMatchingNotInitialized;
    if (mode < 1 || mode > 3) return kMatchingInvalidMode;  // (host, client, peer to peer)
    const i32 id = m.next_id++;
    m.contexts[id] = false;
    return id;
}
// (the matching context, checked: 0, or the error)
i32 matching_context(i32 id) {
    if (!matching().initialized) return kMatchingNotInitialized;
    return matching().contexts.contains(id) ? 0 : kMatchingInvalidId;
}
i32 sceNetAdhocMatchingStart(i32 id, i32, i32, i32, i32 /*hello_length*/, Ptr<const void> /*hello*/) {
    Held held(net().lock);
    if (const i32 r = matching_context(id); r < 0) return r;
    if (matching().contexts[id]) return kMatchingIsRunning;
    matching().contexts[id] = true;
    return 0;
}
i32 sceNetAdhocMatchingStop(i32 id) {
    Held held(net().lock);
    if (const i32 r = matching_context(id); r < 0) return r;
    if (!matching().contexts[id]) return kMatchingNotRunning;
    matching().contexts[id] = false;
    return 0;
}
i32 sceNetAdhocMatchingDelete(i32 id) {
    Held held(net().lock);
    if (const i32 r = matching_context(id); r < 0) return r;
    if (matching().contexts[id]) return kMatchingIsRunning;
    matching().contexts.erase(id);
    return 0;
}
i32 running_context(i32 id) {
    if (const i32 r = matching_context(id); r < 0) return r;
    return matching().contexts[id] ? 0 : kMatchingNotRunning;
}
i32 unknown_target(i32 id, Ptr<const u32> target) {
    Held held(net().lock);
    if (const i32 r = running_context(id); r < 0) return r;
    return target ? kMatchingUnknownTarget : kMatchingInvalidArg;
}
i32 sceNetAdhocMatchingSelectTarget(i32 id, Ptr<const u32> target, i32, Ptr<const void>) { return unknown_target(id, target); }
i32 sceNetAdhocMatchingCancelTarget(i32 id, Ptr<const u32> target) { return unknown_target(id, target); }
i32 sceNetAdhocMatchingCancelTargetWithOpt(i32 id, Ptr<const u32> target, i32, Ptr<const void>) {
    return unknown_target(id, target);
}
i32 sceNetAdhocMatchingSendData(i32 id, Ptr<const u32> target, i32, Ptr<const void>) { return unknown_target(id, target); }
i32 sceNetAdhocMatchingSetHelloOpt(i32 id, i32 length, Ptr<const void> hello) {
    Held held(net().lock);
    if (const i32 r = running_context(id); r < 0) return r;
    return length < 0 || (length && !hello) ? kMatchingInvalidArg : 0;
}
// (sending to an unknown peer was never started; the hello option is none)
i32 sceNetAdhocMatchingAbortSendData(i32 id, Ptr<const u32> target) { return unknown_target(id, target); }
i32 sceNetAdhocMatchingGetHelloOpt(i32 id, Ptr<i32> length, Ptr<void>) {
    Held held(net().lock);
    if (const i32 r = running_context(id); r < 0) return r;
    if (!length || !writable(length.address, 4)) return kMatchingInvalidArg;
    *length = 0;
    return 0;
}
// (no members: nobody was found)
i32 sceNetAdhocMatchingGetMembers(i32 id, Ptr<u32> count, Ptr<void> /*members*/) {
    Held held(net().lock);
    if (const i32 r = running_context(id); r < 0) return r;
    if (!count || !writable(count.address, 4)) return kMatchingInvalidArg;
    *count = 0;
    return 0;
}

i32 pspnet_adhoc_done() { return 0; }

const Function kAdhocMatching[] = {
    HLE_FUNCTION(0x8E0E0EAE, sceNetAdhocMatchingInit),
    HLE_FUNCTION(0x6E195CD1, sceNetAdhocMatchingTerm),
    HLE_FUNCTION(0x7BAD7EA2, sceNetAdhocMatchingCreate),
    HLE_FUNCTION(0xF13E17BE, sceNetAdhocMatchingStart),
    HLE_FUNCTION(0x63ABB632, sceNetAdhocMatchingStop),
    HLE_FUNCTION(0x7BCDD889, sceNetAdhocMatchingDelete),
    HLE_FUNCTION(0x9A6B1D0F, sceNetAdhocMatchingSelectTarget),
    HLE_FUNCTION(0x04FF010C, sceNetAdhocMatchingCancelTarget),
    HLE_FUNCTION(0x54F6534D, sceNetAdhocMatchingCancelTargetWithOpt),
    HLE_FUNCTION(0x83C0E435, sceNetAdhocMatchingSendData),
    HLE_FUNCTION(0x659D4B04, sceNetAdhocMatchingSetHelloOpt),
    HLE_FUNCTION(0x85B23CEB, sceNetAdhocMatchingGetMembers),
    HLE_FUNCTION(0xFE77831E, sceNetAdhocMatchingAbortSendData),
    HLE_FUNCTION(0x90AFFEDD, sceNetAdhocMatchingGetHelloOpt),
};
// (the PSP-style ad hoc library and its control start and end; nothing more is asked of them)
const Function kPspnetAdhoc[] = {
    Function{0x87805E13, "sceNetAdhocInit", &entry<&pspnet_adhoc_done>},
    Function{0x6C0B4971, "sceNetAdhocTerm", &entry<&pspnet_adhoc_done>},
    Function{0xE2FE9DD6, "sceNetAdhocctlInit", &entry<&pspnet_adhoc_done>},
    Function{0x97A80065, "sceNetAdhocctlTerm", &entry<&pspnet_adhoc_done>},
};

}  // namespace

void register_net() {
    {
        Net& n = net();
        Held held(n.lock);
        n.initialized = false;
        n.next_id = 1;
        n.sockets.clear();
        n.epolls.clear();
        n.resolvers.clear();
        n.p2p_ports.clear();
        n.link = Link{};
        Held errno_held(n.errno_lock);
        n.errno_at.clear();
    }
    register_library({"SceNet", kSceNet, kNet});
    {
        Held held(net().lock);
        rudp() = Rudp{};
        matching() = Matching{};
    }
    register_library({"SceLibRudp", kSceLibRudp, kRudp});
    register_library({"SceNetAdhocMatching", kSceNetAdhocMatching, kAdhocMatching});
    register_library({"ScePspnetAdhoc", kScePspnetAdhoc, kPspnetAdhoc});
}

// ---- the ad hoc connection

namespace {

bool parse_ipv4(nx::string_view t, u32* out) {
    u32 part[4] = {}, n = 0, digits = 0;
    for (const char c : t) {
        if (c >= '0' && c <= '9') {
            part[n] = part[n] * 10 + static_cast<u32>(c - '0');
            if (++digits > 3 || part[n] > 255) return false;
        } else if (c == '.' && digits && n < 3) {
            ++n, digits = 0;
        } else {
            return false;
        }
    }
    if (n != 3 || !digits) return false;
    *out = part[0] << 24 | part[1] << 16 | part[2] << 8 | part[3];
    return true;
}

// The interface the setting names (an address, a name, a loopback address), or the host's local network: up, IPv4,
// not the loopback, not a tunnel or cellular data; one with a gateway, then Wi-Fi or Ethernet, first; a link-local
// address only when there is nothing else.
bool choose_link(Link* out, nx::string* why) {
    const nx::string& spec = runtime().config().network.adhoc_interface;
    u32 wanted = 0;
    const bool by_address = !spec.empty() && parse_ipv4(spec.view(), &wanted);
    if (by_address && is_loopback(wanted)) {
        *out = Link{true, true, wanted, 0xFF000000u, "loopback"};
        return true;
    }
    if (!hostnet::available()) {
        *why = "the host's sockets cannot be used";
        return false;
    }
    const nx::vector<hostnet::Interface> list = hostnet::interfaces();
    const hostnet::Interface* best = nullptr;
    i32 best_score = -1000;
    for (const hostnet::Interface& i : list) {
        if (!spec.empty()) {
            if ((by_address && i.addr == wanted) || (!by_address && i.name == spec)) {
                best = &i;
                break;
            }
            continue;
        }
        if (i.loopback || !i.addr) continue;
        i32 score = 0;
        if (i.gateway) score += 8;
        if (i.wireless || i.wired) score += 4;
        if (i.virtual_adapter) score -= 32;
        if ((i.addr >> 16) == 0xA9FE) score -= 16;  // (169.254: link-local, no network configured)
        if (score > best_score) best = &i, best_score = score;
    }
    if (!best) {
        nx::string names;
        for (const hostnet::Interface& i : list) names += nx::format("{}{} ({})", names.empty() ? "" : ", ", i.name, text(i.addr));
        *why = spec.empty() ? nx::format("no local network interface is up (the host has: {})", names.empty() ? "none" : names.c_str())
                            : nx::format("no interface '{}' is up (the host has: {})", spec, names.empty() ? "none" : names.c_str());
        return false;
    }
    u32 mask = best->mask;
    if (mask == 0 || mask == 0xFFFFFFFFu) mask = 0xFFFFFF00u;  // (no subnet given: a /24)
    *out = Link{true, best->loopback, best->addr, mask, best->name};
    return true;
}

}  // namespace

bool adhoc_enabled() { return current_runtime() && runtime().config().network.adhoc; }

bool adhoc_connect(nx::string* description) {
    Link l;
    nx::string why;
    if (!adhoc_enabled()) {
        *description = "ad hoc mode is off";
        return false;
    }
    if (!choose_link(&l, &why)) {
        *description = why;
        return false;
    }
    {
        Net& n = net();
        Held held(n.lock);
        n.link = l;
    }
    *description = nx::format("{} ({}/{})", l.name, text(l.addr), 32 - __builtin_ctz(l.mask ? l.mask : 1));
    nx::logi(logs::hle, "SceNet: ad hoc connection made on {}", *description);
    if (const auto on = runtime().config().network.on_adhoc) on(true);
    return true;
}

void adhoc_disconnect() {
    bool was = false;
    {
        Net& n = net();
        Held held(n.lock);
        was = n.link.up;
        n.link.up = false;
    }
    if (!was) return;
    nx::logi(logs::hle, "SceNet: ad hoc connection ended");
    if (const auto on = runtime().config().network.on_adhoc) on(false);
}

bool adhoc_address(u32* network_order) {
    Net& n = net();
    Held held(n.lock);
    if (!n.link.up) return false;
    *network_order = __builtin_bswap32(n.link.addr);
    return true;
}

}  // namespace psvrecore::runtime::hle
