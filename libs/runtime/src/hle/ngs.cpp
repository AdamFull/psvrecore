// NGS (SceNgs): the system, its racks of voices built from the voice definitions, the patches between voices, and
// sceNgsSystemUpdate, which processes every playing voice once (one packet of the system's granularity) in the order
// the patches make (a rack after the racks that feed it), then calls the callbacks the update raised. What the
// modules do is in ngs/modules.cpp.
//
// Handles are guest addresses in the memory the guest gave: the system's in its system memory, a rack's at the
// start of its rack memory, a voice's and its patches' inside that. Each module of a voice has its parameter area
// there too: sceNgsVoiceLockParams hands it out, and the parameters written in it (or given with
// sceNgsVoiceSetParamsBlock) take effect at the next update that does not fall within sceNgsSystemLock and
// sceNgsSystemUnlock, as do voices started, keyed off, paused and resumed.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

#include "../ngs/modules.hpp"
#include "libatrac9.h"
#include "hle_common.hpp"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/threading/thread.h"

namespace psvrecore::runtime::hle {

namespace {

using namespace ngs;
namespace nerr = ngs::err;

constexpr u32 kSceNgs = 0xB01598D9;

// ---- voice definitions

enum class Kind { Buss, Master, MixerBuss, SideChain, SasEmu, Simple, Template1 };

struct Definition {
    const char* name;
    Kind kind;
    u32 modules[10];
    u32 module_count;
    u32 inputs, outputs;
    u32 bypassed;  // (bit per module: bypassed when a voice is initialized)
};

constexpr Definition kDefinitions[] = {
    {"master buss", Kind::Master, {kInputMixer, kOutput}, 2, 1, 0, 0},
    {"mixer buss", Kind::MixerBuss, {kInputMixer}, 1, 1, 1, 0},
    {"compressor buss", Kind::Buss, {kInputMixer, kCompressor}, 2, 1, 1, 0},
    {"side chain compressor buss", Kind::SideChain, {kInputMixer, kInputMixer, kCompressor}, 3, 2, 1, 0},
    {"delay buss", Kind::Buss, {kInputMixer, kDelay}, 2, 1, 1, 0},
    {"distortion buss", Kind::Buss, {kInputMixer, kDistortion}, 2, 1, 1, 0},
    {"envelope buss", Kind::Buss, {kInputMixer, kEnvelope}, 2, 1, 1, 0},
    {"EQ buss", Kind::Buss, {kInputMixer, kParamEq}, 2, 1, 1, 0},
    {"pauser buss", Kind::Buss, {kInputMixer, kPauser}, 2, 1, 1, 0},
    {"pitch shift buss", Kind::Buss, {kInputMixer, kPitchShift}, 2, 1, 1, 0},
    {"reverb buss", Kind::Buss, {kInputMixer, kReverb}, 2, 1, 1, 0},
    {"SAS emulation voice", Kind::SasEmu, {kPlayer, kEnvelope}, 2, 0, 1, 0},
    {"simple voice", Kind::Simple, {kPlayer, kParamEq, kEnvelope, kPauser, kFilter, kFilter}, 6, 0, 2, 0b111110},
    {"simple ATRAC9 voice", Kind::Simple, {kAt9Player, kParamEq, kEnvelope, kPauser, kFilter, kFilter}, 6, 0, 2, 0b111110},
    {"template 1 voice",
     Kind::Template1,
     {kPlayer, kGenerator, kMixer, kParamEq, kEnvelope, kDistortion, kParamEq, kParamEq, kParamEq, kParamEq},
     10, 0, 4, 0b1111111010},
    {"ATRAC9 voice",
     Kind::Template1,
     {kAt9Player, kGenerator, kMixer, kParamEq, kEnvelope, kDistortion, kParamEq, kParamEq, kParamEq, kParamEq},
     10, 0, 4, 0b1111111010},
};
enum DefinitionIndex : u32 {
    kMasterBuss, kMixerBuss, kCompressorBuss, kSideChainBuss, kDelayBuss, kDistortionBuss, kEnvelopeBuss, kEqBuss,
    kPauserBuss, kPitchShiftBuss, kReverbBuss, kSasEmu, kSimple, kSimpleAt9, kTemplate1, kAt9Voice, kDefinitionCount
};

// ---- the guest's structures

struct InitParams {
    i32 max_racks, max_voices, granularity, sample_rate, max_modules;
};
struct RackDescription {
    u32 definition;
    i32 voices, channels, max_patches_per_input, patches_per_output;
    u32 user_release_data;
};
struct BufferInfo {
    u32 data, size;
};
struct PatchSetup {
    u32 source;
    i32 output, sub_index;
    u32 destination;
    i32 input;
};
struct VolumeMatrix {
    f32 m[2][2];
};
struct RouteInfo {
    i32 output_channels, input_channels;
    VolumeMatrix vols;
};
struct VoiceInfo {
    u32 state, modules, inputs, outputs, patches_per_output, update_calls_active;
};
struct CallbackInfo {
    u32 voice, rack, module;
    i32 data, data2;
    u32 ptr, user;
};
struct VoicePreset {
    i32 name_offset;
    u32 name_length;
    i32 data_offset;
    u32 data_size;
    i32 bypass_offset;
    u32 bypass_count;
};
struct ParamHeader {
    i32 module, channel;
};

// ---- the system

constexpr u32 kVoiceHeader = 64, kPatchSlot = 32, kRackHeader = 64;
constexpr u32 kStateAvailable = 0, kStateActive = 1, kStateFinalizing = 4, kStatePending = 16, kStatePaused = 32,
              kStateKeyOff = 64;

struct Rack;
struct Patch;

u32 align16(u32 v) { return (v + 15) & ~15u; }

struct Voice {
    u32 handle = 0;
    Rack* rack = nullptr;
    u32 index = 0;
    std::vector<nx::unique_ptr<Module>> modules;
    std::vector<u32> areas;       // guest parameter area of each module
    std::vector<u32> area_iface;  // the interface each area is in
    std::vector<bool> locked;
    std::vector<nx::string> out_of_range;
    struct Change {
        u32 module, iface;
        nx::vector<u8> bytes;
    };
    nx::vector<Change> changes;

    enum class Run { Stopped, Active, Finalizing } run = Run::Stopped;
    bool play_requested = false, key_off_requested = false;
    bool paused = false, pause_requested = false, resume_requested = false;
    bool pausing = false, resuming = false;
    bool ended = false, finished = false;
    u32 update_calls = 0;
    u64 active_samples = 0;
    u32 finished_callback = 0, finished_user = 0;

