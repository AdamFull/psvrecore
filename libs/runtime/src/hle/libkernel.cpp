// SceLibKernel's C library helpers (sceClib*), formatted output, and the stack protector's guard and failure.

#include <cstdio>
#include <cstring>
#include <random>

#include "hle_common.hpp"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceLibKernel = 0xCAE9ACE6, kSceLibRng = 0xF9AC7CF8;

// Reads the arguments of a formatted-output call: from a variadic call's registers and stack, or from a guest
// va_list (a pointer into the argument area; 8-byte values 8-byte aligned).
struct FormatArgs {
    Variadic* variadic = nullptr;
    u32 va = 0;

    u32 word() {
        if (variadic) return variadic->word();
        const u32 v = psv_ld32(va);
        va += 4;
        return v;
    }
    u64 dword() {
        if (variadic) return variadic->dword();
        va = (va + 7) & ~7u;
        const u64 v = psv_ld64(va);
        va += 8;
        return v;
    }
};

// printf-style formatting with guest arguments (int and long are 32-bit, long long 64-bit, pointers 32-bit,
// doubles 64-bit).
nx::string format(nx::string_view fmt, FormatArgs& args) {
    nx::string out;
    char spec[32];
    char buffer[512];
    for (usize i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') {
            out += fmt[i];
            continue;
        }
        usize n = 0;
        spec[n++] = '%';
        ++i;
        while (i < fmt.size() && nx::string_view("-+ #0").find(fmt[i]) != nx::string_view::npos && n < 8) spec[n++] = fmt[i++];
        auto number = [&]() {
            if (i < fmt.size() && fmt[i] == '*') {
                const i32 v = static_cast<i32>(args.word());
                n += static_cast<usize>(std::snprintf(spec + n, sizeof spec - n, "%d", v));
                ++i;
                return;
            }
            while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9' && n < 20) spec[n++] = fmt[i++];
        };
        number();
        if (i < fmt.size() && fmt[i] == '.') {
            spec[n++] = fmt[i++];
            number();
        }
        int longs = 0;
        bool half = false;
        while (i < fmt.size() && nx::string_view("hlLqjzt").find(fmt[i]) != nx::string_view::npos) {
            const char c = fmt[i++];
            if (c == 'l') ++longs;
            else if (c == 'q' || c == 'j' || c == 'L') longs = 2;
            else if (c == 'h') half = true;
        }
        if (i >= fmt.size()) break;
        const char conv = fmt[i];
        int written = 0;
        switch (conv) {
            case 'd':
            case 'i':
            case 'u':
            case 'x':
            case 'X':
            case 'o': {
                if (longs >= 2) {
                    spec[n++] = 'l';
                    spec[n++] = 'l';
                    spec[n++] = conv;
                    spec[n] = 0;
                    const u64 v = args.dword();
                    written = (conv == 'd' || conv == 'i') ? std::snprintf(buffer, sizeof buffer, spec, static_cast<long long>(v))
                                                           : std::snprintf(buffer, sizeof buffer, spec, static_cast<unsigned long long>(v));
                } else {
                    spec[n++] = conv;
                    spec[n] = 0;
                    u32 v = args.word();
                    (void)half;
                    written = (conv == 'd' || conv == 'i') ? std::snprintf(buffer, sizeof buffer, spec, static_cast<int>(v))
                                                           : std::snprintf(buffer, sizeof buffer, spec, static_cast<unsigned>(v));
                }
                break;
            }
            case 'c': {
                spec[n++] = 'c';
                spec[n] = 0;
                written = std::snprintf(buffer, sizeof buffer, spec, static_cast<int>(args.word() & 0xFF));
                break;
            }
            case 's': {
                spec[n++] = 's';
                spec[n] = 0;
                const u32 p = args.word();
                const nx::string s = p ? memory().string(p, 1 << 16) : nx::string("(null)");
                const int need = std::snprintf(nullptr, 0, spec, s.c_str());
                nx::string tmp(static_cast<usize>(need > 0 ? need : 0), '\0');
                if (need > 0) std::snprintf(tmp.data(), tmp.size() + 1, spec, s.c_str());
                out += tmp;
                continue;
            }
            case 'p': {
                written = std::snprintf(buffer, sizeof buffer, "0x%08x", args.word());
                break;
            }
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
            case 'a':
            case 'A': {
                spec[n++] = conv;
                spec[n] = 0;
                const u64 bits = args.dword();
                double d;
                std::memcpy(&d, &bits, 8);
                written = std::snprintf(buffer, sizeof buffer, spec, d);
                break;
            }
            case 'n': args.word(); continue;  // (not supported: nothing written)
            case '%': out += '%'; continue;
            default:
                out.append(spec, n);
                out += conv;
                continue;
        }
        if (written > 0) out.append(buffer, std::min<usize>(static_cast<usize>(written), sizeof buffer - 1));
    }
    return out;
}

