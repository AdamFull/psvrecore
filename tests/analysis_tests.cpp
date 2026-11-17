// psvrecore::analysis: code discovery on small hand-encoded functions (Thumb halfwords as clang's assembler encodes
// the instruction in each comment).

#include <initializer_list>

#include "nxtest.h"
#include "psvrecore/analysis/program.hpp"

using namespace psvrecore;

namespace {

constexpr u32 kBase = 0x81000000;

analysis::Image image_of(std::initializer_list<u16> halfwords) {
    analysis::Segment seg;
    seg.base = seg.linked = kBase;
    seg.flags = elf::kSegmentRead | elf::kSegmentExecute;
    for (u16 h : halfwords) {
        seg.bytes.push_back(static_cast<u8>(h));
        seg.bytes.push_back(static_cast<u8>(h >> 8));
    }
    seg.file_size = seg.size();
    return analysis::Image::from_segments({seg});
}

// The table branch of the function at kBase (Thumb).
const analysis::JumpTable* table_of(const analysis::Program& program) {
    const auto* fn = program.function_at(kBase | 1);
    if (!fn || fn->tables.size() != 1) return nullptr;
    return &fn->tables.begin()->second;
}

analysis::Program discover(const analysis::Image& image) {
    analysis::Options options;
    options.extra_entries = {kBase | 1};
    options.use_exidx = false;
    return analysis::analyze(image, options);
}

}  // namespace

// A switch over five cases whose index is a copy of the register compared, made after the compare (SNC moves a
// register with adds rd, rm, #0): the table's size comes from the compare.
TEST_CASE("analysis: a table branch indexed by a copy of the register its bounds check compares") {
    for (const u16 copy : {u16(0x1C1A) /* adds r2, r3, #0 */, u16(0x461A) /* mov r2, r3 */}) {
        const auto image = image_of({
            0x2B04,          // 00 cmp r3, #4
            0xD80C,          // 02 bhi 0x1e
            copy,            // 04 r2 = r3
            0xE8DF, 0xF012,  // 06 tbh [pc, r2, lsl #1]
            5, 6, 7, 8, 9,   // 0a the table: 0x14, 0x16, 0x18, 0x1a, 0x1c
            0x4770, 0x4770, 0x4770, 0x4770, 0x4770,  // 14-1c bx lr
            0x4770,          // 1e bx lr
        });
        const auto program = discover(image);
        const auto* table = table_of(program);
        REQUIRE(table != nullptr);
        CHECK(table->bounded);
        CHECK(table->index_reg == 2);
        REQUIRE(table->targets.size() == 5);
        CHECK(table->targets[0] == ((kBase + 0x14) | 1));
        CHECK(table->targets[4] == ((kBase + 0x1C) | 1));
    }
}

// A flag-setting copy between the compare and its branch replaces the flags the branch tests: no bound from it.
TEST_CASE("analysis: a flag-setting copy between the compare and its branch gives no bound") {
    const auto image = image_of({
        0x2B04,          // 00 cmp r3, #4
        0x1C1A,          // 02 adds r2, r3, #0
        0xD80B,          // 04 bhi 0x1e
        0xE8DF, 0xF012,  // 06 tbh [pc, r2, lsl #1]
        5, 6, 7, 8, 9,   // 0a the table
        0x4770, 0x4770, 0x4770, 0x4770, 0x4770, 0x4770,
    });
    const auto program = discover(image);
    const auto* table = table_of(program);
    REQUIRE(table != nullptr);
    CHECK(!table->bounded);
}
