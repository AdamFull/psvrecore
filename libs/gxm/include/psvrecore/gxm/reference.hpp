#pragma once

// The reference renderer: draws with GXM programs run by the USSE reference interpreter, rasterized on the CPU. It
// is the oracle of the rendering tests (tools/psvrender, scripts/render_check.py) and is never linked into a game.
//
// It covers what the tests use: triangle lists with float vertex attributes, the default viewport (the whole
// target, as sceGxmBeginScene sets it), perspective-correct varyings, RGBA8 textures (nearest or bilinear, repeat),
// an RGBA8 target, a LESS_EQUAL depth test with writes, source-alpha blending, discard. No clipping: geometry must
// stay inside the view volume (w > 0).
//
// Rasterization: pixel centres, the top-left rule. Pixels whose centre is within 1/64 of a pixel of an edge are
// marked: hardware rasterizers snap vertices to a sub-pixel grid and may cover them differently, so comparisons
// leave them out.

#include <span>

#include "psvrecore/error.hpp"
#include "psvrecore/usse/decode.hpp"
#include "psvrecore/usse/gxp.hpp"

namespace psvrecore::gxm::reference {

struct Program {
    const usse::Gxp* gxp = nullptr;
    std::span<const usse::Decoded> primary, secondary;
};

struct Texture {
    u32 width = 0, height = 0;
    nx::vector<u32> texels;  // RGBA8, rows from the top (red in the low byte)
    bool linear = false;     // bilinear (else nearest); repeat addressing
};

struct Attribute {
    u32 reg = 0;         // the attribute's primary register (its parameter's resource index)
    u32 components = 4;  // per vertex, f32
    nx::vector<f32> values;
};

struct Draw {
    Program vertex, fragment;
    nx::vector<Attribute> attributes;
    nx::vector<u32> indices;  // a triangle list
    nx::vector<u32> vertex_uniforms, fragment_uniforms;  // the default uniform buffers' words
    // Uniform buffers (BUFFER[n]) as words in memory, at addresses the renderer chooses.
    nx::vector<nx::vector<u32>> vertex_buffers, fragment_buffers;
    nx::vector<Texture> textures;  // fragment texture units 0, 1, ...
    bool depth_test = false;       // LESS_EQUAL, with depth writes
    bool blend = false;            // src * a + dst * (1 - a), alpha likewise
};

struct Image {
    u32 width = 0, height = 0;
    nx::vector<u32> rgba;  // RGBA8 (red in the low byte)
    nx::vector<f32> depth;
    nx::vector<u8> edge;   // 1: a pixel some triangle's edge passes too close to (see above)
    void reset(u32 w, u32 h, u32 clear_rgba = 0, f32 clear_depth = 1.0f);
};

Result<void> render(const Draw& draw, Image& target);

}  // namespace psvrecore::gxm::reference
