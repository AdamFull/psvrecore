// JSON (SceLibJson): the sce::Json classes, which the game constructs in its own memory, so their layouts here are
// the sizes its header gives them (32-bit ARM): String, Array, Object and their iterators one pointer; Value 24 bytes
// (parent, root, an 8-byte payload at 8, its type at 20); Object::Pair 32 (the key, then the value at 8).
// - A String points at its text: length, capacity, then the bytes (null-terminated); none for the empty string.
// - An Array or Object points at a list (first node, last node, count); a node is next, previous, then its Value (or
//   Pair) at 8. An iterator is its node (none at the end). The payload of a string, array or object Value points at
//   a String, Array or Object of its own.
// - Everything is allocated through the game's allocator given at initialization (its allocate and deallocate,
//   called through its virtual table: slots 2 and 3, after the two destructors); out of memory, its notifyError (slot
//   4) hears of it and the call gives NOMEM.
// - The getters of a mismatched type give the library's null value of that type (a shared one), except that the
//   signed and unsigned integers read each other.
// - The parser takes standard JSON: integers to Integer (or UInteger above the signed range), others to Real.
// - Objects keep their keys in insertion order. Copies are deep.

#include "hle_common.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceLibJson = 0x0854B69C;
constexpr i32 kParseInvalidChar = err::code(0x80920101), kNoMem = err::code(0x80920102), kNoInit = err::code(0x80920110),
              kMultipleInit = err::code(0x80920111), kInvalidArgument = err::code(0x80920112);

enum Type : u32 { kNull = 0, kBoolean, kInteger, kUInteger, kReal, kString, kArray, kObject };

constexpr u32 kValueSize = 24, kPayload = 8, kTypeAt = 20;
constexpr u32 kPairSize = 32, kPairValue = 8;
constexpr u32 kNodeData = 8;

struct Json {
    nx::mutex lock;
    bool initialized = false;
    u32 allocator = 0, user_data = 0;
    // shared null objects (guest memory, made once)
    u32 null_value = 0, null_string = 0, null_array = 0, null_object = 0, null_number = 0, empty_text = 0, base_vtable = 0;
};
Json& json() {
    static Json j;
    return j;
}

u32 ld(u32 a) { return psv_ld32(a); }
void st(u32 a, u32 v) { psv_st32(a, v); }

void make_statics() {
    Json& j = json();
    if (j.null_value) return;
    const u32 block = runtime().allocate_system(kValueSize + 4 * 3 + 8 + 4 + 5 * 4, "sce::Json null objects");
    std::memset(memory().host(block), 0, kValueSize + 4 * 3 + 8 + 4 + 5 * 4);
    j.null_value = block;
    j.null_string = block + kValueSize, j.null_array = j.null_string + 4, j.null_object = j.null_array + 4;
    j.null_number = j.null_object + 4, j.empty_text = j.null_number + 8, j.base_vtable = j.empty_text + 4;
}

// ---- memory, through the game's allocator

u32 alloc(PsvContext* ctx, u32 size) {
    Json& j = json();
    u32 p = 0;
    if (j.allocator) {
        const u32 vtable = ld(j.allocator);
        const u32 args[] = {j.allocator, size, j.user_data};
        p = call_guest(ctx, ld(vtable + 8), args);
        if (!p) {
            const u32 error[] = {j.allocator, static_cast<u32>(kNoMem), size, j.user_data};
            if (const u32 notify = ld(vtable + 16)) call_guest(ctx, notify, error);
        }
    } else {
        p = runtime().allocate_system(size, "sce::Json");  // (no allocator given: never freed)
    }
    if (p) std::memset(memory().host(p), 0, size);
    return p;
}
void release(PsvContext* ctx, u32 p) {
    Json& j = json();
    if (!p || !j.allocator) return;
    const u32 args[] = {j.allocator, p, j.user_data};
    call_guest(ctx, ld(ld(j.allocator) + 12), args);
}

// ---- strings

u32 str_len(u32 s) {
    const u32 impl = ld(s);
    return impl ? ld(impl) : 0;
}
const char* str_text(u32 s) {
    const u32 impl = ld(s);
    return impl ? reinterpret_cast<const char*>(memory().host(impl + 8)) : "";
}
u32 str_c(u32 s) {
    const u32 impl = ld(s);
    return impl ? impl + 8 : json().empty_text;
}
bool str_set(PsvContext* ctx, u32 s, const char* text, u32 n) {
    const u32 old = ld(s);
    st(s, 0);
    release(ctx, old);
    if (!n) return true;
    const u32 impl = alloc(ctx, 8 + n + 1);
    if (!impl) return false;
    st(impl, n), st(impl + 4, n);
    std::memcpy(memory().host(impl + 8), text, n);
    st(s, impl);
    return true;
}
void str_free(PsvContext* ctx, u32 s) {
    release(ctx, ld(s));
    st(s, 0);
}
nx::string str_host(u32 s) { return nx::string(str_text(s), str_len(s)); }

