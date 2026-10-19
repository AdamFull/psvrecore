#pragma once

// The shader cache: what the renderer made for a game's programs, kept from one run to the next so a later run makes
// it before the game starts (warm_up) instead of at a program's first draw.
//
// - SPIR-V by key: a hash of the program's bytes (its GXP), what the translation depends on besides them (the vertex
//   inputs, the fragment output and its format) and the translator itself (PSVRECORE_TRANSLATOR_ID, a hash of its
//   sources: a cache an older translator wrote is not used).
// - The graphics pipelines made (their descriptions, the shaders by key) and the compute ones (secondary programs).
// - Beside them, the driver's pipeline cache (vulkan::Pipelines::save_cache).
//
// One file, shaders.bin, appended to as things are made (a run that ends any way keeps what it made), in a folder
// the frontend gives (gxm::Options::cache_folder). Nothing in it is the game's: it is made from its programs on the
// user's machine, as the driver's own caches are.

#include <array>
#include <cstdio>
#include <span>

#include "psvrecore/vulkan/pipelines.hpp"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/threading/sync.h"

namespace psvrecore::gxm::hle {

using ShaderKey = std::array<u8, 16>;

// A key from the translator's identity and the parts given (each as bytes).
ShaderKey shader_key(std::initializer_list<std::span<const u8>> parts);
u64 short_key(const ShaderKey& key);  // (the first 8 bytes, for hash maps)

class ShaderCache {
public:
    struct Graphics {
        vulkan::GraphicsPipelineDesc desc;  // (its shader modules and layout zero)
        ShaderKey vertex, fragment;
    };

    ~ShaderCache();
    // Reads the folder's file (one another translator wrote is dropped) and appends to it from then on. Empty: in
    // memory only.
    void open(const nx::string& folder);
    const nx::string& folder() const { return folder_; }

    const nx::vector<u32>* spirv(const ShaderKey& key);
    void add_spirv(const ShaderKey& key, std::span<const u32> words);
    void add_graphics(const Graphics& g);
    void add_compute(const ShaderKey& key);
    nx::vector<Graphics> graphics();
    nx::vector<ShaderKey> compute();
    usize spirv_count();

private:
    void append(u8 type, std::span<const u8> payload);

    nx::mutex lock_;
    nx::string folder_;
    std::FILE* file_ = nullptr;
    nx::hash_map<u64, std::pair<ShaderKey, nx::vector<u32>>> spirv_;
    nx::vector<Graphics> graphics_;
    nx::hash_map<u64, bool> graphics_seen_;  // (by the description's hash and its shaders)
    nx::vector<ShaderKey> compute_;
    nx::hash_map<u64, bool> compute_seen_;
};

}  // namespace psvrecore::gxm::hle
