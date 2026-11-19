// PlayStation Network, as on a console that is offline and signed out: the libraries start (SceNpManager,
// SceNpScore, SceNpActivity, SceNpTrophy); asking for the user's NP ID or posting an activity answers "signed out";
// the service state callback is never called (the state does not change). Trophies are local, as on the console:
// contexts and handles are made, and an unlocked trophy is logged and remembered for the run (unlocking it again
// answers "already unlocked"), and the host shows a notice of it. Authentication (SceNpCommon's NpAuth), friends and
// presence (SceNpBasic) and user lookups (SceNpUtility) start, and what would reach PSN answers "signed out".
// The trophy set's details (the game's title and trophies: names, grades, groups) come from the game's trophy pack,
// app0:sce_sys/trophy/<communication ID>/TROPHY.TRP, when its files are plain (TROP.SFM, the XML the set was made
// from); a pack whose files are encrypted answers "unsupported trophy pack", as before.

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "hle_common.hpp"
#include "psvrecore/io/file.hpp"
#include "psvrecore/runtime/dialogs.hpp"
#include "core/foundation/containers/hash_map.h"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceNpManager = 0xD8835093, kSceNpScore = 0x75F052DB, kSceNpActivity = 0xFEF492ED,
              kSceNpTrophy = 0x4332C10D, kSceNpCommon = 0xECB62224, kSceNpBasic = 0x605BA760, kSceNpUtility = 0x7B47D9CF;
constexpr i32 kAuthSignedOut = err::code(0x80550310), kBasicSignedOut = err::code(0x80551D06);
constexpr i32 kServiceStateSignedOut = 1;
constexpr i32 kManagerSignedOut = err::code(0x8055050A), kActivitySignedOut = err::code(0x80552306),
              kNpInvalidArgument = err::code(0x80550003);
// (trophy errors: invalid argument, already unlocked)
constexpr i32 kTrophyInvalidArgument = err::code(0x80551604), kTrophyAlreadyUnlocked = err::code(0x8055160F);
// (trophy errors: an unknown group or trophy; a pack that cannot be read)
constexpr i32 kTrophyInvalidGroup = err::code(0x8055160D), kTrophyInvalidTrophy = err::code(0x8055160E),
              kTrophyUnsupportedTrp = err::code(0x8055161B);
constexpr i32 kBaseGameGroup = -1;  // (SCE_NP_TROPHY_BASE_GAME_GROUP_ID)

// The trophy set, from the game's trophy pack.
struct TrophySet {
    struct Group {
        i32 id = kBaseGameGroup;
        nx::string name, detail;
    };
    struct Trophy {
        i32 id = 0, grade = 0, group = kBaseGameGroup;  // (grade: 1 platinum, 2 gold, 3 silver, 4 bronze)
        bool hidden = false;
        nx::string name, detail;
    };
    nx::string title, detail;
    nx::vector<Group> groups;  // (not the base game)
    nx::vector<Trophy> trophies;
};

struct Np {
    nx::mutex lock;
    u32 next_context = 1, next_handle = 1;
    nx::hash_map<i32, nx::string> contexts;                   // (each context's communication ID: NPWR00000_00)
    nx::hash_map<nx::string, std::optional<TrophySet>> sets;  // (by communication ID; empty: not readable)
    nx::hash_map<i32, bool> unlocked;
};
Np& np() {
    static Np n;
    return n;
}

