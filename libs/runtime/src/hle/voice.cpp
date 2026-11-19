// Voice chat (SceVoice): ports joined into a topology, input ports (the microphone, PCM the game writes, voice data
// from the network) connected to output ports (PCM, voice data for the network, the speaker). Offline, as here:
// - the microphone gives nothing (no audio input: its port reports the input inactive), the speaker plays nothing;
// - data written to an input port reaches the output ports connected to it of the same kind (PCM to PCM, voice data
//   to voice data), each a queue the game reads from; between kinds nothing arrives (there is no voice codec);
// - the event callback is never called (no data is ever produced by the library itself).
// A PCM port's frame is 512 bytes (16 ms of 16-bit 16 kHz mono); a voice port's is 16 ms at its bit rate.

#include "hle_common.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceVoice = 0xCE56E214;
constexpr i32 kNotInit = err::code(0x804E0801), kInitialized = err::code(0x804E0802), kPortInvalid = err::code(0x804E0804),
              kArgumentInvalid = err::code(0x804E0805), kTopology = err::code(0x804E0807),
              kResourceInsufficient = err::code(0x804E0808);
constexpr u32 kVersion100 = 100;
constexpr u32 kAttrAudioInputActive = 1000;

enum PortType : i32 { kInDevice = 0, kInPcm = 1, kInVoice = 2, kOutPcm = 3, kOutVoice = 4, kOutDevice = 5 };
enum PortState : i32 { kIdle = 0, kReady = 1, kRunning = 3 };
constexpr u32 kMaxPorts = 32, kPcmFrame = 512, kQueueLimit = 64 * 1024;

bool is_input(i32 type) { return type >= kInDevice && type <= kInVoice; }
// (the kind of data a port carries: 0 none, 1 PCM, 2 voice data)
u32 kind(i32 type) { return type == kInPcm || type == kOutPcm ? 1 : type == kInVoice || type == kOutVoice ? 2 : 0; }
bool valid_bitrate(i32 b) { return b == 3850 || b == 4650 || b == 5700 || b == 7300; }

struct Port {
    i32 type = 0;
    u32 threshold = 0;
    bool muted = false, paused = false;
    f32 volume = 1.0f;
    u32 bitrate = 3850, buffer_size = 0;
    std::deque<u8> queue;  // (an output port's data waiting to be read)
    u32 frame() const { return kind(type) == 2 ? (bitrate * 16 / 1000 + 7) / 8 : kPcmFrame; }
};
struct Voice {
    nx::mutex lock;
    bool initialized = false, started = false, all_muted = false;
    std::map<u32, Port> ports;
    std::map<u32, nx::vector<u32>> edges;  // input port: its output ports
};
Voice& voice() {
    static Voice v;
    return v;
}

using Lock = nx::scoped_lock<nx::mutex>;

Port* port(Voice& v, u32 id) {
    auto it = v.ports.find(id);
    return it == v.ports.end() ? nullptr : &it->second;
}

// SceVoiceInitParam: application type, event callback, its user data.
i32 sceVoiceInit(Ptr<const u32> param, u32 version) {
    if (!param || !memory().valid(param.address, 12) || version != kVersion100) return kArgumentInvalid;
    Voice& v = voice();
    Lock held(v.lock);
    if (v.initialized) return kInitialized;
    v.initialized = true, v.started = false, v.all_muted = false;
    v.ports.clear(), v.edges.clear();
    nx::logi(logs::hle, "SceVoice: started (offline: no microphone, nothing to talk to)");
    return 0;
}
i32 sceVoiceEnd() {
    Voice& v = voice();
    Lock held(v.lock);
    if (!v.initialized) return kNotInit;
    v.initialized = v.started = false;
    v.ports.clear(), v.edges.clear();
    return 0;
}
// SceVoiceStartParam: a memory container.
i32 sceVoiceStart(Ptr<const u32> param) {
    if (!param || !memory().valid(param.address, 4)) return kArgumentInvalid;
    Voice& v = voice();
    Lock held(v.lock);
    if (!v.initialized) return kNotInit;
    v.started = true;
    return 0;
}
i32 sceVoiceStop() {
    Voice& v = voice();
    Lock held(v.lock);
    if (!v.initialized) return kNotInit;
    v.started = false;
    for (auto& [id, p] : v.ports) p.queue.clear();
    return 0;
}