i32 put_formatted(u32 buffer, u32 size, const nx::string& text) {
    if (size) {
        if (!writable(buffer, size)) return err::kFault;
        const u32 n = std::min<u32>(size - 1, static_cast<u32>(text.size()));
        std::memcpy(memory().host(buffer), text.data(), n);
        memory().host(buffer)[n] = 0;
    }
    return static_cast<i32>(text.size());
}

i32 sceClibPrintf(Ptr<const char> fmt, Variadic va) {
    FormatArgs args{&va, 0};
    const nx::string text = format(str(fmt, 1 << 16), args);
    runtime().files().write(runtime().files().stdout_fd(), text.data(), static_cast<u32>(text.size()));
    return static_cast<i32>(text.size());
}

i32 sceClibVprintf(Ptr<const char> fmt, u32 va_list) {
    FormatArgs args{nullptr, va_list};
    const nx::string text = format(str(fmt, 1 << 16), args);
    runtime().files().write(runtime().files().stdout_fd(), text.data(), static_cast<u32>(text.size()));
    return static_cast<i32>(text.size());
}

i32 sceClibSnprintf(u32 buffer, u32 size, Ptr<const char> fmt, Variadic va) {
    FormatArgs args{&va, 0};
    return put_formatted(buffer, size, format(str(fmt, 1 << 16), args));
}

i32 sceClibVsnprintf(u32 buffer, u32 size, Ptr<const char> fmt, u32 va_list) {
    FormatArgs args{nullptr, va_list};
    return put_formatted(buffer, size, format(str(fmt, 1 << 16), args));
}

// ---- memory and strings (on guest memory; the guest's own checks are its business, as in its libc)

u32 sceClibMemset(u32 dst, i32 c, u32 n) {
    if (n) std::memset(memory().host(dst), c, n);
    return dst;
}
u32 sceClibMemcpy(u32 dst, u32 src, u32 n) {
    if (n) std::memcpy(memory().host(dst), memory().host(src), n);
    return dst;
}
u32 sceClibMemmove(u32 dst, u32 src, u32 n) {
    if (n) std::memmove(memory().host(dst), memory().host(src), n);
    return dst;
}
i32 sceClibMemcmp(u32 a, u32 b, u32 n) { return n ? std::memcmp(memory().host(a), memory().host(b), n) : 0; }
u32 sceClibMemchr(u32 p, i32 c, u32 n) {
    if (!n) return 0;
    const void* hit = std::memchr(memory().host(p), c & 0xFF, n);
    return hit ? guest_address(memory(), hit) : 0;
}

const char* cstr(u32 p) { return reinterpret_cast<const char*>(memory().host(p)); }
char* mstr(u32 p) { return reinterpret_cast<char*>(memory().host(p)); }

// The calling thread's call frames: not walked here (guest code runs as host code; its frames are not the guest's),
// so none are reported.
i32 sceKernelBacktraceSelf(Ptr<u8> /*frames*/, u32 /*size*/, Ptr<u32> count, i32 /*mode*/) {
    if (count && writable(count.address, 4)) *count = 0;
    return 0;
}
// Random bytes (at most 64 at a time, as the console's), from the host's random device.
i32 sceKernelGetRandomNumber(Ptr<u8> out, u32 size) {
    if (size > 64) return err::kInvalidArgument;
    if (size && (!out || !writable(out.address, size))) return err::kIllegalAddress;
    static nx::mutex lock;
    static std::random_device device;
    nx::scoped_lock<nx::mutex> held(lock);
    for (u32 i = 0; i < size; ++i) out.get()[i] = static_cast<u8>(device());
    return 0;
}