nx::string guest_cstring(u32 a) {
    nx::string out;
    for (u32 i = 0; a && i < 0x100000 && memory().valid(a + i, 1); ++i) {
        const char c = static_cast<char>(memory().host(a + i)[0]);
        if (!c) break;
        out.push_back(c);
    }
    return out;
}

// ---- lists (arrays and objects)

u32 list_first(u32 container) {
    const u32 impl = ld(container);
    return impl ? ld(impl) : 0;
}
u32 list_count(u32 container) {
    const u32 impl = ld(container);
    return impl ? ld(impl + 8) : 0;
}
// A new node (`data` bytes after the links) at the list's end; 0 when out of memory.
u32 list_append(PsvContext* ctx, u32 container, u32 data) {
    u32 impl = ld(container);
    if (!impl) {
        impl = alloc(ctx, 12);
        if (!impl) return 0;
        st(container, impl);
    }
    const u32 node = alloc(ctx, kNodeData + data);
    if (!node) return 0;
    const u32 tail = ld(impl + 4);
    st(node + 4, tail);
    if (tail) st(tail, node);
    else st(impl, node);
    st(impl + 4, node);
    st(impl + 8, ld(impl + 8) + 1);
    return node;
}

void value_clear(PsvContext* ctx, u32 v);

void array_clear(PsvContext* ctx, u32 a) {
    const u32 impl = ld(a);
    if (!impl) return;
    for (u32 n = ld(impl); n;) {
        const u32 next = ld(n);
        value_clear(ctx, n + kNodeData);
        release(ctx, n);
        n = next;
    }
    release(ctx, impl);
    st(a, 0);
}
void object_clear(PsvContext* ctx, u32 o) {
    const u32 impl = ld(o);
    if (!impl) return;
    for (u32 n = ld(impl); n;) {
        const u32 next = ld(n);
        str_free(ctx, n + kNodeData);
        value_clear(ctx, n + kNodeData + kPairValue);
        release(ctx, n);
        n = next;
    }
    release(ctx, impl);
    st(o, 0);
}

// ---- values

u32 value_type(u32 v) { return ld(v + kTypeAt); }

void value_clear(PsvContext* ctx, u32 v) {
    const u32 type = value_type(v), held = ld(v + kPayload);
    if (type == kString && held) str_free(ctx, held), release(ctx, held);
    if (type == kArray && held) array_clear(ctx, held), release(ctx, held);
    if (type == kObject && held) object_clear(ctx, held), release(ctx, held);
    st(v + kPayload, 0), st(v + kPayload + 4, 0);
    st(v + kTypeAt, kNull);
}
void value_init(u32 v) { std::memset(memory().host(v), 0, kValueSize); }

bool value_copy(PsvContext* ctx, u32 dst, u32 src);

bool array_copy(PsvContext* ctx, u32 dst, u32 src) {
    for (u32 n = list_first(src); n; n = ld(n)) {
        const u32 node = list_append(ctx, dst, kValueSize);
        if (!node || !value_copy(ctx, node + kNodeData, n + kNodeData)) return false;
    }
    return true;
}
bool object_copy(PsvContext* ctx, u32 dst, u32 src) {
    for (u32 n = list_first(src); n; n = ld(n)) {
        const u32 node = list_append(ctx, dst, kPairSize);
        if (!node) return false;
        const u32 key = n + kNodeData;
        if (!str_set(ctx, node + kNodeData, str_text(key), str_len(key))) return false;
        if (!value_copy(ctx, node + kNodeData + kPairValue, n + kNodeData + kPairValue)) return false;
    }
    return true;
}

// (dst empty)
bool value_copy(PsvContext* ctx, u32 dst, u32 src) {
    const u32 type = value_type(src);
    st(dst + kTypeAt, type);
    if (type == kString || type == kArray || type == kObject) {
        const u32 held = alloc(ctx, 4);
        if (!held) return false;
        st(dst + kPayload, held);
        const u32 from = ld(src + kPayload);
        if (!from) return true;
        if (type == kString) return str_set(ctx, held, str_text(from), str_len(from));
        if (type == kArray) return array_copy(ctx, held, from);
        return object_copy(ctx, held, from);
    }
    std::memcpy(memory().host(dst + kPayload), memory().host(src + kPayload), 8);
    return true;
}