// SceVoicePortParam: type, threshold (u16), muted (u16), volume (f32), then the bit rate (voice), or the buffer size,
// data type and sampling rate (PCM), or the player (devices).
i32 port_param(u32 address, Port* p) {
    if (!address || !memory().valid(address, 24)) return kArgumentInvalid;
    p->type = static_cast<i32>(psv_ld32(address));
    if (p->type < kInDevice || p->type > kOutDevice) return kArgumentInvalid;
    p->threshold = psv_ld16(address + 4);
    p->muted = psv_ld16(address + 6) != 0;
    const u32 volume = psv_ld32(address + 8);
    std::memcpy(&p->volume, &volume, 4);
    if (kind(p->type) == 2) {
        const i32 bitrate = static_cast<i32>(psv_ld32(address + 12));
        if (!valid_bitrate(bitrate)) return kArgumentInvalid;
        p->bitrate = static_cast<u32>(bitrate);
    } else if (kind(p->type) == 1) {
        p->buffer_size = psv_ld32(address + 12);
        const i32 data_type = static_cast<i32>(psv_ld32(address + 16)), rate = static_cast<i32>(psv_ld32(address + 20));
        if (!p->buffer_size || data_type != 0 || rate != 16000) return kArgumentInvalid;
    }
    return 0;
}

i32 sceVoiceCreatePort(Ptr<u32> id, u32 param) {
    if (!id || !writable(id.address, 4)) return kArgumentInvalid;
    Port p;
    if (const i32 r = port_param(param, &p); r < 0) return r;
    Voice& v = voice();
    Lock held(v.lock);
    if (!v.initialized) return kNotInit;
    u32 n = 0;
    while (n < kMaxPorts && v.ports.count(n)) ++n;
    if (n == kMaxPorts) return kResourceInsufficient;
    v.ports[n] = p;
    *id = n;
    return 0;
}
i32 sceVoiceUpdatePort(u32 id, u32 param) {
    Port p;
    if (const i32 r = port_param(param, &p); r < 0) return r;
    Voice& v = voice();
    Lock held(v.lock);
    if (!v.initialized) return kNotInit;
    Port* old = port(v, id);
    if (!old) return kPortInvalid;
    if (p.type != old->type) return kArgumentInvalid;
    p.queue = std::move(old->queue);
    *old = std::move(p);
    return 0;
}
i32 sceVoiceDeletePort(u32 id) {
    Voice& v = voice();
    Lock held(v.lock);
    if (!v.initialized) return kNotInit;
    if (!v.ports.erase(id)) return kPortInvalid;
    v.edges.erase(id);
    for (auto& [in, outs] : v.edges) std::erase(outs, id);
    return 0;
}
i32 sceVoiceResetPort(u32 id) {
    Voice& v = voice();
    Lock held(v.lock);
    if (!v.initialized) return kNotInit;
    Port* p = port(v, id);
    if (!p) return kPortInvalid;
    p->queue.clear();
    return 0;
}

i32 sceVoiceConnectIPortToOPort(u32 in, u32 out) {
    Voice& v = voice();
    Lock held(v.lock);
    if (!v.initialized) return kNotInit;
    Port *i = port(v, in), *o = port(v, out);
    if (!i || !o) return kPortInvalid;
    if (!is_input(i->type) || is_input(o->type)) return kTopology;
    auto& outs = v.edges[in];
    if (std::find(outs.begin(), outs.end(), out) != outs.end()) return kTopology;
    outs.push_back(out);
    return 0;
}
i32 sceVoiceDisconnectIPortFromOPort(u32 in, u32 out) {
    Voice& v = voice();
    Lock held(v.lock);
    if (!v.initialized) return kNotInit;
    if (!port(v, in) || !port(v, out)) return kPortInvalid;
    auto it = v.edges.find(in);
    if (it == v.edges.end() || !std::erase(it->second, out)) return kTopology;
    return 0;
}
i32 sceVoiceCheckTopology() {
    Voice& v = voice();
    Lock held(v.lock);
    return v.initialized ? 0 : kNotInit;
}

// Whole frames of `*size` bytes into an input port (PCM or voice data), to the connected output ports of its kind;
// `*size` becomes what was taken.
i32 sceVoiceWriteToIPort(u32 in, u32 data, Ptr<u32> size, i32 /*frame_gaps*/) {
    if (!size || !writable(size.address, 4)) return kArgumentInvalid;
    Voice& v = voice();
    Lock held(v.lock);
    if (!v.initialized) return kNotInit;
    Port* p = port(v, in);
    if (!p || (p->type != kInPcm && p->type != kInVoice)) return kPortInvalid;
    const u32 n = *size / p->frame() * p->frame();
    if (n && !memory().valid(data, n)) return kArgumentInvalid;
    *size = n;
    if (!v.started || p->paused || p->muted || v.all_muted || !n) return 0;
    const u8* bytes = memory().host(data);
    for (u32 out : v.edges[in]) {
        Port* o = port(v, out);
        if (!o || kind(o->type) != kind(p->type) || o->paused) continue;
        o->queue.insert(o->queue.end(), bytes, bytes + n);
        while (o->queue.size() > kQueueLimit) o->queue.pop_front();
    }
    return 0;
}

