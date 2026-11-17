// Textures decoded ahead (prefetch.cpp): when a game sets a texture up, its texels are decoded on a thread of their
// own, so that the scene that first draws it has them already.
#pragma once

#include <optional>

#include "psvrecore/gxm/texture.hpp"

namespace psvrecore::gxm::hle {

// A texture's contents as the renderer tells them apart: a hash of its texels' bytes and then its palette's
// (`palette` 0: none). In render.cpp.
u64 texture_contents(const TextureInfo& info, u32 size, u32 palette);

// The texture whose control words are at guest address `words` was set up or changed (the game's thread): decoded a
// little later, once the game is done setting it up (its palette and gamma are set after its init).
void prefetch_texture(u32 words);

// Decoding begins (the renderer, its device made): BC1-BC3 blocks kept as they are when the device samples them.
// PSVRECORE_TEXTURE_PREFETCH=0 turns it off.
void prefetch_start(bool native_bc);

// The texels decoded ahead for a texture with this description and contents, taken (none: not decoded, or its
// contents changed after).
std::optional<Texels> take_prefetched(const TextureInfo& info, u64 contents);

struct PrefetchStats {
    u64 decoded = 0, taken = 0, changed = 0, dropped = 0, bytes = 0;  // (dropped: past the budget; bytes: kept now)
};
PrefetchStats prefetch_stats();

}  // namespace psvrecore::gxm::hle
