// Online services on a console that is offline and signed out: NP lookup (SceNpUtility), title user storage
// (SceNpTus, SceNpTss), rankings (SceNpScore, beyond the start and end in np.cpp) and HTTP (SceHttp, beyond its
// start in system.cpp). Libraries start and end; contexts, requests, templates and connections are made and deleted
// (each a new positive ID); every operation that would reach a server answers "signed out" (NP) or "network" (HTTP),
// polling answers the same, and aborting answers done. URI escaping works (it needs no network).

#include <atomic>
#include <cstring>

#include "hle_common.hpp"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceNpUtility = 0x7B47D9CF, kSceNpTus = 0xAE84BDEA, kSceNpScore = 0x75F052DB, kSceHttp = 0xE8F15CDE;
constexpr i32 kSignedOut = err::code(0x8055050A);  // (SceNpManager's: not signed in)
constexpr i32 kHttpNetwork = err::code(0x80431063), kHttpOutOfSize = err::code(0x80431104),
              kHttpInvalidValue = err::code(0x804311FE);

std::atomic<i32> g_next_id{1};

i32 done() { return 0; }
i32 new_id() { return g_next_id.fetch_add(1); }
i32 signed_out() { return kSignedOut; }
i32 network() { return kHttpNetwork; }

// sceHttpUriEscape / sceHttpUriUnescape (out, &required, out's size, in): the size needed (with the terminator) in
// `required`; written when `out` is given and large enough.
bool unreserved(u8 c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
           c == '~';
}
i32 put_text(const nx::string& s, Ptr<char> out, Ptr<u32> required, u32 prepare) {
    const u32 need = static_cast<u32>(s.size()) + 1;
    if (required && writable(required.address, 4)) *required = need;
    if (!out) return 0;
    if (prepare < need) return kHttpOutOfSize;
    if (!writable(out.address, need)) return kHttpInvalidValue;
    std::memcpy(out.get(), s.c_str(), need);
    return 0;
}
i32 sceHttpUriEscape(Ptr<char> out, Ptr<u32> required, u32 prepare, Ptr<const char> in) {
    if (!in) return kHttpInvalidValue;
    const nx::string text = str(in, 65536);
    nx::string s;
    for (char ch : text) {
        const u8 c = static_cast<u8>(ch);
        if (unreserved(c)) s += ch;
        else s += nx::format("%{:02X}", c);
    }
    return put_text(s, out, required, prepare);
}
i32 sceHttpUriUnescape(Ptr<char> out, Ptr<u32> required, u32 prepare, Ptr<const char> in) {
    if (!in) return kHttpInvalidValue;
    const nx::string text = str(in, 65536);
    const auto hex = [](char c) -> i32 {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    nx::string s;
    for (usize i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() + 0 && hex(text[i + 1]) >= 0 && hex(text[i + 2]) >= 0) {
            s += static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2]));
            i += 2;
        } else {
            s += text[i];
        }
    }
    return put_text(s, out, required, prepare);
}

// The pool given to sceHttpInit: what sceHttpGetMemoryPoolStats reports (nothing of it in use).
std::atomic<u32> g_http_pool{0};
i32 sceHttpInit(u32 pool_size) {
    g_http_pool = pool_size;
    nx::logi(logs::hle, "SceHttp: started (offline: nothing connects)");
    return 0;
}
// SceHttpMemoryPoolStats: the pool's size, the most of it ever in use, in use now.
i32 sceHttpGetMemoryPoolStats(Ptr<u32> stats) {
    if (!stats || !writable(stats.address, 16)) return kHttpInvalidValue;
    const u32 v[4] = {g_http_pool.load(), 0, 0, 0};
    std::memcpy(stats.get(), v, sizeof v);
    return 0;
}

// sceHttpParseResponseHeader(header, length, field, &value, &value length): the field's value in a block of response
// headers ("Name: value" lines, CRLF or LF), the name matched without regard to case, the value's leading spaces
// skipped; a pointer into the block. Needs no network.
constexpr i32 kHttpParseNotFound = err::code(0x80432025), kHttpParseInvalidValue = err::code(0x804321FE);
i32 sceHttpParseResponseHeader(Ptr<const char> header, u32 length, Ptr<const char> field, Ptr<u32> value,
                               Ptr<u32> value_length) {
    if (!header || !field || !value || !value_length || !readable(header.address, length) ||
        !writable(value.address, 4) || !writable(value_length.address, 4))
        return kHttpParseInvalidValue;
    const nx::string name = str(field, 256);
    const char* h = header.get();
    u32 at = 0;
    while (at < length) {
        u32 end = at;
        while (end < length && h[end] != 0x0A) ++end;
        u32 line_end = end;
        if (line_end > at && h[line_end - 1] == 0x0D) --line_end;
        u32 colon = at;
        while (colon < line_end && h[colon] != ':') ++colon;
        if (colon < line_end && colon - at == name.size()) {
            bool same = true;
            for (u32 i = 0; i < name.size() && same; ++i) {
                const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
                same = lower(h[at + i]) == lower(name[i]);
            }
            if (same) {
                u32 v = colon + 1;
                while (v < line_end && (h[v] == ' ' || h[v] == 0x09)) ++v;
                *value = header.address + v;
                *value_length = line_end - v;
                return static_cast<i32>(line_end - v);
            }
        }
        at = end + 1;
    }
    return kHttpParseNotFound;
}

