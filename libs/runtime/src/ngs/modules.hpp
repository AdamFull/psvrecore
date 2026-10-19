#pragma once

// NGS modules: what each module type's parameters are (their interfaces, sizes, defaults and ranges, laid out as
// the guest's structures) and what the module does to a voice's audio, one packet (the system's granularity) at a
// time. Audio inside NGS is planar float (1.0 = full scale of a 16-bit sample); the output module turns it into
// interleaved 16-bit samples.
//
// The player decodes and resamples exactly as far as that is observable: at a playback rate equal to the system's
// rate its output is its input, sample for sample, and its counters (samples generated, bytes consumed) are exact.
// Between rates it interpolates linearly. The DSP effects follow their documented behaviour (filters as the usual
// biquad designs, envelope points and loops, polynomial distortion, the compressor's ratio, threshold, knee and
// time constants); their exact arithmetic on the console is unknown, so their output is close but not bit-exact.

#include <memory>

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore::runtime::ngs {

inline constexpr u32 kMaxChannels = 2;
inline constexpr u32 kMaxGranularity = 512;

enum ModuleType : u32 {
    kInputMixer = 0x5CE0,
    kCompressor = 0x5CE1,
    kDistortion = 0x5CE2,
    kEnvelope = 0x5CE3,
    kFilter = 0x5CE4,
    kPauser = 0x5CE5,
    kPlayer = 0x5CE6,
    kReverb = 0x5CE7,
    kGenerator = 0x5CE8,
    kMixer = 0x5CE9,
    kPitchShift = 0x5CEA,
    kDelay = 0x5CEB,
    kParamEq = 0x5CEC,
    kOutput = 0x5CED,
    kAt9Player = 0x5CAA,
};

constexpr u32 params_id(u32 module, u32 index, u32 version) {
    return (module & 0xFFFF) | ((version & 0xFF) << 16) | ((index & 0xFF) << 24);
}

// Errors (SCE_NGS_ERROR_*).
namespace err {
inline constexpr i32 kError = static_cast<i32>(0x804A0001u), kInvalidParam = static_cast<i32>(0x804A0002u),
                     kInvalidAlignment = static_cast<i32>(0x804A0003u), kDependency = static_cast<i32>(0x804A0005u),
                     kOutOfAssets = static_cast<i32>(0x804A0006u), kResourceLocked = static_cast<i32>(0x804A0008u),
                     kParamOutOfRange = static_cast<i32>(0x804A0009u), kInvalidVoiceType = static_cast<i32>(0x804A000Au),
                     kInvalidHandle = static_cast<i32>(0x804A000Cu), kSizeMismatch = static_cast<i32>(0x804A000Du),
                     kPatchNotAvail = static_cast<i32>(0x804A000Eu), kParamTypeMismatch = static_cast<i32>(0x804A000Fu),
                     kInvalidState = static_cast<i32>(0x804A0010u), kInvalidBuffer = static_cast<i32>(0x804A0013u);
}

// One packet of audio: [channel][sample].
struct Packet {
    f32 s[kMaxChannels][kMaxGranularity];
    void clear(u32 n) {
        for (auto& c : s)
            for (u32 i = 0; i < n; ++i) c[i] = 0.0f;
    }
};

// What a module sees of the system and its voice.
struct Context {
    u32 granularity = 512;
    u32 sample_rate = 48000;
    u32 channels = 2;   // the voice's
    u64 active_samples = 0;  // samples the voice has been active for, before this packet
};

// A callback a module raised in an update (only the last of an update is reported).
struct Event {
    bool raised = false;
    i32 data = 0, data2 = 0;
    u32 ptr = 0;
};

// The parameter interfaces of a module type: their struct ids and sizes, whether there is one per channel.
struct Interface {
    u32 id;
    u32 size;
};
struct TypeInfo {
    u32 type;
    const Interface* interfaces;
    u32 interface_count;
    bool per_channel;
    u32 state_size;  // SceNgs*States (0: none; the output module's is the packet)
};
const TypeInfo* type_info(u32 type);
// The largest parameter structure of a type (0: it has none).
u32 max_params_size(u32 type);

// A module of a voice. Its parameters are kept as the guest lays them out (`params`, one structure per channel
// for per-channel types), applied by apply().
class Module {
public:
    explicit Module(u32 type, u32 channels);
    virtual ~Module() = default;

    u32 type() const { return type_; }
    const TypeInfo& info() const { return *info_; }
    u32 param_channels() const { return info_->per_channel ? channels_ : 1; }
    u32 interface_id() const { return iface_; }
    u32 interface_size() const;
    // The parameters (param_channels() structures of interface_size() bytes, one after another).
    u8* params() { return params_.data(); }
    const u8* params() const { return params_.data(); }
    usize params_bytes() const { return interface_size() * param_channels(); }

    // Takes parameters as the guest area holds them (an interface of the type, param_channels() structures).
    void load(const u8* bytes, u32 interface_id);
    // Switches to an interface (its defaults where the module had another); false if the type has no such one.
    bool select_interface(u32 id);
    // Writes the module's default parameters (preset 0).
    virtual void defaults();
    // Checks the parameters of a channel; on failure says which (for sceNgsVoiceGetParamsOutOfRange).
    virtual bool check(const u8* params, nx::string* message) const;
    // The parameters changed.
    virtual void apply(const Context&) {}
    // The voice starts playing (a key on): running state from the beginning.
    virtual void reset(const Context&) {}
    // The user state (sceNgsVoiceGetStateData), or false if there is none.
    virtual bool state(void* out, u32 size) const;
    // In-place processing (effects).
    virtual void process(const Context&, Packet& io) { (void)io; }

    bool bypassed = false;
    u32 callback = 0, callback_user = 0;  // guest function and data
    Event event;

protected:
    template <typename T>
    T* param(u32 channel = 0) {
        return reinterpret_cast<T*>(params_.data() + channel * interface_size());
    }
    template <typename T>
    const T* param(u32 channel = 0) const {
        return reinterpret_cast<const T*>(params_.data() + channel * interface_size());
    }
    void write_header(u32 id, u32 size);

    u32 type_;
    const TypeInfo* info_;
    u32 channels_;
    u32 iface_ = 0;
    nx::vector<u8> params_;
};

// The number of a module type's presets, and one of them written over a parameter buffer of `size` bytes.
u32 preset_count(u32 type);
bool preset(u32 type, u32 index, u8* out, u32 size);

std::unique_ptr<Module> make_module(u32 type, u32 channels);

// ---- what the voice graph calls on particular modules

// Sources: fill `out` with a packet (silence when there is nothing to play). The player says when its data ended.
void render_player(Module& m, const Context& c, Packet& out, bool* ended);
void render_generator(Module& m, const Context& c, Packet& out);
// The mixer: out = a * gain[0] + b * gain[1].
void mix(Module& m, const Context& c, const Packet& a, const Packet& b, Packet& out);
// The compressor, with a side chain (null: none).
void compress(Module& m, const Context& c, Packet& io, const Packet* side_chain);
// Envelope: whether the voice is to be released (key off), and where it stands.
enum class EnvelopeEnd { None, Kill, Release, Finished };
EnvelopeEnd envelope(Module& m, const Context& c, Packet& io);
void envelope_key_off(Module& m);
// Fade lengths for pausing and resuming (the pauser's and the mixer's).
u32 pause_lead_in(const Module& m);
u32 pause_lead_out(const Module& m);

}  // namespace psvrecore::runtime::ngs