// XML's entities decoded (&amp; &lt; &gt; &quot; &apos; and character references, as UTF-8).
void append_utf8(nx::string& out, u32 cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}
nx::string xml_unescape(nx::string_view s) {
    nx::string out;
    for (usize i = 0; i < s.size(); ++i) {
        const usize end = s[i] == '&' ? s.find(';', i) : nx::string_view::npos;
        if (end == nx::string_view::npos) {
            out += s[i];
            continue;
        }
        const nx::string_view e = s.substr(i + 1, end - i - 1);
        if (e == "amp") out += '&';
        else if (e == "lt") out += '<';
        else if (e == "gt") out += '>';
        else if (e == "quot") out += '"';
        else if (e == "apos") out += '\'';
        else if (e.size() > 1 && e[0] == '#') {
            const bool hex = e[1] == 'x' || e[1] == 'X';
            append_utf8(out, static_cast<u32>(std::strtoul(nx::string(e.substr(hex ? 2 : 1)).c_str(), nullptr, hex ? 16 : 10)));
        } else {
            out += s.substr(i, end - i + 1);
        }
        i = end;
    }
    return out;
}
// The text of an element's child `<tag>...</tag>`, and an attribute of a start tag (empty when there is none).
nx::string xml_child(nx::string_view element, nx::string_view tag) {
    const nx::string open = nx::format("<{}>", tag), close = nx::format("</{}>", tag);
    const usize a = element.find(open);
    if (a == nx::string_view::npos) return {};
    const usize b = element.find(close, a);
    if (b == nx::string_view::npos) return {};
    return xml_unescape(element.substr(a + open.size(), b - a - open.size()));
}
nx::string xml_attribute(nx::string_view start_tag, nx::string_view name) {
    const nx::string key = nx::format(" {}=\"", name);
    const usize a = start_tag.find(key);
    if (a == nx::string_view::npos) return {};
    const usize b = start_tag.find('"', a + key.size());
    if (b == nx::string_view::npos) return {};
    return xml_unescape(start_tag.substr(a + key.size(), b - a - key.size()));
}
// Each `<tag ...>...</tag>` (or `<tag .../>`) in `xml`, given its start tag and the whole element.
template <typename F>
void xml_elements(nx::string_view xml, nx::string_view tag, F&& each) {
    const nx::string open = nx::format("<{} ", tag), close = nx::format("</{}>", tag);
    for (usize at = xml.find(open); at != nx::string_view::npos; at = xml.find(open, at + 1)) {
        const usize start_end = xml.find('>', at);
        if (start_end == nx::string_view::npos) return;
        const nx::string_view start = xml.substr(at, start_end - at + 1);
        usize end = start_end + 1;
        if (start.size() < 2 || start[start.size() - 2] != '/') {
            const usize c = xml.find(close, start_end);
            if (c == nx::string_view::npos) return;
            end = c + close.size();
        }
        each(start, xml.substr(at, end - at));
    }
}
i32 decimal(const nx::string& s) { return static_cast<i32>(std::strtol(s.c_str(), nullptr, 10)); }

