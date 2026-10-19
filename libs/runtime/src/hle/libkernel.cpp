// SceLibKernel's C library helpers (sceClib*), formatted output, and the stack protector's guard and failure.

#include <cstdio>

#include "hle_common.hpp"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceLibKernel = 0xCAE9ACE6;

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

u32 sceClibStrncpy(u32 dst, u32 src, u32 n) {
    std::strncpy(mstr(dst), cstr(src), n);
    return dst;
}
i32 sceClibStrcmp(u32 a, u32 b) { return std::strcmp(cstr(a), cstr(b)); }
i32 sceClibStrncmp(u32 a, u32 b, u32 n) { return std::strncmp(cstr(a), cstr(b), n); }
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
    HLE_FUNCTION(0xAC595E68, sceClibStrnlen),
    HLE_FUNCTION(0x614076B7, sceClibStrchr),
    HLE_FUNCTION(0x6E728AAE, sceClibStrrchr),
    HLE_FUNCTION(0x2CDFCD1C, sceClibStrlcpy),
    HLE_FUNCTION(0xA37E6383, sceClibStrncat),
    HLE_FUNCTION(0x70CBC2D5, sceClibStrlcat),
    HLE_FUNCTION(0x2F2C6046, sceClibAbort),
    Function{0x37691BF8, "__stack_chk_fail", &entry<&stack_chk_fail>},
    Function{0x4373B548, "__sce_aeabi_idiv0", &entry<&sce_aeabi_idiv0>},
    Function{0xFB235848, "__sce_aeabi_ldiv0", &entry<&sce_aeabi_ldiv0>},
};

const Variable kLibKernelVariables[] = {
    {0x93B8AA67, "__stack_chk_guard", 4, &init_stack_guard},
};

}  // namespace

void register_libkernel() { register_library({"SceLibKernel", kSceLibKernel, kLibKernel, kLibKernelVariables}); }

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
    register_apputil();
    register_ngs();
    register_avplayer();
    register_system();
    register_fiber();
    register_np();
    register_videodec();
    register_commondialog();
}

}  // namespace psvrecore::runtime::hle