// A value holding a new, empty String, Array or Object; its address (0: out of memory).
u32 value_make_container(PsvContext* ctx, u32 v, u32 type) {
    const u32 held = alloc(ctx, 4);
    if (!held) return 0;
    st(v + kTypeAt, type), st(v + kPayload, held);
    return held;
}

u32 object_find(u32 o, const char* key, u32 n) {
    for (u32 node = list_first(o); node; node = ld(node)) {
        const u32 k = node + kNodeData;
        if (str_len(k) == n && std::memcmp(str_text(k), key, n) == 0) return node;
    }
    return 0;
}

// ---- the parser

struct Parser {
    PsvContext* ctx;
    const char* p;
    const char* end;
    bool nomem = false;
    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }
    bool literal(const char* word) {
        const usize n = std::strlen(word);
        if (static_cast<usize>(end - p) < n || std::memcmp(p, word, n) != 0) return false;
        p += n;
        return true;
    }
    static void utf8(nx::string& out, u32 c) {
        if (c < 0x80) out.push_back(static_cast<char>(c));
        else if (c < 0x800) out.push_back(static_cast<char>(0xC0 | c >> 6)), out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        else if (c < 0x10000)
            out.push_back(static_cast<char>(0xE0 | c >> 12)), out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F))),
                out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        else
            out.push_back(static_cast<char>(0xF0 | c >> 18)), out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F))),
                out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F))), out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
    bool hex4(u32* out) {
        if (end - p < 4) return false;
        u32 v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<u32>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<u32>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<u32>(c - 'A' + 10);
            else return false;
        }
        *out = v;
        return true;
    }
    bool string(nx::string& out) {
        if (p >= end || *p != '"') return false;
        ++p;
        while (p < end && *p != '"') {
            const char c = *p++;
            if (static_cast<u8>(c) < 0x20) return false;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (p >= end) return false;
            const char e = *p++;
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back(static_cast<char>(0x08)); break;
            case 'f': out.push_back(static_cast<char>(0x0C)); break;
            case 'n': out.push_back(static_cast<char>(0x0A)); break;
            case 'r': out.push_back(static_cast<char>(0x0D)); break;
            case 't': out.push_back(static_cast<char>(0x09)); break;
            case 'u': {
                u32 c1;
                if (!hex4(&c1)) return false;
                if (c1 >= 0xD800 && c1 < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    u32 c2;
                    if (!hex4(&c2) || c2 < 0xDC00 || c2 >= 0xE000) return false;
                    c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                }
                utf8(out, c1);
                break;
            }
            default: return false;
            }
        }
        if (p >= end) return false;
        ++p;
        return true;
    }
    bool number(u32 v) {
        const char* start = p;
        if (p < end && *p == '-') ++p;
        if (p >= end || *p < '0' || *p > '9') return false;
        while (p < end && *p >= '0' && *p <= '9') ++p;
        bool integral = true;
        if (p < end && *p == '.') {
            integral = false, ++p;
            if (p >= end || *p < '0' || *p > '9') return false;
            while (p < end && *p >= '0' && *p <= '9') ++p;
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            integral = false, ++p;
            if (p < end && (*p == '+' || *p == '-')) ++p;
            if (p >= end || *p < '0' || *p > '9') return false;
            while (p < end && *p >= '0' && *p <= '9') ++p;
        }
        const nx::string text(start, static_cast<usize>(p - start));
        u8* payload = memory().host(v + kPayload);
        if (integral) {
            errno = 0;
            if (text[0] != '-') {
                const unsigned long long u = std::strtoull(text.c_str(), nullptr, 10);
                if (errno == 0) {
                    const bool big = u > 0x7FFFFFFFFFFFFFFFull;
                    std::memcpy(payload, &u, 8);
                    st(v + kTypeAt, big ? kUInteger : kInteger);
                    return true;
                }
            } else {
                const long long s = std::strtoll(text.c_str(), nullptr, 10);
                if (errno == 0) {
                    std::memcpy(payload, &s, 8);
                    st(v + kTypeAt, kInteger);
                    return true;
                }
            }
        }
        const f64 d = std::strtod(text.c_str(), nullptr);
        std::memcpy(payload, &d, 8);
        st(v + kTypeAt, kReal);
        return true;
    }
    bool value(u32 v, u32 depth) {
        if (depth > 512) return false;
        ws();
        if (p >= end) return false;
        switch (*p) {
        case 'n': return literal("null");
        case 't':
            if (!literal("true")) return false;
            st(v + kTypeAt, kBoolean), st(v + kPayload, 1);
            return true;
        case 'f':
            if (!literal("false")) return false;
            st(v + kTypeAt, kBoolean), st(v + kPayload, 0);
            return true;
        case '"': {
            nx::string s;
            if (!string(s)) return false;
            const u32 held = value_make_container(ctx, v, kString);
            if (!held || !str_set(ctx, held, s.data(), static_cast<u32>(s.size()))) return nomem = true, false;
            return true;
        }
        case '[': {
            ++p;
            const u32 held = value_make_container(ctx, v, kArray);
            if (!held) return nomem = true, false;
            ws();
            if (p < end && *p == ']') return ++p, true;
            for (;;) {
                const u32 node = list_append(ctx, held, kValueSize);
                if (!node) return nomem = true, false;
                if (!value(node + kNodeData, depth + 1)) return false;
                ws();
                if (p < end && *p == ',') {
                    ++p;
                    continue;
                }
                if (p < end && *p == ']') return ++p, true;
                return false;
            }
        }
        case '{': {
            ++p;
            const u32 held = value_make_container(ctx, v, kObject);
            if (!held) return nomem = true, false;
            ws();
            if (p < end && *p == '}') return ++p, true;
            for (;;) {
                ws();
                nx::string key;
                if (!string(key)) return false;
                ws();
                if (p >= end || *p != ':') return false;
                ++p;
                const u32 node = list_append(ctx, held, kPairSize);
                if (!node || !str_set(ctx, node + kNodeData, key.data(), static_cast<u32>(key.size()))) return nomem = true, false;
                if (!value(node + kNodeData + kPairValue, depth + 1)) return false;
                ws();
                if (p < end && *p == ',') {
                    ++p;
                    continue;
                }
                if (p < end && *p == '}') return ++p, true;
                return false;
            }
        }
        default: return number(v);
        }
    }
};

