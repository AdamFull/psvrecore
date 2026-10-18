/* The differential test corpus fuzzgen writes: blocks of random instructions and the functions the code generator
 * made of them. */
#ifndef PSVRECORE_TESTS_FUZZ_BLOCKS_H
#define PSVRECORE_TESTS_FUZZ_BLOCKS_H

#include "psvrecore/recomp/guest.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FuzzBlock {
    uint32_t address;
    uint32_t thumb;
    uint32_t count;
    const uint32_t* raw;  /* each instruction's encoding (32-bit Thumb: the first halfword in bits 31-16) */
    PsvFunction function;
} FuzzBlock;

extern const FuzzBlock fuzz_blocks[];
extern const uint32_t fuzz_block_count;

#ifdef __cplusplus
}
#endif

#endif
