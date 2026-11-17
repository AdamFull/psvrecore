// The start-up check of abi.h: is this psvrecore the one the game's executable was built for?

#include "psvrecore/abi.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>

#include "core/foundation/strings/format.h"
#include "core/foundation/strings/utf8_string.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif !defined(__ANDROID__)
#include <dlfcn.h>
#endif

namespace {

// Where this psvrecore was loaded from (the library's file; the executable's in a static build).
nx::string library_file() {
#if defined(_WIN32)
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&psvrecore_abi_version), &module))
        return "psvrecore";
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "psvrecore";
    char utf8[MAX_PATH * 3];
    const int m = WideCharToMultiByte(CP_UTF8, 0, path, static_cast<int>(n), utf8, sizeof utf8, nullptr, nullptr);
    return m > 0 ? nx::string(nx::string_view(utf8, static_cast<usize>(m))) : nx::string("psvrecore");
#elif !defined(__ANDROID__)
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&psvrecore_abi_version), &info) && info.dli_fname) return info.dli_fname;
    return "psvrecore";
#else
    return "psvrecore";
#endif
}

}  // namespace

extern "C" uint32_t psvrecore_abi_version(void) { return PSVRECORE_ABI_VERSION; }

extern "C" int psvrecore_check_abi(uint32_t abi_version, const char* build_signature, int argc,
                                   const char* const* argv) {
    const char* const signature = build_signature ? build_signature : "";
    const bool same_version = abi_version == PSVRECORE_ABI_VERSION;
    if (same_version && std::strcmp(signature, PSVRECORE_BUILD_SIGNATURE) == 0) return 1;

    const char* program = argc > 0 && argv && argv[0] ? argv[0] : "This program";
    nx::string message;
    if (!same_version)
        message = nx::format("{} was built for psvrecore ABI version {}, but {} is ABI version {}: they cannot run "
                             "together. Rebuild the game against this psvrecore, or put the psvrecore library it "
                             "was built with next to it.",
                             program, abi_version, library_file(), PSVRECORE_ABI_VERSION);
    else
        message = nx::format("{} was built with {}, but {} with {}. The game and psvrecore must be built with the "
                             "same compiler version and settings (psvrecore's docs/BUILDING.md): rebuild one of "
                             "them.",
                             program, signature, library_file(), PSVRECORE_BUILD_SIGNATURE);
    std::fprintf(stderr, "%s\n", message.c_str());
    std::fflush(stderr);
    bool hidden = false;
    for (int i = 1; i < argc && argv; ++i)
        if (argv[i] && std::strcmp(argv[i], "--hidden") == 0) hidden = true;
    if (!hidden) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "psvrecore", message.c_str(), nullptr);
    return 0;
}