// The trophy set of a communication ID, from app0:sce_sys/trophy/<ID>/TROPHY.TRP: a big-endian header (magic
// 0xDCA24D00, the entries' count at 0x10 and their size at 0x14) and, from 0x40, entries of a name (32 bytes), the
// file's offset and its size (u64 each). TROP_<language>.SFM, else TROP.SFM, is the set's XML when it is plain.
std::optional<TrophySet> read_trophy_set(const nx::string& comm_id) {
    nx::string host_path;
    const nx::string guest_path = nx::format("app0:sce_sys/trophy/{}/TROPHY.TRP", comm_id);
    if (comm_id.empty() || runtime().files().resolve(guest_path, &host_path, nullptr, true) < 0) {
        nx::logw(logs::hle, "trophies: {} not found", guest_path);
        return std::nullopt;
    }
    auto bytes = psvrecore::io::read_file(nx::fs::path_view(host_path));
    if (!bytes || bytes->size() < 0x40) return std::nullopt;
    const u8* d = bytes->data();
    auto be32 = [&](usize o) { return (u32{d[o]} << 24) | (u32{d[o + 1]} << 16) | (u32{d[o + 2]} << 8) | u32{d[o + 3]}; };
    auto be64 = [&](usize o) { return (u64{be32(o)} << 32) | be32(o + 4); };
    if (be32(0) != 0xDCA24D00u) return std::nullopt;
    const u32 count = be32(0x10), entry_size = be32(0x14);
    if (entry_size < 0x30 || 0x40 + u64{count} * entry_size > bytes->size()) return std::nullopt;
    const nx::string wanted = nx::format("TROP_{:02d}.SFM", runtime().config().system.language);
    nx::string_view chosen, fallback;
    for (u32 i = 0; i < count; ++i) {
        const usize e = 0x40 + usize{i} * entry_size;
        const char* name_bytes = reinterpret_cast<const char*>(d + e);
        const nx::string_view name(name_bytes, strnlen(name_bytes, 32));
        const u64 offset = be64(e + 32), size = be64(e + 40);
        if (offset > bytes->size() || size > bytes->size() - offset) continue;
        const nx::string_view file(reinterpret_cast<const char*>(d + offset), static_cast<usize>(size));
        if (name == wanted) chosen = file;
        else if (name == "TROP.SFM") fallback = file;
    }
    if (chosen.empty()) chosen = fallback;
    if (chosen.find("<trophyconf") == nx::string_view::npos) {
        nx::logi(logs::hle, "trophies: {}'s trophy pack has no plain TROP.SFM (encrypted): no details", comm_id);
        return std::nullopt;
    }
    TrophySet set;
    set.title = xml_child(chosen, "title-name");
    set.detail = xml_child(chosen, "title-detail");
    xml_elements(chosen, "group", [&](nx::string_view start, nx::string_view element) {
        set.groups.push_back({decimal(xml_attribute(start, "id")), xml_child(element, "name"), xml_child(element, "detail")});
    });
    xml_elements(chosen, "trophy", [&](nx::string_view start, nx::string_view element) {
        TrophySet::Trophy t;
        t.id = decimal(xml_attribute(start, "id"));
        const nx::string type = xml_attribute(start, "ttype");
        t.grade = type == "P" ? 1 : type == "G" ? 2 : type == "S" ? 3 : type == "B" ? 4 : 0;
        t.hidden = xml_attribute(start, "hidden") == "yes";
        if (const nx::string gid = xml_attribute(start, "gid"); !gid.empty()) t.group = decimal(gid);
        t.name = xml_child(element, "name");
        t.detail = xml_child(element, "detail");
        set.trophies.push_back(std::move(t));
    });
    nx::logi(logs::hle, "trophies: {} \"{}\": {} trophies, {} groups besides the base game", comm_id, set.title,
             set.trophies.size(), set.groups.size());
    return set;
}

// The set of a context (np().lock held), or null and the error (a context not made here has no pack to read).
const TrophySet* trophy_set(i32 context, i32* error) {
    Np& n = np();
    const auto c = n.contexts.find(context);
    if (c == n.contexts.end()) {
        *error = kTrophyUnsupportedTrp;
        return nullptr;
    }
    auto s = n.sets.find(c->second);
    if (s == n.sets.end()) s = n.sets.emplace(c->second, read_trophy_set(c->second)).first;
    if (!s->second) {
        *error = kTrophyUnsupportedTrp;
        return nullptr;
    }
    return &*s->second;
}

void copy_text(u8* out, usize size, const nx::string& text) {
    const usize n = std::min(text.size(), size - 1);
    std::memcpy(out, text.data(), n);
    std::memset(out + n, 0, size - n);
}

// The structures filled: SceNpTrophyGameDetails and GroupDetails (size; the group count, or the group's ID; the
// trophies' count, then platinum, gold, silver and bronze; a title of 128 bytes and a description of 1024),
// SceNpTrophyGameData and GroupData (size; the group's ID; the unlocked counts the same way; the progress in percent),
// SceNpTrophyDetails (size, ID, grade, group, hidden, a name of 128 bytes, a description of 1024) and SceNpTrophyData
// (size, ID, unlocked, 4 reserved bytes, when it was unlocked as a u64 tick).
struct Counts {
    u32 v[5] = {};  // (all, platinum, gold, silver, bronze)
    void add(i32 grade) {
        ++v[0];
        if (grade >= 1 && grade <= 4) ++v[grade];
    }
    u32 percent_of(const Counts& all) const { return all.v[0] ? v[0] * 100 / all.v[0] : 0; }
};
constexpr u32 kDetailsText = 128 + 1024;

