#include "decoders.hpp"

#include <cstdlib>
#include <cstring>

#include "codec_api.h"
#include "pvmp4audiodecoder_api.h"

namespace psvrecore::runtime::media {

// ---- H.264

struct H264Decoder::State {
    ISVCDecoder* decoder = nullptr;
    nx::vector<u8> parameter_sets;  // (Annex B: the SPS and PPS of the avcC record)
    u32 length_size = 4;
    nx::vector<u8> annexb;

    ~State() { close(); }
    void close() {
        if (decoder) {
            decoder->Uninitialize();
            WelsDestroyDecoder(decoder);
            decoder = nullptr;
        }
    }
    bool create() {
        close();
        if (WelsCreateDecoder(&decoder) != 0 || !decoder) return false;
        SDecodingParam p{};
        p.sVideoProperty.size = sizeof(p.sVideoProperty);
        p.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
        p.eEcActiveIdc = ERROR_CON_SLICE_COPY;
        if (decoder->Initialize(&p) != 0) return false;
        // (the parameter sets first: they make no picture)
        nx::vector<Picture> none;
        return feed(parameter_sets.data(), parameter_sets.size(), 0, none);
    }
    // A picture openh264 hands out (I420 planes, cropped) as AvPlayer's: 16-aligned, U and V interleaved.
    static Picture picture(unsigned char* planes[3], const SBufferInfo& info) {
        const SSysMEMBuffer& b = info.UsrData.sSystemBuffer;
        const u32 w = static_cast<u32>(b.iWidth), h = static_cast<u32>(b.iHeight);
        Picture p;
        p.time = static_cast<i64>(info.uiOutYuvTimeStamp);
        p.width = (w + 15) & ~15u;
        p.height = (h + 15) & ~15u;
        p.crop_right = p.width - w;
        p.crop_bottom = p.height - h;
        p.data.assign(usize{p.width} * p.height * 3 / 2, 0);
        u8* y = p.data.data();
        u8* uv = y + usize{p.width} * p.height;
        // (the padding repeats the picture's last column and row)
        for (u32 row = 0; row < p.height; ++row) {
            const u8* src = planes[0] + usize{nx::min(row, h - 1)} * b.iStride[0];
            u8* dst = y + usize{row} * p.width;
            std::memcpy(dst, src, w);
            std::memset(dst + w, src[w - 1], p.width - w);
        }
        const u32 cw = (w + 1) / 2, ch = (h + 1) / 2;
        for (u32 row = 0; row < p.height / 2; ++row) {
            const usize r = nx::min(row, ch - 1);
            const u8* u = planes[1] + r * b.iStride[1];
            const u8* v = planes[2] + r * b.iStride[1];
            u8* dst = uv + usize{row} * p.width;
            for (u32 x = 0; x < p.width / 2; ++x) {
                const u32 sx = nx::min(x, cw - 1);
                dst[2 * x] = u[sx];
                dst[2 * x + 1] = v[sx];
            }
        }
        return p;
    }
    bool feed(const u8* data, usize size, i64 time, nx::vector<Picture>& out) {
        unsigned char* planes[3] = {};
        SBufferInfo info{};
        info.uiInBsTimeStamp = static_cast<unsigned long long>(time);
        const DECODING_STATE r = decoder->DecodeFrameNoDelay(data, static_cast<int>(size), planes, &info);
        if (info.iBufferStatus == 1 && planes[0]) out.push_back(picture(planes, info));
        return r == dsErrorFree || info.iBufferStatus == 1;
    }
};

H264Decoder::H264Decoder() : s_(std::make_unique<State>()) {}
H264Decoder::~H264Decoder() = default;

bool H264Decoder::open(std::span<const u8> avcc) {
    // (the record: version, profile, compatibility, level, NAL length size, then its SPSs and PPSs)
    if (avcc.size() < 7 || avcc[0] != 1) return false;
    s_->length_size = (avcc[4] & 3u) + 1;
    s_->parameter_sets.clear();
    usize at = 5;
    for (u32 kind = 0; kind < 2; ++kind) {
        if (at >= avcc.size()) return false;
        const u32 count = kind == 0 ? (avcc[at] & 31u) : avcc[at];
        ++at;
        for (u32 i = 0; i < count; ++i) {
            if (at + 2 > avcc.size()) return false;
            const usize n = (usize{avcc[at]} << 8) | avcc[at + 1];
            at += 2;
            if (at + n > avcc.size()) return false;
            const u8 start[4] = {0, 0, 0, 1};
            s_->parameter_sets.insert(s_->parameter_sets.end(), start, start + 4);
            s_->parameter_sets.insert(s_->parameter_sets.end(), avcc.begin() + at, avcc.begin() + at + n);
            at += n;
        }
    }
    return s_->create();
}

bool H264Decoder::decode(std::span<const u8> sample, i64 time, nx::vector<Picture>& out) {
    if (!s_->decoder) return false;
    // (length-prefixed NAL units to start codes)
    nx::vector<u8>& b = s_->annexb;
    b.clear();
    usize at = 0;
    while (at + s_->length_size <= sample.size()) {
        usize n = 0;
        for (u32 i = 0; i < s_->length_size; ++i) n = (n << 8) | sample[at + i];
        at += s_->length_size;
        if (at + n > sample.size()) return false;
        const u8 start[4] = {0, 0, 0, 1};
        b.insert(b.end(), start, start + 4);
        b.insert(b.end(), sample.begin() + at, sample.begin() + at + n);
        at += n;
    }
    if (b.empty()) return true;
    return s_->feed(b.data(), b.size(), time, out);
}

void H264Decoder::flush(nx::vector<Picture>& out) {
    if (!s_->decoder) return;
    for (u32 guard = 0; guard < 32; ++guard) {
        int remaining = 0;
        s_->decoder->GetOption(DECODER_OPTION_NUM_OF_FRAMES_REMAINING_IN_BUFFER, &remaining);
        if (remaining <= 0) break;
        unsigned char* planes[3] = {};
        SBufferInfo info{};
        s_->decoder->FlushFrame(planes, &info);
        if (info.iBufferStatus == 1 && planes[0]) out.push_back(State::picture(planes, info));
        else break;
    }
}

void H264Decoder::reset() {
    if (s_->decoder) s_->create();
}

// ---- AAC

struct AacDecoder::State {
    tPVMP4AudioDecoderExternal ext{};
    void* memory = nullptr;
    nx::vector<u8> config;
    nx::vector<i16> out;
    u32 frames = 0;
    i32 upsampling = 1;