// Up to `*size` bytes (whole frames) from an output port; `*size` becomes what was read.
i32 sceVoiceReadFromOPort(u32 out, u32 data, Ptr<u32> size) {
    if (!size || !writable(size.address, 4)) return kArgumentInvalid;
    Voice& v = voice();
    Lock held(v.lock);
    if (!v.initialized) return kNotInit;
    Port* p = port(v, out);
    if (!p || (p->type != kOutPcm && p->type != kOutVoice)) return kPortInvalid;
    const u32 n = std::min<u32>(*size, static_cast<u32>(p->queue.size())) / p->frame() * p->frame();
    if (n && !writable(data, n)) return kArgumentInvalid;
    u8* bytes = memory().host(data);
    for (u32 i = 0; i < n; ++i) bytes[i] = p->queue[i];
    p->queue.erase(p->queue.begin(), p->queue.begin() + n);
    *size = n;
    return 0;
}

template <class F>
i32 with_port(u32 id, F f) {
    Voice& v = voice();
    Lock held(v.lock);
    if (!v.initialized) return kNotInit;
    Port* p = port(v, id);
    if (!p) return kPortInvalid;
    return f(*p);
}

i32 sceVoiceSetMuteFlagAll(u32 muted) {
    Voice& v = voice();
    Lock held(v.lock);
    if (!v.initialized) return kNotInit;
    v.all_muted = (muted & 0xFFFF) != 0;
    return 0;
}
i32 sceVoiceSetMuteFlag(u32 id, u32 muted) {
    return with_port(id, [&](Port& p) { return p.muted = (muted & 0xFFFF) != 0, 0; });
}
i32 sceVoiceGetMuteFlag(u32 id, Ptr<u16> muted) {
    if (!muted || !writable(muted.address, 2)) return kArgumentInvalid;
    return with_port(id, [&](Port& p) { return *muted = p.muted ? 1 : 0, 0; });
}
i32 sceVoiceSetVolume(u32 id, f32 volume) {
    if (!(volume >= 0.0f)) return kArgumentInvalid;
    return with_port(id, [&](Port& p) { return p.volume = volume, 0; });
}
i32 sceVoiceGetVolume(u32 id, Ptr<f32> volume) {
    if (!volume || !writable(volume.address, 4)) return kArgumentInvalid;
    return with_port(id, [&](Port& p) { return *volume = p.volume, 0; });
}
i32 sceVoiceSetBitRate(u32 id, i32 bitrate) {
    if (!valid_bitrate(bitrate)) return kArgumentInvalid;
    return with_port(id, [&](Port& p) {
        if (kind(p.type) != 2) return kPortInvalid;
        p.bitrate = static_cast<u32>(bitrate);
        return 0;
    });
}
i32 sceVoiceGetBitRate(u32 id, Ptr<u32> bitrate) {
    if (!bitrate || !writable(bitrate.address, 4)) return kArgumentInvalid;
    return with_port(id, [&](Port& p) {
        if (kind(p.type) != 2) return kPortInvalid;
        *bitrate = p.bitrate;
        return 0;
    });
}
i32 sceVoicePausePort(u32 id) {
    return with_port(id, [](Port& p) { return p.paused = true, 0; });
}
i32 sceVoiceResumePort(u32 id) {
    return with_port(id, [](Port& p) { return p.paused = false, 0; });
}
i32 pause_all(bool paused) {
    Voice& v = voice();
    Lock held(v.lock);
    if (!v.initialized) return kNotInit;
    for (auto& [id, p] : v.ports) p.paused = paused;
    return 0;
}
i32 sceVoicePausePortAll() { return pause_all(true); }
i32 sceVoiceResumePortAll() { return pause_all(false); }

// SceVoiceBasePortInfo: type, state, the connected ports (an array the caller gives, numEdge long), bytes readable
// (output ports) or writable (input ports), frame size, connections (u16).
i32 sceVoiceGetPortInfo(u32 id, u32 info) {
    if (!info || !writable(info, 24)) return kArgumentInvalid;
    Voice& v = voice();
    Lock held(v.lock);
    if (!v.initialized) return kNotInit;
    Port* p = port(v, id);
    if (!p) return kPortInvalid;
    nx::vector<u32> connected;
    if (is_input(p->type)) {
        if (auto it = v.edges.find(id); it != v.edges.end()) connected = it->second;
    } else {
        for (const auto& [in, outs] : v.edges)
            if (std::find(outs.begin(), outs.end(), id) != outs.end()) connected.push_back(in);
    }
    const u32 edge = psv_ld32(info + 8), capacity = psv_ld16(info + 20);
    for (u32 i = 0; edge && i < connected.size() && i < capacity && writable(edge + 4 * i, 4); ++i) psv_st32(edge + 4 * i, connected[i]);
    const i32 state = !v.started ? kIdle : connected.empty() ? kReady : kRunning;
    u32 bytes = 0;
    if (!is_input(p->type)) bytes = static_cast<u32>(p->queue.size());
    else if (p->type != kInDevice) bytes = kind(p->type) == 1 ? p->buffer_size : kQueueLimit;
    psv_st32(info, static_cast<u32>(p->type));
    psv_st32(info + 4, static_cast<u32>(state));
    psv_st32(info + 12, bytes);
    psv_st32(info + 16, p->type == kInDevice || p->type == kOutDevice ? kPcmFrame : p->frame());
    psv_st16(info + 20, static_cast<u32>(connected.size()));
    psv_st16(info + 22, 0);
    return 0;
}