// ---- writing JSON

void escape(nx::string& out, const char* s, u32 n) {
    out.push_back('"');
    for (u32 i = 0; i < n; ++i) {
        const u8 c = static_cast<u8>(s[i]);
        if (c == '"' || c == '\\') out.push_back('\\'), out.push_back(static_cast<char>(c));
        else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else out.push_back(static_cast<char>(c));
    }
    out.push_back('"');
}
void write_value(nx::string& out, u32 v, bool top_string_raw) {
    const u32 type = value_type(v);
    const u8* payload = memory().host(v + kPayload);
    char buf[40];
    switch (type) {
    case kNull: out += "null"; break;
    case kBoolean: out += payload[0] ? "true" : "false"; break;
    case kInteger: {
        long long x;
        std::memcpy(&x, payload, 8);
        std::snprintf(buf, sizeof buf, "%lld", x);
        out += buf;
        break;
    }
    case kUInteger: {
        unsigned long long x;
        std::memcpy(&x, payload, 8);
        std::snprintf(buf, sizeof buf, "%llu", x);
        out += buf;
        break;
    }
    case kReal: {
        f64 x;
        std::memcpy(&x, payload, 8);
        std::snprintf(buf, sizeof buf, "%.17g", x);
        out += buf;
        break;
    }
    case kString: {
        const u32 s = ld(v + kPayload);
        if (top_string_raw) out.append(str_text(s), str_len(s));
        else escape(out, str_text(s), str_len(s));
        break;
    }
    case kArray: {
        out.push_back('[');
        bool first = true;
        for (u32 n = list_first(ld(v + kPayload)); n; n = ld(n)) {
            if (!first) out.push_back(',');
            first = false;
            write_value(out, n + kNodeData, false);
        }
        out.push_back(']');
        break;
    }
    case kObject: {
        out.push_back('{');
        bool first = true;
        for (u32 n = list_first(ld(v + kPayload)); n; n = ld(n)) {
            if (!first) out.push_back(',');
            first = false;
            escape(out, str_text(n + kNodeData), str_len(n + kNodeData));
            out.push_back(':');
            write_value(out, n + kNodeData + kPairValue, false);
        }
        out.push_back('}');
        break;
    }
    }
}

using Lock = nx::scoped_lock<nx::mutex>;

// ---- Initializer, MemAllocator

