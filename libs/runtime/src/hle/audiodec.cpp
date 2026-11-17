// The audio decoders (SceAudiodecUser): ATRAC9 (LibAtrac9, as NGS's player), MP3 (minimp3: MPEG-1, 2 and 2.5) and
// AAC (the PacketVideo decoder, as AvPlayer's), raw or ADTS; 16-bit PCM, interleaved, in the channels the decoder was
// made for (a mono stream doubled for stereo; stereo's left for mono). CELP has no decoder here: its library starts,
// but a decoder of its is refused (SCE_AUDIODEC_ERROR_UNSUPPORTED), named in the log.
//
// The game's SceAudiodecCtrl: its size, the decoder's handle, the stream (pointer, bytes used, the most a call
// reads), the PCM (pointer, bytes written, the most a call writes), the word length (16) and its SceAudiodecInfo,
// which CreateDecoder reads (ATRAC9: the 4 configuration bytes; MP3: channels, MPEG version; AAC: ADTS or not, channels,
// rate, SBR) and fills in
// (ATRAC9: channels, rate, superframe size, frames in it). A decode call decodes one frame (ATRAC9: up to 8, with
// DecodeNFrames; the last frame of a superframe uses the bytes up to its end); the decoder's context lives here, not in the game's memory (the "external" calls take it and give
// it back unused).

#include <cstring>

#include "../media/decoders.hpp"
#include "../media/mp3.hpp"
#include "hle_common.hpp"
#include "core/foundation/containers/hash_map.h"
#include "libatrac9.h"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceAudiodecUser = 0x2AA62046;
constexpr u32 kTypeAt9 = 0x1003, kTypeMp3 = 0x1004, kTypeAac = 0x1005, kTypeCelp = 0x1006;
constexpr i32 kInvalidType = err::code(0x807F0001), kInvalidInitParam = err::code(0x807F0002),
              kAlreadyInitialized = err::code(0x807F0003), kNotInitialized = err::code(0x807F0005),
              kAllHandlesInUse = err::code(0x807F0007), kInvalidPtr = err::code(0x807F0008),
              kInvalidHandle = err::code(0x807F0009), kInvalidWordLength = err::code(0x807F000C),
              kInvalidSize = err::code(0x807F000D), kUnsupported = err::code(0x807F000E),
              kInvalidNFrames = err::code(0x807F000F), kApiFail = err::code(0x807F0000),
              kAt9InvalidConfig = err::code(0x807F2000), kAacInvalidCh = err::code(0x807F3000),
              kMp3InvalidCh = err::code(0x807F2800), kMp3InvalidVersion = err::code(0x807F2801);
constexpr u32 kContextSize = 0x10000;

struct Ctrl {
    u32 size;
    i32 handle;
    u32 es;
    u32 es_used, es_max;
    u32 pcm;
    u32 pcm_written, pcm_max;
    u32 word_length;
    u32 info;
};
static_assert(sizeof(Ctrl) == 40);

struct Decoder {
    u32 type = 0;
    u32 channels = 0;  // (what the game asked for, and is given)
    void* at9 = nullptr;
    Atrac9CodecInfo at9_info{};
    i32 frame_in_superframe = 0;  // (ATRAC9: the next frame's place in its superframe, and its bytes so far)
    u32 superframe_used = 0;
    nx::unique_ptr<media::AacDecoder> aac;
    nx::unique_ptr<media::Mp3Decoder> mp3;
    bool adts = false;
    u32 aac_samples = 1024;
    i32 last_error = 0;
    ~Decoder() {
        if (at9) Atrac9ReleaseHandle(at9);
    }
};

struct Audiodec {
    nx::mutex lock;
    u32 limit[4] = {};  // (per type: AT9 channels, or streams; 0: the library not started)
    u32 used[4] = {};
    i32 next_handle = 1;
    nx::hash_map<i32, nx::unique_ptr<Decoder>> decoders;
};
Audiodec& audiodec() {
    static Audiodec a;
    return a;
}
i32 type_index(u32 type) { return type >= kTypeAt9 && type <= kTypeCelp ? static_cast<i32>(type - kTypeAt9) : -1; }

