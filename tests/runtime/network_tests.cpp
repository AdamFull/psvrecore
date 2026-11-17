// The network libraries of an offline console: SceNet's sockets, epolls and resolver, SceNetCtl, HTTP's parsing and
// its offline answers, SSL, RUDP and ad hoc matching.

#include <cstring>

#include "nxtest.h"
#include "runtime_support.hpp"

using namespace psvrecore;
using namespace psvrecore::runtime;
using namespace psvrecore::runtime::test;

namespace {

u32 u(i32 v) { return static_cast<u32>(v); }

constexpr u32 kSceNet = 0x6BF8B2A2, kSceNetCtl = 0xC6083AE1, kSceHttp = 0xE8F15CDE, kSceSsl = 0xCAF5F2E9,
              kSceSslInternal = 0x68D6C3FF, kSceNetInternal = 0x9E8A69FD, kSceLibRudp = 0x32910D11,
              kSceNetAdhocMatching = 0x00962A4A, kScePspnetAdhoc = 0x19C6AFDC;

// SceNet's NIDs.
constexpr u32 kInit = 0xEB03E265, kTerm = 0xEA3CC286, kErrnoLoc = 0xE37F34AA, kSocket = 0xF084FCE3,
              kClose = 0x29822B4D, kAbort = 0x891C1B9B, kBind = 0x1296A94B, kConnect = 0x11E5B6F6, kListen = 0x7A8DA094,
              kAccept = 0x1ADF9BB1, kRecv = 0x023643B7, kRecvfrom = 0xB226138B, kSend = 0xE3DD8CD9, kSendto = 0x52DB31D5,
              kSendmsg = 0x99C579AE, kShutdown = 0x69E50BB5, kSetsockopt = 0x065505CA, kGetsockopt = 0xBA652062,
              kGetsockname = 0x1C66A6DB, kGetpeername = 0x2348D353, kGetSockInfo = 0xB1AF6840;
constexpr u32 kEpollCreate = 0xF9D102AE, kEpollDestroy = 0x7915CAF3, kEpollAbort = 0x93FCC4E8,
              kEpollControl = 0x4C8764AC, kEpollWait = 0x45CE337D;
constexpr u32 kResolverCreate = 0x6DA29319, kResolverDestroy = 0x3559F098, kResolverAbort = 0x38EBBD57,
              kNtoa = 0x1EB11857, kAton = 0x0424AE26, kResolverGetError = 0x874EF500;

constexpr u32 net_error(u32 e) { return 0x80410100u + e; }

// A SceNetSockaddrIn: AF_INET, the port and address in network order.
u32 sockaddr(u8 a, u8 b, u8 c, u8 d, u16 port) {
    const u32 at = scratch(16);
    u8* p = current_runtime()->memory().host(at);
    std::memset(p, 0, 16);
    p[0] = 16, p[1] = 2;
    p[2] = static_cast<u8>(port >> 8), p[3] = static_cast<u8>(port);
    p[4] = a, p[5] = b, p[6] = c, p[7] = d;
    return at;
}

}  // namespace