i32 sceNpInit(Ptr<const void>, Ptr<const void>) {
    nx::logi(logs::hle, "PSN: started offline, signed out");
    return 0;
}
i32 sceNpCheckCallback() { return 0; }
i32 sceNpRegisterServiceStateCallback(u32 /*callback*/, u32 /*userdata*/) { return 0; }
i32 sceNpManagerGetNpId(Ptr<void> out) { return out ? kManagerSignedOut : kNpInvalidArgument; }
i32 sceNpTerm() { return 0; }
i32 sceNpGetServiceState(Ptr<i32> state) {
    if (!state || !writable(state.address, 4)) return kNpInvalidArgument;
    *state = kServiceStateSignedOut;
    return 0;
}
i32 sceNpManagerGetChatRestrictionFlag(Ptr<i32> flag) { return flag ? kManagerSignedOut : kNpInvalidArgument; }
i32 sceNpManagerGetContentRatingFlag(Ptr<i32> restricted, Ptr<i32> age) {
    return restricted && age ? kManagerSignedOut : kNpInvalidArgument;
}

// (NpAuth: a ticket request needs PSN)
i32 sceNpAuthInit() { return 0; }
i32 sceNpAuthTerm() { return 0; }
i32 sceNpAuthCreateStartRequest(Ptr<const void> param) { return param ? kAuthSignedOut : kNpInvalidArgument; }

// (NpBasic: friends and presence need PSN; handlers are kept and never called)
i32 sceNpBasicInit(Ptr<const void>) { return 0; }
i32 sceNpBasicTerm() { return 0; }
i32 sceNpBasicRegisterHandler(Ptr<const void>, Ptr<const void>, Ptr<void>) { return 0; }
i32 sceNpBasicUnregisterHandler() { return 0; }
i32 sceNpBasicCheckCallback() { return 0; }
i32 sceNpBasicSetInGamePresence(Ptr<const void>) { return kBasicSignedOut; }
i32 sceNpBasicGetGameJoiningPresence(Ptr<const void>, Ptr<void>) { return kBasicSignedOut; }
i32 sceNpBasicGetFriendListEntryCount(Ptr<u32>) { return kBasicSignedOut; }
i32 sceNpBasicGetFriendListEntries(u32, Ptr<void>, u32, Ptr<u32>) { return kBasicSignedOut; }

// (NpLookup: a title context needs PSN; the error assumed as the manager's "signed out")
i32 sceNpLookupInit(i32 /*async*/, i32 /*priority*/, i32 /*affinity*/, Ptr<const void>) { return 0; }
i32 sceNpLookupTerm() { return 0; }
i32 sceNpLookupCreateTitleCtx(Ptr<const void> comm_id, Ptr<const void> self) {
    return comm_id && self ? kManagerSignedOut : kNpInvalidArgument;
}

i32 sceNpScoreInit(i32, i32, Ptr<const void>) { return 0; }
i32 sceNpScoreTerm() { return 0; }

i32 sceNpActivityInit(Ptr<const void>) { return 0; }
i32 sceNpActivityTerm() { return 0; }
i32 sceNpActivityPostStatus(Ptr<const void>, Ptr<const void>, Ptr<const void>) { return kActivitySignedOut; }