// SceVoiceResourceInfo: the most voice input, voice output, device input, device output and all ports (u16 each).
i32 sceVoiceGetResourceInfo(u32 info) {
    if (!info || !writable(info, 10)) return kArgumentInvalid;
    Voice& v = voice();
    Lock held(v.lock);
    if (!v.initialized) return kNotInit;
    psv_st16(info, 7);
    psv_st16(info + 2, 7);
    psv_st16(info + 4, 1);
    psv_st16(info + 6, 1);
    psv_st16(info + 8, kMaxPorts);
    return 0;
}

// The one attribute: whether the microphone's port has audio input (a bool): never.
i32 sceVoiceGetPortAttr(u32 id, u32 attr, u32 value) {
    if (attr != kAttrAudioInputActive) return kArgumentInvalid;
    if (!value || !writable(value, 1)) return kArgumentInvalid;
    return with_port(id, [&](Port& p) {
        if (p.type != kInDevice) return kPortInvalid;
        memory().host(value)[0] = 0;
        return 0;
    });
}
i32 sceVoiceSetPortAttr(u32 id, u32 /*attr*/, u32 /*value*/) {
    return with_port(id, [](Port&) { return kArgumentInvalid; });  // (no attribute can be set)
}

const Function kFunctions[] = {
    HLE_FUNCTION(0x805CC20F, sceVoiceInit),
    HLE_FUNCTION(0xAC98853E, sceVoiceEnd),
    HLE_FUNCTION(0xB2ED725B, sceVoiceStart),
    HLE_FUNCTION(0xC3868DF6, sceVoiceStop),
    HLE_FUNCTION(0x5E1CE910, sceVoiceResetPort),
    HLE_FUNCTION(0xFA4E57B1, sceVoiceCreatePort),
    HLE_FUNCTION(0xCE855C50, sceVoiceUpdatePort),
    HLE_FUNCTION(0x698BDAAE, sceVoiceConnectIPortToOPort),
    HLE_FUNCTION(0x5F0260F4, sceVoiceDisconnectIPortFromOPort),
    HLE_FUNCTION(0xAE46564D, sceVoiceDeletePort),
    HLE_FUNCTION(0x0A22EC0E, sceVoiceWriteToIPort),
    HLE_FUNCTION(0x09E4D18C, sceVoiceReadFromOPort),
    HLE_FUNCTION(0xDB90EAC4, sceVoiceSetMuteFlagAll),
    HLE_FUNCTION(0x0B9E4AE2, sceVoiceSetMuteFlag),
    HLE_FUNCTION(0xC913F7E9, sceVoiceGetMuteFlag),
    HLE_FUNCTION(0xD93769E6, sceVoiceSetVolume),
    HLE_FUNCTION(0x875CC80D, sceVoiceGetVolume),
    HLE_FUNCTION(0x02F58D6F, sceVoiceSetBitRate),
    HLE_FUNCTION(0xD02C00B4, sceVoiceGetBitRate),
    HLE_FUNCTION(0x5933CCFB, sceVoiceGetPortInfo),
    HLE_FUNCTION(0x23C6B16B, sceVoicePausePort),
    HLE_FUNCTION(0x2DE35411, sceVoiceResumePort),
    HLE_FUNCTION(0x39AA3884, sceVoicePausePortAll),
    HLE_FUNCTION(0x1F93FC0C, sceVoiceResumePortAll),
    HLE_FUNCTION(0x6E46950E, sceVoiceGetResourceInfo),
    HLE_FUNCTION(0x9B0D3490, sceVoiceSetPortAttr),
    HLE_FUNCTION(0x73F72940, sceVoiceGetPortAttr),
    HLE_FUNCTION(0x874054CE, sceVoiceCheckTopology),
};

}  // namespace

void register_voice() {
    {
        Voice& v = voice();
        Lock held(v.lock);
        v.initialized = v.started = false;
        v.ports.clear(), v.edges.clear();
    }
    register_library({"SceVoice", kSceVoice, kFunctions, {}, true});
}

}  // namespace psvrecore::runtime::hle