i32 sceAudiodecInitLibrary(u32 type, Ptr<const u32> param) {
    const i32 t = type_index(type);
    if (t < 0) return kInvalidType;
    if (!param || !readable(param.address, 8)) return kInvalidPtr;
    if (param.get()[0] != 8 || param.get()[1] == 0) return kInvalidInitParam;
    Audiodec& a = audiodec();
    nx::scoped_lock<nx::mutex> held(a.lock);
    if (a.limit[t]) return kAlreadyInitialized;
    a.limit[t] = param.get()[1];
    a.used[t] = 0;
    return 0;
}
i32 sceAudiodecTermLibrary(u32 type) {
    const i32 t = type_index(type);
    if (t < 0) return kInvalidType;
    Audiodec& a = audiodec();
    nx::scoped_lock<nx::mutex> held(a.lock);
    if (!a.limit[t]) return kNotInitialized;
    for (auto it = a.decoders.begin(); it != a.decoders.end();) {
        if (it->second->type == type) it = a.decoders.erase(it);
        else ++it;
    }
    a.limit[t] = a.used[t] = 0;
    return 0;
}

// An AAC AudioSpecificConfig for an LC stream (SBR is found in the stream).
nx::vector<u8> aac_config(u32 rate, u32 channels) {
    static const u32 kRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    u32 index = 4;
    for (u32 i = 0; i < 13; ++i)
        if (kRates[i] == rate) index = i;
    const u16 v = static_cast<u16>((2u << 11) | (index << 7) | (channels << 3));
    return {static_cast<u8>(v >> 8), static_cast<u8>(v)};
}

i32 create(Ptr<Ctrl> ctrl, u32 type) {
    const i32 t = type_index(type);
    if (t < 0) return kInvalidType;
    if (!ctrl || !writable(ctrl.address, sizeof(Ctrl))) return kInvalidPtr;
    Ctrl& c = *ctrl;
    if (c.size != sizeof(Ctrl)) return kInvalidSize;
    if (c.word_length != 16) return kInvalidWordLength;
    if (!c.info || !writable(c.info, 32)) return kInvalidPtr;
    Audiodec& a = audiodec();
    nx::scoped_lock<nx::mutex> held(a.lock);
    if (!a.limit[t]) return kNotInitialized;
    auto d = nx::make_unique<Decoder>();
    d->type = type;
    u32* info = reinterpret_cast<u32*>(memory().host(c.info));
    u32 cost = 1;
    if (type == kTypeAt9) {
        u8 config[4];
        std::memcpy(config, memory().host(c.info + 4), 4);
        d->at9 = Atrac9GetHandle();
        if (!d->at9 || Atrac9InitDecoder(d->at9, config) != 0 || Atrac9GetCodecInfo(d->at9, &d->at9_info) != 0)
            return kAt9InvalidConfig;
        d->channels = static_cast<u32>(d->at9_info.channels);
        cost = d->channels;
        // (SceAudiodecInfoAt9: size, config, channels, bit rate, rate, superframe size, frames in it)
        info[2] = d->channels;
        info[3] = static_cast<u32>(d->at9_info.superframeSize) * 8u * static_cast<u32>(d->at9_info.samplingRate) /
                  (static_cast<u32>(d->at9_info.frameSamples * d->at9_info.framesInSuperframe));
        info[4] = static_cast<u32>(d->at9_info.samplingRate);
        info[5] = static_cast<u32>(d->at9_info.superframeSize);
        info[6] = static_cast<u32>(d->at9_info.framesInSuperframe);
        c.es_max = static_cast<u32>(d->at9_info.superframeSize);
        c.pcm_max = static_cast<u32>(d->at9_info.frameSamples) * d->channels * 2;
    } else if (type == kTypeAac) {
        // (SceAudiodecInfoAac: size, ADTS, channels, rate, SBR)
        d->adts = info[1] != 0;
        d->channels = info[2];
        if (d->channels < 1 || d->channels > 2) return kAacInvalidCh;
        d->aac = nx::make_unique<media::AacDecoder>();
        if (!d->aac->open(aac_config(info[3], d->channels))) return kApiFail;
        d->aac_samples = info[4] ? 2048 : 1024;
        c.es_max = 1792;
        c.pcm_max = 2048 * d->channels * 2;
    } else if (type == kTypeMp3) {
        // (SceAudiodecInfoMp3: size, channels, MPEG version: 3 MPEG-1, 2 MPEG-2, 0 MPEG-2.5)
        d->channels = info[1];
        if (d->channels < 1 || d->channels > 2) return kMp3InvalidCh;
        if (info[2] != 0 && info[2] != 2 && info[2] != 3) return kMp3InvalidVersion;
        d->mp3 = nx::make_unique<media::Mp3Decoder>();
        c.es_max = 1441;
        c.pcm_max = 1152 * d->channels * 2;
    } else {
        nx::logw(logs::hle, "audio decoder for CELP: not supported here");
        return kUnsupported;
    }
    if (a.used[t] + cost > a.limit[t]) return kAllHandlesInUse;
    a.used[t] += cost;
    const i32 handle = a.next_handle++;
    a.decoders[handle] = std::move(d);
    c.handle = handle;
    return 0;
}

