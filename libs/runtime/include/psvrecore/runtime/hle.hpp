#pragma once

// High-level emulation of system libraries: host functions that implement what the console's libraries export, by
// library and NID. Each one is a PsvFunction, called by recompiled code exactly as it calls another module's
// export (guest.h): its arguments are in the guest registers and stack (the ARM procedure call standard), its result
// goes back in r0 (and r1).
//
// HLE functions are written as ordinary C++ functions with guest-typed parameters, and entry<&f> adapts them:
//
//     i32 sceKernelSignalSema(i32 uid, i32 count);
//     ...  HLE_FUNCTION(0x4F0EA1B4, sceKernelSignalSema)  ->  {nid, "sceKernelSignalSema", &entry<&sceKernelSignalSema>}
//
// Parameter types: 32-bit integers (i32, u32), 64-bit ones (i64, u64; in an even/odd register pair or 8-byte
// aligned on the stack), f32 and f64 (in VFP registers: the system's ABI is hard-float), Ptr<T> (a guest pointer),
// and Variadic (the rest of a variadic call's arguments). A PsvContext* parameter receives the calling thread's
// context. Results: void, 32-bit and 64-bit integers, f32 and f64, Ptr<T>.

#include <bit>
#include <span>
#include <tuple>
#include <type_traits>

#include "psvrecore/recomp/guest.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore::runtime::hle {

// A guest pointer: the address, and its host view.
template <class T>
struct Ptr {
    u32 address = 0;

    Ptr() = default;
    explicit Ptr(u32 a) : address(a) {}
    explicit operator bool() const { return address != 0; }
    T* get() const { return address ? reinterpret_cast<T*>(psv_membase + address) : nullptr; }
    T* operator->() const { return get(); }
    template <class U = T>
        requires(!std::is_void_v<U>)
    U& operator*() const {
        return *get();
    }
};

// Reads the arguments of a call: r0-r3, then the stack.
class ArgReader {
public:
    explicit ArgReader(PsvContext* ctx) : ctx_(ctx) {}
    u32 word() {
        const u32 i = next_++;
        if (i < 4) return ctx_->r[i];
        return psv_ld32(ctx_->r[13] + 4 * (i - 4));
    }
    u64 dword() {
        next_ = (next_ + 1) & ~1u;  // an even register, or an 8-byte aligned stack slot
        const u64 lo = word();
        return lo | (u64{word()} << 32);
    }
    // Floating-point arguments, in s0-s15 / d0-d7 in order (the AAPCS's back-filling of a single after a double is
    // not needed by any system function).
    f32 single() {
        const u32 i = next_vfp_++;
        if (i < 16) return std::bit_cast<f32>(ctx_->vfp.s[i]);
        return std::bit_cast<f32>(word());  // (more than sixteen: on the stack)
    }
    f64 dbl() {
        next_vfp_ = (next_vfp_ + 1) & ~1u;
        const u32 i = next_vfp_;
        next_vfp_ += 2;
        if (i < 16) return std::bit_cast<f64>(ctx_->vfp.d[i / 2]);
        return std::bit_cast<f64>(dword());
    }
    PsvContext* context() const { return ctx_; }

private:
    PsvContext* ctx_;
    u32 next_ = 0;
    u32 next_vfp_ = 0;
};

// The remaining arguments of a variadic call (C's default promotions apply: integers are words, doubles are
// 8-byte values in core registers or on the stack, never in VFP registers).
struct Variadic {
    ArgReader* reader;
    u32 word() const { return reader->word(); }
    u64 dword() const { return reader->dword(); }
};

namespace detail {

template <class T>
struct is_ptr : std::false_type {};
template <class T>
struct is_ptr<Ptr<T>> : std::true_type {};

template <class T>
T read(ArgReader& r) {
    if constexpr (std::is_same_v<T, PsvContext*>) return r.context();
    else if constexpr (std::is_same_v<T, Variadic>) return Variadic{&r};
    else if constexpr (is_ptr<T>::value) return T(r.word());
    else if constexpr (std::is_same_v<T, f32>) return r.single();
    else if constexpr (std::is_same_v<T, f64>) return r.dbl();
    else if constexpr (std::is_integral_v<T> && sizeof(T) == 8) return static_cast<T>(r.dword());
    else if constexpr (std::is_integral_v<T> && sizeof(T) <= 4) return static_cast<T>(r.word());
    else static_assert(sizeof(T) == 0, "HLE parameter type not supported");
}

template <class T>
void write(PsvContext* ctx, T value) {
    if constexpr (is_ptr<T>::value) ctx->r[0] = value.address;
    else if constexpr (std::is_same_v<T, f32>) ctx->vfp.s[0] = std::bit_cast<u32>(value);
    else if constexpr (std::is_same_v<T, f64>) ctx->vfp.d[0] = std::bit_cast<u64>(value);
    else if constexpr (std::is_integral_v<T> && sizeof(T) == 8) {
        ctx->r[0] = static_cast<u32>(static_cast<u64>(value));
        ctx->r[1] = static_cast<u32>(static_cast<u64>(value) >> 32);
    } else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) ctx->r[0] = static_cast<u32>(value);
    else static_assert(sizeof(T) == 0, "HLE result type not supported");
}

template <class F>
struct signature;
template <class R, class... A>
struct signature<R (*)(A...)> {
    using result = R;
    using args = std::tuple<std::decay_t<A>...>;
};

// After every HLE call: the calling thread gives up its CPU if the kernel took it (scheduler.cpp).
void after_call();

}  // namespace detail

template <auto F>
void entry(PsvContext* ctx, uint32_t /*ret*/) {
    using Sig = detail::signature<decltype(F)>;
    ArgReader reader(ctx);
    // (a braced list evaluates left to right: arguments are read in order)
    auto args = [&]<class... A>(std::tuple<A...>*) { return std::tuple<A...>{detail::read<A>(reader)...}; }(
        static_cast<typename Sig::args*>(nullptr));
    if constexpr (std::is_void_v<typename Sig::result>) std::apply(F, args);
    else detail::write(ctx, std::apply(F, args));
    detail::after_call();
}

struct Function {
    u32 nid;
    const char* name;
    PsvFunction function;
};

// A variable the runtime exports: guest memory it allocates and initializes once.
struct Variable {
    u32 nid;
    const char* name;
    u32 size;
    void (*init)(u8* host, u32 guest);
};

struct Library {
    const char* name;
    u32 nid;
    std::span<const Function> functions;
    std::span<const Variable> variables = {};
};

#define HLE_FUNCTION(nid, fn) ::psvrecore::runtime::hle::Function{nid, #fn, &::psvrecore::runtime::hle::entry<&fn>}

// The registry of system libraries the runtime implements.
void register_library(const Library& library);
void register_system_libraries();  // every library in libs/runtime/src/hle_*.cpp
void clear_libraries();

// An implementation for an import, looked up by library NID and then by library name (NIDs of a library can differ
// between SDK versions); nullptr if the runtime has none.
const Function* find_function(u32 library_nid, nx::string_view library, u32 nid);
const Variable* find_variable(u32 library_nid, nx::string_view library, u32 nid, u32* guest_address);

// Calls a guest function from host code (a thread's entry, a callback): `args` in r0-r3 and on the stack, LR the
// return address that ends the call. Returns r0. The callee preserves r4-r11 and sp, as any guest function does.
inline constexpr u32 kHostReturn = 0xFFFF0001u;
u32 call_guest(PsvContext* ctx, u32 code, std::span<const u32> args);

}  // namespace psvrecore::runtime::hle
