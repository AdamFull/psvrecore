// Host implementations of guest functions (guest.h, psv_host_*; psvrecomp --replace; ARCHITECTURE.md, "Host
// implementations of guest functions").
//
// Each is a function of the generated code's signature, called where the module's code calls the guest function. It
// does to guest memory what the guest function does, puts the guest function's result in r0, and writes nothing else of
// the context: r1-r3, r12, lr, the flags and the floating-point registers are what the AAPCS lets a callee leave
// changed, and their values after the guest's own code (its pointers advanced, its loop counters) are not part of
// what a C caller may read. The results are the checked SceLibc build's (tools/psvrecomp/host_functions.tsv): memcmp and strcmp
// return -1, 0 or 1 and compare unsigned bytes, wchar_t is 16 bits, and strncpy copies byte by byte as the guest's loop
// does, so that overlapping buffers end up the same.
//
// Reads go no further than the guest's own: past the end of a string or the first difference the host reads at most
// the rest of an aligned 8-byte word, which lies on the same page as the byte the guest reads (guest memory is mapped
// by pages, psv_membase page-aligned), so a string at the end of mapped memory does not fault here when it does not
// there.

#include <cstring>

#include "psvrecore/recomp/guest.h"

namespace {

inline uint8_t* host(uint32_t address) { return PSV_HOST(address); }

inline uint64_t load64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

inline bool word_aligned(const uint8_t* p) { return (reinterpret_cast<uintptr_t>(p) & 7u) == 0; }

constexpr uint64_t kOnes = 0x0101010101010101ull;
constexpr uint64_t kHighs = 0x8080808080808080ull;
inline bool has_zero_byte(uint64_t v) { return ((v - kOnes) & ~v & kHighs) != 0; }

// The bytes before the first NUL at `s` (at most `limit` of them looked at: strnlen).
uint32_t string_length(const uint8_t* s, uint32_t limit) {
    uint32_t n = 0;
    while (n < limit && !word_aligned(s + n)) {
        if (s[n] == 0) return n;
        ++n;
    }
    while (limit - n >= 8 && !has_zero_byte(load64(s + n))) n += 8;  // (aligned: on the page of its first byte)
    while (n < limit && s[n] != 0) ++n;
    return n;
}

}  // namespace

