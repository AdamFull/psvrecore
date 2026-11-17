// The MP4 reader (SceMp4, system module 0x2D): known only by NID (no database, stub library or header names its
// functions). What is known comes from the games' callers: five games (WipEout 2048, Lumines, LittleBigPlanet,
// Gravity Rush, Uncharted) link the same movie source code, a copy of AvPlayer's own file source (its log names its
// steps: VerifyURI, Init, GetStreamInfo, GetNextAU), which calls these:
// - 0x05474AF0, open: (path, file replacement {object, open, close, read at an offset, size}, memory replacement
//   {object, allocate, free}, two words the source was given, a third (its caller's), a log level 0-4 on the stack):
//   a handle, 0 when the file does not open.
// - 0x30E49E4D, start: (handle, u32* stream count, u64* duration); negative on failure.
// - 0x8BE0E3D3 (0xD5B26179 in LittleBigPlanet and Uncharted, the same call site): a stream's information: (handle,
//   index, a 0x158-byte record the caller zeroes): type at 0 (0 video, 1 audio), codec at 4 (video: 1, AVC; audio: 2
//   or 4), the video's width and height at 8 and 12, the audio's channels (u16) at 0x4C and rate at 0x38, a 64-bit
//   value at 0x30, the language at 0x50.
// - 0x609E57AD: a stream turned on or off (handle, index, 1/0).
// - 0x20A01112, 0x32A15788, 0x40C64DC0: the next access unit, one per kind of reader: (handle, record) giving its data
//   at 8, two 64-bit times at 0x10 and 0x18, flags at 0x28-0x30; negative at the end or on failure.
// - 0x920623C8: stop (handle); 0x7B4832FE: close (handle, record).
// - 0x40351E1A (handle and two more words, passed through: a seek, likely), and seven queries of the handle alone
//   (0xD092A066, 0x96676BA0, 0x94E1305D, 0xEFE299C9, 0xA870587F, 0xD2871F1C, 0xCB91CEBC); 0xA7C09746 and 0x6B69F900 take
//   two words and no handle.
// The record layouts beyond the fields read are unknown, so no MP4 is read here: the module is not provided (the
// games check its load and skip their movies), and should a game call in anyway, opening gives no handle and
// everything else SCE_KERNEL_ERROR_UNSUP. AvPlayer plays MP4 movies itself.

#include "hle_common.hpp"

#include <atomic>

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceMp4 = 0x8FEACABD;

void note(u32 nid) {
    static std::atomic<u32> logged{0};
    if (logged.fetch_add(1) < 4) nx::logw(logs::hle, "SceMp4 0x{:08x}: no MP4 reader here (the module is not provided)", nid);
}

u32 mp4_open() {
    note(0x05474AF0);
    return 0;  // (no handle)
}
template <u32 Nid>
i32 mp4_unsupported() {
    note(Nid);
    return err::kUnsupported;
}

#define MP4_CALL(nid, role) Function{nid, "(SceMp4 " #nid ": " role ")", &entry<&mp4_unsupported<nid>>}

const Function kFunctions[] = {
    Function{0x05474AF0, "(SceMp4 0x05474AF0: open)", &entry<&mp4_open>},
    MP4_CALL(0x30E49E4D, "start"),
    MP4_CALL(0x8BE0E3D3, "stream information"),
    MP4_CALL(0xD5B26179, "stream information"),
    MP4_CALL(0x609E57AD, "stream on or off"),
    MP4_CALL(0x20A01112, "next access unit"),
    MP4_CALL(0x32A15788, "next access unit"),
    MP4_CALL(0x40C64DC0, "next access unit"),
    MP4_CALL(0x920623C8, "stop"),
    MP4_CALL(0x7B4832FE, "close"),
    MP4_CALL(0x40351E1A, "seek?"),
    MP4_CALL(0xD092A066, "query"),
    MP4_CALL(0x96676BA0, "query"),
    MP4_CALL(0x94E1305D, "query"),
    MP4_CALL(0xEFE299C9, "query"),
    MP4_CALL(0xA870587F, "query"),
    MP4_CALL(0xD2871F1C, "query"),
    MP4_CALL(0xCB91CEBC, "query"),
    MP4_CALL(0xA7C09746, "no handle"),
    MP4_CALL(0x6B69F900, "no handle"),
};

}  // namespace

void register_mp4lib() { register_library({"SceMp4", kSceMp4, kFunctions}); }

}  // namespace psvrecore::runtime::hle