i32 sceAudiodecCreateDecoder(Ptr<Ctrl> ctrl, u32 type) { return create(ctrl, type); }
i32 sceAudiodecCreateDecoderExternal(Ptr<Ctrl> ctrl, u32 type, u32 /*context*/, u32 size) {
    if (size < kContextSize) return kInvalidSize;
    return create(ctrl, type);
}
i32 sceAudiodecGetContextSize(Ptr<Ctrl> ctrl, u32 type) {
    if (type_index(type) < 0) return kInvalidType;
    return ctrl ? static_cast<i32>(kContextSize) : kInvalidPtr;
}

i32 remove(Ptr<Ctrl> ctrl) {
    if (!ctrl || !writable(ctrl.address, sizeof(Ctrl))) return kInvalidPtr;
    Audiodec& a = audiodec();
    nx::scoped_lock<nx::mutex> held(a.lock);
    auto it = a.decoders.find(ctrl->handle);
    if (it == a.decoders.end()) return kInvalidHandle;
    const i32 t = type_index(it->second->type);
    a.used[t] -= it->second->type == kTypeAt9 ? it->second->channels : 1;
    a.decoders.erase(it);
    return 0;
}
i32 sceAudiodecDeleteDecoder(Ptr<Ctrl> ctrl) { return remove(ctrl); }
i32 sceAudiodecDeleteDecoderExternal(Ptr<Ctrl> ctrl, Ptr<u32> context) {
    if (context && writable(context.address, 4)) *context = 0;  // (no context in the game's memory was used)
    return remove(ctrl);
}

// One frame from `es` (`available` bytes) into `pcm`; the bytes used and written.
i32 decode_frame(Decoder& d, const u8* es, u32 available, i16* pcm, u32* used, u32* written) {
    if (d.type == kTypeAt9) {
        // (a superframe's frames differ in size: one reads at most the superframe)
        if (available < static_cast<u32>(d.at9_info.superframeSize / d.at9_info.framesInSuperframe)) return kInvalidSize;
        int n = 0;
        if (Atrac9Decode(d.at9, es, pcm, &n) != 0) return d.last_error = kApiFail;
        *used = static_cast<u32>(n);
        // (the superframe's last frame: on to the next superframe, past any padding)
        d.superframe_used += *used;
        if (++d.frame_in_superframe == d.at9_info.framesInSuperframe) {
            const u32 sf = static_cast<u32>(d.at9_info.superframeSize);
            if (d.superframe_used < sf) *used += sf - d.superframe_used;
            d.frame_in_superframe = 0;
            d.superframe_used = 0;
        }
        *written = static_cast<u32>(d.at9_info.frameSamples) * d.channels * 2;
        return 0;
    }
    if (d.type == kTypeMp3) {
        i16 frame[1152 * 2];
        const media::Mp3Decoder::Frame f = d.mp3->decode(std::span<const u8>(es, available), frame);
        if (!f.used) return d.last_error = kApiFail;  // (no whole frame in the bytes given)
        *used = f.used;
        for (u32 i = 0; i < f.samples; ++i)
            for (u32 ch = 0; ch < d.channels; ++ch) pcm[i * d.channels + ch] = frame[i * f.channels + (f.channels == 2 ? ch : 0)];
        *written = f.samples * d.channels * 2;
        return 0;
    }
    u32 offset = 0, size = available;
    if (d.adts) {
        if (available < 7 || es[0] != 0xFF || (es[1] & 0xF0) != 0xF0) return d.last_error = kApiFail;
        const u32 length = ((es[3] & 3u) << 11) | (u32{es[4]} << 3) | (es[5] >> 5);
        offset = (es[1] & 1) ? 7 : 9;
        if (length > available || length <= offset) return kInvalidSize;
        size = length - offset;
    }
    nx::vector<i16> stereo;
    const bool ok = d.aac->decode(std::span<const u8>(es + offset, size), stereo);
    if (!ok) d.last_error = kApiFail;
    const u32 samples = static_cast<u32>(stereo.size() / 2);
    if (d.channels == 2) std::memcpy(pcm, stereo.data(), stereo.size() * 2);
    else
        for (u32 i = 0; i < samples; ++i) pcm[i] = stereo[2 * i];
    *used = d.adts ? size + offset : nx::max<u32>(d.aac->bytes_used(), 1);
    *written = samples * d.channels * 2;
    return ok ? 0 : kApiFail;
}