i32 sceNpTrophyInit(Ptr<const void>) { return 0; }
i32 sceNpTrophyCreateContext(Ptr<i32> context, Ptr<const u8> comm_id, Ptr<const void> /*comm_sign*/, u64 /*options*/) {
    if (!context || !writable(context.address, 4)) return kTrophyInvalidArgument;
    // (SceNpCommunicationId: 9 characters, a terminator, a number: NPWR00000_00)
    nx::string id;
    if (comm_id && readable(comm_id.address, 12)) {
        const char* c = reinterpret_cast<const char*>(comm_id.get());
        id = nx::format("{}_{:02d}", nx::string_view(c, strnlen(c, 9)), static_cast<u32>(comm_id.get()[10]));
    }
    nx::scoped_lock<nx::mutex> held(np().lock);
    *context = static_cast<i32>(np().next_context++);
    np().contexts[*context] = id;
    return 0;
}
i32 sceNpTrophyGetGameInfo(i32 context, i32 /*handle*/, Ptr<u8> details, Ptr<u8> data) {
    if ((details && !writable(details.address, 0x1C + kDetailsText)) || (data && !writable(data.address, 0x1C)))
        return kTrophyInvalidArgument;
    nx::scoped_lock<nx::mutex> held(np().lock);
    i32 error = 0;
    const TrophySet* set = trophy_set(context, &error);
    if (!set) return error;
    Counts all, unlocked;
    for (const auto& t : set->trophies) {
        all.add(t.grade);
        if (np().unlocked.contains(t.id)) unlocked.add(t.grade);
    }
    if (details) {
        u8* p = details.get();
        const u32 groups = static_cast<u32>(set->groups.size());
        std::memcpy(p + 4, &groups, 4);
        std::memcpy(p + 8, all.v, sizeof all.v);
        copy_text(p + 0x1C, 128, set->title);
        copy_text(p + 0x1C + 128, 1024, set->detail);
    }
    if (data) {
        u8* p = data.get();
        std::memcpy(p + 4, unlocked.v, sizeof unlocked.v);
        const u32 percent = unlocked.percent_of(all);
        std::memcpy(p + 0x18, &percent, 4);
    }
    return 0;
}
i32 sceNpTrophyGetGroupInfo(i32 context, i32 /*handle*/, i32 group, Ptr<u8> details, Ptr<u8> data) {
    if ((details && !writable(details.address, 0x1C + kDetailsText)) || (data && !writable(data.address, 0x20)))
        return kTrophyInvalidArgument;
    nx::scoped_lock<nx::mutex> held(np().lock);
    i32 error = 0;
    const TrophySet* set = trophy_set(context, &error);
    if (!set) return error;
    const TrophySet::Group* g = nullptr;
    for (const auto& candidate : set->groups)
        if (candidate.id == group) g = &candidate;
    if (!g && group != kBaseGameGroup) return kTrophyInvalidGroup;
    Counts all, unlocked;
    for (const auto& t : set->trophies)
        if (t.group == group) {
            all.add(t.grade);
            if (np().unlocked.contains(t.id)) unlocked.add(t.grade);
        }
    if (details) {
        u8* p = details.get();
        std::memcpy(p + 4, &group, 4);
        std::memcpy(p + 8, all.v, sizeof all.v);
        copy_text(p + 0x1C, 128, g ? g->name : set->title);
        copy_text(p + 0x1C + 128, 1024, g ? g->detail : set->detail);
    }
    if (data) {
        u8* p = data.get();
        std::memcpy(p + 4, &group, 4);
        std::memcpy(p + 8, unlocked.v, sizeof unlocked.v);
        const u32 percent = unlocked.percent_of(all);
        std::memcpy(p + 0x1C, &percent, 4);
    }
    return 0;
}
i32 sceNpTrophyGetTrophyInfo(i32 context, i32 /*handle*/, i32 trophy, Ptr<u8> details, Ptr<u8> data) {
    if ((details && !writable(details.address, 0x14 + kDetailsText)) || (data && !writable(data.address, 0x18)))
        return kTrophyInvalidArgument;
    nx::scoped_lock<nx::mutex> held(np().lock);
    i32 error = 0;
    const TrophySet* set = trophy_set(context, &error);
    if (!set) return error;
    const TrophySet::Trophy* t = nullptr;
    for (const auto& candidate : set->trophies)
        if (candidate.id == trophy) t = &candidate;
    if (!t) return kTrophyInvalidTrophy;
    if (details) {
        u8* p = details.get();
        const u32 v[4] = {static_cast<u32>(t->id), static_cast<u32>(t->grade), static_cast<u32>(t->group), t->hidden ? 1u : 0u};
        std::memcpy(p + 4, v, sizeof v);
        copy_text(p + 0x14, 128, t->name);
        copy_text(p + 0x14 + 128, 1024, t->detail);
    }
    if (data) {
        u8* p = data.get();
        const u32 v[2] = {static_cast<u32>(t->id), np().unlocked.contains(t->id) ? 1u : 0u};
        std::memcpy(p + 4, v, sizeof v);
        std::memset(p + 0xC, 0, 4 + 8);  // (reserved; when it was unlocked: not known)
    }
    return 0;
}
i32 sceNpTrophyCreateHandle(Ptr<i32> handle) {
    if (!handle || !writable(handle.address, 4)) return kTrophyInvalidArgument;
    nx::scoped_lock<nx::mutex> held(np().lock);
    *handle = static_cast<i32>(np().next_handle++);
    return 0;
}
i32 sceNpTrophyDestroyHandle(i32) { return 0; }
i32 sceNpTrophyUnlockTrophy(i32 context, i32 /*handle*/, i32 trophy, Ptr<i32> platinum) {
    if (platinum && writable(platinum.address, 4)) *platinum = -1;  // (SCE_NP_TROPHY_INVALID_TROPHY_ID: none unlocked)
    nx::scoped_lock<nx::mutex> held(np().lock);
    if (np().unlocked.contains(trophy)) return kTrophyAlreadyUnlocked;
    np().unlocked[trophy] = true;
    // (shown by the host: its name from the trophy pack when that is plain, else its number)
    nx::string name = nx::format("Trophy #{}", trophy);
    i32 error = 0;
    if (const TrophySet* set = trophy_set(context, &error))
        for (const auto& t : set->trophies)
            if (t.id == trophy && !t.name.empty()) name = t.name;
    nx::logi(logs::hle, "trophy {} unlocked: {}", trophy, name);
    dialogs::notify(SystemNotice{"Trophy unlocked", name, 0});
    return 0;
}

