#pragma once

// Translation of USSE primary programs to SPIR-V. A program's registers become function-scope arrays of 32-bit words
// and each instruction the same operations on them as the reference interpreter (usse/interp.hpp) performs; branches
// become a loop around a switch over the program's blocks. Secondary programs are not translated: they run once
// per draw on the CPU, and their results are the primary program's secondary attributes.
//
// The compute form (the translator's test form) runs one instance per invocation: it reads the instance's primary
// attributes from a storage buffer, the secondary attributes from another, and writes the primary attributes, the
// output registers and whether the instance was discarded to a third. Textures and guest memory are storage buffers
// too, in the test model of psvusse --run (RGBA8 images sampled at the nearest texel with wrapping), so that the
// GPU's results can be compared with the interpreter's exactly where the arithmetic allows.

#include <span>

#include "psvrecore/error.hpp"
#include "psvrecore/usse/decode.hpp"
#include "psvrecore/usse/gxp.hpp"
#include "core/foundation/core/foundation.h"

namespace psvrecore::gxm {

// The compute form's buffers, in 32-bit words.
struct ComputeLayout {
    static constexpr u32 kPrimary = 64;   // per invocation, in: primary attributes 0-63; out: the same, after the run
    static constexpr u32 kOutput = 64;    // per invocation, out: output registers 0-63
    static constexpr u32 kSecondary = 256;  // shared: the secondary attributes
    static constexpr u32 kIn = kPrimary;
    static constexpr u32 kOut = kPrimary + kOutput + 1;  // (+ 1: non-zero when discarded)
    // Textures: kTextures headers of four words (width, height, the word index of the first texel, 0) by the unit
    // a sampler's first state word names, then the texels (RGBA8, rows from the top; cube faces stacked).
    static constexpr u32 kTextures = 16;
    // Memory: the guest address of word 2, the number of words, then the words (and two words of padding).
    static constexpr u32 kMemoryHeader = 2;
    static constexpr u32 kLocalSize = 1;
};

// Bindings of the compute form (descriptor set 0, all storage buffers).
inline constexpr u32 kBindingIn = 0, kBindingSecondary = 1, kBindingOut = 2, kBindingTextures = 3, kBindingMemory = 4;

// Translates a primary program to the compute form. Fails on an instruction the translator does not implement.
Result<nx::vector<u32>> translate_compute(const usse::Gxp& gxp, std::span<const usse::Decoded> program);

}  // namespace psvrecore::gxm
