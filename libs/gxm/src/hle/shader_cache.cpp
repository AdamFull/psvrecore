#include "shader_cache.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "psvrecore/log.hpp"
#include "core/foundation/core/sha256.h"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/strings/format.h"

#ifndef PSVRECORE_TRANSLATOR_ID
#define PSVRECORE_TRANSLATOR_ID "unknown"
#endif

namespace psvrecore::gxm::hle {

namespace {

// The file: this header, then records (a type byte, a length, the payload).
constexpr char kMagic[8] = {'P', 'S', 'V', 'S', 'H', 'C', 0, 1};
constexpr u8 kSpirv = 1, kGraphics = 2, kCompute = 3;

struct Header {
    char magic[8];
    char translator[64];             // PSVRECORE_TRANSLATOR_ID (hex)
    u32 desc_size;                   // sizeof(GraphicsPipelineDesc): its layout changing drops the file too
    u32 reserved;
};

Header header() {
    Header h{};
    std::memcpy(h.magic, kMagic, sizeof kMagic);
    const char* id = PSVRECORE_TRANSLATOR_ID;
    std::memcpy(h.translator, id, std::min<usize>(std::strlen(id), sizeof h.translator));
    h.desc_size = sizeof(vulkan::GraphicsPipelineDesc);
    return h;
}

u64 graphics_hash(const ShaderCache::Graphics& g) {
    return g.desc.hash() ^ (short_key(g.vertex) * 31) ^ (short_key(g.fragment) * 131);
}

}  // namespace

ShaderKey shader_key(std::initializer_list<std::span<const u8>> parts) {
    nx::vector<u8> bytes;
    const char* id = PSVRECORE_TRANSLATOR_ID;
    bytes.insert(bytes.end(), id, id + std::strlen(id));
    for (const auto& p : parts) {
        const u32 n = static_cast<u32>(p.size());  // (lengths too: the parts' borders are part of the key)
        bytes.insert(bytes.end(), reinterpret_cast<const u8*>(&n), reinterpret_cast<const u8*>(&n) + 4);
        bytes.insert(bytes.end(), p.begin(), p.end());
    }
    const auto digest = nx::sha256(bytes);
    ShaderKey key;
    std::memcpy(key.data(), digest.data(), key.size());
    return key;
}

u64 short_key(const ShaderKey& key) {
    u64 v;
    std::memcpy(&v, key.data(), 8);
    return v;
}

ShaderCache::~ShaderCache() {
    if (file_) std::fclose(file_);
}

void ShaderCache::open(const nx::string& folder) {
    nx::scoped_lock<nx::mutex> held(lock_);
    folder_ = folder;
    if (folder.empty()) return;
    (void)nx::fs::create_directories(nx::fs::path_view(folder));
    const nx::string path = folder + "/shaders.bin";
    const Header want = header();
    // What is there, if a file this translator wrote.
    usize kept = 0, dropped = 0;
    bool fresh = true;
    if (std::FILE* in = std::fopen(path.c_str(), "rb")) {
        Header h{};
        if (std::fread(&h, sizeof h, 1, in) == 1 && std::memcmp(&h, &want, sizeof h) == 0) {
            fresh = false;
            while (true) {
                u8 type = 0;
                u32 size = 0;
                if (std::fread(&type, 1, 1, in) != 1 || std::fread(&size, 4, 1, in) != 1) break;
                nx::vector<u8> payload(size);
                if (size && std::fread(payload.data(), 1, size, in) != size) break;  // (a record cut short: the end)
                if (type == kSpirv && size >= 16 && (size - 16) % 4 == 0) {
                    ShaderKey key;
                    std::memcpy(key.data(), payload.data(), 16);
                    nx::vector<u32> words((size - 16) / 4);
                    std::memcpy(words.data(), payload.data() + 16, size - 16);
                    spirv_[short_key(key)] = {key, std::move(words)};
                } else if (type == kGraphics && size == sizeof(Graphics)) {
                    Graphics g;
                    std::memcpy(&g, payload.data(), sizeof g);
                    if (!graphics_seen_.contains(graphics_hash(g))) {
                        graphics_seen_[graphics_hash(g)] = true;
                        graphics_.push_back(g);
                    }
                } else if (type == kCompute && size == 16) {
                    ShaderKey key;
                    std::memcpy(key.data(), payload.data(), 16);
                    if (!compute_seen_.contains(short_key(key))) {
                        compute_seen_[short_key(key)] = true;
                        compute_.push_back(key);
                    }
                } else {
                    ++dropped;
                    continue;
                }
                ++kept;
            }
        } else {
            nx::logi(logs::gpu, "shader cache: {} is another translator's: made again", path);
        }
        std::fclose(in);
    }
    if (fresh) {
        file_ = std::fopen(path.c_str(), "wb");
        if (file_) std::fwrite(&want, sizeof want, 1, file_);
    } else {
        file_ = std::fopen(path.c_str(), "ab");
    }
    if (!file_) nx::logw(logs::gpu, "shader cache: {} cannot be written: kept in memory only", path);
    if (file_) std::fflush(file_);
    nx::logi(logs::gpu, "shader cache: {} shaders, {} graphics and {} compute pipelines from {}{}", spirv_.size(),
             graphics_.size(), compute_.size(), path, dropped ? nx::format(" ({} records not read)", dropped) : nx::string());
    (void)kept;
}

void ShaderCache::append(u8 type, std::span<const u8> payload) {
    if (!file_) return;
    const u32 size = static_cast<u32>(payload.size());
    std::fwrite(&type, 1, 1, file_);
    std::fwrite(&size, 4, 1, file_);
    std::fwrite(payload.data(), 1, payload.size(), file_);
    std::fflush(file_);  // (the process may end without warning)
}

const nx::vector<u32>* ShaderCache::spirv(const ShaderKey& key) {
    nx::scoped_lock<nx::mutex> held(lock_);
    auto it = spirv_.find(short_key(key));
    return it != spirv_.end() && it->second.first == key ? &it->second.second : nullptr;
}

void ShaderCache::add_spirv(const ShaderKey& key, std::span<const u32> words) {
    nx::scoped_lock<nx::mutex> held(lock_);
    if (spirv_.contains(short_key(key))) return;
    spirv_[short_key(key)] = {key, nx::vector<u32>(words.begin(), words.end())};
    nx::vector<u8> payload(16 + words.size() * 4);
    std::memcpy(payload.data(), key.data(), 16);
    std::memcpy(payload.data() + 16, words.data(), words.size() * 4);
    append(kSpirv, payload);
}

void ShaderCache::add_graphics(const Graphics& g) {
    nx::scoped_lock<nx::mutex> held(lock_);
    const u64 h = graphics_hash(g);
    if (graphics_seen_.contains(h)) return;
    graphics_seen_[h] = true;
    graphics_.push_back(g);
    append(kGraphics, std::span<const u8>(reinterpret_cast<const u8*>(&g), sizeof g));
}

void ShaderCache::add_compute(const ShaderKey& key) {
    nx::scoped_lock<nx::mutex> held(lock_);
    if (compute_seen_.contains(short_key(key))) return;
    compute_seen_[short_key(key)] = true;
    compute_.push_back(key);
    append(kCompute, key);
}

nx::vector<ShaderCache::Graphics> ShaderCache::graphics() {
    nx::scoped_lock<nx::mutex> held(lock_);
    return graphics_;
}

nx::vector<ShaderKey> ShaderCache::compute() {
    nx::scoped_lock<nx::mutex> held(lock_);
    return compute_;
}

usize ShaderCache::spirv_count() {
    nx::scoped_lock<nx::mutex> held(lock_);
    return spirv_.size();
}

}  // namespace psvrecore::gxm::hle
