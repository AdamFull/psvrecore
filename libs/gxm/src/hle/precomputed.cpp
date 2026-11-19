// Precomputed draws and states (sceGxmPrecomputed*): plain records in the game's memory (state.hpp), read when a
// draw is recorded (context.cpp: sceGxmDrawPrecomputed, sceGxmSetPrecomputedVertexState and ...FragmentState).

#include <cstring>

#include "state.hpp"

namespace psvrecore::gxm::hle {

namespace {

constexpr u32 kSceGxm = 0xF76B66BD;
constexpr u32 kAlignment = 16;  // SCE_GXM_PRECOMPUTED_ALIGNMENT

runtime::GuestMemory& memory() { return runtime::runtime().memory(); }

// What a program's precomputed objects hold: its streams (a vertex program's), its uniform buffers (up to the highest
// BUFFER[n] it reads) and its texture units (up to the highest it samples). Unknown programs: none.
struct Needs {
    u32 streams = 0, buffers = 0, textures = 0;
};
Needs needs_of(const usse::Gxp& gxp) {
    Needs n;
    for (const usse::Parameter& p : gxp.parameters) {
        if (p.category == usse::ParameterCategory::Sampler) n.textures = nx::max(n.textures, p.resource + 1);
        if (p.category == usse::ParameterCategory::UniformBuffer) n.buffers = nx::max(n.buffers, p.resource + 1);
    }
    return n;
}
Needs vertex_needs(u32 program) {
    nx::scoped_lock<nx::mutex> held(state().lock);
    auto it = state().vertex_programs.find(program);
    if (it == state().vertex_programs.end() || !it->second->program) return {};
    Needs n = needs_of(it->second->program->gxp);
    n.streams = nx::min<u32>(static_cast<u32>(it->second->streams.size()), kPrecomputedStreams);
    n.textures = nx::min(n.textures, kPrecomputedVertexTextures);
    n.buffers = nx::min(n.buffers, kPrecomputedBuffers);
    return n;
}
Needs fragment_needs(u32 program) {
    nx::scoped_lock<nx::mutex> held(state().lock);
    auto it = state().fragment_programs.find(program);
    if (it == state().fragment_programs.end() || !it->second->program) return {};
    Needs n = needs_of(it->second->program->gxp);
    n.textures = nx::min(n.textures, kPrecomputedFragmentTextures);
    n.buffers = nx::min(n.buffers, kPrecomputedBuffers);
    return n;
}
u32 state_block_size(const Needs& n) { return precomputed_textures_offset(n.buffers) + n.textures * 16; }

// (a block of 0 bytes needs no memory: a null block is accepted then)
u32 sceGxmGetPrecomputedDrawSize(u32 vertex_program) { return vertex_program ? vertex_needs(vertex_program).streams * 4 : 0; }
u32 sceGxmGetPrecomputedVertexStateSize(u32 vertex_program) {
    return vertex_program ? state_block_size(vertex_needs(vertex_program)) : 0;
}
u32 sceGxmGetPrecomputedFragmentStateSize(u32 fragment_program) {
    return fragment_program ? state_block_size(fragment_needs(fragment_program)) : 0;
}

i32 init_block(u32 block, u32 size) {
    if (!size) return 0;
    if (!block) return err::kInvalidPointer;
    if (block % kAlignment) return err::kInvalidAlignment;
    if (!memory().valid(block, size, true)) return err::kInvalidPointer;
    std::memset(memory().host(block), 0, size);
    return 0;
}

// ---- Draws -----------------------------------------------------------------------------------------------------

i32 sceGxmPrecomputedDrawInit(Ptr<PrecomputedDraw> d, u32 vertex_program, u32 block) {
    if (!d || !vertex_program) return err::kInvalidPointer;
    const u32 streams = vertex_needs(vertex_program).streams;
    if (const i32 e = init_block(block, streams * 4)) return e;
    *d = PrecomputedDraw{};
    d->vertex = vertex_program;
    d->block = block;
    d->instances = 1;
    d->streams = streams;
    return 0;
}

i32 sceGxmPrecomputedDrawSetAllVertexStreams(Ptr<PrecomputedDraw> d, u32 streams) {
    if (!d || !streams) return err::kInvalidPointer;
    // (as many as the vertex program has)
    const u32 count = nx::min(d->streams, kPrecomputedStreams);
    if (!count) return 0;
    if (!memory().valid(streams, count * 4) || !memory().valid(d->block, count * 4, true)) return err::kInvalidPointer;
    std::memcpy(memory().host(d->block), memory().host(streams), count * 4);
    return 0;
}

i32 sceGxmPrecomputedDrawSetVertexStream(Ptr<PrecomputedDraw> d, u32 index, u32 data) {
    if (!d) return err::kInvalidPointer;
    if (index >= kPrecomputedStreams) return err::kInvalidValue;
    if (index >= d->streams) return 0;  // (a stream the program does not have)
    if (!memory().valid(d->block + index * 4, 4, true)) return err::kInvalidPointer;
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

// ---- States (one template for both stages: up to `Units` texture units) ------------------------------------------

template <typename StateT, u32 Units, Needs (*NeedsOf)(u32)>
struct States {
    static i32 init(Ptr<StateT> s, u32 program, u32 block) {
        if (!s || !program) return err::kInvalidPointer;
        const Needs n = NeedsOf(program);
        if (const i32 e = init_block(block, state_block_size(n))) return e;
        *s = StateT{};
        s->program = program;
        s->block = block;
        s->buffers = n.buffers;
        s->textures = n.textures;
        return 0;
    }
    static u32 buffers(const StateT& s) { return nx::min(s.buffers, kPrecomputedBuffers); }
    static u32 textures(const StateT& s) { return nx::min(s.textures, Units); }
    static u32 textures_at(const StateT& s) { return s.block + precomputed_textures_offset(buffers(s)); }
    static i32 set_default(Ptr<StateT> s, u32 buffer) {
        if (!s) return err::kInvalidPointer;
        s->default_buffer = buffer;
        return 0;
    }
    static u32 get_default(Ptr<const StateT> s) { return s ? s->default_buffer : 0; }
    // (a buffer or unit the program does not read is not kept: nothing would read it)
    static i32 set_buffer(Ptr<StateT> s, u32 index, u32 data) {
        if (!s) return err::kInvalidPointer;
        if (index >= kPrecomputedBuffers) return err::kInvalidValue;
        if (index >= buffers(*s)) return 0;
        if (!memory().valid(s->block + index * 4, 4, true)) return err::kInvalidPointer;
        std::memcpy(memory().host(s->block + index * 4), &data, 4);
        return 0;
    }
    static i32 set_all_buffers(Ptr<StateT> s, u32 array) {
        if (!s || !array) return err::kInvalidPointer;
        const u32 n = buffers(*s);
        if (!n) return 0;
        if (!memory().valid(array, n * 4) || !memory().valid(s->block, n * 4, true)) return err::kInvalidPointer;
        std::memcpy(memory().host(s->block), memory().host(array), n * 4);
        return 0;
    }
    static i32 set_texture(Ptr<StateT> s, u32 index, Ptr<const TextureWords> t) {
        if (!s || !t) return err::kInvalidPointer;
        if (index >= Units) return err::kInvalidValue;
        if (index >= textures(*s)) return 0;
        const u32 at = textures_at(*s) + index * 16;
        if (!memory().valid(at, 16, true)) return err::kInvalidPointer;
        std::memcpy(memory().host(at), t.get(), 16);
        return 0;
    }
    static i32 set_all_textures(Ptr<StateT> s, u32 array) {
        if (!s || !array) return err::kInvalidPointer;
        const u32 n = textures(*s);
        if (!n) return 0;
        if (!memory().valid(array, n * 16) || !memory().valid(textures_at(*s), n * 16, true)) return err::kInvalidPointer;
        std::memcpy(memory().host(textures_at(*s)), memory().host(array), n * 16);
        return 0;
    }
};
using VertexStates = States<PrecomputedVertexState, kPrecomputedVertexTextures, vertex_needs>;
using FragmentStates = States<PrecomputedFragmentState, kPrecomputedFragmentTextures, fragment_needs>;

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
