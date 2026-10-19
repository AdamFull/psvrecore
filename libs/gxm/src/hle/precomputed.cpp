// Precomputed draws and states (sceGxmPrecomputed*): plain records in the game's memory (state.hpp), read when a
// draw is recorded (context.cpp: sceGxmDrawPrecomputed, sceGxmSetPrecomputedVertexState and ...FragmentState).

#include <cstring>

#include "state.hpp"

namespace psvrecore::gxm::hle {

namespace {

constexpr u32 kSceGxm = 0xF76B66BD;
constexpr u32 kAlignment = 16;  // SCE_GXM_PRECOMPUTED_ALIGNMENT

runtime::GuestMemory& memory() { return runtime::runtime().memory(); }

u32 sceGxmGetPrecomputedDrawSize(u32 vertex_program) { return vertex_program ? kPrecomputedDrawBlock : 0; }
u32 sceGxmGetPrecomputedVertexStateSize(u32 vertex_program) { return vertex_program ? kPrecomputedVertexBlock : 0; }
u32 sceGxmGetPrecomputedFragmentStateSize(u32 fragment_program) { return fragment_program ? kPrecomputedFragmentBlock : 0; }

i32 init_block(u32 block, u32 size) {
    if (!block) return err::kInvalidPointer;
    if (block % kAlignment) return err::kInvalidAlignment;
    if (!memory().valid(block, size, true)) return err::kInvalidPointer;
    std::memset(memory().host(block), 0, size);
    return 0;
}

// ---- Draws -----------------------------------------------------------------------------------------------------

i32 sceGxmPrecomputedDrawInit(Ptr<PrecomputedDraw> d, u32 vertex_program, u32 block) {
    if (!d || !vertex_program) return err::kInvalidPointer;
    if (const i32 e = init_block(block, kPrecomputedDrawBlock)) return e;
    *d = PrecomputedDraw{};
    d->vertex = vertex_program;
    d->block = block;
    d->instances = 1;
    return 0;
}

i32 sceGxmPrecomputedDrawSetAllVertexStreams(Ptr<PrecomputedDraw> d, u32 streams) {
    if (!d || !streams) return err::kInvalidPointer;
    // (as many as the vertex program has; the others stay unset)
    u32 count = 0;
    {
        nx::scoped_lock<nx::mutex> held(state().lock);
        auto it = state().vertex_programs.find(d->vertex);
        if (it == state().vertex_programs.end()) return err::kInvalidPointer;
        count = static_cast<u32>(nx::min<usize>(it->second->streams.size(), 16));
    }
    if (!memory().valid(streams, count * 4) || !memory().valid(d->block, kPrecomputedDrawBlock, true))
        return err::kInvalidPointer;
    if (count) std::memcpy(memory().host(d->block), memory().host(streams), count * 4);
    return 0;
}

i32 sceGxmPrecomputedDrawSetVertexStream(Ptr<PrecomputedDraw> d, u32 index, u32 data) {
    if (!d) return err::kInvalidPointer;
    if (index >= 16) return err::kInvalidValue;
    if (!memory().valid(d->block, kPrecomputedDrawBlock, true)) return err::kInvalidPointer;
    std::memcpy(memory().host(d->block + index * 4), &data, 4);
    return 0;
}

i32 set_params(Ptr<PrecomputedDraw> d, u32 primitive, u32 index_format, u32 indices, u32 count, u32 instances) {
    if (!d || !indices) return err::kInvalidPointer;
    if (!count) return err::kInvalidIndexCount;
    if (primitive > 0x14000000u || (primitive & 0x03FFFFFFu)) return err::kInvalidPrimitiveType;
    d->primitive = primitive;
    d->index_format = index_format;
    d->index_data = indices;
    d->index_count = count;
    d->instances = instances;
    return 0;
}
i32 sceGxmPrecomputedDrawSetParams(Ptr<PrecomputedDraw> d, u32 primitive, u32 index_format, u32 indices, u32 count) {
    return set_params(d, primitive, index_format, indices, count, 1);
}
i32 sceGxmPrecomputedDrawSetParamsInstanced(Ptr<PrecomputedDraw> d, u32 primitive, u32 index_format, u32 indices,
                                            u32 count, u32 wrap) {
    if (!wrap || count % wrap) return err::kInvalidValue;
    return set_params(d, primitive, index_format, indices, wrap, count / wrap);
}

// ---- States (one template for both stages: `Units` texture units) ----------------------------------------------

template <typename StateT, u32 Units>
struct States {
    static constexpr u32 kBlock = 16 * 4 + Units * 16;

