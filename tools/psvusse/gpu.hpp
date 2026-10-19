#pragma once

// psvusse --gpu: the primary program translated (gxm/translate.hpp) and run on a Vulkan device in the compute form,
// on the register state the interpreter set up (the secondary attributes, the primary attributes).

#include <span>

#include "psvrecore/error.hpp"
#include "psvrecore/usse/interp.hpp"

namespace psvusse {

using namespace psvrecore;

struct GpuTexture {
    u32 unit = 0, width = 0, height = 0;
    std::span<const u32> texels;
};

struct GpuOptions {
    i32 gpu = -1;
    bool validation = false;
};

// Runs the primary program on m's primary and secondary attributes and writes back m.primary, m.output and m.killed.
// `memory` is the guest memory from address `memory_base`.
Result<void> run_on_gpu(const GpuOptions& options, const usse::Gxp& gxp, std::span<const usse::Decoded> program,
                        usse::Machine& m, std::span<const GpuTexture> textures, u32 memory_base,
                        std::span<const u32> memory);

}  // namespace psvusse