TEST_CASE("net: sockets bind and keep their options; nothing beyond the machine is reachable; waits end") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    CHECK(u(call32(ctx, kSceNet, kSocket, {0, 2, 2, 0})) == 0x804101C8u);  // (before sceNetInit: ENOTINIT)
    const u32 param = scratch(12);
    psv_st32(param + 4, 0x20000);
    REQUIRE(call32(ctx, kSceNet, kInit, {param}) == 0);
    CHECK(u(call32(ctx, kSceNet, kInit, {param})) == net_error(16));  // (EBUSY)
    CHECK(u(call32(ctx, kSceNet, kSocket, {0, 23, 2, 0})) == net_error(47));  // (only AF_INET)
    CHECK(u(call32(ctx, kSceNet, kSocket, {0, 2, 99, 0})) == net_error(43));

    const i32 udp = call32(ctx, kSceNet, kSocket, {guest_string("udp"), 2, 2, 0});
    const i32 tcp = call32(ctx, kSceNet, kSocket, {guest_string("tcp"), 2, 1, 0});
    REQUIRE(udp > 0);
    REQUIRE(tcp > 0);
    // Bound (any address, port 0: an ephemeral one); its name read back.
    REQUIRE(call32(ctx, kSceNet, kBind, {u(udp), sockaddr(0, 0, 0, 0, 0), 16}) == 0);
    CHECK(u(call32(ctx, kSceNet, kBind, {u(udp), sockaddr(0, 0, 0, 0, 0), 16})) == net_error(22));  // (bound)
    const u32 name = scratch(16), length = scratch(4);
    psv_st32(length, 16);
    REQUIRE(call32(ctx, kSceNet, kGetsockname, {u(udp), name, length}) == 0);
    CHECK(t.rt.memory().host(name)[1] == 2);
    CHECK((t.rt.memory().host(name)[2] | t.rt.memory().host(name)[3]) != 0);
    // Options kept: non-blocking, a receive time-out, any other.
    const u32 value = scratch(4), value_length = scratch(4);
    psv_st32(value, 1);
    REQUIRE(call32(ctx, kSceNet, kSetsockopt, {u(udp), 0xFFFF, 0x1100, value, 4}) == 0);
    psv_st32(value, 0);
    psv_st32(value_length, 4);
    REQUIRE(call32(ctx, kSceNet, kGetsockopt, {u(udp), 0xFFFF, 0x1100, value, value_length}) == 0);
    CHECK(psv_ld32(value) == 1);
    REQUIRE(call32(ctx, kSceNet, kGetsockopt, {u(udp), 0xFFFF, 0x1008, value, value_length}) == 0);  // SO_TYPE
    CHECK(psv_ld32(value) == 2);
    psv_st32(value, 0x4000);
    REQUIRE(call32(ctx, kSceNet, kSetsockopt, {u(udp), 0xFFFF, 0x1002, value, 4}) == 0);  // SO_RCVBUF
    REQUIRE(call32(ctx, kSceNet, kGetsockopt, {u(udp), 0xFFFF, 0x1002, value, value_length}) == 0);
    CHECK(psv_ld32(value) == 0x4000);

    // Receiving: nothing comes; a socket that does not block says so at once.
    const u32 buffer = scratch(64);
    CHECK(u(call32(ctx, kSceNet, kRecv, {u(udp), buffer, 64, 0})) == net_error(35));
    CHECK(u(call32(ctx, kSceNet, kRecvfrom, {u(udp), buffer, 64, 0, 0, 0})) == net_error(35));
    // (and the failure is the thread's errno)
    const u32 errno_at = u(call32(ctx, kSceNet, kErrnoLoc, {}));
    REQUIRE(errno_at != 0);
    CHECK(psv_ld32(errno_at) == 35);
    // A blocking one with a time-out: waits that long.
    psv_st32(value, 0);
    REQUIRE(call32(ctx, kSceNet, kSetsockopt, {u(udp), 0xFFFF, 0x1100, value, 4}) == 0);
    psv_st32(value, 30'000);
    REQUIRE(call32(ctx, kSceNet, kSetsockopt, {u(udp), 0xFFFF, 0x1006, value, 4}) == 0);
    const u64 before = t.rt.kernel().clock_ns();
    CHECK(u(call32(ctx, kSceNet, kRecv, {u(udp), buffer, 64, 0})) == net_error(35));
    CHECK(t.rt.kernel().clock_ns() - before >= 25'000'000);
    CHECK(u(call32(ctx, kSceNet, kRecv, {u(udp), buffer, 64, 0x80})) == net_error(35));  // (MSG_DONTWAIT)
    // An aborted socket's wait ends at once.
    REQUIRE(call32(ctx, kSceNet, kAbort, {u(udp), 0}) == 0);
    CHECK(u(call32(ctx, kSceNet, kRecv, {u(udp), buffer, 64, 0})) == net_error(4));

    // Sending: off the machine there is no route; to the loopback it goes (nobody receives it).
    CHECK(u(call32(ctx, kSceNet, kSendto, {u(udp), buffer, 8, 0, sockaddr(192, 168, 1, 2, 3658), 16})) == net_error(51));
    CHECK(call32(ctx, kSceNet, kSendto, {u(udp), buffer, 8, 0, sockaddr(127, 0, 0, 1, 3658), 16}) == 8);
    CHECK(u(call32(ctx, kSceNet, kSend, {u(udp), buffer, 8, 0})) == net_error(39));  // (no destination)
    const u32 iov = scratch(8), msg = scratch(28);
    psv_st32(iov, buffer), psv_st32(iov + 4, 12);
    psv_st32(msg, sockaddr(127, 0, 0, 1, 1)), psv_st32(msg + 4, 16), psv_st32(msg + 8, iov), psv_st32(msg + 12, 1);
    CHECK(call32(ctx, kSceNet, kSendmsg, {u(udp), msg, 0}) == 12);
    // A stream reaches nobody; never connected, it can neither send nor receive.
    CHECK(u(call32(ctx, kSceNet, kConnect, {u(tcp), sockaddr(10, 0, 0, 1, 80), 16})) == net_error(51));
    CHECK(u(call32(ctx, kSceNet, kConnect, {u(tcp), sockaddr(127, 0, 0, 1, 80), 16})) == net_error(61));
    CHECK(u(call32(ctx, kSceNet, kSend, {u(tcp), buffer, 8, 0})) == net_error(57));
    CHECK(u(call32(ctx, kSceNet, kRecv, {u(tcp), buffer, 8, 0})) == net_error(57));
    CHECK(u(call32(ctx, kSceNet, kGetpeername, {u(tcp), name, length})) == net_error(57));
    CHECK(u(call32(ctx, kSceNet, kShutdown, {u(tcp), 2})) == net_error(57));
    CHECK(u(call32(ctx, kSceNet, kAccept, {u(tcp), 0, 0})) == net_error(22));  // (not listening)
    REQUIRE(call32(ctx, kSceNet, kBind, {u(tcp), sockaddr(0, 0, 0, 0, 8080), 16}) == 0);
    REQUIRE(call32(ctx, kSceNet, kListen, {u(tcp), 4}) == 0);
    psv_st32(value, 1);
    REQUIRE(call32(ctx, kSceNet, kSetsockopt, {u(tcp), 0xFFFF, 0x1100, value, 4}) == 0);
    CHECK(u(call32(ctx, kSceNet, kAccept, {u(tcp), 0, 0})) == net_error(35));  // (nobody connects)
    CHECK(call32(ctx, kSceNet, kGetSockInfo, {u(udp), scratch(64), 1, 0}) == 0);

    // Epoll: a datagram socket asked about for writing is ready; nothing is ever readable.
    const i32 ep = call32(ctx, kSceNet, kEpollCreate, {guest_string("ep"), 0});
    REQUIRE(ep > 0);
    const u32 ev = scratch(32), events = scratch(32 * 4);
    std::memset(t.rt.memory().host(ev), 0, 32);
    psv_st32(ev, 0x1);  // EPOLLIN
    psv_st32(ev + 24, 0xABCD);
    REQUIRE(call32(ctx, kSceNet, kEpollControl, {u(ep), 1, u(udp), ev}) == 0);
    CHECK(u(call32(ctx, kSceNet, kEpollControl, {u(ep), 1, u(udp), ev})) == net_error(17));  // (EEXIST)
    CHECK(call32(ctx, kSceNet, kEpollWait, {u(ep), events, 4, 0}) == 0);
    CHECK(call32(ctx, kSceNet, kEpollWait, {u(ep), events, 4, 20'000}) == 0);  // (the time-out passes)
    psv_st32(ev, 0x3);  // EPOLLIN | EPOLLOUT
    REQUIRE(call32(ctx, kSceNet, kEpollControl, {u(ep), 2, u(udp), ev}) == 0);
    REQUIRE(call32(ctx, kSceNet, kEpollWait, {u(ep), events, 4, 0}) == 1);
    CHECK(psv_ld32(events) == 0x2);
    CHECK(psv_ld32(events + 24) == 0xABCD);
    REQUIRE(call32(ctx, kSceNet, kEpollControl, {u(ep), 3, u(udp), 0}) == 0);
    REQUIRE(call32(ctx, kSceNet, kEpollAbort, {u(ep), 0}) == 0);
    CHECK(u(call32(ctx, kSceNet, kEpollWait, {u(ep), events, 4, static_cast<u32>(-1)})) == net_error(4));
    REQUIRE(call32(ctx, kSceNet, kEpollDestroy, {u(ep)}) == 0);
    CHECK(u(call32(ctx, kSceNet, kEpollWait, {u(ep), events, 4, 0})) == net_error(9));

    // The resolver: numbers and "localhost" need no DNS server; names do.
    const i32 rid = call32(ctx, kSceNet, kResolverCreate, {guest_string("r"), 0, 0});
    REQUIRE(rid > 0);
    const u32 addr = scratch(4), error = scratch(4);
    REQUIRE(call32(ctx, kSceNet, kNtoa, {u(rid), guest_string("10.1.2.3"), addr, 0, 0, 0}) == 0);
    CHECK(psv_ld32(addr) == 0x0302010Au);
    REQUIRE(call32(ctx, kSceNet, kNtoa, {u(rid), guest_string("localhost"), addr, 0, 0, 0}) == 0);
    CHECK(psv_ld32(addr) == 0x0100007Fu);
    CHECK(u(call32(ctx, kSceNet, kNtoa, {u(rid), guest_string("example.com"), addr, 0, 0, 0})) == 0x804101E1u);
    REQUIRE(call32(ctx, kSceNet, kResolverGetError, {u(rid), error}) == 0);
    CHECK(psv_ld32(error) == 0x804101E1u);
    CHECK(u(call32(ctx, kSceNet, kAton, {u(rid), addr, scratch(64), 64, 0, 0, 0})) == 0x804101E1u);
    CHECK(call32(ctx, kSceNet, kResolverAbort, {u(rid), 0}) == 0);
    CHECK(call32(ctx, kSceNet, kResolverDestroy, {u(rid)}) == 0);
    CHECK(u(call32(ctx, kSceNet, kResolverDestroy, {u(rid)})) == net_error(9));

    // Information, and byte order.
    const u32 mac = scratch(6), stats = scratch(24);
    CHECK(call32(ctx, kSceNet, 0x06C05518 /*sceNetGetMacAddress*/, {mac, 0}) == 0);
    CHECK((t.rt.memory().host(mac)[0] & 2) != 0);  // (locally administered)
    CHECK(call32(ctx, kSceNet, 0xA86F8FE5 /*sceNetGetStatisticsInfo*/, {stats, 0}) == 0);
    CHECK(psv_ld32(stats + 16) == 0x20000);
    for (u32 nid : {0x658B903Bu, 0x6AB3B74Bu, 0x338EDC2Eu}) CHECK(call32(ctx, kSceNet, nid, {}) == 0);
    CHECK(call(ctx, kSceNet, 0x12C19209 /*sceNetHtonll*/, {0x04030201, 0x08070605}) == 0x0102030405060708ull);
    CHECK(call(ctx, kSceNet, 0xFB3336A6 /*sceNetNtohll*/, {0x04030201, 0x08070605}) == 0x0102030405060708ull);

    CHECK(call32(ctx, kSceNet, kClose, {u(udp)}) == 0);
    CHECK(call32(ctx, kSceNet, kClose, {u(tcp)}) == 0);
    CHECK(u(call32(ctx, kSceNet, kClose, {u(tcp)})) == net_error(9));
    CHECK(call32(ctx, kSceNet, kTerm, {}) == 0);
}

TEST_CASE("netctl: no NAT information, no traffic, no ad hoc peers") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kNetCtlInit = 0x495CA1DB, kNatInfo = 0x4DDD6149, kIfStat = 0xDA130450, kPeerList = 0x77586C59;
    const u32 out = scratch(64);
    REQUIRE(call32(ctx, kSceNetCtl, kNetCtlInit, {}) == 0);
    CHECK(u(call32(ctx, kSceNetCtl, kNatInfo, {out})) == 0x80412108u);  // (not connected)
    psv_st32(out, 64);
    std::memset(t.rt.memory().host(out + 4), 0xFF, 60);
    REQUIRE(call32(ctx, kSceNetCtl, kIfStat, {0, out}) == 0);
    CHECK(psv_ld32(out) == 64);
    CHECK(psv_ld32(out + 8) == 0);  // (bytes sent)
    psv_st32(out, 5);
    REQUIRE(call32(ctx, kSceNetCtl, kPeerList, {out, 0}) == 0);
    CHECK(psv_ld32(out) == 0);
    for (u32 nid : {0xC33D7374u, 0xE8E8DF18u, 0xF30365D6u}) CHECK(u(call32(ctx, kSceNetCtl, nid, {})) == 0x80412108u);
}

TEST_CASE("http: response headers parsed and URIs built without a network; what needs one fails") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kHttpInit = 0x214926D9, kPoolStats = 0xF98CDFA9, kParseHeader = 0x03A6C89E, kUriBuild = 0x47664424;
    REQUIRE(call32(ctx, kSceHttp, kHttpInit, {0x40000}) == 0);
    const u32 stats = scratch(16);
    REQUIRE(call32(ctx, kSceHttp, kPoolStats, {stats}) == 0);
    CHECK(psv_ld32(stats) == 0x40000);
    CHECK(psv_ld32(stats + 8) == 0);

    const char* headers = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\ncontent-length:   42\r\nX-Last: z";
    const u32 h = guest_string(headers), value = scratch(4), length = scratch(4);
    const u32 n = static_cast<u32>(std::strlen(headers));
    REQUIRE(call32(ctx, kSceHttp, kParseHeader, {h, n, guest_string("Content-Length"), value, length}) == 2);
    CHECK(std::memcmp(t.rt.memory().host(psv_ld32(value)), "42", 2) == 0);
    CHECK(psv_ld32(length) == 2);
    REQUIRE(call32(ctx, kSceHttp, kParseHeader, {h, n, guest_string("x-last"), value, length}) == 1);
    CHECK(t.rt.memory().host(psv_ld32(value))[0] == 'z');
    CHECK(u(call32(ctx, kSceHttp, kParseHeader, {h, n, guest_string("Server"), value, length})) == 0x80432025u);

    // scheme://user:pass@host:port/path?query#fragment, of the parts asked for.
    const u32 element = scratch(44);
    std::memset(t.rt.memory().host(element), 0, 44);
    const u32 parts[7] = {guest_string("https"), guest_string("user"), guest_string("pw"), guest_string("example.com"),
                          guest_string("/a/b"), guest_string("?q=1"), guest_string("#f")};
    for (u32 i = 0; i < 7; ++i) psv_st32(element + 4 + 4 * i, parts[i]);
    psv_st32(element + 32, 8443);
    const u32 out = scratch(128), required = scratch(4);
    REQUIRE(call32(ctx, kSceHttp, kUriBuild, {0, required, 0, element, 0xFFFF}) == 0);
    const nx::string full = "https://user:pw@example.com:8443/a/b?q=1#f";
    CHECK(psv_ld32(required) == full.size() + 1);
    REQUIRE(call32(ctx, kSceHttp, kUriBuild, {out, required, 128, element, 0xFFFF}) == 0);
    CHECK(nx::string(reinterpret_cast<const char*>(t.rt.memory().host(out))) == full);
    REQUIRE(call32(ctx, kSceHttp, kUriBuild, {out, required, 128, element, 0x01 | 0x02 | 0x08}) == 0);
    CHECK(nx::string(reinterpret_cast<const char*>(t.rt.memory().host(out))) == "https://example.com/a/b");
    CHECK(u(call32(ctx, kSceHttp, kUriBuild, {out, required, 4, element, 0xFFFF})) == 0x80431104u);  // (too small)

    // Connections and requests are made; settings taken; what needs the server fails as the network does.
    const i32 conn = call32(ctx, kSceHttp, 0xAEB3307E /*sceHttpCreateConnection*/, {1, guest_string("a.b"), guest_string("http"), 80, 1});
    const i32 req = call32(ctx, kSceHttp, 0xB0284270 /*sceHttpCreateRequest*/, {u(conn), 0, guest_string("/"), 0, 0});
    CHECK(conn > 0);
    CHECK(req > conn);
    for (u32 nid : {0x5EB5F548u, 0x11EC42D0u, 0xE0A3A88Du, 0xAEE573A3u, 0x4E08167Du, 0x37C30C90u, 0x9AB56EA7u,
                    0x8455B5B3u, 0x8577833Fu})
        CHECK(call32(ctx, kSceHttp, nid, {u(req), 0, 0, 0}) == 0);
    for (u32 nid : {0x11F6C27Fu, 0x9A68C3DEu, 0xA9A39BA1u}) CHECK(u(call32(ctx, kSceHttp, nid, {u(req), scratch(4), scratch(4)})) == 0x80431063u);
}

TEST_CASE("ssl: the pool reported, started or not; the internal functions a game's own module imports answer failure") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    const u32 stats = scratch(16);
    CHECK(call32(ctx, kSceSslInternal, 0x4B4A2307 /*sceSslInternalIsInitalized*/, {}) == 0);
    REQUIRE(call32(ctx, kSceSsl, 0x3C733316 /*sceSslInit*/, {0x8000}) == 0);
    CHECK(call32(ctx, kSceSslInternal, 0x4B4A2307, {}) == 1);
    REQUIRE(call32(ctx, kSceSsl, 0xBD203262 /*sceSslGetMemoryPoolStats*/, {stats}) == 0);
    CHECK(psv_ld32(stats) == 0x8000);
    CHECK(u(call32(ctx, kSceSslInternal, 0xF39A49C6 /*sceSslInternalWrite*/, {0, 0, 0})) == 0x80435201u);
    for (u32 nid : {0x0DB6C206u, 0x19B2BB58u, 0x2185BB05u, 0x260F5A85u, 0x2F0FC3CDu, 0x4A1557A7u, 0x4F781BB6u,
                    0x67FB1CD3u, 0xAC1DBE40u, 0xAF50B945u, 0xCC7F4211u, 0xCEE46B33u, 0xD28834E6u, 0xE5EDD91Bu,
                    0xE746465Eu, 0xEC9FAF42u, 0xEF9EBAC7u, 0xF2C9B744u, 0xF71DB06Fu, 0xFBEB1FD7u, 0xFFE2ED51u})
        CHECK(u(call32(ctx, kSceSslInternal, nid, {})) == 0x80435201u);
    for (u32 nid : {0x333F9CB3u, 0x689B9D7Du}) CHECK(u(call32(ctx, kSceNetInternal, nid, {})) == 0x80412108u);
    CHECK(call32(ctx, kSceSsl, 0x03CE6E3A /*sceSslTerm*/, {}) == 0);
    CHECK(call32(ctx, kSceSslInternal, 0x4B4A2307, {}) == 0);
}

