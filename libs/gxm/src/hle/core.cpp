// GXM's library state: initialization, memory mapped for the GPU, sync objects, notifications, the display queue,
// render targets and contexts.

#include <cstring>

#include "psvrecore/log.hpp"
#include "state.hpp"
#include "core/foundation/strings/format.h"

namespace psvrecore::gxm::hle {

State& state() {
    static State s;
    return s;
}

u32 guest_alloc(u32 size) {
    State& s = state();
    size = (size + 15) & ~15u;
    if (s.arena_next + size > s.arena_end) {
        const u32 chunk = nx::max<u32>(64 * 1024, (size + 0xFFFF) & ~0xFFFFu);
        const u32 at = runtime::runtime().allocate_system(chunk, "SceGxm");
        if (!at) return 0;
        s.arena_next = at;
        s.arena_end = at + chunk;
    }
    const u32 at = s.arena_next;
    s.arena_next += size;
    std::memset(runtime::runtime().memory().host(at), 0, size);
    return at;
}

namespace {

constexpr u32 kSceGxm = 0xF76B66BD;
constexpr u32 kNotificationRegionSize = 512 * 4;
constexpr u32 kMinimumContextHostMemSize = 2048;

bool valid(u32 address, u32 size, bool write = false) {
    return address && runtime::runtime().memory().valid(address, size, write);
}

i32 sceGxmInitialize(Ptr<const InitializeParams> p) {
    State& s = state();
    nx::scoped_lock<nx::mutex> held(s.lock);
    if (s.initialized) return err::kAlreadyInitialized;
    if (!p) return err::kInvalidPointer;
    const InitializeParams params = *p;
    if (params.display_queue_max_pending < 1 || params.display_queue_max_pending > 16 ||
        params.display_queue_callback_data_size > 512 || (params.parameter_buffer_size & 0x3FFFF))
        return err::kInvalidValue;
    s.init = params;
    s.notification_region = guest_alloc(kNotificationRegionSize);
    s.queue.clear();
    s.queue_data_free.clear();
    s.display_holds.clear();
    for (u32 i = 0; i < params.display_queue_max_pending + 1; ++i)
        s.queue_data_free.push_back(guest_alloc(nx::max<u32>(params.display_queue_callback_data_size, 4)));
    s.initialized = true;
    start_display_queue();
    nx::logi(logs::gxm, "initialized: display queue of {}, callback 0x{:08x} ({} bytes of data), parameter buffer {} KiB",
             params.display_queue_max_pending, params.display_queue_callback, params.display_queue_callback_data_size,
             params.parameter_buffer_size / 1024);
    return 0;
}

i32 sceGxmTerminate() {
    State& s = state();
    {
        nx::scoped_lock<nx::mutex> held(s.lock);
        if (!s.initialized) return err::kUninitialized;
    }
    stop_display_queue();
    nx::scoped_lock<nx::mutex> held(s.lock);
    s.initialized = false;
    return 0;
}

Ptr<u32> sceGxmGetNotificationRegion() { return Ptr<u32>(state().notification_region); }

i32 sceGxmMapMemory(u32 base, u32 size, u32 attribs) {
    (void)attribs;
    if (!base) return err::kInvalidPointer;
    if ((base & 0xFFF) || !size || (size & 0xFFF)) return err::kInvalidAlignment;
    if (!valid(base, size)) return err::kInvalidValue;
    State& s = state();
    nx::scoped_lock<nx::mutex> held(s.lock);
    for (const auto& [b, n] : s.mappings)
        if (base < b + n && b < base + size) return err::kInvalidValue;
    s.mappings.push_back({base, size});
    return 0;
}

i32 sceGxmUnmapMemory(u32 base) {
    State& s = state();
    nx::scoped_lock<nx::mutex> held(s.lock);
    for (usize i = 0; i < s.mappings.size(); ++i) {
        if (s.mappings[i].first == base) {
            s.mappings.erase(s.mappings.begin() + static_cast<isize>(i));
            return 0;
        }
    }
    return err::kInvalidMapping;
}

// USSE code memory: the patcher's programs are host objects here, so its offsets only need to be valid numbers.
i32 sceGxmMapVertexUsseMemory(u32 base, u32 size, Ptr<u32> offset) {
    if (!base || !offset) return err::kInvalidPointer;
    if ((base & 0xFFF) || !size || (size & 0xFFF)) return err::kInvalidAlignment;
    *offset = base;
    return 0;
}
i32 sceGxmUnmapVertexUsseMemory(u32 base) { return base ? 0 : err::kInvalidPointer; }
i32 sceGxmMapFragmentUsseMemory(u32 base, u32 size, Ptr<u32> offset) {
    return sceGxmMapVertexUsseMemory(base, size, offset);
}
i32 sceGxmUnmapFragmentUsseMemory(u32 base) { return base ? 0 : err::kInvalidPointer; }

i32 sceGxmSyncObjectCreate(Ptr<u32> out) {
    if (!out) return err::kInvalidPointer;
    State& s = state();
    nx::scoped_lock<nx::mutex> held(s.lock);
    const u32 guest = guest_alloc(16);
    if (!guest) return err::kOutOfMemory;
    auto o = nx::make_unique<SyncObject>();
    o->guest = guest;
    s.sync_objects[guest] = std::move(o);
    *out = guest;
    return 0;
}

i32 sceGxmSyncObjectDestroy(u32 sync) {
    State& s = state();
    {
        nx::scoped_lock<nx::mutex> held(s.lock);
        if (!s.sync_objects.erase(sync)) return err::kInvalidPointer;
    }
    {
        nx::scoped_lock<nx::mutex> held(s.queue_lock);
        s.display_holds.erase(sync);
    }
    s.queue_changed.notify_all();
    return 0;
}

// Scenes are rendered when they end, and their notifications written then: a wait finds them written.
i32 sceGxmNotificationWait(Ptr<const Notification> n) {
    if (!n || !valid(n->address, 4)) return err::kInvalidPointer;
    return 0;
}

i32 sceGxmFinish(u32 context) {
    if (!context) return err::kInvalidPointer;
    return 0;
}

i32 sceGxmDisplayQueueAddEntry(u32 old_buffer, u32 new_buffer, u32 data) {
    State& s = state();
    if (!s.initialized) return err::kUninitialized;
    if (!old_buffer || !new_buffer) return err::kInvalidPointer;
    if (s.init.display_queue_callback_data_size && !valid(data, s.init.display_queue_callback_data_size))
        return err::kInvalidPointer;
    runtime::Runtime& rt = runtime::runtime();
    u32 slot = 0;
    {
        // (at most the queue's pending count waiting: the caller waits for room, as libgxm's does)
        runtime::Kernel::Blocking blocking(rt.kernel());
        nx::scoped_lock<nx::mutex> held(s.queue_lock);
        s.queue_changed.wait(s.queue_lock, [&] {
            return s.stopping || (s.queue.size() < s.init.display_queue_max_pending && !s.queue_data_free.empty());
        });
        if (s.stopping) return err::kUninitialized;
        slot = s.queue_data_free.back();
        s.queue_data_free.pop_back();
    }
    if (s.init.display_queue_callback_data_size)
        std::memcpy(rt.memory().host(slot), rt.memory().host(data), s.init.display_queue_callback_data_size);
    {
        // (the new buffer's sync object held until a later entry replaces it; the old one released after this flip)
        nx::scoped_lock<nx::mutex> held(s.queue_lock);
        ++s.display_holds[new_buffer];
        s.queue.push_back(DisplayEntry{old_buffer, new_buffer, slot});
    }
    s.queue_changed.notify_all();
    return 0;
}

i32 sceGxmDisplayQueueFinish() {
    State& s = state();
    if (!s.initialized) return err::kUninitialized;
    runtime::Kernel::Blocking blocking(runtime::runtime().kernel());
    nx::scoped_lock<nx::mutex> held(s.queue_lock);
    s.queue_changed.wait(s.queue_lock, [&] { return s.stopping || (s.queue.empty() && !s.queue_busy); });
    return 0;
}

i32 sceGxmPadHeartbeat(u32 surface, u32 sync) { return surface && sync ? 0 : err::kInvalidPointer; }
i32 sceGxmPadTriggerGpuPaTrace() { return 0; }
i32 sceGxmAddRazorGpuCaptureBuffer(u32, u32) { return 0; }
void sceGxmSetWarningEnabled(u32, u32) {}

// ---- Render targets --------------------------------------------------------------------------------------------

i32 check_target(const RenderTargetParams& p) {
    if (!p.width || !p.height || p.width > 4096 || p.height > 4096) return err::kInvalidValue;
    if (!p.scenes_per_frame || p.scenes_per_frame > 8) return err::kInvalidValue;
    if (p.multisample_mode > 2) return err::kInvalidValue;
    return 0;
}

i32 sceGxmGetRenderTargetMemSize(Ptr<const RenderTargetParams> p, Ptr<u32> size) {
    if (!p || !size) return err::kInvalidPointer;
    if (const i32 e = check_target(*p)) return e;
    *size = 256 * 1024;  // (what the hardware's tiling would need is not needed here)
    return 0;
}

i32 sceGxmCreateRenderTarget(Ptr<const RenderTargetParams> p, Ptr<u32> out) {
    if (!p || !out) return err::kInvalidPointer;
    if (const i32 e = check_target(*p)) return e;
    State& s = state();
    nx::scoped_lock<nx::mutex> held(s.lock);
    if (s.render_targets.size() >= 48) return err::kOutOfRenderTargets;
    const u32 guest = guest_alloc(16);
    if (!guest) return err::kOutOfMemory;
    auto t = nx::make_unique<RenderTarget>();
    t->guest = guest;
    t->width = p->width;
    t->height = p->height;
    t->scenes_per_frame = p->scenes_per_frame;
    t->multisample = p->multisample_mode;
    *reinterpret_cast<u32*>(runtime::runtime().memory().host(guest)) = static_cast<u32>(p->driver_mem_block);
    s.render_targets[guest] = std::move(t);
    *out = guest;
    return 0;
}

i32 sceGxmDestroyRenderTarget(u32 target) {
    State& s = state();
    nx::scoped_lock<nx::mutex> held(s.lock);
    if (!s.render_targets.erase(target)) return err::kInvalidPointer;
    return 0;
}

i32 sceGxmRenderTargetGetDriverMemBlock(u32 target, Ptr<i32> block) {
    if (!target || !block) return err::kInvalidPointer;
    *block = *reinterpret_cast<const i32*>(runtime::runtime().memory().host(target));
    return 0;
}

// ---- Contexts --------------------------------------------------------------------------------------------------

i32 sceGxmCreateContext(Ptr<const ContextParams> p, Ptr<u32> out) {
    if (!p || !out) return err::kInvalidPointer;
    const ContextParams params = *p;
    if (!params.host_mem || params.host_mem_size < kMinimumContextHostMemSize || !valid(params.host_mem, params.host_mem_size, true))
        return err::kInvalidValue;
    State& s = state();
    nx::scoped_lock<nx::mutex> held(s.lock);
    if (!s.initialized) return err::kUninitialized;
    auto c = nx::make_unique<Context>();
    c->guest = params.host_mem;  // (libgxm keeps the context in the memory it is given)
    c->params = params;
    s.contexts[c->guest] = std::move(c);
    *out = params.host_mem;
    return 0;
}

i32 sceGxmDestroyContext(u32 context) {
    State& s = state();
    nx::scoped_lock<nx::mutex> held(s.lock);
    if (!s.contexts.erase(context)) return err::kInvalidPointer;
    return 0;
}

i32 sceGxmGetContextType(u32 context, Ptr<u32> type) {
    if (!context || !type) return err::kInvalidPointer;
    nx::scoped_lock<nx::mutex> held(state().lock);
    auto it = state().contexts.find(context);
    if (it == state().contexts.end()) return err::kInvalidPointer;
    *type = it->second->deferred ? 1 : 0;  // SCE_GXM_CONTEXT_TYPE_DEFERRED, _IMMEDIATE
    return 0;
}

const runtime::hle::Function kFunctions[] = {
    HLE_FUNCTION(0xB0F1E4EC, sceGxmInitialize),
    HLE_FUNCTION(0xB627DE66, sceGxmTerminate),
    HLE_FUNCTION(0x8BDE825A, sceGxmGetNotificationRegion),
    HLE_FUNCTION(0xC61E34FC, sceGxmMapMemory),
    HLE_FUNCTION(0x828C68E8, sceGxmUnmapMemory),
    HLE_FUNCTION(0xFA437510, sceGxmMapVertexUsseMemory),
    HLE_FUNCTION(0x099134F5, sceGxmUnmapVertexUsseMemory),
    HLE_FUNCTION(0x008402C6, sceGxmMapFragmentUsseMemory),
    HLE_FUNCTION(0x80CCEDBB, sceGxmUnmapFragmentUsseMemory),
    HLE_FUNCTION(0x6A6013E1, sceGxmSyncObjectCreate),
    HLE_FUNCTION(0x889AE88C, sceGxmSyncObjectDestroy),
    HLE_FUNCTION(0x9F448E79, sceGxmNotificationWait),
    HLE_FUNCTION(0x0733D8AE, sceGxmFinish),
    HLE_FUNCTION(0xEC5C26B5, sceGxmDisplayQueueAddEntry),
    HLE_FUNCTION(0xB98C5B0D, sceGxmDisplayQueueFinish),
    HLE_FUNCTION(0x3D25FCE9, sceGxmPadHeartbeat),
    HLE_FUNCTION(0x47E06984, sceGxmPadTriggerGpuPaTrace),
    HLE_FUNCTION(0xE9E81073, sceGxmAddRazorGpuCaptureBuffer),
    HLE_FUNCTION(0x12D18B3D, sceGxmSetWarningEnabled),
    HLE_FUNCTION(0xB291C959, sceGxmGetRenderTargetMemSize),
    HLE_FUNCTION(0x207AF96B, sceGxmCreateRenderTarget),
    HLE_FUNCTION(0x0B94C50A, sceGxmDestroyRenderTarget),
    HLE_FUNCTION(0x49553737, sceGxmRenderTargetGetDriverMemBlock),
    HLE_FUNCTION(0xE84CE5B4, sceGxmCreateContext),
    HLE_FUNCTION(0xEDDC5FB2, sceGxmDestroyContext),
    HLE_FUNCTION(0x2A1BCDDB, sceGxmGetContextType),
};

}  // namespace

void register_core() { runtime::hle::register_library({"SceGxm", kSceGxm, kFunctions}); }

}  // namespace psvrecore::gxm::hle

namespace psvrecore::gxm {

void register_libraries(const Options& options) {
    hle::state().options = &options;
    hle::register_display();
    hle::register_core();
    hle::register_surfaces();
    hle::register_patcher();
    hle::register_context();
    hle::register_textures();
    hle::register_transfer();
    hle::register_precomputed();
}

}  // namespace psvrecore::gxm