i32 decode(Ptr<Ctrl> ctrl, u32 frames) {
    if (!ctrl || !writable(ctrl.address, sizeof(Ctrl))) return kInvalidPtr;
    Ctrl& c = *ctrl;
    Audiodec& a = audiodec();
    nx::scoped_lock<nx::mutex> held(a.lock);
    auto it = a.decoders.find(c.handle);
    if (it == a.decoders.end()) return kInvalidHandle;
    Decoder& d = *it->second;
    if (frames < 1 || frames > (d.type == kTypeAt9 ? 8u : 1u)) return kInvalidNFrames;
    if (!c.es || !c.pcm || !readable(c.es, c.es_max) || !writable(c.pcm, c.pcm_max * frames)) return kInvalidPtr;
    u32 used_total = 0, written_total = 0;
    for (u32 f = 0; f < frames; ++f) {
        nx::vector<i16> out(4096, 0);
        u32 used = 0, written = 0;
        const i32 r = decode_frame(d, memory().host(c.es + used_total), c.es_max * frames - used_total, out.data(), &used,
                                   &written);
        if (r < 0) {
            c.es_used = used_total, c.pcm_written = written_total;
            return r;
        }
        std::memcpy(memory().host(c.pcm + written_total), out.data(), written);
        used_total += used, written_total += written;
    }
    c.es_used = used_total;
    c.pcm_written = written_total;
    return 0;
}
i32 sceAudiodecDecode(Ptr<Ctrl> ctrl) { return decode(ctrl, 1); }
i32 sceAudiodecDecodeNFrames(Ptr<Ctrl> ctrl, u32 frames) { return decode(ctrl, frames); }

// Back to the stream's start (after a seek): the decoder's state forgotten.
i32 sceAudiodecClearContext(Ptr<Ctrl> ctrl) {
    if (!ctrl || !readable(ctrl.address, sizeof(Ctrl))) return kInvalidPtr;
    Audiodec& a = audiodec();
    nx::scoped_lock<nx::mutex> held(a.lock);
    auto it = a.decoders.find(ctrl->handle);
    if (it == a.decoders.end()) return kInvalidHandle;
    Decoder& d = *it->second;
    if (d.aac) d.aac->reset();
    if (d.mp3) d.mp3->reset();
    if (d.at9) {
        u8 config[4];
        std::memcpy(config, d.at9_info.configData, 4);
        Atrac9ReleaseHandle(d.at9);
        d.at9 = Atrac9GetHandle();
        if (!d.at9 || Atrac9InitDecoder(d.at9, config) != 0) return kApiFail;
    }
    d.last_error = 0;
    d.frame_in_superframe = 0;
    d.superframe_used = 0;
    return 0;
}
// The decoder's last error (of the stream: 0, or the call's).
i32 sceAudiodecGetInternalError(Ptr<Ctrl> ctrl, Ptr<i32> error) {
    if (!ctrl || !error || !writable(error.address, 4)) return kInvalidPtr;
    Audiodec& a = audiodec();
    nx::scoped_lock<nx::mutex> held(a.lock);
    auto it = a.decoders.find(ctrl->handle);
    if (it == a.decoders.end()) return kInvalidHandle;
    *error = it->second->last_error;
    return 0;
}

const Function kFunctions[] = {
    HLE_FUNCTION(0x445C2CEF, sceAudiodecInitLibrary),
    HLE_FUNCTION(0x45719B9D, sceAudiodecTermLibrary),
    HLE_FUNCTION(0x4DFD3AAA, sceAudiodecCreateDecoder),
    HLE_FUNCTION(0xE7A24E16, sceAudiodecDeleteDecoder),
    HLE_FUNCTION(0x56085DFB, sceAudiodecCreateDecoderExternal),
    HLE_FUNCTION(0xE4EA05BB, sceAudiodecDeleteDecoderExternal),
    HLE_FUNCTION(0xDB712ABC, sceAudiodecGetContextSize),
    HLE_FUNCTION(0xCCDABA04, sceAudiodecDecode),
    HLE_FUNCTION(0x8018AA9B, sceAudiodecDecodeNFrames),
    HLE_FUNCTION(0xF72F9B64, sceAudiodecClearContext),
    HLE_FUNCTION(0x883B0CF5, sceAudiodecGetInternalError),
};

}  // namespace

void register_audiodec() {
    {
        Audiodec& a = audiodec();
        nx::scoped_lock<nx::mutex> held(a.lock);
        a.decoders.clear();
        for (u32 i = 0; i < 4; ++i) a.limit[i] = a.used[i] = 0;
    }
    register_library({"SceAudiodecUser", kSceAudiodecUser, kFunctions});
}

}  // namespace psvrecore::runtime::hle
