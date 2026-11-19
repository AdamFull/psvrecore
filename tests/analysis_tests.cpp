// psvrecore::analysis: code discovery on small hand-encoded functions (Thumb halfwords as clang's assembler encodes
// the instruction in each comment).

#include <initializer_list>

#include "formats_builders.hpp"
#include "nxtest.h"
#include "psvrecore/analysis/program.hpp"
#include "psvrecore/formats/elf.hpp"

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

// A fixed-address executable is loaded where it was linked: given those bases it loads, given others it is refused.
TEST_CASE("analysis: a fixed-address executable loads at the bases it was linked at, and only there") {
    auto bytes = test::module_elf();
    bytes[0x10] = static_cast<u8>(elf::kTypeSceExec), bytes[0x11] = static_cast<u8>(elf::kTypeSceExec >> 8);
    const u32 linked[] = {test::kBase0, test::kBase1}, moved[] = {0x82000000, 0x82010000};
    CHECK(static_cast<bool>(analysis::Image::from_bytes(bytes)));
    CHECK(static_cast<bool>(analysis::Image::from_bytes(bytes, linked)));
    CHECK(!analysis::Image::from_bytes(bytes, moved));
}

// Table branches bounded by the check before them, a constant compared through a register, a subtraction
// after the check, or the branches that reach them.
namespace bounds {

// An image of one executable segment holding these halfwords from its start (the rest `bx lr`).
analysis::Image thumb_image(std::initializer_list<u16> halfwords) {
    analysis::Segment seg;
    seg.base = seg.linked = kBase;
    seg.flags = elf::kSegmentRead | elf::kSegmentExecute;
    seg.bytes.assign(0x100, 0);
    for (u32 i = 0; i < seg.bytes.size(); i += 2) seg.bytes[i] = 0x70, seg.bytes[i + 1] = 0x47;  // bx lr
    u32 at = 0;
    for (u16 h : halfwords) {
        seg.bytes[at] = static_cast<u8>(h);
        seg.bytes[at + 1] = static_cast<u8>(h >> 8);
        at += 2;
    }
    return analysis::Image::from_segments({seg});
}

const analysis::JumpTable* table_at(const analysis::Program& p, u32 site) {
    auto fn = p.functions.find(kBase | 1u);
    if (fn == p.functions.end()) return nullptr;
    auto t = fn->second.tables.find(site);
    return t == fn->second.tables.end() ? nullptr : &t->second;
}

usize errors(const analysis::Program& p) {
    usize n = 0;
    for (const auto& d : p.diagnostics) n += d.severity == analysis::Severity::Error;
    return n;
}

}  // namespace bounds

using namespace bounds;

TEST_CASE("analysis: a table branch bounded before a subtraction, against a constant in a register") {
    // movw r3, #0x84; cmp r1, r3; bhi out; subs r1, #0x80; tbb [pc, r1]; 5 entries (0x84 - 0x80 + 1), a pad byte,
    // then the cases (`bx lr` each) at +0x14..+0x1C. Read up to its first target instead, the table would take the
    // pad byte as a sixth entry (a branch into the table).
    const auto image = thumb_image({0xF240, 0x0384, 0x4299, 0xD80B, 0x3980, 0xE8DF, 0xF001,
                                    0x0403, 0x0605, 0x0007});
    analysis::Options options;
    options.extra_entries = {kBase | 1u};
    options.use_exidx = false;
    const auto p = analysis::analyze(image, options);
    const auto* t = table_at(p, kBase + 0x0A);
    REQUIRE(t != nullptr);
    CHECK(t->bounded);
    REQUIRE(t->targets.size() == 5u);
    for (u32 i = 0; i < 5; ++i) CHECK(t->targets[i] == ((kBase + 0x14 + 2 * i) | 1u));
    CHECK(errors(p) == 0u);
}

TEST_CASE("analysis: a table branch on a value less a constant, checked before (subw)") {
    // cmp r1, #0x87; bhi out; subw r0, r1, #0x84; tbb [pc, r0]: 4 entries, the cases after them.
    const auto image = thumb_image({0x2987, 0xD80B, 0xF2A1, 0x0084, 0xE8DF, 0xF000, 0x0302, 0x0504});
    analysis::Options options;
    options.extra_entries = {kBase | 1u};
    options.use_exidx = false;
    const auto p = analysis::analyze(image, options);
    const auto* t = table_at(p, kBase + 0x08);
    REQUIRE(t != nullptr);
    CHECK(t->bounded);
    REQUIRE(t->targets.size() == 4u);
    for (u32 i = 0; i < 4; ++i) CHECK(t->targets[i] == ((kBase + 0x10 + 2 * i) | 1u));
    CHECK(errors(p) == 0u);
}

