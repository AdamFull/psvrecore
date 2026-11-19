#pragma once

// The code generator's synthetic corpus (tests/codegen/corpus/, built into corpus_data.inc by
// scripts/codegen_corpus.py): small modules of compiled and hand-written ARM code at fixed addresses, the functions
// in them, and the functions the differential test runs with how their arguments are drawn.
//
// corpusgen translates the modules (as code discovery finds them from the function symbols); codegen_tests.cpp runs
// the generated functions against the interpreter; scripts/codegen_metrics.py measures the generated code.

#include <cstdint>

struct CorpusFunction {
    const char* name;
    uint32_t address;  // code address (bit 0: Thumb)
};

struct CorpusModule {
    const char* name;
    uint32_t base;  // guest address of its one read-only, executable segment
    const uint8_t* bytes;
    uint32_t size;
    const CorpusFunction* functions;
    uint32_t function_count;
};

struct CorpusTest {
    const char* module;
    const char* function;
    uint32_t address;  // code address
    const char* spec;  // "r0=ptr r1=int:0:40 ...": see scripts/codegen_corpus.py
};

#include "corpus_data.inc"

// The guest memory the corpus runs in: the modules from kCorpusBase (0x8000 each), then data the tests' pointer
// arguments point into (4 KiB regions), then the stack.
inline constexpr uint32_t kCorpusBase = 0x82000000;
inline constexpr uint32_t kCorpusModuleStride = 0x8000;
inline constexpr uint32_t kCorpusDataBase = 0x82030000;
inline constexpr uint32_t kCorpusRegionSize = 0x1000;
inline constexpr uint32_t kCorpusRegions = 32;
inline constexpr uint32_t kCorpusStackBase = kCorpusDataBase + kCorpusRegions * kCorpusRegionSize;
inline constexpr uint32_t kCorpusStackSize = 0x8000;
inline constexpr uint32_t kCorpusEnd = kCorpusStackBase + kCorpusStackSize;
static_assert(sizeof kCorpusModules / sizeof kCorpusModules[0] * kCorpusModuleStride <= kCorpusDataBase - kCorpusBase,
              "the corpus modules run into the data");