u32 sceClibStrncpy(u32 dst, u32 src, u32 n) {
    std::strncpy(mstr(dst), cstr(src), n);
    return dst;
}
i32 sceClibStrcmp(u32 a, u32 b) { return std::strcmp(cstr(a), cstr(b)); }
i32 sceClibStrncmp(u32 a, u32 b, u32 n) { return std::strncmp(cstr(a), cstr(b), n); }
i32 sceClibStrncasecmp(u32 a, u32 b, u32 n) {
    const char *x = cstr(a), *y = cstr(b);
    for (u32 i = 0; i < n; ++i) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : static_cast<u8>(c); };
        const i32 cx = lower(x[i]), cy = lower(y[i]);
        if (cx != cy || !cx) return cx - cy;
    }
    return 0;
}
// A character's class bits (the C library's table: 0x01 upper, 0x02 lower, 0x04 digit, 0x08 space, 0x10
// punctuation, 0x20 control, 0x40 hexadecimal digit, 0x80 blank), for the 7-bit characters; 0 above.
u32 sceClibLookCtypeTable(u32 c) {
    c &= 0xFF;
    if (c > 0x7F) return 0;
    u32 bits = 0;
    if (c >= 'A' && c <= 'Z') bits |= 0x01;
    if (c >= 'a' && c <= 'z') bits |= 0x02;
    if (c >= '0' && c <= '9') bits |= 0x04;
    if (c == ' ' || (c >= 0x09 && c <= 0x0D)) bits |= 0x08;
    if (c > 0x20 && c < 0x7F && !(bits & 0x07)) bits |= 0x10;
    if (c < 0x20 || c == 0x7F) bits |= 0x20;
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) bits |= 0x40;
    if (c == ' ' || c == 0x09) bits |= 0x80;
    return bits;
}
// strtoll: base 0 (the prefix decides), or 2-36; the end in *end; out of range, the nearest limit.
i64 sceClibStrtoll(u32 text, Ptr<u32> end, i32 base) {
    const char* s = cstr(text);
    u32 i = 0;
    while (s[i] == ' ' || (s[i] >= 0x09 && s[i] <= 0x0D)) ++i;
    bool negative = false;
    if (s[i] == '+' || s[i] == '-') negative = s[i++] == '-';
    if ((base == 0 || base == 16) && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        const char h = s[i + 2];
        if ((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F')) i += 2, base = 16;
    }
    if (base == 0) base = s[i] == '0' ? 8 : 10;
    u64 value = 0;
    bool any = false, over = false;
    const u64 limit = negative ? 0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFull;
    for (;; ++i) {
        const char c = s[i];
        const i32 d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99;
        if (base < 2 || base > 36 || d >= base) break;
        any = true;
        if (value > (limit - static_cast<u64>(d)) / static_cast<u64>(base)) over = true;
        else value = value * static_cast<u64>(base) + static_cast<u64>(d);
    }
    if (end && writable(end.address, 4)) *end = any ? text + i : text;
    if (over) return negative ? static_cast<i64>(0x8000000000000000ull) : 0x7FFFFFFFFFFFFFFFll;
    return negative ? static_cast<i64>(0 - value) : static_cast<i64>(value);
}

u32 sceClibStrnlen(u32 s, u32 max) {
    u32 n = 0;
    while (n < max && cstr(s)[n]) ++n;
    return n;
}
u32 sceClibStrchr(u32 s, i32 c) {
    const char* hit = std::strchr(cstr(s), c & 0xFF);
    return hit ? guest_address(memory(), hit) : 0;
}
u32 sceClibStrrchr(u32 s, i32 c) {
    const char* hit = std::strrchr(cstr(s), c & 0xFF);
    return hit ? guest_address(memory(), hit) : 0;
}
u32 sceClibStrlcpy(u32 dst, u32 src, u32 size) {
    const usize len = std::strlen(cstr(src));
    if (size) {
        const usize n = std::min<usize>(len, size - 1);
        std::memcpy(mstr(dst), cstr(src), n);
        mstr(dst)[n] = 0;
    }
    return static_cast<u32>(len);
}
u32 sceClibStrncat(u32 dst, u32 src, u32 n) {
    std::strncat(mstr(dst), cstr(src), n);
    return dst;
}
u32 sceClibStrlcat(u32 dst, u32 src, u32 size) {
    const usize dlen = sceClibStrnlen(dst, size), slen = std::strlen(cstr(src));
    if (dlen == size) return static_cast<u32>(size + slen);
    const usize n = std::min<usize>(slen, size - dlen - 1);
    std::memcpy(mstr(dst) + dlen, cstr(src), n);
    mstr(dst)[dlen + n] = 0;
    return static_cast<u32>(dlen + slen);
}

// ---- failures

// The console's OpenPSID (16 bytes identifying it): one fixed value here, the same on every run.
i32 sceKernelGetOpenPsId(u32 id) {
    if (!id || !memory().valid(id, 16)) return err::kIllegalAddress;
    static constexpr u8 kId[16] = {'p', 's', 'v', 'r', 'e', 'c', 'o', 'r', 'e', 0, 0, 0, 0, 0, 0, 1};
    std::memcpy(memory().host(id), kId, sizeof kId);
    return 0;
}
i32 sceClibAbort() {
    nx::loge(logs::kernel, "the guest called sceClibAbort()");
    runtime().exit_process(134);
    kernel().exit_thread(134, false);
}

void stack_chk_fail(PsvContext* ctx) {
    nx::loge(logs::kernel, "the guest's stack protector found its stack overwritten (lr 0x{:08x}, sp 0x{:08x})", ctx->r[14],
             ctx->r[13]);
    runtime().exit_process(134);
    kernel().exit_thread(134, false);
}

// Integer division by zero: the console's handlers return, and the division's result is what they return.
i32 sce_aeabi_idiv0(i32) { return 0; }
i64 sce_aeabi_ldiv0(i64) { return 0; }

void init_stack_guard(u8* host, u32) {
    // Any value works; a fixed one keeps runs alike.
    const u32 guard = 0x5CA1AB1Eu;
    std::memcpy(host, &guard, 4);
}

const Function kLibKernel[] = {
    HLE_FUNCTION(0xFA26BC62, sceClibPrintf),
    HLE_FUNCTION(0x8CBA03D5, sceClibSnprintf),
    HLE_FUNCTION(0xFA6BE467, sceClibVsnprintf),
    HLE_FUNCTION(0x632980D7, sceClibMemset),
    HLE_FUNCTION(0x14E9DBD7, sceClibMemcpy),
    HLE_FUNCTION(0x2E3B02A1, sceClibMemcpy),  // (sceClibMemcpy_safe)
    HLE_FUNCTION(0x736753C8, sceClibMemmove),
    HLE_FUNCTION(0x9CC2BFDF, sceClibMemcmp),
    HLE_FUNCTION(0x407D6153, sceClibMemchr),
    HLE_FUNCTION(0xC458D60A, sceClibStrncpy),
    HLE_FUNCTION(0xA2FB4D9D, sceClibStrcmp),
    HLE_FUNCTION(0x660D1F6D, sceClibStrncmp),
    HLE_FUNCTION(0xB54C0BE4, sceClibStrncasecmp),
    HLE_FUNCTION(0xD8EBBB7E, sceClibLookCtypeTable),
    HLE_FUNCTION(0x2E581B88, sceClibStrtoll),
    HLE_FUNCTION(0xD16C03B0, sceKernelBacktraceSelf),
    HLE_FUNCTION(0x5EA3B6CE, sceClibVprintf),
    HLE_FUNCTION(0xAC595E68, sceClibStrnlen),
    HLE_FUNCTION(0x614076B7, sceClibStrchr),
    HLE_FUNCTION(0x6E728AAE, sceClibStrrchr),
    HLE_FUNCTION(0x2CDFCD1C, sceClibStrlcpy),
    HLE_FUNCTION(0xA37E6383, sceClibStrncat),
    HLE_FUNCTION(0x70CBC2D5, sceClibStrlcat),
    HLE_FUNCTION(0x2F2C6046, sceClibAbort),
    HLE_FUNCTION(0x261E2C34, sceKernelGetOpenPsId),
    Function{0x37691BF8, "__stack_chk_fail", &entry<&stack_chk_fail>},
    Function{0x4373B548, "__sce_aeabi_idiv0", &entry<&sce_aeabi_idiv0>},
    Function{0xFB235848, "__sce_aeabi_ldiv0", &entry<&sce_aeabi_ldiv0>},
};

const Function kLibRng[] = {HLE_FUNCTION(0xB2700165, sceKernelGetRandomNumber)};

const Variable kLibKernelVariables[] = {
    {0x93B8AA67, "__stack_chk_guard", 4, &init_stack_guard},
};

}  // namespace

void register_libkernel() {
    register_library({"SceLibKernel", kSceLibKernel, kLibKernel, kLibKernelVariables});
    register_library({"SceLibRng", kSceLibRng, kLibRng});
}

void register_system_libraries() {
    register_threadmgr();
    register_sysmem();
    register_iofilemgr();
    register_processmgr();
    register_libkernel();
    register_modulemgr();
    register_rtc();
    register_fios2();
    register_ctrl();
    register_audioout();
    register_touch();
    register_systemgesture();
    register_mspace();
    register_jpegenc();
    register_motion();
    register_apputil();
    register_ngs();
    register_avplayer();
    register_system();
    register_fiber();
    register_np();
    register_online();
    register_net();
    register_psn();
    register_services();
    register_videodec();
    register_commondialog();
}

}  // namespace psvrecore::runtime::hle