TEST_CASE("analysis: a table branch bounded on its own index is unchanged") {
    // cmp r0, #3; bhi out; tbb [pc, r0]: 4 entries.
    const auto image = thumb_image({0x2803, 0xD808, 0xE8DF, 0xF000, 0x0302, 0x0504});
    analysis::Options options;
    options.extra_entries = {kBase | 1u};
    options.use_exidx = false;
    const auto p = analysis::analyze(image, options);
    const auto* t = table_at(p, kBase + 0x04);
    REQUIRE(t != nullptr);
    CHECK(t->bounded);
    REQUIRE(t->targets.size() == 4u);
    for (u32 i = 0; i < 4; ++i) CHECK(t->targets[i] == ((kBase + 0x0C + 2 * i) | 1u));
    CHECK(errors(p) == 0u);
}

TEST_CASE("analysis: a table of addresses whose address is put in a register just before the branch") {
    // cmp r7, #3; bhi out; adr r8, #4; ldr.w pc, [r8, r7, lsl #2]; four words (the cases, at +0x20..+0x26), then
    // `bx lr`s. (SNC's form; WipEout 2048's executable has it.)
    const auto image = thumb_image({0x2F03, 0xD80B, 0xF20F, 0x0804, 0xF858, 0xF027, 0x0021, 0x8100, 0x0023, 0x8100,
                                    0x0025, 0x8100, 0x0027, 0x8100});
    analysis::Options options;
    options.extra_entries = {kBase | 1u};
    options.use_exidx = false;
    const auto p = analysis::analyze(image, options);
    const auto* t = table_at(p, kBase + 0x08);
    REQUIRE(t != nullptr);
    CHECK(t->bounded);
    CHECK(t->base_reg == 8);
    CHECK(t->index_reg == 7);
    CHECK(t->table == kBase + 0x0C);
    REQUIRE(t->targets.size() == 4u);
    for (u32 i = 0; i < 4; ++i) CHECK(t->targets[i] == ((kBase + 0x20 + 2 * i) | 1u));
    CHECK(errors(p) == 0u);
}

TEST_CASE("analysis: a table address in a register that something else writes before the branch is not followed") {
    // cmp r7, #3; bhi out; adr r8, #8; mov r8, r1; ldr.w pc, [r8, r7, lsl #2]: r8 is not the table's address.
    const auto image = thumb_image({0x2F03, 0xD80D, 0xF20F, 0x0808, 0x4688, 0xF858, 0xF027, 0x0000, 0x0021, 0x8100,
                                    0x0023, 0x8100, 0x0025, 0x8100, 0x0027, 0x8100});
    analysis::Options options;
    options.extra_entries = {kBase | 1u};
    options.use_exidx = false;
    const auto p = analysis::analyze(image, options);
    CHECK(p.functions.contains(kBase | 1u));
    CHECK(table_at(p, kBase + 0x0A) == nullptr);
}

TEST_CASE("analysis: a table branch reached only by a branch that checks its index") {
    // cmp r2, #3; bls tbb; bx lr; (bx lr); tbb [pc, r2]: 4 entries, two zero bytes, then the cases. Read up to its
    // first target instead, the zero bytes would be two more entries (branches into the table).
    const auto image = thumb_image({0x2A03, 0xD901, 0x4770, 0x4770, 0xE8DF, 0xF002, 0x0403, 0x0605, 0x0000});
    analysis::Options options;
    options.extra_entries = {kBase | 1u};
    options.use_exidx = false;
    const auto p = analysis::analyze(image, options);
    const auto* t = table_at(p, kBase + 0x08);
    REQUIRE(t != nullptr);
    CHECK(t->bounded);
    REQUIRE(t->targets.size() == 4u);
    for (u32 i = 0; i < 4; ++i) CHECK(t->targets[i] == ((kBase + 0x12 + 2 * i) | 1u));
    CHECK(errors(p) == 0u);
}