extern "C" {

void psv_host_memcpy(PsvContext* ctx, uint32_t ret) {
    (void)ret;
    const uint32_t d = ctx->r[0], s = ctx->r[1], n = ctx->r[2];
    if (n != 0 && d != s) std::memmove(host(d), host(s), n);
}

int psv_host_memcpy_exact(const PsvContext* ctx) {
    const uint32_t d = ctx->r[0], s = ctx->r[1], n = ctx->r[2];
    // (d - s >= n, unsigned: no overlap with the destination above the source; d <= s: at or below it)
    return n == 0 || d <= s || d - s >= n;
}

void psv_host_memmove(PsvContext* ctx, uint32_t ret) {
    (void)ret;
    const uint32_t d = ctx->r[0], s = ctx->r[1], n = ctx->r[2];
    if (n != 0 && d != s) std::memmove(host(d), host(s), n);
}

void psv_host_memset(PsvContext* ctx, uint32_t ret) {
    (void)ret;
    const uint32_t d = ctx->r[0], n = ctx->r[2];
    if (n != 0) std::memset(host(d), static_cast<int>(ctx->r[1] & 0xFFu), n);
}

void psv_host_memcmp(PsvContext* ctx, uint32_t ret) {
    (void)ret;
    const uint8_t* a = host(ctx->r[0]);
    const uint8_t* b = host(ctx->r[1]);
    const uint32_t n = ctx->r[2];
    uint32_t i = 0;
    // (8 bytes at a time; 8 that cross a page, which the guest's byte loop might not reach past a difference, byte by
    // byte)
    while (n - i >= 8) {
        const uintptr_t pa = reinterpret_cast<uintptr_t>(a + i) & 0xFFFu, pb = reinterpret_cast<uintptr_t>(b + i) & 0xFFFu;
        if (pa > 0xFF8u || pb > 0xFF8u) {
            for (const uint32_t end = i + 8; i < end; ++i)
                if (a[i] != b[i]) {
                    ctx->r[0] = a[i] > b[i] ? 1u : 0xFFFFFFFFu;
                    return;
                }
            continue;
        }
        if (load64(a + i) != load64(b + i)) break;
        i += 8;
    }
    for (; i < n; ++i)
        if (a[i] != b[i]) {
            ctx->r[0] = a[i] > b[i] ? 1u : 0xFFFFFFFFu;
            return;
        }
    ctx->r[0] = 0;
}

void psv_host_strlen(PsvContext* ctx, uint32_t ret) {
    (void)ret;
    ctx->r[0] = string_length(host(ctx->r[0]), 0xFFFFFFFFu);
}

void psv_host_strcmp(PsvContext* ctx, uint32_t ret) {
    (void)ret;
    const uint8_t* a = host(ctx->r[0]);
    const uint8_t* b = host(ctx->r[1]);
    for (uint32_t i = 0;; ++i) {
        if (a[i] != b[i]) {
            ctx->r[0] = a[i] > b[i] ? 1u : 0xFFFFFFFFu;
            return;
        }
        if (a[i] == 0) break;
    }
    ctx->r[0] = 0;
}

void psv_host_strncpy(PsvContext* ctx, uint32_t ret) {
    (void)ret;
    const uint32_t d = ctx->r[0], s = ctx->r[1], n = ctx->r[2];
    uint8_t* dst = host(d);
    const uint8_t* src = host(s);
    // Buffers apart (the destination's n bytes and the source's n): the copy and the padding at once. Else the guest's
    // loop, byte by byte: each source byte read (twice: tested, then loaded) before it is stored, so that a source the
    // copy runs into reads what the copy wrote there, as on the console.
    if (d - s >= n && s - d >= n) {
        const uint32_t len = string_length(src, n);
        std::memcpy(dst, src, len);
        std::memset(dst + len, 0, n - len);
        return;
    }
    uint32_t i = 0;
    for (; i < n && src[i] != 0; ++i) dst[i] = src[i];
    for (; i < n; ++i) dst[i] = 0;
}

void psv_host_wcslen(PsvContext* ctx, uint32_t ret) {
    (void)ret;
    const uint32_t s = ctx->r[0];
    uint32_t p = s;
    for (;;) {
        uint16_t c;
        std::memcpy(&c, host(p), 2);
        if (c == 0) break;
        p += 2;
    }
    // (the guest's subs r0, r1, r0; asrs r0, r0, #1)
    ctx->r[0] = static_cast<uint32_t>(static_cast<int32_t>(p - s) >> 1);
}

int psv_host_atomic_order_valid(const PsvContext* ctx) { return ctx->r[2] <= 5; }

void psv_host_atomic_fetch_add_4(PsvContext* ctx, uint32_t ret) {
    (void)ret;
    const uint32_t a = ctx->r[0];
    const uint32_t old = __atomic_fetch_add(reinterpret_cast<uint32_t*>(host(a)), ctx->r[1], __ATOMIC_SEQ_CST);
    // (what the guest's ldrex/strex loop leaves in the monitor: the address and value of its last exclusive load, no
    // exclusive access outstanding)
    ctx->excl_address = a;
    ctx->excl_value = old;
    ctx->excl_size = 0;
    ctx->r[0] = old;
}

void psv_host_atomic_fetch_sub_4(PsvContext* ctx, uint32_t ret) {
    (void)ret;
    const uint32_t a = ctx->r[0];
    const uint32_t old = __atomic_fetch_sub(reinterpret_cast<uint32_t*>(host(a)), ctx->r[1], __ATOMIC_SEQ_CST);
    ctx->excl_address = a;
    ctx->excl_value = old;
    ctx->excl_size = 0;
    ctx->r[0] = old;
}

}  // extern "C"
