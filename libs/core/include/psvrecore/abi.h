/* The runtime's binary interface: what a game's executable and its generated code see of psvrecore when psvrecore
 * is a shared library (PSVRECORE_SHARED: psvrecore.dll, libpsvrecore.so.N), and the version both sides check.
 *
 * PSVRECORE_API marks what the library exports: the frontend's entry, the services generated code calls (guest.h),
 * the function registry (recomp/support.hpp), whole-file I/O and the few runtime calls a game's own additions make.
 * Nothing else leaves the library. In a static build (Android, the tests, the tools) it is empty.
 *
 * PSVRECORE_ABI_VERSION changes whenever the exported interface, or a type passed through it, changes in a way an
 * executable built before would get wrong: a function's parameters, a struct's layout (frontend::Game, PsvContext,
 * PsvModuleCode), what generated code expects of the runtime. The executable carries the version it was built with
 * and the library checks it at start-up (psvrecore_check_abi); on Linux it is also the library's SOVERSION, so an
 * executable asks for libpsvrecore.so.<its version>.
 *
 * The interface is C++ (nx-foundation's strings and containers cross it), which holds under one rule: the game and
 * psvrecore are built with the same compiler version and the same settings (docs/BUILDING.md). The start-up check
 * compares those too (PSVRECORE_BUILD_SIGNATURE). C, for generated code. */
#ifndef PSVRECORE_ABI_H
#define PSVRECORE_ABI_H

#include <stdint.h>

#define PSVRECORE_ABI_VERSION 2

/* PSVRECORE_SHARED: psvrecore is (or is being built as) a shared library. PSVRECORE_BUILDING: this is psvrecore's
 * own code. Both are set by the build (CMakeLists.txt), never by hand. */
#if defined(PSVRECORE_SHARED)
#if defined(_WIN32)
#if defined(PSVRECORE_BUILDING)
#define PSVRECORE_API __declspec(dllexport)
#else
#define PSVRECORE_API __declspec(dllimport)
#endif
#else
#define PSVRECORE_API __attribute__((visibility("default")))
#endif
#else
#define PSVRECORE_API
#endif

#ifdef __cplusplus
#include <version>  /* (the C++ library's own macros, for the signature) */

#define PSVRECORE_ABI_STR2(x) #x
#define PSVRECORE_ABI_STR(x) PSVRECORE_ABI_STR2(x)

/* The compiler and its version. */
#if defined(__clang__)
#define PSVRECORE_ABI_COMPILER "clang " PSVRECORE_ABI_STR(__clang_major__) "." PSVRECORE_ABI_STR(__clang_minor__)
#elif defined(__GNUC__)
#define PSVRECORE_ABI_COMPILER "gcc " PSVRECORE_ABI_STR(__GNUC__) "." PSVRECORE_ABI_STR(__GNUC_MINOR__)
#elif defined(_MSC_VER)
#define PSVRECORE_ABI_COMPILER "msvc"
#else
#define PSVRECORE_ABI_COMPILER "unknown compiler"
#endif

/* The C++ library, and on Windows the C runtime (a debug CRT and STL differ from the release ones in layout). */
#if defined(_MSVC_STL_VERSION)
#if defined(_DEBUG)
#define PSVRECORE_ABI_LIBRARY ", MSVC " PSVRECORE_ABI_STR(_MSC_VER) " STL " PSVRECORE_ABI_STR(_MSVC_STL_VERSION) " debug"
#else
#define PSVRECORE_ABI_LIBRARY ", MSVC " PSVRECORE_ABI_STR(_MSC_VER) " STL " PSVRECORE_ABI_STR(_MSVC_STL_VERSION)
#endif
#elif defined(_LIBCPP_VERSION)
#define PSVRECORE_ABI_LIBRARY ", libc++ " PSVRECORE_ABI_STR(_LIBCPP_VERSION)
#elif defined(__GLIBCXX__)
#define PSVRECORE_ABI_LIBRARY ", libstdc++ " PSVRECORE_ABI_STR(_GLIBCXX_RELEASE) " abi " PSVRECORE_ABI_STR(_GLIBCXX_USE_CXX11_ABI)
#else
#define PSVRECORE_ABI_LIBRARY ""
#endif

/* Assertions on or off (nx-foundation's debug checks, and the members some of its types keep for them). */
#if defined(NDEBUG)
#define PSVRECORE_ABI_CHECKS ", release"
#else
#define PSVRECORE_ABI_CHECKS ", debug"
#endif

/* What the game and psvrecore must agree on besides the version: "clang 22.1, MSVC 1944 STL 145, release". */
#define PSVRECORE_BUILD_SIGNATURE PSVRECORE_ABI_COMPILER PSVRECORE_ABI_LIBRARY PSVRECORE_ABI_CHECKS

extern "C" {
#endif

/* The ABI version psvrecore was built with. */
PSVRECORE_API uint32_t psvrecore_abi_version(void);

/* Whether a program built for `abi_version` with `build_signature` (its PSVRECORE_ABI_VERSION and
 * PSVRECORE_BUILD_SIGNATURE) can run on this psvrecore. If not, it says why and what to do on stderr, and in a
 * message box unless the command line (argc, argv) has --hidden, and returns 0. The one function whose parameters
 * never change: it is called before anything else crosses. */
PSVRECORE_API int psvrecore_check_abi(uint32_t abi_version, const char* build_signature, int argc,
                                      const char* const* argv);

#ifdef __cplusplus
}
#endif

#endif /* PSVRECORE_ABI_H */
