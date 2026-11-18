// RenderDoc's in-application API, looked up in the process (never loaded by psvrecore itself).

#include "psvrecore/vulkan/renderdoc.hpp"

#include <mutex>

#include "psvrecore/log.hpp"
#include "renderdoc_app.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace psvrecore::vulkan::renderdoc {

namespace {

RENDERDOC_API_1_6_0* api() {
    static RENDERDOC_API_1_6_0* found = [] {
        pRENDERDOC_GetAPI get = nullptr;
#if defined(_WIN32)
        if (HMODULE m = GetModuleHandleA("renderdoc.dll"))
            get = reinterpret_cast<pRENDERDOC_GetAPI>(reinterpret_cast<void*>(GetProcAddress(m, "RENDERDOC_GetAPI")));
#else
#if defined(__ANDROID__)
        const char* name = "libVkLayer_GLES_RenderDoc.so";
#else
        const char* name = "librenderdoc.so";
#endif
        if (void* m = dlopen(name, RTLD_NOW | RTLD_NOLOAD))
            get = reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(m, "RENDERDOC_GetAPI"));
#endif
        RENDERDOC_API_1_6_0* a = nullptr;
        if (!get || !get(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&a)) || !a)
            return static_cast<RENDERDOC_API_1_6_0*>(nullptr);
        int major = 0, minor = 0, patch = 0;
        a->GetAPIVersion(&major, &minor, &patch);
        a->SetCaptureKeys(nullptr, 0);
        nx::logi(logs::gpu, "RenderDoc {}.{}.{} is loaded: F12 (or a game's Razor trigger) captures a frame", major,
                 minor, patch);
        return a;
    }();
    return found;
}

std::mutex g_lock;
bool g_capturing = false;

}  // namespace

bool loaded() { return api() != nullptr; }

void start(VkInstance instance, nx::string_view title) {
    RENDERDOC_API_1_6_0* a = api();
    if (!a || !instance) return;
    std::scoped_lock held(g_lock);
    if (g_capturing) return;
    a->StartFrameCapture(RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(instance), nullptr);
    if (!title.empty()) a->SetCaptureTitle(nx::string(title).c_str());
    g_capturing = true;
}

bool end(VkInstance instance) {
    RENDERDOC_API_1_6_0* a = api();
    if (!a || !instance) return false;
    std::scoped_lock held(g_lock);
    if (!g_capturing) return false;
    g_capturing = false;
    const bool ok = a->EndFrameCapture(RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(instance), nullptr) != 0;
    nx::logi(logs::gpu, "RenderDoc: {}", ok ? "a frame captured" : "the capture failed");
    return ok;
}

bool capturing() {
    std::scoped_lock held(g_lock);
    return g_capturing;
}

}  // namespace psvrecore::vulkan::renderdoc