const Function kNpManager[] = {
    HLE_FUNCTION(0x04D9F484, sceNpInit),
    HLE_FUNCTION(0x3B0AE9A9, sceNpCheckCallback),
    HLE_FUNCTION(0x3C94B4B4, sceNpManagerGetNpId),
    HLE_FUNCTION(0x44239C35, sceNpRegisterServiceStateCallback),
    HLE_FUNCTION(0x19E40AE1, sceNpTerm),
    HLE_FUNCTION(0x54060DF6, sceNpGetServiceState),
    HLE_FUNCTION(0x60C575B1, sceNpManagerGetChatRestrictionFlag),
    HLE_FUNCTION(0xAF0073B2, sceNpManagerGetContentRatingFlag),
};
const Function kNpCommon[] = {
    HLE_FUNCTION(0x441D8B4E, sceNpAuthInit),
    HLE_FUNCTION(0x6093B689, sceNpAuthTerm),
    HLE_FUNCTION(0xED42079F, sceNpAuthCreateStartRequest),
};
const Function kNpBasic[] = {
    HLE_FUNCTION(0xEFB91A99, sceNpBasicInit),
    HLE_FUNCTION(0x389BCB3B, sceNpBasicTerm),
    HLE_FUNCTION(0x26E6E048, sceNpBasicRegisterHandler),
    HLE_FUNCTION(0x050AE072, sceNpBasicUnregisterHandler),
    HLE_FUNCTION(0x20146AEC, sceNpBasicCheckCallback),
    HLE_FUNCTION(0x51D75562, sceNpBasicSetInGamePresence),
    HLE_FUNCTION(0x863249CB, sceNpBasicGetGameJoiningPresence),
    HLE_FUNCTION(0xDF41F308, sceNpBasicGetFriendListEntryCount),
    HLE_FUNCTION(0xFF07E787, sceNpBasicGetFriendListEntries),
};
const Function kNpUtility[] = {
    HLE_FUNCTION(0x9246A673, sceNpLookupInit),
    HLE_FUNCTION(0x0158B61B, sceNpLookupTerm),
    HLE_FUNCTION(0x5110E17E, sceNpLookupCreateTitleCtx),
};
const Function kNpScore[] = {
    HLE_FUNCTION(0x0433069F, sceNpScoreInit),
    HLE_FUNCTION(0x2050F98F, sceNpScoreTerm),
};
const Function kNpActivity[] = {
    HLE_FUNCTION(0xE0FFEE97, sceNpActivityInit),
    HLE_FUNCTION(0x9EA4901F, sceNpActivityTerm),
    HLE_FUNCTION(0xBC7FDC77, sceNpActivityPostStatus),
};
i32 sceNpTrophyDestroyContext(i32) { return 0; }
i32 sceNpTrophyTerm() { return 0; }
// SceNpTrophyFlagArray (128 bits): the trophies unlocked in this run; `count` is the set's size, unknown here (0).
i32 sceNpTrophyGetTrophyUnlockState(i32 /*context*/, i32 /*handle*/, Ptr<u32> flags, Ptr<u32> count) {
    if (!flags || !count || !writable(flags.address, 16) || !writable(count.address, 4)) return kTrophyInvalidArgument;
    u32 bits[4] = {};
    nx::scoped_lock<nx::mutex> held(np().lock);
    for (const auto& [trophy, on] : np().unlocked)
        if (on && trophy >= 0 && trophy < 128) bits[trophy >> 5] |= 1u << (trophy & 31);
    std::memcpy(flags.get(), bits, sizeof bits);
    *count = 0;
    return 0;
}

