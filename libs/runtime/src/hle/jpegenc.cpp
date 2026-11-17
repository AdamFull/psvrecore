// The JPEG encoder (SceJpegEncUser): an encoder's set-up, kept here by its context's address (the size, the pixel
// format, the output buffer, the compression ratio). Encoding and the colour conversion are not implemented: a call to
// either stops the program until they are.

#include "hle_common.hpp"

#include "core/foundation/containers/hash_map.h"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceJpegEncUser = 0x3748BFD3;
constexpr u32 kContextSize = 0x100;
constexpr i32 kImageSize = err::code(0x80650200), kInvalidRatio = err::code(0x80650202),
              kInvalidPixelFormat = err::code(0x80650203), kInvalidPointer = err::code(0x80650205);

struct Encoder {
    u32 width = 0, height = 0, pixel_format = 0, output = 0, output_size = 0, ratio = 64;
};
struct Encoders {
    nx::mutex lock;
    nx::flat_hash_map<u32, Encoder> by_context;
};
Encoders& encoders() {
    static Encoders e;
    return e;
}

bool known_format(u32 f) { return f == 0 || f == 4 || f == 8 || f == 9; }  // (RGBA, BGRA, YCbCr 4:2:0, 4:2:2)

i32 sceJpegEncoderGetContextSize() { return kContextSize; }

i32 sceJpegEncoderInit(u32 context, i32 width, i32 height, u32 pixel_format, u32 output, u32 output_size) {
    if (!context || !writable(context, kContextSize) || !output) return kInvalidPointer;
    if (width <= 0 || height <= 0 || width > 65535 || height > 65535) return kImageSize;
    if (!known_format(pixel_format)) return kInvalidPixelFormat;
    Encoders& all = encoders();
    nx::scoped_lock<nx::mutex> held(all.lock);
    all.by_context[context] = Encoder{static_cast<u32>(width), static_cast<u32>(height), pixel_format, output, output_size, 64};
    nx::logd(logs::hle, "JPEG encoder at 0x{:08x}: {}x{}, pixel format {}", context, width, height, pixel_format);
    return 0;
}

i32 sceJpegEncoderEnd(u32 context) {
    Encoders& all = encoders();
    nx::scoped_lock<nx::mutex> held(all.lock);
    return all.by_context.erase(context) ? 0 : kInvalidPointer;
}

i32 sceJpegEncoderSetOutputAddr(u32 context, u32 output, u32 output_size) {
    if (!output) return kInvalidPointer;
    Encoders& all = encoders();
    nx::scoped_lock<nx::mutex> held(all.lock);
    auto it = all.by_context.find(context);
    if (it == all.by_context.end()) return kInvalidPointer;
    it->second.output = output, it->second.output_size = output_size;
    return 0;
}

i32 sceJpegEncoderSetCompressionRatio(u32 context, i32 ratio) {
    if (ratio < 0 || ratio > 255) return kInvalidRatio;
    Encoders& all = encoders();
    nx::scoped_lock<nx::mutex> held(all.lock);
    auto it = all.by_context.find(context);
    if (it == all.by_context.end()) return kInvalidPointer;
    it->second.ratio = static_cast<u32>(ratio);
    return 0;
}

const Function kFunctions[] = {
    HLE_FUNCTION(0x2B55844D, sceJpegEncoderGetContextSize),
    HLE_FUNCTION(0x88DA92B4, sceJpegEncoderInit),
    HLE_FUNCTION(0xC87AA849, sceJpegEncoderEnd),
    HLE_FUNCTION(0x25D52D97, sceJpegEncoderSetOutputAddr),
    HLE_FUNCTION(0xB2B828EC, sceJpegEncoderSetCompressionRatio),
};

}  // namespace

void register_jpegenc() {
    {
        Encoders& all = encoders();
        nx::scoped_lock<nx::mutex> held(all.lock);
        all.by_context.clear();
    }
    register_library({"SceJpegEncUser", kSceJpegEncUser, kFunctions});
}

}  // namespace psvrecore::runtime::hle