TEST_CASE("rudp: contexts and polls made and given back; a session never comes up") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kRudpInit = 0x22C9AA29, kEnd = 0x3EA0961E, kCreate = 0x772C07E8, kTerminate = 0x552B2D3E,
                  kRudpBind = 0x8B766108, kActivate = 0xF1151D91, kInitiate = 0x46B635BD, kRead = 0x30A9438E,
                  kWrite = 0x69C9DE15, kReadable = 0x6B74CA22, kWritable = 0xADE196DC, kPollCreate = 0x943D96EC,
                  kPollDestroy = 0xC9ACBDF0, kPollControl = 0x18668BD8, kPollWait = 0x55918C7C;
    const u32 id = scratch(4), status = scratch(64), to = sockaddr(10, 0, 0, 2, 3658);
    CHECK(u(call32(ctx, kSceLibRudp, kCreate, {0, 0, id})) == 0x80770001u);  // (not initialized)
    REQUIRE(call32(ctx, kSceLibRudp, kRudpInit, {0, 0}) == 0);
    CHECK(u(call32(ctx, kSceLibRudp, kRudpInit, {0, 0})) == 0x80770002u);
    for (u32 nid : {0x3A495E69u, 0x396A24D6u, 0x24E36E39u}) CHECK(call32(ctx, kSceLibRudp, nid, {0, 0}) == 0);
    REQUIRE(call32(ctx, kSceLibRudp, kCreate, {0, 0, id}) == 0);
    const u32 c = psv_ld32(id);
    CHECK(call32(ctx, kSceLibRudp, kRudpBind, {c, 3, 1, 0}) == 0);
    CHECK(call32(ctx, kSceLibRudp, kActivate, {c, to, 16}) == 0);
    CHECK(call32(ctx, kSceLibRudp, kInitiate, {c, to, 16, 1}) == 0);
    CHECK(u(call32(ctx, kSceLibRudp, kRead, {c, status, 64, 0, 0})) == 0x80770016u);   // (would block)
    CHECK(u(call32(ctx, kSceLibRudp, kWrite, {c, status, 64, 0})) == 0x80770011u);     // (not acceptable)
    CHECK(call32(ctx, kSceLibRudp, kReadable, {c}) == 0);
    CHECK(call32(ctx, kSceLibRudp, kWritable, {c}) == 0);
    CHECK(call32(ctx, kSceLibRudp, 0x8D45BA52 /*Flush*/, {c}) == 0);
    CHECK(call32(ctx, kSceLibRudp, 0x2C1E736E /*NetReceived*/, {3, status, 8, to, 16}) == 0);
    CHECK(call32(ctx, kSceLibRudp, 0xBD87A5CA /*SetOption*/, {c, 1, status, 4}) == 0);
    CHECK(call32(ctx, kSceLibRudp, 0xE68005E3 /*GetOption*/, {c, 1, status, 4}) == 0);
    CHECK(call32(ctx, kSceLibRudp, 0xD620B557 /*GetContextStatus*/, {c, status, 64}) == 0);
    CHECK(call32(ctx, kSceLibRudp, 0xD77928C5 /*GetStatus*/, {status, 64}) == 0);
    const i32 poll = call32(ctx, kSceLibRudp, kPollCreate, {4});
    REQUIRE(poll > 0);
    CHECK(call32(ctx, kSceLibRudp, kPollControl, {u(poll), 1, c, 1}) == 0);
    CHECK(call32(ctx, kSceLibRudp, kPollWait, {u(poll), status, 4, 0, 0, 0}) == 0);    // (no events, at once)
    CHECK(call32(ctx, kSceLibRudp, kPollWait, {u(poll), status, 4, 0, 10'000, 0}) == 0);  // (after 10 ms)
    CHECK(call32(ctx, kSceLibRudp, kPollDestroy, {u(poll)}) == 0);
    CHECK(u(call32(ctx, kSceLibRudp, kPollDestroy, {u(poll)})) == 0x8077001Fu);
    CHECK(call32(ctx, kSceLibRudp, kTerminate, {c}) == 0);
    CHECK(u(call32(ctx, kSceLibRudp, kTerminate, {c})) == 0x80770003u);
    CHECK(call32(ctx, kSceLibRudp, kEnd, {}) == 0);
}

TEST_CASE("ad hoc: matching runs without ever finding anyone; the PSP-style library starts and ends") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kMInit = 0x8E0E0EAE, kMTerm = 0x6E195CD1, kMCreate = 0x7BAD7EA2, kMStart = 0xF13E17BE,
                  kMStop = 0x63ABB632, kMDelete = 0x7BCDD889, kSelect = 0x9A6B1D0F, kCancel = 0x04FF010C,
                  kCancelOpt = 0x54F6534D, kSendData = 0x83C0E435, kHello = 0x659D4B04, kMembers = 0x85B23CEB;
    const u32 target = scratch(4), count = scratch(4);
    REQUIRE(call32(ctx, kSceNetAdhocMatching, kMInit, {0x20000, 0}) == 0);
    CHECK(u(call32(ctx, kSceNetAdhocMatching, kMCreate, {9, 4, 1, 1024, 0, 0, 0, 0, 0})) == 0x80413101u);
    const i32 id = call32(ctx, kSceNetAdhocMatching, kMCreate, {3, 4, 1, 1024, 200000, 200000, 3, 200000, 0});
    REQUIRE(id > 0);
    CHECK(u(call32(ctx, kSceNetAdhocMatching, kSelect, {u(id), target, 0, 0})) == 0x8041310Bu);  // (not running)
    REQUIRE(call32(ctx, kSceNetAdhocMatching, kMStart, {u(id), 64, 0x4000, 0, 0, 0}) == 0);
    CHECK(u(call32(ctx, kSceNetAdhocMatching, kMStart, {u(id), 64, 0x4000, 0, 0, 0})) == 0x8041310Au);
    CHECK(call32(ctx, kSceNetAdhocMatching, kHello, {u(id), 0, 0}) == 0);
    psv_st32(count, 7);
    REQUIRE(call32(ctx, kSceNetAdhocMatching, kMembers, {u(id), count, 0}) == 0);
    CHECK(psv_ld32(count) == 0);
    for (u32 nid : {kSelect, kCancel, kCancelOpt, kSendData})
        CHECK(u(call32(ctx, kSceNetAdhocMatching, nid, {u(id), target, 0, 0})) == 0x8041310Cu);  // (unknown target)
    CHECK(u(call32(ctx, kSceNetAdhocMatching, kMDelete, {u(id)})) == 0x8041310Au);  // (running)
    REQUIRE(call32(ctx, kSceNetAdhocMatching, kMStop, {u(id)}) == 0);
    REQUIRE(call32(ctx, kSceNetAdhocMatching, kMDelete, {u(id)}) == 0);
    CHECK(u(call32(ctx, kSceNetAdhocMatching, kMDelete, {u(id)})) == 0x80413107u);
    REQUIRE(call32(ctx, kSceNetAdhocMatching, kMTerm, {}) == 0);
    CHECK(u(call32(ctx, kSceNetAdhocMatching, kMTerm, {})) == 0x80413113u);
    for (u32 nid : {0x87805E13u, 0x6C0B4971u, 0xE2FE9DD6u, 0x97A80065u}) CHECK(call32(ctx, kScePspnetAdhoc, nid, {0}) == 0);
}