const Function kNpTrophy[] = {
    HLE_FUNCTION(0x34516838, sceNpTrophyInit),
    HLE_FUNCTION(0xC49FD33F, sceNpTrophyCreateContext),
    HLE_FUNCTION(0x4EBC6977, sceNpTrophyCreateHandle),
    HLE_FUNCTION(0xFF142071, sceNpTrophyDestroyHandle),
    HLE_FUNCTION(0xB397AA24, sceNpTrophyUnlockTrophy),
    HLE_FUNCTION(0x56F5CBA5, sceNpTrophyDestroyContext),
    HLE_FUNCTION(0xBFE0F28F, sceNpTrophyTerm),
    HLE_FUNCTION(0xC8D2A4DE, sceNpTrophyGetTrophyUnlockState),
    HLE_FUNCTION(0xBA2B7F2A, sceNpTrophyGetGameInfo),
    HLE_FUNCTION(0x087B0535, sceNpTrophyGetGroupInfo),
    HLE_FUNCTION(0xA4ADDD91, sceNpTrophyGetTrophyInfo),
};

}  // namespace

void register_np() {
    {
        nx::scoped_lock<nx::mutex> held(np().lock);
        np().unlocked.clear();
        np().contexts.clear();
        np().sets.clear();
        np().next_context = np().next_handle = 1;
    }
    register_library({"SceNpManager", kSceNpManager, kNpManager});
    register_library({"SceNpScore", kSceNpScore, kNpScore});
    register_library({"SceNpActivity", kSceNpActivity, kNpActivity});
    register_library({"SceNpTrophy", kSceNpTrophy, kNpTrophy});
    register_library({"SceNpCommon", kSceNpCommon, kNpCommon});
    register_library({"SceNpBasic", kSceNpBasic, kNpBasic});
    register_library({"SceNpUtility", kSceNpUtility, kNpUtility});
}

}  // namespace psvrecore::runtime::hle