u32 Initializer_C1(u32 self) {
    memory().host(self)[0] = 0;
    return self;
}
u32 Initializer_D1(u32 self) { return self; }
// InitParameter: allocator, user data, file buffer size.
i32 Initializer_initialize(u32 self, u32 param) {
    if (!param || !memory().valid(param, 12)) return kInvalidArgument;
    Json& j = json();
    Lock held(j.lock);
    if (j.initialized) return kMultipleInit;
    make_statics();
    j.initialized = true;
    j.allocator = ld(param), j.user_data = ld(param + 4);
    memory().host(self)[0] = 1;
    return 0;
}
i32 Initializer_terminate(u32 self) {
    Json& j = json();
    Lock held(j.lock);
    if (!j.initialized) return kNoInit;
    j.initialized = false;
    memory().host(self)[0] = 0;
    return 0;
}
u32 MemAllocator_C2(u32 self) {
    make_statics();
    st(self, json().base_vtable);  // (the abstract base's table: the derived class's constructor replaces it)
    return self;
}
u32 MemAllocator_D2(u32 self) { return self; }
void MemAllocator_notifyError(u32, i32 error, u32 size, u32) {
    nx::logw(logs::hle, "sce::Json: error 0x{:08x} allocating {} bytes", static_cast<u32>(error), size);
}

// ---- String

u32 String_C1(u32 self) {
    st(self, 0);
    return self;
}
u32 String_C1_text(PsvContext* ctx, u32 self, u32 text) {
    st(self, 0);
    const nx::string s = guest_cstring(text);
    str_set(ctx, self, s.data(), static_cast<u32>(s.size()));
    return self;
}
u32 String_D1(PsvContext* ctx, u32 self) {
    str_free(ctx, self);
    return self;
}
u32 String_assign(PsvContext* ctx, u32 self, u32 other) {
    if (self != other) {
        const nx::string s = str_host(other);
        str_set(ctx, self, s.data(), static_cast<u32>(s.size()));
    }
    return self;
}
u32 String_size(u32 self) { return str_len(self); }
u32 String_c_str(u32 self) {
    make_statics();
    return str_c(self);
}
i32 String_compare(u32 self, u32 text) {
    const nx::string other = guest_cstring(text);
    const i32 r = std::strcmp(str_host(self).c_str(), other.c_str());
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}
u32 String_empty(u32 self) { return str_len(self) == 0; }
void String_clear(PsvContext* ctx, u32 self) { str_free(ctx, self); }
u32 String_append_n(PsvContext* ctx, u32 self, u32 text, u32 n) {
    nx::string s = str_host(self);
    if (text && memory().valid(text, n)) s.append(reinterpret_cast<const char*>(memory().host(text)), n);
    str_set(ctx, self, s.data(), static_cast<u32>(s.size()));
    return self;
}
u32 String_append(PsvContext* ctx, u32 self, u32 text) {
    const nx::string add = guest_cstring(text);
    nx::string s = str_host(self) + add;
    str_set(ctx, self, s.data(), static_cast<u32>(s.size()));
    return self;
}

// ---- Array and its iterator

u32 Array_C1(u32 self) {
    st(self, 0);
    return self;
}
u32 Array_D1(PsvContext* ctx, u32 self) {
    array_clear(ctx, self);
    return self;
}
void Array_begin(u32 out, u32 self) { st(out, list_first(self)); }  // (the iterator returned through r0)
void Array_end(u32 out, u32) { st(out, 0); }
u32 Array_size(u32 self) { return list_count(self); }
u32 Array_empty(u32 self) { return list_count(self) == 0; }
void Array_push_back(PsvContext* ctx, u32 self, u32 value) {
    const u32 node = list_append(ctx, self, kValueSize);
    if (node) value_copy(ctx, node + kNodeData, value);
}
u32 ArrayIt_D1(u32 self) { return self; }
u32 ArrayIt_next(u32 self) {
    if (const u32 n = ld(self)) st(self, ld(n));
    return self;
}
u32 ArrayIt_deref(u32 self) {
    const u32 n = ld(self);
    return n ? n + kNodeData : json().null_value;
}
u32 ArrayIt_ne(u32 self, u32 other) { return ld(self) != ld(other); }

// ---- Object and its iterator