// sceHttpUriBuild(out, &required, out's size, element, which parts): a URI from its parts (SceHttpUriElement: opaque,
// scheme, user name, password, host, path, query, fragment, port), those `option` asks for that are given. The size
// needed (with the terminator) goes to `required`; written when `out` is given and large enough. Needs no network.
struct UriElement {
    u32 opaque;
    Ptr<const char> scheme, username, password, hostname, path, query, fragment;
    u16 port;
    u8 reserved[10];
};
static_assert(sizeof(UriElement) == 44);
i32 sceHttpUriBuild(Ptr<char> out, Ptr<u32> required, u32 prepare, Ptr<const UriElement> element, u32 option) {
    if (!element || !readable(element.address, sizeof(UriElement))) return kHttpInvalidValue;
    const UriElement e = *element;
    const auto part = [](Ptr<const char> p) { return p ? str(p, 4096) : nx::string(); };
    nx::string s;
    if ((option & 0x01) && e.scheme) s += part(e.scheme) + (e.opaque ? ":" : "://");
    if ((option & 0x10) && e.username && !part(e.username).empty()) {
        s += part(e.username);
        if ((option & 0x20) && e.password) s += ":" + part(e.password);
        s += "@";
    }
    if ((option & 0x02) && e.hostname) s += part(e.hostname);
    if ((option & 0x04) && e.port) s += nx::format(":{}", e.port);
    if ((option & 0x08) && e.path) s += part(e.path);
    if ((option & 0x40) && e.query) s += part(e.query);
    if ((option & 0x80) && e.fragment) s += part(e.fragment);
    return put_text(s, out, required, prepare);
}

#define DONE(nid, name) Function{nid, name, &entry<&done>}
#define NEW_ID(nid, name) Function{nid, name, &entry<&new_id>}
#define SIGNED_OUT(nid, name) Function{nid, name, &entry<&signed_out>}
#define NETWORK(nid, name) Function{nid, name, &entry<&network>}

// (SceNpUtility's start, end and title contexts are np.cpp's)
const Function kNpUtility[] = {
    DONE(0x33B64699, "sceNpLookupDeleteTitleCtx"),
    NEW_ID(0x9E42E922, "sceNpLookupCreateRequest"),
    DONE(0x8B608BF6, "sceNpLookupDeleteRequest"),
    SIGNED_OUT(0xB1A14879, "sceNpLookupNpId"),
    SIGNED_OUT(0x5387BABB, "sceNpLookupNpIdAsync"),
    SIGNED_OUT(0xFCDBA234, "sceNpLookupPollAsync"),
};

const Function kNpTus[] = {
    DONE(0xB2141F8D, "sceNpTusInit"),
    DONE(0x7EDC33B3, "sceNpTusTerm"),
    NEW_ID(0xF1D9529E, "sceNpTusCreateTitleCtx"),
    DONE(0xD53D3692, "sceNpTusDeleteTitleCtx"),
    NEW_ID(0x99DC7420, "sceNpTusCreateRequest"),
    DONE(0xB46B5D7F, "sceNpTusDeleteRequest"),
    DONE(0x2C5F1162, "sceNpTusAbortRequest"),
    DONE(0x88A2D551, "sceNpTusChangeModeForOtherSaveDataOwners"),
    SIGNED_OUT(0x246DB508, "sceNpTusDeleteMultiSlotVariable"),
    SIGNED_OUT(0x420350F9, "sceNpTusDeleteMultiSlotVariableVUser"),
    SIGNED_OUT(0xF6D24DF7, "sceNpTusDeleteMultiSlotData"),
    SIGNED_OUT(0x41FBED79, "sceNpTusDeleteMultiSlotDataVUser"),
    SIGNED_OUT(0x33E29819, "sceNpTusSetData"),
    SIGNED_OUT(0x6A175F75, "sceNpTusSetDataVUser"),
    SIGNED_OUT(0x9677BDD6, "sceNpTusGetData"),
    SIGNED_OUT(0xCCB9E791, "sceNpTusGetDataVUser"),
    SIGNED_OUT(0xD97E51EC, "sceNpTusGetMultiSlotDataStatus"),
    SIGNED_OUT(0x50FC680F, "sceNpTusGetMultiSlotDataStatusVUser"),
    SIGNED_OUT(0x5EB05C7A, "sceNpTusGetMultiSlotVariable"),
    SIGNED_OUT(0xFE92D6CB, "sceNpTusGetMultiSlotVariableVUser"),
    SIGNED_OUT(0xA3F00EE6, "sceNpTusSetMultiSlotVariable"),
    SIGNED_OUT(0x9A1E4472, "sceNpTusSetMultiSlotVariableVUser"),
    SIGNED_OUT(0x4099FC56, "sceNpTusAddAndGetVariable"),
    SIGNED_OUT(0xE1D6D4CF, "sceNpTusAddAndGetVariableVUser"),
    SIGNED_OUT(0xB0A6F839, "sceNpTusTryAndSetVariable"),
    SIGNED_OUT(0x6B2A54EB, "sceNpTusTryAndSetVariableVUser"),
    SIGNED_OUT(0xD8D5CABA, "sceNpTssGetData"),
};