    std::vector<Packet> inputs, outputs;
    nx::vector<i16> pcm;
    std::vector<Patch*> out_patches;  // [output * patches_per_output + sub]
    std::vector<Patch*> in_patches;   // (patches into this voice)
};

struct Rack {
    u32 handle = 0;
    u32 size = 0;
    const Definition* def = nullptr;
    i32 channels = 1, max_patches_per_input = 1, patches_per_output = 1;
    u32 user_release_data = 0;
    u32 error_callback = 0;
    std::vector<nx::unique_ptr<Voice>> voices;
    bool releasing = false;
    u32 release_callback = 0;
};

struct Patch {
    u32 handle = 0;
    Voice* source = nullptr;
    i32 output = 0, sub = 0;
    Voice* destination = nullptr;
    i32 input = 0;
    VolumeMatrix vols{};
};

struct Pending {
    u32 function;
    CallbackInfo info;
};

struct Ngs {
    nx::mutex lock;
    bool exists = false;
    u32 handle = 0;
    InitParams params{};
    u32 flags = 1;  // SCE_NGS_SYSTEM_FLAG_MODULE_PARAM_CHECKING
    u32 error_callback = 0;
    bool user_locked = false;
    u32 voice_count = 0;
    std::vector<nx::unique_ptr<Rack>> racks;
    nx::flat_hash_map<u32, Voice*> voices;
    nx::flat_hash_map<u32, nx::unique_ptr<Patch>> patches;
    u32 definitions[kDefinitionCount] = {};
};

Ngs& ngs_state() {
    static Ngs n;
    return n;
}

const Definition* definition_at(u32 address) {
    Ngs& n = ngs_state();
    for (u32 i = 0; i < kDefinitionCount; ++i)
        if (address && n.definitions[i] == address) return &kDefinitions[i];
    return nullptr;
}

Context context_of(const Ngs& n, const Voice& v) {
    Context c;
    c.granularity = static_cast<u32>(n.params.granularity);
    c.sample_rate = static_cast<u32>(n.params.sample_rate);
    c.channels = static_cast<u32>(v.rack->channels);
    c.active_samples = v.active_samples;
    return c;
}

Voice* voice_of(Ngs& n, u32 handle) {
    if (!n.exists) return nullptr;
    auto it = n.voices.find(handle);
    return it == n.voices.end() || it->second->rack->releasing ? nullptr : it->second;
}
Rack* rack_of(Ngs& n, u32 handle) {
    if (!n.exists) return nullptr;
    for (auto& r : n.racks)
        if (r->handle == handle && !r->releasing) return r.get();
    return nullptr;
}
Patch* patch_of(Ngs& n, u32 handle) {
    auto it = n.patches.find(handle);
    return it == n.patches.end() ? nullptr : it->second.get();
}

// The memory a rack needs: its header, and for each voice a header, its modules' parameter areas and its patch
// slots.
u32 voice_size(const Definition& d, const RackDescription& desc) {
    u32 size = kVoiceHeader;
    for (u32 m = 0; m < d.module_count; ++m) {
        const TypeInfo* t = type_info(d.modules[m]);
        size += align16(max_params_size(d.modules[m]) * (t->per_channel ? static_cast<u32>(desc.channels) : 1));
    }
    return size + d.outputs * static_cast<u32>(nx::max(desc.patches_per_output, 1)) * kPatchSlot;
}
u32 rack_size(const Definition& d, const RackDescription& desc) {
    return kRackHeader + voice_size(d, desc) * static_cast<u32>(desc.voices);
}

void write_area(Voice& v, u32 m) {
    Module& mod = *v.modules[m];
    if (!v.areas[m]) return;
    std::memcpy(memory().host(v.areas[m]), mod.params(), mod.params_bytes());
    v.area_iface[m] = mod.interface_id();
}

void remove_patch(Ngs& n, Patch* p) {
    auto& outs = p->source->out_patches;
    for (auto& slot : outs)
        if (slot == p) slot = nullptr;
    auto& ins = p->destination->in_patches;
    ins.erase(std::remove(ins.begin(), ins.end(), p), ins.end());
    n.patches.erase(p->handle);
}

void remove_patches_of(Ngs& n, Voice& v, bool outputs, bool inputs) {
    nx::vector<Patch*> doomed;
    if (outputs)
        for (Patch* p : v.out_patches)
            if (p) doomed.push_back(p);
    if (inputs)
        for (Patch* p : v.in_patches) doomed.push_back(p);
    for (Patch* p : doomed)
        if (n.patches.contains(p->handle)) remove_patch(n, p);
}

void stop(Voice& v) {
    v.run = Voice::Run::Stopped;
    v.play_requested = v.key_off_requested = false;
    v.paused = v.pause_requested = v.resume_requested = v.pausing = v.resuming = false;
}

// Module parameters and bypasses as a voice starts out (the definition's, or a preset's).
i32 apply_block(Ngs& n, Voice& v, u32 address, u32 size, i32* errors);

i32 apply_preset(Ngs& n, Voice& v, u32 preset) {
    if (!memory().valid(preset, sizeof(VoicePreset))) return nerr::kInvalidParam;
    VoicePreset p;
    std::memcpy(&p, memory().host(preset), sizeof p);
    if (p.data_offset && p.data_size) {
        i32 errors = 0;
        if (const i32 r = apply_block(n, v, preset + static_cast<u32>(p.data_offset), p.data_size, &errors); r < 0) return r;
    }
    if (p.bypass_offset && p.bypass_count) {
        const u32 list = preset + static_cast<u32>(p.bypass_offset);
        if (!memory().valid(list, 4 * p.bypass_count)) return nerr::kInvalidParam;
        for (auto& m : v.modules) m->bypassed = false;
        for (u32 i = 0; i < p.bypass_count; ++i) {
            u32 m;
            std::memcpy(&m, memory().host(list + 4 * i), 4);
            if (m < v.modules.size()) v.modules[m]->bypassed = true;
        }
    }
    return 0;
}

void reset_modules(Ngs& n, Voice& v) {
    const Definition& d = *v.rack->def;
    for (u32 m = 0; m < v.modules.size(); ++m) {
        v.modules[m]->defaults();
        v.modules[m]->bypassed = (d.bypassed >> m) & 1;
        v.modules[m]->apply(context_of(n, v));
        write_area(v, m);
        v.locked[m] = false;
    }
    v.changes.clear();
}

// The parameters the guest wrote in a module's area take effect at the next update.
i32 queue_change(Ngs& n, Voice& v, u32 m) {
    Module& mod = *v.modules[m];
    nx::unique_ptr<Module> probe = make_module(mod.type(), static_cast<u32>(v.rack->channels));
    probe->load(memory().host(v.areas[m]), v.area_iface[m]);
    const u32 stride = probe->interface_size();
    if (n.flags & 1) {
        for (u32 ch = 0; ch < probe->param_channels(); ++ch) {
            nx::string message;
            if (!probe->check(probe->params() + ch * stride, &message)) {
                // (the first 20 are logged; the result tells the game every time)
                static std::atomic<u32> reported{0};
                if (reported.fetch_add(1) < 20)
                    nx::logw(logs::hle, "NGS: voice 0x{:08x} module {}: parameters refused, {}", v.handle, m, message);
                v.out_of_range[m] = message;
                return nerr::kParamOutOfRange;
            }
        }
    }
    Voice::Change c{m, v.area_iface[m], {}};
    c.bytes.assign(probe->params(), probe->params() + probe->params_bytes());
    v.changes.push_back(std::move(c));
    return 0;
}

// Switches a module's guest area to an interface (keeping what carries over).
bool switch_area(Voice& v, u32 m, u32 iface) {
    if (v.area_iface[m] == iface) return true;
    Module& mod = *v.modules[m];
    nx::unique_ptr<Module> probe = make_module(mod.type(), static_cast<u32>(v.rack->channels));
    probe->load(memory().host(v.areas[m]), v.area_iface[m]);
    if (!probe->select_interface(iface)) return false;
    std::memcpy(memory().host(v.areas[m]), probe->params(), probe->params_bytes());
    v.area_iface[m] = iface;
    return true;
}

i32 apply_block(Ngs& n, Voice& v, u32 address, u32 size, i32* errors) {
    i32 result = 0;
    u32 at = address;
    const u32 end = address + size;
    while (at + sizeof(ParamHeader) + 8 <= end) {
        if (!memory().valid(at, sizeof(ParamHeader) + 8)) return nerr::kInvalidParam;
        ParamHeader h;
        u32 desc[2];
        std::memcpy(&h, memory().host(at), sizeof h);
        std::memcpy(desc, memory().host(at + sizeof h), 8);
        const u32 block = static_cast<u32>(sizeof h) + desc[1];
        if (desc[1] < 8 || at + block > end || !memory().valid(at, block)) {
            ++*errors;
            return nerr::kSizeMismatch;
        }
        i32 r = 0;
        const u32 m = static_cast<u32>(h.module);
        if (m >= v.modules.size()) r = nerr::kInvalidParam;
        else if (v.locked[m]) r = nerr::kResourceLocked;
        else if (!switch_area(v, m, desc[0])) r = nerr::kParamTypeMismatch;
        if (r == 0) {
            Module& mod = *v.modules[m];
            nx::unique_ptr<Module> probe = make_module(mod.type(), static_cast<u32>(v.rack->channels));
            probe->load(memory().host(v.areas[m]), v.area_iface[m]);
            const u32 stride = probe->interface_size();
            if (desc[1] != stride) {
                r = nerr::kSizeMismatch;
            } else if (h.channel != -1 && (h.channel < 0 || static_cast<u32>(h.channel) >= probe->param_channels())) {
                r = nerr::kInvalidParam;
            } else {
                for (u32 ch = 0; ch < probe->param_channels(); ++ch)
                    if (h.channel == -1 || static_cast<u32>(h.channel) == ch)
                        std::memcpy(memory().host(v.areas[m] + ch * stride), memory().host(at + sizeof h), stride);
                r = queue_change(n, v, m);
            }
        }
        if (r < 0) {
            ++*errors;
            result = r;
        }
        at += block;
    }
    return result;
}

// ---- processing

void add_inputs(Ngs& n, Voice& v) {
    const u32 g = static_cast<u32>(n.params.granularity);
    for (Packet& p : v.inputs) p.clear(g);
    for (Patch* p : v.in_patches) {
        const Voice& s = *p->source;
        if (s.run == Voice::Run::Stopped && !s.pausing) continue;
        if (static_cast<usize>(p->output) >= s.outputs.size() || static_cast<usize>(p->input) >= v.inputs.size()) continue;
        const Packet& from = s.outputs[p->output];
        Packet& to = v.inputs[p->input];
        const u32 sc = static_cast<u32>(s.rack->channels), dc = static_cast<u32>(v.rack->channels);
        for (u32 a = 0; a < sc; ++a)
            for (u32 b = 0; b < dc; ++b) {
                const f32 vol = p->vols.m[a][b];
                if (vol == 0) continue;
                for (u32 i = 0; i < g; ++i) to.s[b][i] += from.s[a][i] * vol;
            }
    }
}

// A ramp over the start (fade in) or the start then silence (fade out) of a packet.
void fade(Packet& p, const Context& c, u32 length, bool out) {
    for (u32 i = 0; i < c.granularity; ++i) {
        f32 g = 1;
        if (out) g = i < length ? 1.0f - static_cast<f32>(i + 1) / static_cast<f32>(length) : 0.0f;
        else if (i < length) g = static_cast<f32>(i + 1) / static_cast<f32>(length);
        for (u32 ch = 0; ch < c.channels; ++ch) p.s[ch][i] *= g;
    }
}

void process_voice(Ngs& n, Voice& v) {
    Context c = context_of(n, v);
    const u32 g = c.granularity;
    const Definition& d = *v.rack->def;
    auto& mods = v.modules;
    for (Packet& p : v.outputs) p.clear(g);
    Packet a;
    a.clear(g);

    auto effect = [&](u32 m, Packet& io) {
        Module& mod = *mods[m];
        if (mod.bypassed) return;
        if (mod.type() == kEnvelope) {
            switch (envelope(mod, c, io)) {
                case EnvelopeEnd::Kill: v.ended = true; break;
                case EnvelopeEnd::Release: v.run = Voice::Run::Finalizing; break;
                case EnvelopeEnd::Finished: v.ended = v.finished = true; break;
                case EnvelopeEnd::None: break;
            }
        } else if (mod.type() == kCompressor) {
            compress(mod, c, io, nullptr);
        } else {
            mod.process(c, io);
        }
    };
    // (pausing and resuming fade where the voice's pauser or mixer is)
    auto pause_stage = [&](u32 m, Packet& io) {
        Module& mod = *mods[m];
        if (mod.bypassed) return;
        if (v.pausing) fade(io, c, pause_lead_out(mod), true);
        else if (v.resuming) fade(io, c, pause_lead_in(mod), false);
    };
    auto input = [&](u32 m, u32 index) {
        if (mods[m]->bypassed) a.clear(g);
        else a = v.inputs[index];
    };

    switch (d.kind) {
        case Kind::Master:
        case Kind::MixerBuss:
        case Kind::Buss:
            input(0, 0);
            if (d.kind == Kind::Buss) {
                effect(1, a);
                if (mods[1]->type() == kPauser) pause_stage(1, a);
            }
            if (d.kind == Kind::Master) {
                const u32 ch = static_cast<u32>(v.rack->channels);
                for (u32 i = 0; i < g; ++i)
                    for (u32 k = 0; k < ch; ++k) {
                        const f32 s = std::nearbyint(a.s[k][i] * 32768.0f);
                        v.pcm[i * ch + k] = static_cast<i16>(s > 32767.0f ? 32767.0f : s < -32768.0f ? -32768.0f : s);
                    }
            } else {
                v.outputs[0] = a;
            }
            break;
        case Kind::SideChain: {
            input(0, 0);
            const bool chain = !mods[1]->bypassed;
            if (!mods[2]->bypassed) compress(*mods[2], c, a, chain ? &v.inputs[1] : nullptr);
            v.outputs[0] = a;
            break;
        }
        case Kind::SasEmu:
        case Kind::Simple:
        case Kind::Template1: {
            bool ended = false;
            if (!mods[0]->bypassed) render_player(*mods[0], c, a, &ended);
            if (ended) v.ended = v.finished = true;
            if (d.kind == Kind::SasEmu) {
                effect(1, a);
                v.outputs[0] = a;
                break;
            }
            if (d.kind == Kind::Simple) {
                effect(1, a);
                effect(2, a);
                pause_stage(3, a);
                for (u32 o = 0; o < 2; ++o) {
                    v.outputs[o] = a;
                    effect(4 + o, v.outputs[o]);
                }
                break;
            }
            Packet b;
            b.clear(g);
            if (!mods[1]->bypassed) render_generator(*mods[1], c, b);
            if (!mods[2]->bypassed) {
                Packet mixed;
                mix(*mods[2], c, a, b, mixed);
                a = mixed;
                pause_stage(2, a);
            }
            effect(3, a);
            effect(4, a);
            effect(5, a);
            for (u32 o = 0; o < 4; ++o) {
                v.outputs[o] = a;
                effect(6 + o, v.outputs[o]);
            }
            break;
        }
    }
    v.active_samples += g;
    ++v.update_calls;
}

// Starts what was asked for since the last update (none of it while the system is locked).
void apply_requests(Ngs& n, Voice& v) {
    for (auto& ch : v.changes) {
        Module& mod = *v.modules[ch.module];
        mod.load(ch.bytes.data(), ch.iface);
        mod.apply(context_of(n, v));
    }
    v.changes.clear();
    v.resuming = false;
    if (v.play_requested) {
        v.play_requested = false;
        v.run = Voice::Run::Active;
        v.active_samples = 0;
        v.update_calls = 0;
        v.ended = v.finished = false;
        v.paused = v.pausing = false;
        for (auto& m : v.modules) {
            m->reset(context_of(n, v));
            m->event = Event{};
        }
    }
    if (v.key_off_requested) {
        v.key_off_requested = false;
        if (v.run == Voice::Run::Active) {
            Module* env = nullptr;
            for (auto& m : v.modules)
                if (m->type() == kEnvelope && !m->bypassed) env = m.get();
            if (env) {
                envelope_key_off(*env);
                v.run = Voice::Run::Finalizing;
            } else {
                stop(v);
            }
        }
    }
    if (v.pause_requested) {
        v.pause_requested = false;
        if (v.run != Voice::Run::Stopped && !v.paused) {
            // (a pauser or mixer fades out over this update; without one the voice stops at once)
            bool fades = false;
            for (auto& m : v.modules)
                if ((m->type() == kPauser || m->type() == kMixer) && !m->bypassed && pause_lead_out(*m)) fades = true;
            if (fades) v.pausing = true;
            else v.paused = true;
        }
    }
    if (v.resume_requested) {
        v.resume_requested = false;
        if (v.paused) v.paused = false, v.resuming = true;
    }
}

// The racks in processing order: each after the racks patched into it.
std::vector<Rack*> rack_order(Ngs& n) {
    std::vector<Rack*> order, pending;
    for (auto& r : n.racks) pending.push_back(r.get());
    while (!pending.empty()) {
        bool progressed = false;
        for (usize i = 0; i < pending.size(); ++i) {
            Rack* r = pending[i];
            bool ready = true;
            for (auto& v : r->voices)
                for (Patch* p : v->in_patches)
                    if (std::find(pending.begin(), pending.end(), p->source->rack) != pending.end() && p->source->rack != r)
                        ready = false;
            if (!ready) continue;
            order.push_back(r);
            pending.erase(pending.begin() + static_cast<isize>(i));
            progressed = true;
            break;
        }
        if (!progressed) {  // (cannot happen: patches never make a cycle)
            order.insert(order.end(), pending.begin(), pending.end());
            break;
        }
    }
    return order;
}

bool depends_on(Rack* from, Rack* target, u32 depth = 0) {
    // (does `from` feed `target`, through patches?)
    if (from == target) return true;
    if (depth > 64) return true;
    for (auto& v : from->voices)
        for (Patch* p : v->out_patches)
            if (p && depends_on(p->destination->rack, target, depth + 1)) return true;
    return false;
}

void run_callbacks(PsvContext* ctx, const nx::vector<Pending>& calls) {
    for (const Pending& p : calls) {
        // (the information on the guest's stack, below what the caller uses)
        const u32 saved = ctx->r[13];
        const u32 info = (saved - sizeof(CallbackInfo) - 16) & ~15u;
        std::memcpy(memory().host(info), &p.info, sizeof p.info);
        ctx->r[13] = info;
        const u32 args[] = {info};
        call_guest(ctx, p.function, args);
        ctx->r[13] = saved;
    }
}

i32 update(PsvContext* ctx, Ngs& n) {
    nx::vector<Pending> calls;
    {
        nx::scoped_lock<nx::mutex> held(n.lock);
        if (!n.user_locked)
            for (auto& r : n.racks)
                for (auto& v : r->voices) apply_requests(n, *v);
        for (Rack* r : rack_order(n)) {
            if (r->releasing) continue;
            for (auto& vp : r->voices) {
                Voice& v = *vp;
                const bool live = v.run != Voice::Run::Stopped && (!v.paused || v.pausing);
                if (!live) {
                    for (Packet& p : v.outputs) p.clear(static_cast<u32>(n.params.granularity));
                    std::fill(v.pcm.begin(), v.pcm.end(), i16{0});
                    continue;
                }
                add_inputs(n, v);
                process_voice(n, v);
                if (v.pausing) v.pausing = false, v.paused = true;
            }
        }
        // Callbacks: the last each module raised, then the voices that finished, then the racks released.
        for (auto& r : n.racks) {
            for (auto& vp : r->voices) {
                Voice& v = *vp;
                for (u32 m = 0; m < v.modules.size(); ++m) {
                    Module& mod = *v.modules[m];
                    if (mod.event.raised && mod.callback)
                        calls.push_back({mod.callback, {v.handle, r->handle, m, mod.event.data, mod.event.data2,
                                                        mod.event.ptr, mod.callback_user}});
                    mod.event = Event{};
                }
                if (v.ended) {
                    const bool finished = v.finished;
                    stop(v);
                    v.ended = v.finished = false;
                    if (finished && v.finished_callback)
                        calls.push_back({v.finished_callback, {v.handle, r->handle, 0, 0, 0, 0, v.finished_user}});
                }
            }
        }
        for (usize i = 0; i < n.racks.size();) {
            Rack* r = n.racks[i].get();
            if (!r->releasing) {
                ++i;
                continue;
            }
            if (r->release_callback)
                calls.push_back({r->release_callback, {0, r->handle, 0, 0, 0, 0, r->user_release_data}});
            for (auto& v : r->voices) n.voices.erase(v->handle);
            n.voice_count -= static_cast<u32>(r->voices.size());
            n.racks.erase(n.racks.begin() + static_cast<isize>(i));
        }
    }
    run_callbacks(ctx, calls);
    kernel().checkpoint();
    return 0;
}

// ---- system functions

// The system's memory: its state is the host's, the guest's memory only names it, so it asks for little. (Games
// size it for the console's: CRI's sound engine initializes the system in a static 8 KiB buffer and gives up when
// more is asked for; 43 racks and some hundred voices fit there.)
u32 system_size(const InitParams& p) {
    return 512 + 8 * static_cast<u32>(nx::max(p.max_racks, 0)) + 8 * static_cast<u32>(nx::max(p.max_voices, 0));
}

i32 sceNgsSystemGetRequiredMemorySize(Ptr<const InitParams> params, Ptr<u32> size) {
    if (!params || !memory().valid(params.address, sizeof(InitParams)) || !writable(size.address, 4))
        return nerr::kInvalidParam;
    *size = system_size(*params);
    return 0;
}

i32 sceNgsSystemInit(Ptr<u8> mem, u32 size, Ptr<const InitParams> params, Ptr<u32> handle) {
    if (!mem || !params || !memory().valid(params.address, sizeof(InitParams)) || !writable(handle.address, 4))
        return nerr::kInvalidParam;
    if (mem.address & 15) return nerr::kInvalidAlignment;
    const InitParams p = *params;
    const i32 g = p.granularity;
    if (p.max_racks < 1 || p.max_voices < 1 || p.sample_rate <= 0 || (g != 64 && g != 128 && g != 256 && g != 512))
        return nerr::kInvalidParam;
    const u32 need = system_size(p);
    if (size < need || !writable(mem.address, need)) return nerr::kSizeMismatch;
    Ngs& n = ngs_state();
    nx::scoped_lock<nx::mutex> held(n.lock);
    if (n.exists) return nerr::kError;  // (one system per process)
    n.exists = true;
    n.handle = mem.address;
    n.params = p;
    n.flags = 1;
    n.error_callback = 0;
    n.user_locked = false;
    n.voice_count = 0;
    *handle = n.handle;
    return 0;
}

i32 sceNgsSystemUpdate(PsvContext* ctx, u32 handle) {
    Ngs& n = ngs_state();
    if (!n.exists || handle != n.handle) return nerr::kInvalidHandle;
    return update(ctx, n);
}

i32 sceNgsSystemRelease(u32 handle) {
    Ngs& n = ngs_state();
    nx::scoped_lock<nx::mutex> held(n.lock);
    if (!n.exists || handle != n.handle) return nerr::kInvalidHandle;
    n.patches.clear();
    n.voices.clear();
    n.racks.clear();
    n.exists = false;
    return 0;
}

i32 sceNgsSystemLock(u32 handle) {
    Ngs& n = ngs_state();
    for (;;) {
        {
            nx::scoped_lock<nx::mutex> held(n.lock);
            if (!n.exists || handle != n.handle) return nerr::kInvalidHandle;
            if (!n.user_locked) {
                n.user_locked = true;
                return 0;
            }
        }
        Kernel::Blocking blocking(kernel());
        nx::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

i32 sceNgsSystemUnlock(u32 handle) {
    Ngs& n = ngs_state();
    nx::scoped_lock<nx::mutex> held(n.lock);
    if (!n.exists || handle != n.handle) return nerr::kInvalidHandle;
    n.user_locked = false;
    return 0;
}

i32 sceNgsSystemSetParamErrorCallback(u32 handle, u32 callback) {
    Ngs& n = ngs_state();
    nx::scoped_lock<nx::mutex> held(n.lock);
    if (!n.exists || handle != n.handle) return nerr::kInvalidHandle;
    n.error_callback = callback;
    return 0;
}

i32 sceNgsSystemSetFlags(u32 handle, u32 flags) {
    Ngs& n = ngs_state();
    nx::scoped_lock<nx::mutex> held(n.lock);
    if (!n.exists || handle != n.handle) return nerr::kInvalidHandle;
    if (flags > 1) return nerr::kInvalidParam;
    n.flags = flags;
    return 0;
}

// ---- racks

bool read_description(Ptr<const RackDescription> desc, RackDescription* out, const Definition** def) {
    if (!desc || !memory().valid(desc.address, sizeof(RackDescription))) return false;
    *out = *desc;
    *def = definition_at(out->definition);
    return *def && out->voices >= 1 && out->channels >= 1 && out->channels <= 2 && out->max_patches_per_input >= 0 &&
           out->patches_per_output >= 0;
}

i32 sceNgsRackGetRequiredMemorySize(u32 system, Ptr<const RackDescription> desc, Ptr<u32> size) {
    Ngs& n = ngs_state();
    if (!n.exists || system != n.handle) return nerr::kInvalidHandle;
    RackDescription d;
    const Definition* def;
    if (!read_description(desc, &d, &def)) return nerr::kInvalidParam;
    if (!writable(size.address, 4)) return nerr::kInvalidParam;
    *size = rack_size(*def, d);
    return 0;
}

i32 sceNgsRackInit(u32 system, Ptr<const BufferInfo> buffer, Ptr<const RackDescription> desc, Ptr<u32> handle) {
    Ngs& n = ngs_state();
    nx::scoped_lock<nx::mutex> held(n.lock);
    if (!n.exists || system != n.handle) return nerr::kInvalidHandle;
    RackDescription d;
    const Definition* def;
    if (!read_description(desc, &d, &def) || !buffer || !memory().valid(buffer.address, sizeof(BufferInfo)) ||
        !writable(handle.address, 4))
        return nerr::kInvalidParam;
    const BufferInfo b = *buffer;
    if (b.data & 15) return nerr::kInvalidAlignment;
    const u32 need = rack_size(*def, d);
    if (b.size < need || !writable(b.data, need)) return nerr::kSizeMismatch;
    if (n.racks.size() >= static_cast<usize>(n.params.max_racks) ||
        n.voice_count + static_cast<u32>(d.voices) > static_cast<u32>(n.params.max_voices))
        return nerr::kOutOfAssets;

    auto rack = nx::make_unique<Rack>();
    Rack& r = *rack;
    r.handle = b.data;
    r.size = need;
    r.def = def;
    r.channels = d.channels;
    r.max_patches_per_input = d.max_patches_per_input;
    r.patches_per_output = nx::max(d.patches_per_output, 1);
    r.user_release_data = d.user_release_data;
    std::memset(memory().host(b.data), 0, need);
    const u32 g = static_cast<u32>(n.params.granularity);
    u32 at = b.data + kRackHeader;
    for (i32 i = 0; i < d.voices; ++i) {
        auto voice = nx::make_unique<Voice>();
        Voice& v = *voice;
        v.handle = at;
        v.rack = &r;
        v.index = static_cast<u32>(i);
        u32 p = at + kVoiceHeader;
        for (u32 m = 0; m < def->module_count; ++m) {
            v.modules.push_back(make_module(def->modules[m], static_cast<u32>(d.channels)));
            const TypeInfo* t = type_info(def->modules[m]);
            const u32 area = align16(max_params_size(def->modules[m]) * (t->per_channel ? static_cast<u32>(d.channels) : 1));
            v.areas.push_back(area ? p : 0);
            v.area_iface.push_back(0);
            v.locked.push_back(false);
            v.out_of_range.emplace_back();
            p += area;
        }
        v.inputs.resize(def->inputs);
        v.outputs.resize(def->outputs);
        for (Packet& pk : v.inputs) pk.clear(g);
        for (Packet& pk : v.outputs) pk.clear(g);
        if (def->kind == Kind::Master) v.pcm.assign(g * static_cast<u32>(d.channels), 0);
        v.out_patches.assign(def->outputs * static_cast<u32>(r.patches_per_output), nullptr);
        reset_modules(n, v);
        n.voices[v.handle] = &v;
        r.voices.push_back(std::move(voice));
        at += voice_size(*def, d);
    }
    n.voice_count += static_cast<u32>(d.voices);
    *handle = r.handle;
    n.racks.push_back(std::move(rack));
    return 0;
}

i32 sceNgsRackGetVoiceHandle(u32 rack, u32 index, Ptr<u32> voice) {
    Ngs& n = ngs_state();
    nx::scoped_lock<nx::mutex> held(n.lock);
    Rack* r = rack_of(n, rack);
    if (!r) return nerr::kInvalidHandle;
    if (index >= r->voices.size() || !writable(voice.address, 4)) return nerr::kInvalidParam;
    *voice = r->voices[index]->handle;
    return 0;
}

i32 sceNgsRackRelease(u32 rack, u32 callback) {
    Ngs& n = ngs_state();
    nx::scoped_lock<nx::mutex> held(n.lock);
    Rack* r = rack_of(n, rack);
    if (!r) return nerr::kInvalidHandle;
    for (auto& v : r->voices) {
        stop(*v);
        remove_patches_of(n, *v, true, true);
    }
    if (callback) {
        // (released at the next update, which calls the callback)
        r->releasing = true;
        r->release_callback = callback;
        return 0;
    }
    for (auto& v : r->voices) n.voices.erase(v->handle);
    n.voice_count -= static_cast<u32>(r->voices.size());
    n.racks.erase(std::find_if(n.racks.begin(), n.racks.end(), [&](auto& x) { return x.get() == r; }));
    return 0;
}

i32 sceNgsRackSetParamErrorCallback(u32 rack, u32 callback) {
    Ngs& n = ngs_state();
    nx::scoped_lock<nx::mutex> held(n.lock);
    Rack* r = rack_of(n, rack);
    if (!r) return nerr::kInvalidHandle;
    r->error_callback = callback;
    return 0;
}

// ---- voices

#define NGS_VOICE(name)                          \
    Ngs& n = ngs_state();                        \
    nx::scoped_lock<nx::mutex> held(n.lock);     \
    Voice* name = voice_of(n, voice);            \
    if (!name) return nerr::kInvalidHandle

i32 sceNgsVoiceInit(u32 voice, u32 preset, u32 flags) {
    NGS_VOICE(v);
    if (v->run != Voice::Run::Stopped) return nerr::kInvalidState;
    stop(*v);
    if (flags & 1) remove_patches_of(n, *v, true, false);  // SCE_NGS_VOICE_INIT_ROUTING
    if (flags & 4) {                                        // SCE_NGS_VOICE_INIT_CALLBACKS
        for (auto& m : v->modules) m->callback = m->callback_user = 0;
        v->finished_callback = v->finished_user = 0;
    }
    if (flags & 2) {  // SCE_NGS_VOICE_INIT_PRESET
        reset_modules(n, *v);
        if (preset) return apply_preset(n, *v, preset);
    }
    return 0;
}

i32 sceNgsVoicePlay(u32 voice) {
    NGS_VOICE(v);
    v->play_requested = true;
    v->key_off_requested = false;
    return 0;
}

i32 sceNgsVoiceKeyOff(u32 voice) {
    NGS_VOICE(v);
    if (v->run == Voice::Run::Stopped && !v->play_requested) return 0;
    v->key_off_requested = true;
    return 0;
}

i32 sceNgsVoiceKill(u32 voice) {
    NGS_VOICE(v);
    stop(*v);
    v->ended = v->finished = false;
    return 0;
}

i32 sceNgsVoicePause(u32 voice) {
    NGS_VOICE(v);
    if (v->run == Voice::Run::Stopped) return nerr::kInvalidState;
    v->pause_requested = true;
    v->resume_requested = false;
    return 0;
}

i32 sceNgsVoiceResume(u32 voice) {
    NGS_VOICE(v);
    if (!v->paused && !v->pause_requested) return nerr::kInvalidState;
    if (v->pause_requested) v->pause_requested = false;
    else v->resume_requested = true;
    return 0;
}

i32 sceNgsVoiceSetPreset(u32 voice, u32 preset) {
    NGS_VOICE(v);
    if (!preset) return nerr::kInvalidParam;
    return apply_preset(n, *v, preset);
}

i32 sceNgsVoiceLockParams(u32 voice, u32 module, u32 id, Ptr<BufferInfo> buffer) {
    NGS_VOICE(v);
    if (module >= v->modules.size() || !writable(buffer.address, sizeof(BufferInfo))) return nerr::kInvalidParam;
    if (!v->areas[module]) return nerr::kParamTypeMismatch;
    if (v->locked[module]) return nerr::kResourceLocked;
    if (!switch_area(*v, module, id)) return nerr::kParamTypeMismatch;
    Module& m = *v->modules[module];
    u32 size = 0;
    for (u32 i = 0; i < m.info().interface_count; ++i)
        if (m.info().interfaces[i].id == id) size = m.info().interfaces[i].size;
    v->locked[module] = true;
    buffer->data = v->areas[module];
    buffer->size = size * m.param_channels();
    return 0;
}

i32 sceNgsVoiceUnlockParams(u32 voice, u32 module) {
    NGS_VOICE(v);
    if (module >= v->modules.size()) return nerr::kInvalidParam;
    if (!v->locked[module]) return nerr::kInvalidState;
    v->locked[module] = false;
    return queue_change(n, *v, module);
}

i32 sceNgsVoiceSetParamsBlock(u32 voice, u32 data, u32 size, Ptr<i32> errors) {
    NGS_VOICE(v);
    if (!data || !memory().valid(data, size)) return nerr::kInvalidParam;
    i32 count = 0;
    const i32 r = apply_block(n, *v, data, size, &count);
    if (errors && writable(errors.address, 4)) *errors = count;
    return r;
}

i32 sceNgsVoiceBypassModule(u32 voice, u32 module, u32 flag) {
    NGS_VOICE(v);
    if (module >= v->modules.size() || (flag != 0 && flag != 2)) return nerr::kInvalidParam;
    v->modules[module]->bypassed = flag == 2;
    return 0;
}

i32 sceNgsVoiceSetModuleCallback(u32 voice, u32 module, u32 callback, u32 user) {
    NGS_VOICE(v);
    if (module >= v->modules.size()) return nerr::kInvalidParam;
    v->modules[module]->callback = callback;
    v->modules[module]->callback_user = user;
    return 0;
}

i32 sceNgsVoiceSetFinishedCallback(u32 voice, u32 callback, u32 user) {
    NGS_VOICE(v);
    v->finished_callback = callback;
    v->finished_user = user;
    return 0;
}

i32 sceNgsVoiceGetStateData(u32 voice, u32 module, Ptr<u8> out, u32 size) {
    NGS_VOICE(v);
    if (module >= v->modules.size() || !out || !writable(out.address, size)) return nerr::kInvalidParam;
    Module& m = *v->modules[module];
    if (m.type() == kOutput) {
        const u32 bytes = static_cast<u32>(v->pcm.size() * sizeof(i16));
        if (size < bytes) return nerr::kSizeMismatch;
        std::memcpy(out.get(), v->pcm.data(), bytes);
        return 0;
    }
    if (!m.info().state_size) return nerr::kInvalidParam;
    if (size < m.info().state_size) return nerr::kSizeMismatch;
    return m.state(out.get(), size) ? 0 : nerr::kInvalidParam;
}

i32 sceNgsVoiceGetInfo(u32 voice, Ptr<VoiceInfo> info) {
    NGS_VOICE(v);
    if (!writable(info.address, sizeof(VoiceInfo))) return nerr::kInvalidParam;
    u32 state = v->run == Voice::Run::Active ? kStateActive : v->run == Voice::Run::Finalizing ? kStateFinalizing : kStateAvailable;
    // (a voice cued to play is active at once, and pending until the next update starts it: CRI's sound engine
    // takes a voice that is neither active nor finalizing for stopped)
    if (v->play_requested) state |= kStateActive | kStatePending;
    if (v->paused || v->pause_requested) state |= kStatePaused;
    if (v->key_off_requested) state |= kStateKeyOff;
    const Definition& d = *v->rack->def;
    *info = VoiceInfo{state, d.module_count, d.inputs, d.outputs, static_cast<u32>(v->rack->patches_per_output), v->update_calls};
    return 0;
}

i32 sceNgsVoiceGetModuleType(u32 voice, u32 module, Ptr<u32> type) {
    NGS_VOICE(v);
    if (module >= v->modules.size() || !writable(type.address, 4)) return nerr::kInvalidParam;
    *type = v->modules[module]->type();
    return 0;
}

i32 sceNgsVoiceGetModuleBypass(u32 voice, u32 module, Ptr<u32> flag) {
    NGS_VOICE(v);
    if (module >= v->modules.size() || !writable(flag.address, 4)) return nerr::kInvalidParam;
    *flag = v->modules[module]->bypassed ? 2 : 0;
    return 0;
}

i32 sceNgsVoiceGetParamsOutOfRange(u32 voice, u32 module, Ptr<char> message) {
    NGS_VOICE(v);
    if (module >= v->modules.size() || !writable(message.address, 128)) return nerr::kInvalidParam;
    const nx::string& m = v->out_of_range[module];
    const usize len = nx::min<usize>(m.size(), 127);
    std::memcpy(message.get(), m.data(), len);
    message.get()[len] = 0;
    return 0;
}

// ---- patches

i32 sceNgsPatchCreateRouting(Ptr<const PatchSetup> setup, Ptr<u32> handle) {
    Ngs& n = ngs_state();
    nx::scoped_lock<nx::mutex> held(n.lock);
    if (!setup || !memory().valid(setup.address, sizeof(PatchSetup)) || !writable(handle.address, 4))
        return nerr::kInvalidParam;
    const PatchSetup s = *setup;
    Voice* src = voice_of(n, s.source);
    Voice* dst = voice_of(n, s.destination);
    if (!src || !dst) return nerr::kInvalidHandle;
    const Definition& sd = *src->rack->def;
    const Definition& dd = *dst->rack->def;
    if (s.output < 0 || static_cast<u32>(s.output) >= sd.outputs || s.input < 0 || static_cast<u32>(s.input) >= dd.inputs)
        return nerr::kPatchNotAvail;
    if (src->rack == dst->rack || depends_on(dst->rack, src->rack)) return nerr::kDependency;
    const i32 per = src->rack->patches_per_output;
    i32 sub = s.sub_index;
    if (sub == -1) {
        for (i32 k = 0; k < per && sub == -1; ++k)
            if (!src->out_patches[static_cast<u32>(s.output * per + k)]) sub = k;
        if (sub == -1) return nerr::kPatchNotAvail;
    }
    if (sub < 0 || sub >= per || src->out_patches[static_cast<u32>(s.output * per + sub)]) return nerr::kPatchNotAvail;
    u32 into = 0;
    for (Patch* p : dst->in_patches) into += p->input == s.input ? 1 : 0;
    if (dst->rack->max_patches_per_input > 0 && into >= static_cast<u32>(dst->rack->max_patches_per_input))
        return nerr::kPatchNotAvail;

    auto patch = nx::make_unique<Patch>();
    Patch& p = *patch;
    // (the patch's slot: after the voice's parameter areas)
    u32 slots = src->handle + kVoiceHeader;
    for (u32 m = 0; m < sd.module_count; ++m) {
        const TypeInfo* t = type_info(sd.modules[m]);
        slots += align16(max_params_size(sd.modules[m]) * (t->per_channel ? static_cast<u32>(src->rack->channels) : 1));
    }
    p.handle = slots + static_cast<u32>(s.output * per + sub) * kPatchSlot;
    p.source = src;
    p.output = s.output;
    p.sub = sub;
    p.destination = dst;
    p.input = s.input;
    // (by default each source channel goes to the destination's channel of the same number; mono feeds both)
    const i32 sc = src->rack->channels, dc = dst->rack->channels;
    for (i32 a = 0; a < sc; ++a)
        for (i32 b = 0; b < dc; ++b) p.vols.m[a][b] = (a == b || sc == 1 || dc == 1) ? 1.0f : 0.0f;
    src->out_patches[static_cast<u32>(s.output * per + sub)] = &p;
    dst->in_patches.push_back(&p);
    *handle = p.handle;
    n.patches[p.handle] = std::move(patch);
    return 0;
}

i32 sceNgsPatchGetInfo(u32 handle, Ptr<RouteInfo> route, Ptr<PatchSetup> setup) {
    Ngs& n = ngs_state();
    nx::scoped_lock<nx::mutex> held(n.lock);
    Patch* p = patch_of(n, handle);
    if (!p) return nerr::kInvalidHandle;
    if (route) {
        if (!writable(route.address, sizeof(RouteInfo))) return nerr::kInvalidParam;
        *route = RouteInfo{p->source->rack->channels, p->destination->rack->channels, p->vols};
    }
    if (setup) {
        if (!writable(setup.address, sizeof(PatchSetup))) return nerr::kInvalidParam;
        *setup = PatchSetup{p->source->handle, p->output, p->sub, p->destination->handle, p->input};
    }
    return 0;
}

i32 sceNgsVoiceGetOutputPatch(u32 voice, i32 output, i32 sub, Ptr<u32> handle) {
    NGS_VOICE(v);
    const i32 per = v->rack->patches_per_output;
    if (output < 0 || static_cast<u32>(output) >= v->rack->def->outputs || sub < 0 || sub >= per || !writable(handle.address, 4))
        return nerr::kInvalidParam;
    Patch* p = v->out_patches[static_cast<u32>(output * per + sub)];
    if (!p) return nerr::kPatchNotAvail;
    *handle = p->handle;
    return 0;
}

i32 sceNgsPatchRemoveRouting(u32 handle) {
    Ngs& n = ngs_state();
    nx::scoped_lock<nx::mutex> held(n.lock);
    Patch* p = patch_of(n, handle);
    if (!p) return nerr::kInvalidHandle;
    remove_patch(n, p);
    return 0;
}

i32 sceNgsVoicePatchSetVolume(u32 handle, i32 out_channel, i32 in_channel, f32 volume) {
    Ngs& n = ngs_state();
    nx::scoped_lock<nx::mutex> held(n.lock);
    Patch* p = patch_of(n, handle);
    if (!p) return nerr::kInvalidHandle;
    if (out_channel < 0 || out_channel >= p->source->rack->channels || in_channel < 0 ||
        in_channel >= p->destination->rack->channels)
        return nerr::kInvalidParam;
    p->vols.m[out_channel][in_channel] = volume;
    return 0;
}

i32 sceNgsVoicePatchSetVolumes(u32 handle, i32 out_channel, Ptr<const f32> volumes, i32 count) {
    Ngs& n = ngs_state();
    nx::scoped_lock<nx::mutex> held(n.lock);
    Patch* p = patch_of(n, handle);
    if (!p) return nerr::kInvalidHandle;
    if (out_channel < 0 || out_channel >= p->source->rack->channels || count < 0 ||
        count > p->destination->rack->channels || (count && !memory().valid(volumes.address, 4 * static_cast<u32>(count))))
        return nerr::kInvalidParam;
    for (i32 i = 0; i < count; ++i) p->vols.m[out_channel][i] = volumes.get()[i];
    return 0;
}

i32 sceNgsVoicePatchSetVolumesMatrix(u32 handle, Ptr<const VolumeMatrix> matrix) {
    Ngs& n = ngs_state();
    nx::scoped_lock<nx::mutex> held(n.lock);
    Patch* p = patch_of(n, handle);
    if (!p) return nerr::kInvalidHandle;
    if (!matrix || !memory().valid(matrix.address, sizeof(VolumeMatrix))) return nerr::kInvalidParam;
    p->vols = *matrix;
    return 0;
}

// ---- module presets

i32 sceNgsModuleGetNumPresets(u32 system, u32 module, Ptr<u32> count) {
    Ngs& n = ngs_state();
    if (!n.exists || system != n.handle) return nerr::kInvalidHandle;
    if (!type_info(module) || !writable(count.address, 4)) return nerr::kInvalidParam;
    *count = preset_count(module);
    return 0;
}

i32 sceNgsModuleGetPreset(u32 system, u32 module, u32 index, Ptr<const BufferInfo> buffer) {
    Ngs& n = ngs_state();
    if (!n.exists || system != n.handle) return nerr::kInvalidHandle;
    if (!type_info(module) || !buffer || !memory().valid(buffer.address, sizeof(BufferInfo))) return nerr::kInvalidParam;
    const BufferInfo b = *buffer;
    if (index >= preset_count(module) || !writable(b.data, b.size)) return nerr::kInvalidParam;
    return preset(module, index, memory().host(b.data), b.size) ? 0 : nerr::kSizeMismatch;
}

// ---- voice definitions

u32 definition(u32 index) {
    Ngs& n = ngs_state();
    nx::scoped_lock<nx::mutex> held(n.lock);
    if (!n.definitions[index]) {
        n.definitions[index] = runtime().allocate_system(16, "NGS voice definition");
        std::memcpy(memory().host(n.definitions[index]), "NGSVDEF", 8);
        std::memcpy(memory().host(n.definitions[index] + 8), &index, 4);
    }
    return n.definitions[index];
}

u32 sceNgsVoiceDefGetMasterBuss() { return definition(kMasterBuss); }
u32 sceNgsVoiceDefGetMixerBuss() { return definition(kMixerBuss); }
u32 sceNgsVoiceDefGetCompressorBuss() { return definition(kCompressorBuss); }
u32 sceNgsVoiceDefGetCompressorSideChainBuss() { return definition(kSideChainBuss); }
u32 sceNgsVoiceDefGetDelayBuss() { return definition(kDelayBuss); }
u32 sceNgsVoiceDefGetDistortionBuss() { return definition(kDistortionBuss); }
u32 sceNgsVoiceDefGetEnvelopeBuss() { return definition(kEnvelopeBuss); }
u32 sceNgsVoiceDefGetEqBuss() { return definition(kEqBuss); }
u32 sceNgsVoiceDefGetPauserBuss() { return definition(kPauserBuss); }
u32 sceNgsVoiceDefGetPitchShiftBuss() { return definition(kPitchShiftBuss); }
u32 sceNgsVoiceDefGetReverbBuss() { return definition(kReverbBuss); }
u32 sceNgsVoiceDefGetSasEmuVoice() { return definition(kSasEmu); }
u32 sceNgsVoiceDefGetSimpleVoice() { return definition(kSimple); }
u32 sceNgsVoiceDefGetSimpleAtrac9Voice() { return definition(kSimpleAt9); }
u32 sceNgsVoiceDefGetTemplate1() { return definition(kTemplate1); }
u32 sceNgsVoiceDefGetAtrac9Voice() { return definition(kAt9Voice); }
// (Scream's voices: their layout is not documented; the template 1 voices stand in)
u32 sceNgsVoiceDefGetScreamVoice() {
    nx::logw(logs::audio, "ngs: the Scream voice definition is played as a template 1 voice");
    return definition(kTemplate1);
}
u32 sceNgsVoiceDefGetScreamAtrac9Voice() {
    nx::logw(logs::audio, "ngs: the Scream ATRAC9 voice definition is played as an ATRAC9 voice");
    return definition(kAt9Voice);
}

// ---- ATRAC9 sections

struct SkipBufferInfo {
    i32 start_byte_offset, bytes;
    i16 start_skip, end_skip;
    i32 is_super_packet;
};

// Where a section of an ATRAC9 stream (in samples of the original, before encoding) lies in the data: whole
// superframes from the one before the section's first (whose overlap the first needs), and the samples to drop at
// both ends. Decoded data runs one frame behind the original (the overlap delay, as the file's fact chunk says),
// which positions taken from the file's header (its loop points) already include.
i32 sceNgsAT9GetSectionDetails(i32 start, i32 count, i32 config, Ptr<SkipBufferInfo> out) {
    if (count < 0 || !writable(out.address, sizeof(SkipBufferInfo))) return nerr::kInvalidParam;
    void* handle = Atrac9GetHandle();
    u8 bytes[4];
    std::memcpy(bytes, &config, 4);
    Atrac9CodecInfo info{};
    const bool ok = handle && Atrac9InitDecoder(handle, bytes) == 0 && Atrac9GetCodecInfo(handle, &info) == 0;
    if (handle) Atrac9ReleaseHandle(handle);
    if (!ok) return nerr::kInvalidParam;
    const bool from_header = (static_cast<u32>(start) & 0x80000000u) != 0;
    const i64 offset = static_cast<i64>(static_cast<u32>(start) & 0x7FFFFFFFu);
    const i64 unit = static_cast<i64>(info.frameSamples) * info.framesInSuperframe;
    const i64 first_sample = offset + (from_header ? 0 : info.frameSamples);
    const i64 end_sample = first_sample + count;
    i64 first = first_sample / unit;
    if (first > 0) --first;
    const i64 last = (end_sample + unit - 1) / unit;
    out->start_byte_offset = static_cast<i32>(first * info.superframeSize);
    out->bytes = static_cast<i32>((last - first) * info.superframeSize);
    out->start_skip = static_cast<i16>(first_sample - first * unit);
    out->end_skip = static_cast<i16>(last * unit - end_sample);
    out->is_super_packet = info.framesInSuperframe > 1 ? 1 : 0;
    return 0;
}

// ---- Sulpha (the debugger's agent): nothing to connect to

i32 sceSulphaNgsGetDefaultConfig(Ptr<u32> config) {
    if (writable(config.address, 8)) config.get()[0] = config.get()[1] = 0;
    return 0;
}
i32 sceSulphaNgsGetNeededMemory(Ptr<const u32>, Ptr<u32> size) {
    if (writable(size.address, 4)) *size = 0;
    return 0;
}
i32 sceSulphaNgsInit(Ptr<const u32>, Ptr<void>, u32) { return 0; }
i32 sceSulphaNgsShutdown() { return 0; }
i32 sceSulphaNgsSetSynthName(u32, Ptr<const char>) { return 0; }
i32 sceSulphaNgsSetRackName(u32, Ptr<const char>) { return 0; }
i32 sceSulphaNgsSetVoiceName(u32, Ptr<const char>) { return 0; }
i32 sceSulphaNgsSetSampleName(Ptr<const void>, u32, Ptr<const char>) { return 0; }
i32 sceSulphaNgsTrace(Ptr<const char>) { return 0; }

const Function kFunctions[] = {
    HLE_FUNCTION(0x6CE8B36F, sceNgsSystemGetRequiredMemorySize),
    HLE_FUNCTION(0xED14CF4A, sceNgsSystemInit),
    HLE_FUNCTION(0x684F080C, sceNgsSystemUpdate),
    HLE_FUNCTION(0x4A25BEBC, sceNgsSystemRelease),
    HLE_FUNCTION(0xB9D971F2, sceNgsSystemLock),
    HLE_FUNCTION(0x0A93EA96, sceNgsSystemUnlock),
    HLE_FUNCTION(0x5ADD22DC, sceNgsSystemSetParamErrorCallback),
    HLE_FUNCTION(0x64D80013, sceNgsSystemSetFlags),
    HLE_FUNCTION(0x477318C0, sceNgsRackGetRequiredMemorySize),
    HLE_FUNCTION(0x0A92E4EC, sceNgsRackInit),
    HLE_FUNCTION(0xFE1A98E9, sceNgsRackGetVoiceHandle),
    HLE_FUNCTION(0xDD5CA10B, sceNgsRackRelease),
    HLE_FUNCTION(0x534B6E3F, sceNgsRackSetParamErrorCallback),
    HLE_FUNCTION(0x1DDBEBEB, sceNgsVoiceInit),
    HLE_FUNCTION(0xFA0A0F34, sceNgsVoicePlay),
    HLE_FUNCTION(0xBB13373D, sceNgsVoiceKeyOff),
    HLE_FUNCTION(0x0E291AAD, sceNgsVoiceKill),
    HLE_FUNCTION(0xD7786E99, sceNgsVoicePause),
    HLE_FUNCTION(0x54CFB981, sceNgsVoiceResume),
    HLE_FUNCTION(0x8A88E665, sceNgsVoiceSetPreset),
    HLE_FUNCTION(0xAB6BEF8F, sceNgsVoiceLockParams),
    HLE_FUNCTION(0x3D46D8A7, sceNgsVoiceUnlockParams),
    HLE_FUNCTION(0xFB8174B1, sceNgsVoiceSetParamsBlock),
    HLE_FUNCTION(0x9AB87E71, sceNgsVoiceBypassModule),
    HLE_FUNCTION(0x24E909A8, sceNgsVoiceSetModuleCallback),
    HLE_FUNCTION(0x17A6F564, sceNgsVoiceSetFinishedCallback),
    HLE_FUNCTION(0xC9B8C0B4, sceNgsVoiceGetStateData),
    HLE_FUNCTION(0x5551410D, sceNgsVoiceGetInfo),
    HLE_FUNCTION(0xB307185E, sceNgsVoiceGetModuleType),
    HLE_FUNCTION(0x431BF3AB, sceNgsVoiceGetModuleBypass),
    HLE_FUNCTION(0x4CBE08F3, sceNgsVoiceGetParamsOutOfRange),
    HLE_FUNCTION(0xD668B49C, sceNgsPatchCreateRouting),
    HLE_FUNCTION(0x98703DBC, sceNgsPatchGetInfo),
    HLE_FUNCTION(0x01A52E3A, sceNgsVoiceGetOutputPatch),
    HLE_FUNCTION(0xD0C9AE5A, sceNgsPatchRemoveRouting),
    HLE_FUNCTION(0xA3C807BC, sceNgsVoicePatchSetVolume),
    HLE_FUNCTION(0xBD6F57F0, sceNgsVoicePatchSetVolumes),
    HLE_FUNCTION(0xA0F5402D, sceNgsVoicePatchSetVolumesMatrix),
    HLE_FUNCTION(0x5C71FE09, sceNgsModuleGetNumPresets),
    HLE_FUNCTION(0xC58298A7, sceNgsModuleGetPreset),
    HLE_FUNCTION(0x79A121D1, sceNgsVoiceDefGetMasterBuss),
    HLE_FUNCTION(0xE0AC8776, sceNgsVoiceDefGetMixerBuss),
    HLE_FUNCTION(0x0E0ACB68, sceNgsVoiceDefGetCompressorBuss),
    HLE_FUNCTION(0x1AF83512, sceNgsVoiceDefGetCompressorSideChainBuss),
    HLE_FUNCTION(0x4D705E3E, sceNgsVoiceDefGetDelayBuss),
    HLE_FUNCTION(0xAAD90DEB, sceNgsVoiceDefGetDistortionBuss),
    HLE_FUNCTION(0xF6B68C31, sceNgsVoiceDefGetEnvelopeBuss),
    HLE_FUNCTION(0xF964120E, sceNgsVoiceDefGetEqBuss),
    HLE_FUNCTION(0x214485D6, sceNgsVoiceDefGetPauserBuss),
    HLE_FUNCTION(0xFBE515D4, sceNgsVoiceDefGetPitchShiftBuss),
    HLE_FUNCTION(0x9DCF50F5, sceNgsVoiceDefGetReverbBuss),
    HLE_FUNCTION(0x1F51C2BA, sceNgsVoiceDefGetSasEmuVoice),
    HLE_FUNCTION(0x0D5399CF, sceNgsVoiceDefGetSimpleVoice),
    HLE_FUNCTION(0x45CF2A73, sceNgsVoiceDefGetSimpleAtrac9Voice),
    HLE_FUNCTION(0xE9B572B7, sceNgsVoiceDefGetTemplate1),
    HLE_FUNCTION(0x14EF65A0, sceNgsVoiceDefGetAtrac9Voice),
    HLE_FUNCTION(0xCE53BC33, sceNgsVoiceDefGetScreamVoice),
    HLE_FUNCTION(0xCD63A2BF, sceNgsVoiceDefGetScreamAtrac9Voice),
    HLE_FUNCTION(0x2A9FA501, sceNgsAT9GetSectionDetails),
    HLE_FUNCTION(0x5FD8AEDB, sceSulphaNgsGetDefaultConfig),
    HLE_FUNCTION(0x793E3E8C, sceSulphaNgsGetNeededMemory),
    HLE_FUNCTION(0xAFCD824F, sceSulphaNgsInit),
    HLE_FUNCTION(0xD124BFB1, sceSulphaNgsShutdown),
    HLE_FUNCTION(0x2F3F7515, sceSulphaNgsSetSynthName),
    HLE_FUNCTION(0x251AF6A9, sceSulphaNgsSetRackName),
    HLE_FUNCTION(0x508975BD, sceSulphaNgsSetVoiceName),
    HLE_FUNCTION(0x54EC5B8D, sceSulphaNgsSetSampleName),
    HLE_FUNCTION(0xDC7C0F05, sceSulphaNgsTrace),
};

}  // namespace

void register_ngs() {
    // (a new runtime starts without a system, and hands out its own voice definitions)
    Ngs& n = ngs_state();
    n.patches.clear();
    n.voices.clear();
    n.racks.clear();
    n.exists = false;
    for (u32& d : n.definitions) d = 0;
    register_library({"SceNgs", kSceNgs, kFunctions});
}

}  // namespace psvrecore::runtime::hle