u32 Object_C1(u32 self) {
    st(self, 0);
    return self;
}
u32 Object_C1_copy(PsvContext* ctx, u32 self, u32 other) {
    st(self, 0);
    object_copy(ctx, self, other);
    return self;
}
u32 Object_D1(PsvContext* ctx, u32 self) {
    object_clear(ctx, self);
    return self;
}
u32 Object_assign(PsvContext* ctx, u32 self, u32 other) {
    if (self == other) return self;
    object_clear(ctx, self);
    object_copy(ctx, self, other);
    return self;
}
u32 Object_index(PsvContext* ctx, u32 self, u32 key) {
    const nx::string k = str_host(key);
    if (const u32 node = object_find(self, k.data(), static_cast<u32>(k.size()))) return node + kNodeData + kPairValue;
    const u32 node = list_append(ctx, self, kPairSize);
    if (!node) return json().null_value;
    str_set(ctx, node + kNodeData, k.data(), static_cast<u32>(k.size()));
    return node + kNodeData + kPairValue;
}
void Object_begin(u32 out, u32 self) { st(out, list_first(self)); }
void Object_end(u32 out, u32) { st(out, 0); }
void Object_find(u32 out, u32 self, u32 key) {
    const nx::string k = str_host(key);
    st(out, object_find(self, k.data(), static_cast<u32>(k.size())));
}
u32 Object_size(u32 self) { return list_count(self); }
u32 Object_empty(u32 self) { return list_count(self) == 0; }
u32 ObjectIt_C1(u32 self) {
    st(self, 0);
    return self;
}
u32 ObjectIt_D1(u32 self) { return self; }
u32 ObjectIt_assign(u32 self, u32 other) {
    st(self, ld(other));
    return self;
}
u32 ObjectIt_next(u32 self) {
    if (const u32 n = ld(self)) st(self, ld(n));
    return self;
}
u32 ObjectIt_ne(u32 self, u32 other) { return ld(self) != ld(other); }
u32 ObjectIt_arrow(u32 self) {
    const u32 n = ld(self);
    return n ? n + kNodeData : 0;
}

// ---- Value

u32 Value_C1(u32 self) {
    value_init(self);
    return self;
}
u32 Value_C1_type(PsvContext* ctx, u32 self, u32 type) {
    value_init(self);
    if (type == kString || type == kArray || type == kObject) value_make_container(ctx, self, type);
    else if (type <= kObject) st(self + kTypeAt, type);
    return self;
}
u32 Value_C1_bool(u32 self, u32 b) {
    value_init(self);
    st(self + kTypeAt, kBoolean), st(self + kPayload, (b & 0xFF) ? 1 : 0);
    return self;
}
u32 Value_C1_int(u32 self, u64 x) {
    value_init(self);
    st(self + kTypeAt, kInteger);
    std::memcpy(memory().host(self + kPayload), &x, 8);
    return self;
}
u32 Value_C1_uint(u32 self, u64 x) {
    value_init(self);
    st(self + kTypeAt, kUInteger);
    std::memcpy(memory().host(self + kPayload), &x, 8);
    return self;
}
u32 Value_C1_real(u32 self, f64 x) {
    value_init(self);
    st(self + kTypeAt, kReal);
    std::memcpy(memory().host(self + kPayload), &x, 8);
    return self;
}
u32 Value_C1_string(PsvContext* ctx, u32 self, u32 s) {
    value_init(self);
    if (const u32 held = value_make_container(ctx, self, kString)) str_set(ctx, held, str_text(s), str_len(s));
    return self;
}
u32 Value_C1_array(PsvContext* ctx, u32 self, u32 a) {
    value_init(self);
    if (const u32 held = value_make_container(ctx, self, kArray)) array_copy(ctx, held, a);
    return self;
}
u32 Value_C1_object(PsvContext* ctx, u32 self, u32 o) {
    value_init(self);
    if (const u32 held = value_make_container(ctx, self, kObject)) object_copy(ctx, held, o);
    return self;
}
u32 Value_C1_copy(PsvContext* ctx, u32 self, u32 other) {
    value_init(self);
    value_copy(ctx, self, other);
    return self;
}
u32 Value_D1(PsvContext* ctx, u32 self) {
    value_clear(ctx, self);
    return self;
}
u32 Value_assign(PsvContext* ctx, u32 self, u32 other) {
    if (self == other) return self;
    value_clear(ctx, self);
    value_copy(ctx, self, other);
    return self;
}
void Value_clear(PsvContext* ctx, u32 self) { value_clear(ctx, self); }
u32 Value_getType(u32 self) { return value_type(self); }