    ~State() { std::free(memory); }
    bool init() {
        std::free(memory);
        memory = std::malloc(PVMP4AudioDecoderGetMemRequirements());
        if (!memory) return false;
        std::memset(&ext, 0, sizeof ext);
        ext.outputFormat = OUTPUTFORMAT_16PCM_INTERLEAVED;
        ext.aacPlusEnabled = true;
        ext.desiredChannels = 2;
        frames = 0;
        upsampling = 1;
        if (PVMP4AudioDecoderInitLibrary(&ext, memory) != 0) return false;
        if (config.empty()) return true;
        ext.pInputBuffer = config.data();
        ext.inputBufferCurrentLength = static_cast<Int>(config.size());
        ext.inputBufferMaxLength = 0;
        return PVMP4AudioDecoderConfig(&ext, memory) == MP4AUDEC_SUCCESS;
    }
};

AacDecoder::AacDecoder() : s_(std::make_unique<State>()) {}
AacDecoder::~AacDecoder() = default;

bool AacDecoder::open(std::span<const u8> config) {
    s_->config.assign(config.begin(), config.end());
    return s_->init();
}

bool AacDecoder::decode(std::span<const u8> frame, nx::vector<i16>& pcm) {
    State& s = *s_;
    if (!s.memory) return false;
    nx::vector<u8> input(frame.begin(), frame.end());
    s.out.assign(4096 * 2, 0);
    s.ext.pInputBuffer = input.data();
    s.ext.inputBufferCurrentLength = static_cast<Int>(input.size());
    s.ext.inputBufferMaxLength = 0;
    s.ext.inputBufferUsedLength = 0;
    s.ext.remainderBits = 0;
    s.ext.pOutputBuffer = s.out.data();
    s.ext.pOutputBuffer_plus = s.out.data() + 2048;
    s.ext.repositionFlag = false;
    const Int err = PVMP4AudioDecodeFrame(&s.ext, s.memory);
    ++s.frames;
    // (as Android's wrapper did: whether SBR doubles the rate is known from the first frames; a stream that turns
    // out to be plain AAC stops the upsampling)
    if (err == MP4AUDEC_SUCCESS && s.frames <= 2) {
        if (s.frames == 1) {
            s.upsampling = s.ext.aacPlusUpsamplingFactor;
        } else if (s.ext.extendedAudioObjectType == MP4AUDIO_AAC_LC || s.ext.extendedAudioObjectType == MP4AUDIO_LTP) {
            if (s.upsampling == 2) s.ext.aacPlusEnabled = false;
        } else if (s.upsampling == 1) {
            s.ext.aacPlusEnabled = false;
        }
    }
    const usize count = static_cast<usize>(s.ext.frameLength) * (s.upsampling == 2 ? 2 : 1) * 2;
    if (err != MP4AUDEC_SUCCESS) {
        pcm.assign(count, 0);
        return false;
    }
    pcm.assign(s.out.begin(), s.out.begin() + static_cast<isize>(nx::min(count, s.out.size())));
    return true;
}

u32 AacDecoder::sample_rate() const { return static_cast<u32>(s_->ext.samplingRate); }

void AacDecoder::reset() {
    if (s_->memory) PVMP4AudioDecoderResetBuffer(s_->memory);
}

}  // namespace psvrecore::runtime::media