// (SceNpScore's start and end are np.cpp's: these are registered with the same library)
const Function kNpScore[] = {
    NEW_ID(0x5685F225, "sceNpScoreCreateTitleCtx"),
    DONE(0xF52EA88A, "sceNpScoreDeleteTitleCtx"),
    NEW_ID(0xD30D1993, "sceNpScoreCreateRequest"),
    DONE(0xFFF24BB1, "sceNpScoreDeleteRequest"),
    DONE(0xA7E93CE1, "sceNpScoreAbortRequest"),
    DONE(0x53D77883, "sceNpScoreSetPlayerCharacterId"),
    SIGNED_OUT(0x24B09634, "sceNpScoreRecordScoreAsync"),
    SIGNED_OUT(0x427D3412, "sceNpScoreGetRankingByRange"),
    SIGNED_OUT(0x871F28AA, "sceNpScoreGetRankingByNpIdPcId"),
    SIGNED_OUT(0x9F2A7AC9, "sceNpScorePollAsync"),
};

// (SceHttp's start is system.cpp's)
const Function kHttp[] = {
    DONE(0xC9076666, "sceHttpTerm"),
    NEW_ID(0x62241DAB, "sceHttpCreateTemplate"),
    DONE(0xEC85ECFB, "sceHttpDeleteTemplate"),
    NEW_ID(0xC616C200, "sceHttpCreateConnectionWithURL"),
    DONE(0xF0F65C15, "sceHttpDeleteConnection"),
    NEW_ID(0xBD5DA1D0, "sceHttpCreateRequestWithURL"),
    DONE(0x3D3D29AD, "sceHttpDeleteRequest"),
    DONE(0xEA61662F, "sceHttpAbortRequest"),
    DONE(0x7B51B122, "sceHttpAddRequestHeader"),
    DONE(0x237CA86E, "sceHttpSetConnectTimeOut"),
    DONE(0x8AE3F008, "sceHttpSetSendTimeOut"),
    DONE(0x94BF196E, "sceHttpSetRecvTimeOut"),
    DONE(0x34891C3F, "sceHttpSetAutoRedirect"),
    DONE(0x9FBE2869, "sceHttpsEnableOption"),
    DONE(0xC6D60403, "sceHttpsDisableOption"),
    DONE(0xA0926037, "sceHttpsSetSslCallback"),
    DONE(0xAE8D7C33, "sceHttpsLoadCert"),
    NETWORK(0x9CA58B99, "sceHttpSendRequest"),
    NETWORK(0x7EDE3979, "sceHttpReadData"),
    NETWORK(0x27071691, "sceHttpGetStatusCode"),
    NETWORK(0xF580D304, "sceHttpGetResponseContentLength"),
    NETWORK(0x2B79BDE0, "sceHttpsGetSslError"),
    HLE_FUNCTION(0x1DA2A673, sceHttpUriEscape),
    HLE_FUNCTION(0x1274D318, sceHttpUriUnescape),
    HLE_FUNCTION(0x214926D9, sceHttpInit),
    HLE_FUNCTION(0xF98CDFA9, sceHttpGetMemoryPoolStats),
    HLE_FUNCTION(0x03A6C89E, sceHttpParseResponseHeader),
    HLE_FUNCTION(0x47664424, sceHttpUriBuild),
    NEW_ID(0xAEB3307E, "sceHttpCreateConnection"),
    NEW_ID(0xB0284270, "sceHttpCreateRequest"),
    DONE(0x5EB5F548, "sceHttpRemoveRequestHeader"),
    DONE(0x11EC42D0, "sceHttpSetAuthEnabled"),
    DONE(0xE0A3A88D, "sceHttpSetAuthInfoCallback"),
    DONE(0xAEE573A3, "sceHttpSetCookieEnabled"),
    DONE(0x4E08167D, "sceHttpSetRedirectCallback"),
    DONE(0x37C30C90, "sceHttpSetRequestContentLength"),
    DONE(0x9AB56EA7, "sceHttpSetResolveRetry"),
    DONE(0x8455B5B3, "sceHttpSetResolveTimeOut"),
    DONE(0x8577833F, "sceHttpsUnloadCert"),
    NETWORK(0x11F6C27F, "sceHttpGetAllResponseHeaders"),
    NETWORK(0x9A68C3DE, "sceHttpGetLastErrno"),
    NETWORK(0xA9A39BA1, "(SceHttp 0xA9A39BA1)"),  // (known only by NID: a game's network module imports it)
};

}  // namespace

void register_online() {
    register_library({"SceNpUtility", kSceNpUtility, kNpUtility});
    register_library({"SceNpTus", kSceNpTus, kNpTus});
    register_library({"SceNpScore", kSceNpScore, kNpScore});
    register_library({"SceHttp", kSceHttp, kHttp});
}

}  // namespace psvrecore::runtime::hle