u32 null_for(u32 type) {
    make_statics();
    Json& j = json();
    switch (type) {
    case kString: return j.null_string;
    case kArray: return j.null_array;
    case kObject: return j.null_object;
    default: return j.null_number;
    }
}
u32 held_or_null(u32 self, u32 type) {
    const u32 held = ld(self + kPayload);
    return value_type(self) == type && held ? held : null_for(type);
}
u32 Value_getString(u32 self) { return held_or_null(self, kString); }
u32 Value_getArray(u32 self) { return held_or_null(self, kArray); }
u32 Value_getObject(u32 self) { return held_or_null(self, kObject); }
u32 Value_getInteger(u32 self) {
    const u32 t = value_type(self);
    return t == kInteger || t == kUInteger ? self + kPayload : null_for(kInteger);
}
u32 Value_getUInteger(u32 self) { return Value_getInteger(self); }
u32 Value_getReal(u32 self) { return value_type(self) == kReal ? self + kPayload : null_for(kReal); }
u32 Value_getBoolean(u32 self) { return value_type(self) == kBoolean ? self + kPayload : null_for(kBoolean); }

u32 value_at(u32 self, u32 index) {
    make_statics();
    if (value_type(self) != kArray) return json().null_value;
    u32 n = list_first(ld(self + kPayload));
    for (u32 i = 0; n && i < index; ++i) n = ld(n);
    return n ? n + kNodeData : json().null_value;
}
u32 value_key(u32 self, const nx::string& key) {
    make_statics();
    if (value_type(self) != kObject) return json().null_value;
    const u32 node = object_find(ld(self + kPayload), key.data(), static_cast<u32>(key.size()));
    return node ? node + kNodeData + kPairValue : json().null_value;
}
u32 Value_index(u32 self, u32 index) { return value_at(self, index); }
u32 Value_key_string(u32 self, u32 key) { return value_key(self, str_host(key)); }
u32 Value_key_text(u32 self, u32 key) { return value_key(self, guest_cstring(key)); }
i32 Value_count(u32 self) {
    const u32 t = value_type(self);
    if (t == kArray || t == kObject) return static_cast<i32>(list_count(ld(self + kPayload)));
    return 0;
}
void Value_toString(PsvContext* ctx, u32 self, u32 out) {
    nx::string s;
    write_value(s, self, true);
    str_set(ctx, out, s.data(), static_cast<u32>(s.size()));
}
i32 Value_serialize(PsvContext* ctx, u32 self, u32 out) {
    nx::string s;
    write_value(s, self, false);
    return str_set(ctx, out, s.data(), static_cast<u32>(s.size())) ? 0 : kNoMem;
}

// ---- Parser

i32 Parser_parse(PsvContext* ctx, u32 dst, u32 src, u32 size) {
    if (!json().initialized) return kNoInit;
    if (!src || !memory().valid(src, size)) return kInvalidArgument;
    value_clear(ctx, dst);
    const char* text = reinterpret_cast<const char*>(memory().host(src));
    Parser p{ctx, text, text + size};
    // (a terminating null ends the text early)
    if (const void* nul = std::memchr(text, 0, size)) p.end = static_cast<const char*>(nul);
    const bool ok = p.value(dst, 0);
    p.ws();
    if (ok && p.p == p.end) return 0;
    value_clear(ctx, dst);
    return p.nomem ? kNoMem : kParseInvalidChar;
}

