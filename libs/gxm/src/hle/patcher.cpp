// The shader patcher (registered programs, vertex and fragment programs made from them) and the queries on GXP
// programs and their parameters. The programs are host objects; their handles are small guest blocks. The
// patcher's memory callbacks are not called: nothing it makes lives in the game's memory.

#include <cstring>

#include "psvrecore/log.hpp"
#include "state.hpp"
#include "core/foundation/strings/format.h"

namespace psvrecore::gxm::hle {

namespace {

constexpr u32 kSceGxm = 0xF76B66BD;

runtime::GuestMemory& memory() { return runtime::runtime().memory(); }

// ---- Programs ----------------------------------------------------------------------------------------------------

// A GXP in guest memory, parsed (once per address).
const usse::Gxp* gxp_at(u32 address) {
    static nx::mutex lock;
    static nx::hash_map<u32, nx::unique_ptr<usse::Gxp>> parsed;
    if (!address || !memory().valid(address, 16)) return nullptr;
    const u32 size = *reinterpret_cast<const u32*>(memory().host(address + 8));
    if (size < 0xA0 || !memory().valid(address, size)) return nullptr;
    nx::scoped_lock<nx::mutex> held(lock);
    if (auto it = parsed.find(address); it != parsed.end() && it->second->size == size) return it->second.get();
    auto g = usse::parse_gxp(std::span<const u8>(memory().host(address), size));
    if (!g) {
        nx::logw(logs::gxm, "program at 0x{:08x}: {}", address, g.error().message);
        return nullptr;
    }
    auto& slot = parsed[address];
    slot = nx::make_unique<usse::Gxp>(std::move(*g));
    return slot.get();
}

i32 sceGxmProgramCheck(u32 program) {
    if (!program) return err::kInvalidPointer;
    if (!memory().valid(program, 16)) return err::kInvalidValue;
    const u32 size = *reinterpret_cast<const u32*>(memory().host(program + 8));
    if (!memory().valid(program, nx::max<u32>(size, 16))) return err::kInvalidValue;
    return usse::gxp_valid(std::span<const u8>(memory().host(program), size)) ? 0 : err::kInvalidValue;
}

u32 sceGxmProgramGetSize(u32 program) {
    return program && memory().valid(program, 12) ? *reinterpret_cast<const u32*>(memory().host(program + 8)) : 0;
}
u32 sceGxmProgramGetType(u32 program) {
    const usse::Gxp* g = gxp_at(program);
    return g && g->type == usse::ShaderType::Fragment ? 1 : 0;
}
u32 sceGxmProgramIsDiscardUsed(u32 program) { const usse::Gxp* g = gxp_at(program); return g && g->discard; }
u32 sceGxmProgramIsDepthReplaceUsed(u32 program) { const usse::Gxp* g = gxp_at(program); return g && g->depth_replace; }
u32 sceGxmProgramIsSpriteCoordUsed(u32 program) { const usse::Gxp* g = gxp_at(program); return g && g->sprite_coord; }
u32 sceGxmProgramIsNativeColorUsed(u32 program) { const usse::Gxp* g = gxp_at(program); return g && g->native_color; }
u32 sceGxmProgramIsFragColorUsed(u32 program) { const usse::Gxp* g = gxp_at(program); return g && g->frag_color; }
u32 sceGxmProgramGetDefaultUniformBufferSize(u32 program) {
    const usse::Gxp* g = gxp_at(program);
    return g ? g->default_uniform_buffer_size : 0;
}
u32 sceGxmProgramGetVertexProgramOutputs(u32 program) { const usse::Gxp* g = gxp_at(program); return g ? g->vertex_outputs : 0; }
u32 sceGxmProgramGetFragmentProgramInputs(u32 program) { const usse::Gxp* g = gxp_at(program); return g ? g->fragment_inputs : 0; }
u32 sceGxmProgramGetParameterCount(u32 program) {
    const usse::Gxp* g = gxp_at(program);
    return g ? static_cast<u32>(g->parameters.size()) : 0;
}
u32 sceGxmProgramGetParameter(u32 program, u32 index) {
    const usse::Gxp* g = gxp_at(program);
    return g && index < g->parameters.size() ? program + g->parameters[index].offset : 0;
}
u32 sceGxmProgramFindParameterByName(u32 program, Ptr<const char> name) {
    const usse::Gxp* g = gxp_at(program);
    if (!g || !name) return 0;
    const auto i = g->find(memory().string(name.address, 256));
    return i ? program + g->parameters[*i].offset : 0;
}
u32 sceGxmProgramFindParameterBySemantic(u32 program, u32 semantic, u32 index) {
    const usse::Gxp* g = gxp_at(program);
    if (!g) return 0;
    const auto i = g->find(static_cast<usse::Semantic>(semantic), index);
    return i ? program + g->parameters[*i].offset : 0;
}

// Parameters: their records' words, read where the game's pointer points.
u32 word(u32 parameter, u32 i) {
    return parameter && memory().valid(parameter, 16) ? *reinterpret_cast<const u32*>(memory().host(parameter + 4 * i)) : 0;
}
u32 sceGxmProgramParameterGetCategory(u32 p) { return word(p, 1) & 0xF; }
u32 sceGxmProgramParameterGetType(u32 p) { return (word(p, 1) >> 4) & 0xF; }
u32 sceGxmProgramParameterGetComponentCount(u32 p) { return (word(p, 1) >> 8) & 0xF; }
u32 sceGxmProgramParameterGetContainerIndex(u32 p) { return (word(p, 1) >> 12) & 0xF; }
u32 sceGxmProgramParameterGetSemantic(u32 p) {
    const u32 info = word(p, 1);
    return (info & 0xF) == 0 ? (info >> 16) & 0xFF : 0;
}
u32 sceGxmProgramParameterGetSemanticIndex(u32 p) { return (word(p, 1) >> 24) & 0xF; }
u32 sceGxmProgramParameterIsSamplerCube(u32 p) { return (word(p, 1) >> 28) & 1; }
u32 sceGxmProgramParameterGetArraySize(u32 p) { return word(p, 2); }
u32 sceGxmProgramParameterGetResourceIndex(u32 p) { return word(p, 3); }
u32 sceGxmProgramParameterGetName(u32 p) {
    const u32 offset = word(p, 0);
    return offset ? p + offset : 0;
}

// ---- The patcher -------------------------------------------------------------------------------------------------

Patcher* patcher_of(u32 guest) {
    auto it = state().patchers.find(guest);
    return it == state().patchers.end() ? nullptr : it->second.get();
}

i32 sceGxmShaderPatcherCreate(Ptr<const PatcherParams> params, Ptr<u32> out) {
    if (!params || !out) return err::kInvalidPointer;
    State& s = state();
    nx::scoped_lock<nx::mutex> held(s.lock);
    const u32 guest = guest_alloc(16);
    if (!guest) return err::kOutOfMemory;
    auto p = nx::make_unique<Patcher>();
    p->guest = guest;
    p->params = *params;
    s.patchers[guest] = std::move(p);
    *out = guest;
    return 0;
}

i32 sceGxmShaderPatcherDestroy(u32 patcher) {
    State& s = state();
    nx::scoped_lock<nx::mutex> held(s.lock);
    Patcher* p = patcher_of(patcher);
    if (!p) return err::kInvalidPointer;
    for (VertexProgram* v : p->vertex_programs) s.vertex_programs.erase(v->guest);
    for (FragmentProgram* f : p->fragment_programs) s.fragment_programs.erase(f->guest);
    for (Program* r : p->programs) s.programs.erase(r->guest);
    s.patchers.erase(patcher);
    return 0;
}

u32 sceGxmShaderPatcherGetUserData(u32 patcher) {
    nx::scoped_lock<nx::mutex> held(state().lock);
    Patcher* p = patcher_of(patcher);
    return p ? p->params.user_data : 0;
}
i32 sceGxmShaderPatcherSetUserData(u32 patcher, u32 data) {
    nx::scoped_lock<nx::mutex> held(state().lock);
    Patcher* p = patcher_of(patcher);
    if (!p) return err::kInvalidPointer;
    p->params.user_data = data;
    return 0;
}
u32 sceGxmShaderPatcherGetHostMemAllocated(u32) { return 0; }
u32 sceGxmShaderPatcherGetBufferMemAllocated(u32) { return 0; }
u32 sceGxmShaderPatcherGetVertexUsseMemAllocated(u32) { return 0; }
u32 sceGxmShaderPatcherGetFragmentUsseMemAllocated(u32) { return 0; }

i32 sceGxmShaderPatcherRegisterProgram(u32 patcher, u32 gxp, Ptr<u32> id) {
    if (!patcher || !gxp || !id) return err::kInvalidPointer;
    if (sceGxmProgramCheck(gxp) != 0) return err::kInvalidValue;
    const usse::Gxp* parsed = gxp_at(gxp);
    if (!parsed) return err::kInvalidValue;
    State& s = state();
    nx::scoped_lock<nx::mutex> held(s.lock);
    Patcher* p = patcher_of(patcher);
    if (!p) return err::kInvalidPointer;
    for (Program* r : p->programs) {
        if (r->gxp_address == gxp) {
            ++r->references;
            *id = r->guest;
            return 0;
        }
    }
    const u32 guest = guest_alloc(16);
    if (!guest) return err::kOutOfMemory;
    auto r = nx::make_unique<Program>();
    r->guest = guest;
    r->gxp_address = gxp;
    r->gxp = *parsed;
    r->primary = usse::decode_program(r->gxp.primary, usse::ProgramKind::Primary);
    r->secondary = usse::decode_program(r->gxp.secondary, usse::ProgramKind::Secondary);
    *reinterpret_cast<u32*>(memory().host(guest)) = gxp;
    p->programs.push_back(r.get());
    s.programs[guest] = std::move(r);
    *id = guest;
    return 0;
}

i32 unregister(u32 patcher, u32 id, bool force) {
    State& s = state();
    nx::scoped_lock<nx::mutex> held(s.lock);
    Patcher* p = patcher_of(patcher);
    auto it = s.programs.find(id);
    if (!p || it == s.programs.end()) return err::kInvalidPointer;
    Program* r = it->second.get();
    if (!force && --r->references > 0) return 0;
    for (VertexProgram* v : p->vertex_programs)
        if (v->program == r && !force) return err::kProgramInUse;
    for (FragmentProgram* f : p->fragment_programs)
        if (f->program == r && !force) return err::kProgramInUse;
    std::erase(p->programs, r);
    s.programs.erase(it);
    return 0;
}
i32 sceGxmShaderPatcherUnregisterProgram(u32 patcher, u32 id) { return unregister(patcher, id, false); }
i32 sceGxmShaderPatcherForceUnregisterProgram(u32 patcher, u32 id) { return unregister(patcher, id, true); }

u32 sceGxmShaderPatcherGetProgramFromId(u32 id) {
    nx::scoped_lock<nx::mutex> held(state().lock);
    auto it = state().programs.find(id);
    return it == state().programs.end() ? 0 : it->second->gxp_address;
}

i32 sceGxmShaderPatcherCreateVertexProgram(u32 patcher, u32 id, Ptr<const VertexAttribute> attributes, u32 attribute_count,
                                           Ptr<const VertexStream> streams, u32 stream_count, Ptr<u32> out) {
    if (!patcher || !id || !out || (attribute_count && !attributes) || (stream_count && !streams))
        return err::kInvalidPointer;
    if (attribute_count > 16 || stream_count > 16) return err::kInvalidValue;
    State& s = state();
    nx::scoped_lock<nx::mutex> held(s.lock);
    Patcher* p = patcher_of(patcher);
    auto it = s.programs.find(id);
    if (!p || it == s.programs.end()) return err::kInvalidPointer;
    Program* r = it->second.get();
    if (r->gxp.type != usse::ShaderType::Vertex) return err::kInvalidValue;
    nx::vector<VertexAttribute> attrs(attributes.get(), attributes.get() + attribute_count);
    nx::vector<VertexStream> strs(streams.get(), streams.get() + stream_count);
    for (const VertexAttribute& a : attrs)
        if (a.stream >= stream_count || a.format > 10 || !a.components || a.components > 4) return err::kInvalidValue;
    // (the same program with the same attributes and streams: the same vertex program, one more reference)
    for (VertexProgram* v : p->vertex_programs) {
        if (v->program != r || v->attributes.size() != attrs.size() || v->streams.size() != strs.size()) continue;
        if (!attrs.empty() && std::memcmp(v->attributes.data(), attrs.data(), attrs.size() * sizeof(VertexAttribute))) continue;
        if (!strs.empty() && std::memcmp(v->streams.data(), strs.data(), strs.size() * sizeof(VertexStream))) continue;
        ++v->references;
        *out = v->guest;
        return 0;
    }
    const u32 guest = guest_alloc(16);
    if (!guest) return err::kOutOfMemory;
    auto v = nx::make_unique<VertexProgram>();
    v->guest = guest;
    v->program = r;
    v->attributes = std::move(attrs);
    v->streams = std::move(strs);
    *reinterpret_cast<u32*>(memory().host(guest)) = r->gxp_address;
    p->vertex_programs.push_back(v.get());
    s.vertex_programs[guest] = std::move(v);
    *out = guest;
    return 0;
}

i32 sceGxmShaderPatcherCreateFragmentProgram(u32 patcher, u32 id, u32 output_format, u32 multisample,
                                             Ptr<const BlendInfo> blend, u32 vertex_gxp, Ptr<u32> out) {
    if (!patcher || !id || !out) return err::kInvalidPointer;
    if (multisample > 2) return err::kInvalidValue;
    State& s = state();
    nx::scoped_lock<nx::mutex> held(s.lock);
    Patcher* p = patcher_of(patcher);
    auto it = s.programs.find(id);
    if (!p || it == s.programs.end()) return err::kInvalidPointer;
    Program* r = it->second.get();
    if (r->gxp.type != usse::ShaderType::Fragment) return err::kInvalidValue;
    const BlendInfo b = blend ? *blend : BlendInfo{};
    for (FragmentProgram* f : p->fragment_programs) {
        if (f->program != r || f->output_format != output_format || f->multisample != multisample ||
            f->blend_enabled != static_cast<bool>(blend) || f->vertex_gxp != vertex_gxp)
            continue;
        if (blend && std::memcmp(&f->blend, &b, sizeof b)) continue;
        ++f->references;
        *out = f->guest;
        return 0;
    }
    const u32 guest = guest_alloc(16);
    if (!guest) return err::kOutOfMemory;
    auto f = nx::make_unique<FragmentProgram>();
    f->guest = guest;
    f->program = r;
    f->output_format = output_format;
    f->multisample = multisample;
    f->blend_enabled = static_cast<bool>(blend);
    f->blend = b;
    f->vertex_gxp = vertex_gxp;
    *reinterpret_cast<u32*>(memory().host(guest)) = r->gxp_address;
    p->fragment_programs.push_back(f.get());
    s.fragment_programs[guest] = std::move(f);
    *out = guest;
    return 0;
}

// One per patcher, shared (reference counted). Its program is an empty fragment program with no resources (the
// driver's own has no SceGxmProgram the game may read: sceGxmFragmentProgramGetProgram returns 0 for it).
i32 sceGxmShaderPatcherCreateMaskUpdateFragmentProgram(u32 patcher, Ptr<u32> out) {
    if (!patcher || !out) return err::kInvalidPointer;
    State& s = state();
    nx::scoped_lock<nx::mutex> held(s.lock);
    Patcher* p = patcher_of(patcher);
    if (!p) return err::kInvalidPointer;
    for (FragmentProgram* f : p->fragment_programs) {
        if (!f->mask_update) continue;
        ++f->references;
        *out = f->guest;
        return 0;
    }
    if (!s.mask_update_program) {
        s.mask_update_program = nx::make_unique<Program>();
        usse::Gxp& g = s.mask_update_program->gxp;
        g.type = usse::ShaderType::Fragment;
        g.output_type = usse::ParameterType::F16;
        g.output_components = 4;
    }
    const u32 guest = guest_alloc(16);
    if (!guest) return err::kOutOfMemory;
    auto f = nx::make_unique<FragmentProgram>();
    f->guest = guest;
    f->program = s.mask_update_program.get();
    f->mask_update = true;
    *reinterpret_cast<u32*>(memory().host(guest)) = 0;
    p->fragment_programs.push_back(f.get());
    s.fragment_programs[guest] = std::move(f);
    *out = guest;
    return 0;
}

i32 sceGxmShaderPatcherAddRefVertexProgram(u32 patcher, u32 program) {
    nx::scoped_lock<nx::mutex> held(state().lock);
    auto it = state().vertex_programs.find(program);
    if (!patcher_of(patcher) || it == state().vertex_programs.end()) return err::kInvalidPointer;
    ++it->second->references;
    return 0;
}
i32 sceGxmShaderPatcherAddRefFragmentProgram(u32 patcher, u32 program) {
    nx::scoped_lock<nx::mutex> held(state().lock);
    auto it = state().fragment_programs.find(program);
    if (!patcher_of(patcher) || it == state().fragment_programs.end()) return err::kInvalidPointer;
    ++it->second->references;
    return 0;
}
i32 sceGxmShaderPatcherGetVertexProgramRefCount(u32 patcher, u32 program, Ptr<u32> count) {
    nx::scoped_lock<nx::mutex> held(state().lock);
    auto it = state().vertex_programs.find(program);
    if (!patcher_of(patcher) || it == state().vertex_programs.end() || !count) return err::kInvalidPointer;
    *count = it->second->references;
    return 0;
}
i32 sceGxmShaderPatcherGetFragmentProgramRefCount(u32 patcher, u32 program, Ptr<u32> count) {
    nx::scoped_lock<nx::mutex> held(state().lock);
    auto it = state().fragment_programs.find(program);
    if (!patcher_of(patcher) || it == state().fragment_programs.end() || !count) return err::kInvalidPointer;
    *count = it->second->references;
    return 0;
}

i32 sceGxmShaderPatcherReleaseVertexProgram(u32 patcher, u32 program) {
    State& s = state();
    nx::scoped_lock<nx::mutex> held(s.lock);
    Patcher* p = patcher_of(patcher);
    auto it = s.vertex_programs.find(program);
    if (!p || it == s.vertex_programs.end()) return err::kInvalidPointer;
    if (--it->second->references > 0) return 0;
    std::erase(p->vertex_programs, it->second.get());
    s.vertex_programs.erase(it);
    return 0;
}
i32 sceGxmShaderPatcherReleaseFragmentProgram(u32 patcher, u32 program) {
    State& s = state();
    nx::scoped_lock<nx::mutex> held(s.lock);
    Patcher* p = patcher_of(patcher);
    auto it = s.fragment_programs.find(program);
    if (!p || it == s.fragment_programs.end()) return err::kInvalidPointer;
    if (--it->second->references > 0) return 0;
    std::erase(p->fragment_programs, it->second.get());
    s.fragment_programs.erase(it);
    return 0;
}

u32 sceGxmVertexProgramGetProgram(u32 program) {
    return program && memory().valid(program, 4) ? *reinterpret_cast<const u32*>(memory().host(program)) : 0;
}
u32 sceGxmFragmentProgramGetProgram(u32 program) { return sceGxmVertexProgramGetProgram(program); }
u32 sceGxmFragmentProgramIsEnabled(u32 program) {
    nx::scoped_lock<nx::mutex> held(state().lock);
    auto it = state().fragment_programs.find(program);
    return it != state().fragment_programs.end() && !it->second->mask_update ? 1 : 0;
}

const runtime::hle::Function kFunctions[] = {
    HLE_FUNCTION(0xED8B6C69, sceGxmProgramCheck),
    HLE_FUNCTION(0xBF5E2090, sceGxmProgramGetSize),
    HLE_FUNCTION(0x04BB3C59, sceGxmProgramGetType),
    HLE_FUNCTION(0x029B4F1C, sceGxmProgramIsDiscardUsed),
    HLE_FUNCTION(0x89613EF2, sceGxmProgramIsDepthReplaceUsed),
    HLE_FUNCTION(0xE63C53D8, sceGxmProgramIsSpriteCoordUsed),
    HLE_FUNCTION(0xA4297E57, sceGxmProgramIsNativeColorUsed),
    HLE_FUNCTION(0x104F23F4, sceGxmProgramIsFragColorUsed),
    HLE_FUNCTION(0x8FA3F9C3, sceGxmProgramGetDefaultUniformBufferSize),
    HLE_FUNCTION(0xFB01C7E5, sceGxmProgramGetVertexProgramOutputs),
    HLE_FUNCTION(0xC6924709, sceGxmProgramGetFragmentProgramInputs),
    HLE_FUNCTION(0xD5D5FCCD, sceGxmProgramGetParameterCount),
    HLE_FUNCTION(0x06FF9151, sceGxmProgramGetParameter),
    HLE_FUNCTION(0x277794C4, sceGxmProgramFindParameterByName),
    HLE_FUNCTION(0x633CAA54, sceGxmProgramFindParameterBySemantic),
    HLE_FUNCTION(0x7FFFDD7A, sceGxmProgramFindParameterBySemantic),  // (_sceGxmProgramFindParameterBySemantic)
    HLE_FUNCTION(0x1997DC17, sceGxmProgramParameterGetCategory),
    HLE_FUNCTION(0x7B9023C3, sceGxmProgramParameterGetType),
    HLE_FUNCTION(0xBD2998D1, sceGxmProgramParameterGetComponentCount),
    HLE_FUNCTION(0xBB58267D, sceGxmProgramParameterGetContainerIndex),
    HLE_FUNCTION(0xE6D9C4CE, sceGxmProgramParameterGetSemantic),
    HLE_FUNCTION(0xAAFD61D5, sceGxmProgramParameterGetSemantic),  // (_sceGxmProgramParameterGetSemantic)
    HLE_FUNCTION(0xB85CC13E, sceGxmProgramParameterGetSemanticIndex),
    HLE_FUNCTION(0xF7AA978B, sceGxmProgramParameterIsSamplerCube),
    HLE_FUNCTION(0xDBA8D061, sceGxmProgramParameterGetArraySize),
    HLE_FUNCTION(0x5C79D59A, sceGxmProgramParameterGetResourceIndex),
    HLE_FUNCTION(0x6AF88A5D, sceGxmProgramParameterGetName),
    HLE_FUNCTION(0x05032658, sceGxmShaderPatcherCreate),
    HLE_FUNCTION(0xEAA5B100, sceGxmShaderPatcherDestroy),
    HLE_FUNCTION(0x96A7E6DD, sceGxmShaderPatcherGetUserData),
    HLE_FUNCTION(0xF9B8FCFD, sceGxmShaderPatcherSetUserData),
    HLE_FUNCTION(0x9DBBC71C, sceGxmShaderPatcherGetHostMemAllocated),
    HLE_FUNCTION(0xC694D039, sceGxmShaderPatcherGetBufferMemAllocated),
    HLE_FUNCTION(0x7D2F83C1, sceGxmShaderPatcherGetVertexUsseMemAllocated),
    HLE_FUNCTION(0x3C9DDB4A, sceGxmShaderPatcherGetFragmentUsseMemAllocated),
    HLE_FUNCTION(0x2B528462, sceGxmShaderPatcherRegisterProgram),
    HLE_FUNCTION(0xF103AF8A, sceGxmShaderPatcherUnregisterProgram),
    HLE_FUNCTION(0x630D4B2E, sceGxmShaderPatcherForceUnregisterProgram),
    HLE_FUNCTION(0xA949A803, sceGxmShaderPatcherGetProgramFromId),
    HLE_FUNCTION(0xB7BBA6D5, sceGxmShaderPatcherCreateVertexProgram),
    HLE_FUNCTION(0x4ED2E49D, sceGxmShaderPatcherCreateFragmentProgram),
    HLE_FUNCTION(0xA4433427, sceGxmShaderPatcherCreateMaskUpdateFragmentProgram),
    HLE_FUNCTION(0x0FD1E589, sceGxmShaderPatcherAddRefVertexProgram),
    HLE_FUNCTION(0x4CD2D19F, sceGxmShaderPatcherAddRefFragmentProgram),
    HLE_FUNCTION(0xA1A16FF6, sceGxmShaderPatcherGetVertexProgramRefCount),
    HLE_FUNCTION(0x2C5550F0, sceGxmShaderPatcherGetFragmentProgramRefCount),
    HLE_FUNCTION(0xAC1FF2DA, sceGxmShaderPatcherReleaseVertexProgram),
    HLE_FUNCTION(0xBE2743D1, sceGxmShaderPatcherReleaseFragmentProgram),
    HLE_FUNCTION(0xBC52320E, sceGxmVertexProgramGetProgram),
    HLE_FUNCTION(0xE0E3B3F8, sceGxmFragmentProgramGetProgram),
    HLE_FUNCTION(0x5748367E, sceGxmFragmentProgramIsEnabled),
};

}  // namespace

void register_patcher() { runtime::hle::register_library({"SceGxm", kSceGxm, kFunctions}); }

}  // namespace psvrecore::gxm::hle