    static i32 init(Ptr<StateT> s, u32 program, u32 block) {
        if (!s || !program) return err::kInvalidPointer;
        if (const i32 e = init_block(block, kBlock)) return e;
        *s = StateT{};
        s->program = program;
        s->block = block;
        return 0;
    }
    static bool block_ok(const StateT& s) { return memory().valid(s.block, kBlock, true); }
    static i32 set_default(Ptr<StateT> s, u32 buffer) {
        if (!s) return err::kInvalidPointer;
        s->default_buffer = buffer;
        return 0;
    }
    static u32 get_default(Ptr<const StateT> s) { return s ? s->default_buffer : 0; }
    static i32 set_buffer(Ptr<StateT> s, u32 index, u32 data) {
        if (!s || !block_ok(*s)) return err::kInvalidPointer;
        if (index >= 14) return err::kInvalidValue;
        std::memcpy(memory().host(s->block + index * 4), &data, 4);
        return 0;
    }
    static i32 set_all_buffers(Ptr<StateT> s, u32 array) {
        if (!s || !block_ok(*s) || !array || !memory().valid(array, 14 * 4)) return err::kInvalidPointer;
        std::memcpy(memory().host(s->block), memory().host(array), 14 * 4);
        return 0;
    }
    static i32 set_texture(Ptr<StateT> s, u32 index, Ptr<const TextureWords> t) {
        if (!s || !block_ok(*s) || !t) return err::kInvalidPointer;
        if (index >= Units) return err::kInvalidValue;
        std::memcpy(memory().host(s->block + 16 * 4 + index * 16), t.get(), 16);
        return 0;
    }
    static i32 set_all_textures(Ptr<StateT> s, u32 array) {
        if (!s || !block_ok(*s) || !array || !memory().valid(array, Units * 16)) return err::kInvalidPointer;
        std::memcpy(memory().host(s->block + 16 * 4), memory().host(array), Units * 16);
        return 0;
    }
};
using VertexStates = States<PrecomputedVertexState, 4>;
using FragmentStates = States<PrecomputedFragmentState, 16>;
static_assert(VertexStates::kBlock == kPrecomputedVertexBlock && FragmentStates::kBlock == kPrecomputedFragmentBlock);

i32 sceGxmPrecomputedVertexStateInit(Ptr<PrecomputedVertexState> s, u32 program, u32 block) { return VertexStates::init(s, program, block); }
i32 sceGxmPrecomputedVertexStateSetDefaultUniformBuffer(Ptr<PrecomputedVertexState> s, u32 b) { return VertexStates::set_default(s, b); }
u32 sceGxmPrecomputedVertexStateGetDefaultUniformBuffer(Ptr<const PrecomputedVertexState> s) { return VertexStates::get_default(s); }
i32 sceGxmPrecomputedVertexStateSetUniformBuffer(Ptr<PrecomputedVertexState> s, u32 i, u32 b) { return VertexStates::set_buffer(s, i, b); }
i32 sceGxmPrecomputedVertexStateSetAllUniformBuffers(Ptr<PrecomputedVertexState> s, u32 a) { return VertexStates::set_all_buffers(s, a); }
i32 sceGxmPrecomputedVertexStateSetTexture(Ptr<PrecomputedVertexState> s, u32 i, Ptr<const TextureWords> t) { return VertexStates::set_texture(s, i, t); }
i32 sceGxmPrecomputedVertexStateSetAllTextures(Ptr<PrecomputedVertexState> s, u32 a) { return VertexStates::set_all_textures(s, a); }

i32 sceGxmPrecomputedFragmentStateInit(Ptr<PrecomputedFragmentState> s, u32 program, u32 block) { return FragmentStates::init(s, program, block); }
i32 sceGxmPrecomputedFragmentStateSetDefaultUniformBuffer(Ptr<PrecomputedFragmentState> s, u32 b) { return FragmentStates::set_default(s, b); }
u32 sceGxmPrecomputedFragmentStateGetDefaultUniformBuffer(Ptr<const PrecomputedFragmentState> s) { return FragmentStates::get_default(s); }
i32 sceGxmPrecomputedFragmentStateSetUniformBuffer(Ptr<PrecomputedFragmentState> s, u32 i, u32 b) { return FragmentStates::set_buffer(s, i, b); }
i32 sceGxmPrecomputedFragmentStateSetAllUniformBuffers(Ptr<PrecomputedFragmentState> s, u32 a) { return FragmentStates::set_all_buffers(s, a); }
i32 sceGxmPrecomputedFragmentStateSetTexture(Ptr<PrecomputedFragmentState> s, u32 i, Ptr<const TextureWords> t) { return FragmentStates::set_texture(s, i, t); }
i32 sceGxmPrecomputedFragmentStateSetAllTextures(Ptr<PrecomputedFragmentState> s, u32 a) { return FragmentStates::set_all_textures(s, a); }

const runtime::hle::Function kFunctions[] = {
    HLE_FUNCTION(0x41BBD792, sceGxmGetPrecomputedDrawSize),
    HLE_FUNCTION(0x9D83CA3B, sceGxmGetPrecomputedVertexStateSize),
    HLE_FUNCTION(0x85DE8506, sceGxmGetPrecomputedFragmentStateSize),
    HLE_FUNCTION(0xA197F096, sceGxmPrecomputedDrawInit),
    HLE_FUNCTION(0xB6C6F571, sceGxmPrecomputedDrawSetAllVertexStreams),
    HLE_FUNCTION(0x6C936214, sceGxmPrecomputedDrawSetVertexStream),
    HLE_FUNCTION(0x884D0D08, sceGxmPrecomputedDrawSetParams),
    HLE_FUNCTION(0x3A7B1633, sceGxmPrecomputedDrawSetParamsInstanced),
    HLE_FUNCTION(0xBE937F8D, sceGxmPrecomputedVertexStateInit),
    HLE_FUNCTION(0x34BF64E3, sceGxmPrecomputedVertexStateSetDefaultUniformBuffer),
    HLE_FUNCTION(0xBE5A68EF, sceGxmPrecomputedVertexStateGetDefaultUniformBuffer),
    HLE_FUNCTION(0xDBF97ED6, sceGxmPrecomputedVertexStateSetUniformBuffer),
    HLE_FUNCTION(0x0389861D, sceGxmPrecomputedVertexStateSetAllUniformBuffers),
    HLE_FUNCTION(0x6A29EB06, sceGxmPrecomputedVertexStateSetTexture),
    HLE_FUNCTION(0xC40C9127, sceGxmPrecomputedVertexStateSetAllTextures),
    HLE_FUNCTION(0xE297D7AF, sceGxmPrecomputedFragmentStateInit),
    HLE_FUNCTION(0x91236858, sceGxmPrecomputedFragmentStateSetDefaultUniformBuffer),
    HLE_FUNCTION(0xCECB584A, sceGxmPrecomputedFragmentStateGetDefaultUniformBuffer),
    HLE_FUNCTION(0xB452F1FB, sceGxmPrecomputedFragmentStateSetUniformBuffer),
    HLE_FUNCTION(0x5A783DC3, sceGxmPrecomputedFragmentStateSetAllUniformBuffers),
    HLE_FUNCTION(0x29118BF1, sceGxmPrecomputedFragmentStateSetTexture),
    HLE_FUNCTION(0xC383DE39, sceGxmPrecomputedFragmentStateSetAllTextures),
};

}  // namespace

void register_precomputed() { runtime::hle::register_library({"SceGxm", kSceGxm, kFunctions}); }

}  // namespace psvrecore::gxm::hle