const Function kFunctions[] = {
    HLE_FUNCTION(0x5405EE04, Initializer_C1),
    HLE_FUNCTION(0xA19AA8B4, Initializer_D1),
    HLE_FUNCTION(0xA3EA44E9, Initializer_initialize),
    HLE_FUNCTION(0x30AD8C6A, Initializer_terminate),
    HLE_FUNCTION(0x5EC4F6CB, MemAllocator_C2),
    HLE_FUNCTION(0x47A56DF5, MemAllocator_D2),
    HLE_FUNCTION(0x38C51D0C, MemAllocator_notifyError),

    HLE_FUNCTION(0x692125B6, String_C1),
    HLE_FUNCTION(0x70703FC7, String_C1_text),
    HLE_FUNCTION(0xB70F12A8, String_D1),
    HLE_FUNCTION(0xC3D22030, String_assign),
    HLE_FUNCTION(0xA15F63C1, String_size),
    HLE_FUNCTION(0x35F974C1, String_size),  // (length)
    HLE_FUNCTION(0x327D74CE, String_c_str),
    HLE_FUNCTION(0x267A86A3, String_compare),
    HLE_FUNCTION(0xAE9ECB11, String_empty),
    HLE_FUNCTION(0x617479FB, String_clear),
    HLE_FUNCTION(0x4DBF6086, String_append),
    HLE_FUNCTION(0xECE95143, String_append_n),

    HLE_FUNCTION(0x8090B2D1, Array_C1),
    HLE_FUNCTION(0xAA0C1A25, Array_D1),
    HLE_FUNCTION(0x82074D93, Array_begin),
    HLE_FUNCTION(0x0F533297, Array_end),
    HLE_FUNCTION(0x9DC07B95, Array_size),
    HLE_FUNCTION(0x7E275CE0, Array_empty),
    HLE_FUNCTION(0x21AE1E00, Array_push_back),
    HLE_FUNCTION(0x866C0131, ArrayIt_D1),
    HLE_FUNCTION(0x24413A1E, ArrayIt_next),
    HLE_FUNCTION(0x81EF83DC, ArrayIt_deref),
    HLE_FUNCTION(0x33BE3C5D, ArrayIt_deref),  // (->)
    HLE_FUNCTION(0x5467A38D, ArrayIt_ne),

    HLE_FUNCTION(0xB669382C, Object_C1),
    HLE_FUNCTION(0xD7070313, Object_C1_copy),
    HLE_FUNCTION(0x3CF6AF04, Object_D1),
    HLE_FUNCTION(0x63E9610C, Object_assign),
    HLE_FUNCTION(0x8F2E1720, Object_index),
    HLE_FUNCTION(0x192FE2BF, Object_begin),
    HLE_FUNCTION(0xCB50C919, Object_end),
    HLE_FUNCTION(0x396D0910, Object_find),
    HLE_FUNCTION(0x5630DAD9, Object_size),
    HLE_FUNCTION(0x168162C2, Object_empty),
    HLE_FUNCTION(0x2E89608C, ObjectIt_C1),
    HLE_FUNCTION(0x2A47D045, ObjectIt_D1),
    HLE_FUNCTION(0x52D081C4, ObjectIt_assign),
    HLE_FUNCTION(0xAEC3AD59, ObjectIt_next),
    HLE_FUNCTION(0x7B74D109, ObjectIt_ne),
    HLE_FUNCTION(0x8FA78056, ObjectIt_arrow),

    HLE_FUNCTION(0x7A9DECC0, Value_C1),
    HLE_FUNCTION(0xB7D13E29, Value_C1_type),
    HLE_FUNCTION(0x7A6FE666, Value_C1_bool),
    HLE_FUNCTION(0x1B23D989, Value_C1_int),
    HLE_FUNCTION(0xF785608C, Value_C1_uint),
    HLE_FUNCTION(0xAD01DFC1, Value_C1_real),
    HLE_FUNCTION(0xB7582884, Value_C1_string),
    HLE_FUNCTION(0x0E917571, Value_C1_array),
    HLE_FUNCTION(0xE1DFAB01, Value_C1_object),
    HLE_FUNCTION(0xCC201CD2, Value_C1_copy),
    HLE_FUNCTION(0xE07648EC, Value_D1),
    HLE_FUNCTION(0x705ED2BB, Value_assign),
    HLE_FUNCTION(0xC9813AC7, Value_clear),
    HLE_FUNCTION(0xA95EF0EC, Value_getType),
    HLE_FUNCTION(0xBC3391BE, Value_getString),
    HLE_FUNCTION(0x000926DB, Value_getArray),
    HLE_FUNCTION(0xF793E30D, Value_getObject),
    HLE_FUNCTION(0x6861C3CC, Value_getInteger),
    HLE_FUNCTION(0x35EAAEDA, Value_getUInteger),
    HLE_FUNCTION(0xFC0C699A, Value_getReal),
    HLE_FUNCTION(0x7DDFCAE2, Value_getBoolean),
    HLE_FUNCTION(0xC1FE4E8D, Value_index),       // (getValue(index))
    HLE_FUNCTION(0x3CE5DEC7, Value_index),       // (operator[](index))
    HLE_FUNCTION(0xAA94ACDD, Value_key_string),  // (getValue(key))
    HLE_FUNCTION(0x910E18C0, Value_key_string),  // (operator[](key))
    HLE_FUNCTION(0xD5B3E509, Value_key_text),    // (operator[](const char*))
    HLE_FUNCTION(0xF14E07A2, Value_count),
    HLE_FUNCTION(0x39748AF8, Value_toString),
    HLE_FUNCTION(0xCFC4D75F, Value_serialize),

    HLE_FUNCTION(0x59B1A5B8, Parser_parse),
};

}  // namespace

void register_json() {
    {
        Json& j = json();
        Lock held(j.lock);
        j.initialized = false;
        j.allocator = j.user_data = 0;
        j.null_value = j.null_string = j.null_array = j.null_object = j.null_number = j.empty_text = j.base_vtable = 0;
    }
    register_library({"SceLibJson", kSceLibJson, kFunctions, {}, true});
}

}  // namespace psvrecore::runtime::hle
