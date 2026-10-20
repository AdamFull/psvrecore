#include "psvrecore/gxm/reference.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <utility>

#include "psvrecore/usse/interp.hpp"
#include "core/foundation/strings/format.h"

namespace psvrecore::gxm::reference {

namespace {

using usse::Machine;
using usse::ProgramKind;

// Guest memory for the programs' loads and stores: regions of words at fixed addresses.
class Memory : public usse::ShaderMemory {
public:
    struct Region {
        u32 base = 0;
        nx::vector<u32> words;
    };
    nx::vector<Region> regions;

    u32 add(u32 base, const nx::vector<u32>& words) {
        regions.push_back(Region{base, words});
        return base;
    }
    bool read(u32 address, void* out, u32 size) override { return access(address, out, size, false); }
    bool write(u32 address, const void* in, u32 size) override { return access(address, const_cast<void*>(in), size, true); }

private:
    bool access(u32 address, void* data, u32 size, bool store) {
        for (Region& r : regions) {
            const u64 end = u64{r.base} + r.words.size() * 4;
            if (address < r.base || u64{address} + size > end) continue;
            u8* at = reinterpret_cast<u8*>(r.words.data()) + (address - r.base);
            if (store) std::memcpy(at, data, size);
            else std::memcpy(data, at, size);
            return true;
        }
        return false;
    }
};

// Texture units: a sampler state's first word is its unit (the states load_secondary is given).
class Textures : public usse::ShaderTextures {
public:
    std::span<const Texture> units;

    bool sample(const usse::SamplerState& state, const usse::TextureRequest& r, f32 rgba[4]) override {
        if (state.words[0] >= units.size() || r.dim == usse::TexDim::Cube) return false;
        const Texture& t = units[state.words[0]];
        if (!t.width || !t.height) return false;
        const f32 u = r.coord[0], v = r.dim == usse::TexDim::D1 ? 0.5f : r.coord[1];
        auto wrap = [](i64 i, u32 n) { return static_cast<u32>(((i % n) + n) % n); };
        auto texel = [&](i64 x, i64 y, u32 k) {
            const u32 w = t.texels[usize{wrap(y, t.height)} * t.width + wrap(x, t.width)];
            return static_cast<f32>((w >> (8 * k)) & 0xFF) / 255.0f;
        };
        if (!t.linear) {
            const i64 x = static_cast<i64>(std::floor(u * static_cast<f32>(t.width)));
            const i64 y = static_cast<i64>(std::floor(v * static_cast<f32>(t.height)));
            for (u32 k = 0; k < 4; ++k) rgba[k] = texel(x, y, k);
            return true;
        }
        const f32 fx = u * static_cast<f32>(t.width) - 0.5f, fy = v * static_cast<f32>(t.height) - 0.5f;
        const i64 x0 = static_cast<i64>(std::floor(fx)), y0 = static_cast<i64>(std::floor(fy));
        const f32 ax = fx - static_cast<f32>(x0), ay = fy - static_cast<f32>(y0);
        for (u32 k = 0; k < 4; ++k) {
            const f32 top = texel(x0, y0, k) * (1 - ax) + texel(x0 + 1, y0, k) * ax;
            const f32 bottom = texel(x0, y0 + 1, k) * (1 - ax) + texel(x0 + 1, y0 + 1, k) * ax;
            rgba[k] = top * (1 - ay) + bottom * ay;
        }
        return true;
    }
};

// The secondary attributes of a stage: the driver's setup, then the secondary program (once per draw).
Result<void> run_secondary(const Program& p, const nx::vector<u32>& uniforms, const nx::vector<nx::vector<u32>>& buffers,
                           u32 buffer_base, Memory& memory, std::span<const usse::SamplerState> samplers, Machine& m) {
    u32 addresses[15] = {};
    for (usize b = 0; b < buffers.size() && b < 14; ++b)
        addresses[b] = memory.add(buffer_base + static_cast<u32>(b) * 0x00100000u, buffers[b]);
    // (the default uniform buffer's copy in memory: what does not fit the secondary attributes is read there)
    addresses[usse::kDefaultUniformContainer] = memory.add(buffer_base + 0x00F00000u, uniforms);
    m.memory = &memory;
    if (auto r = usse::load_secondary(*p.gxp, uniforms, addresses, m, samplers); !r) return r;
    if (!p.secondary.empty())
        if (auto r = usse::run(p.secondary, ProgramKind::Secondary, m); !r) return failure("secondary program: " + r.error().message);
    return {};
}

struct Vertex {
    f32 position[4] = {0, 0, 0, 1};
    nx::vector<std::pair<u32, std::array<f32, 4>>> varyings;  // (varying:: bit, value)
};

Result<void> run_vertex(const Draw& d, const Machine& setup, u32 index, Vertex& out) {
    const usse::Gxp& g = *d.vertex.gxp;
    Machine m = setup;
    for (const Attribute& a : d.attributes) {
        const usse::Parameter* p = nullptr;
        for (const usse::Parameter& q : g.parameters)
            if (q.category == usse::ParameterCategory::Attribute && q.resource == a.reg) p = &q;
        if (!p) continue;
        f32 v[4] = {0, 0, 0, 1};
        for (u32 k = 0; k < a.components && k < 4; ++k) v[k] = a.values[usize{index} * a.components + k];
        const u32 n = nx::max<u32>(p->components, 1);
        if (p->type == usse::ParameterType::F16) {
            for (u32 k = 0; k < n; k += 2)
                m.primary[a.reg + k / 2] = u32{usse::to_half(v[k])} | (k + 1 < 4 ? u32{usse::to_half(v[k + 1])} << 16 : 0u);
        } else if (p->type == usse::ParameterType::F32) {
            for (u32 k = 0; k < n && k < 4; ++k) m.primary[a.reg + k] = std::bit_cast<u32>(v[k]);
        } else {
            return failure(nx::format("attribute type {} not implemented", static_cast<u32>(p->type)));
        }
    }
    if (g.index_register >= 0) m.primary[g.index_register] = index;
    if (g.instance_register >= 0) m.primary[g.instance_register] = 0;
    if (auto r = usse::run(d.vertex.primary, ProgramKind::Primary, m); !r) return failure("vertex program: " + r.error().message);
    const usse::VertexOutputLayout l = usse::vertex_output_layout(g);
    auto o = [&](i32 r) { return std::bit_cast<f32>(m.output[r]); };
    if (l.position >= 0)
        for (u32 k = 0; k < 4; ++k) out.position[k] = o(l.position + static_cast<i32>(k));
    auto varying = [&](u32 bit, i32 reg, u32 n) {
        if (reg < 0) return;
        std::array<f32, 4> v{0, 0, 0, 1};
        for (u32 k = 0; k < n && k < 4; ++k) v[k] = o(reg + static_cast<i32>(k));
        out.varyings.push_back({bit, v});
    };
    varying(usse::varying::kColor0, l.color[0], 4);
    varying(usse::varying::kColor1, l.color[1], 4);
    varying(usse::varying::kFog, l.fog, 2);
    for (u32 k = 0; k < 10; ++k) varying(usse::varying::kTexCoord0 << k, l.texcoord[k], l.texcoord_registers[k]);
    return {};
}

// The fragment program's colour: its output register's lanes in the type it declares (as the hardware converts it
// to the output format; the attachment then quantizes), lanes it does not have 0, 0, 0, 1.
void output_color(const usse::Gxp& g, const Machine& m, f32 rgba[4]) {
    const u32* bank = g.native_color ? m.output : m.primary;
    f32 lane[4] = {0, 0, 0, 1};
    auto half = [&](u32 w, u32 k) { return usse::from_half(static_cast<u16>(k ? w >> 16 : w)); };
    switch (g.output_type) {
        case usse::ParameterType::F32:
            lane[0] = std::bit_cast<f32>(bank[0]);
            if (g.output_components > 1) lane[1] = std::bit_cast<f32>(bank[1]);
            break;
        case usse::ParameterType::U8:
            for (u32 k = 0; k < 4; ++k) lane[k] = static_cast<f32>((bank[0] >> (8 * k)) & 0xFF) / 255.0f;
            break;
        case usse::ParameterType::S8:
            for (u32 k = 0; k < 4; ++k)
                lane[k] = nx::max(static_cast<f32>(static_cast<i8>(bank[0] >> (8 * k))) / 127.0f, -1.0f);
            break;
        case usse::ParameterType::U16:
            for (u32 k = 0; k < 2; ++k) lane[k] = static_cast<f32>((bank[0] >> (16 * k)) & 0xFFFF) / 65535.0f;
            break;
        case usse::ParameterType::S16:
            for (u32 k = 0; k < 2; ++k)
                lane[k] = nx::max(static_cast<f32>(static_cast<i16>(bank[0] >> (16 * k))) / 32767.0f, -1.0f);
            break;
        default:  // F16
            lane[0] = half(bank[0], 0), lane[1] = half(bank[0], 1);
            if (g.output_components > 2) lane[2] = half(bank[1], 0), lane[3] = half(bank[1], 1);
            break;
    }
    for (u32 k = 0; k < 4; ++k) rgba[k] = lane[k];
}

u8 unorm8(f32 v) { return static_cast<u8>(std::lround(nx::clamp(std::isnan(v) ? 0.0f : v, 0.0f, 1.0f) * 255.0f)); }

}  // namespace

void Image::reset(u32 w, u32 h, u32 clear_rgba, f32 clear_depth) {
    width = w, height = h;
    rgba.assign(usize{w} * h, clear_rgba);
    depth.assign(usize{w} * h, clear_depth);
    edge.assign(usize{w} * h, 0);
}

Result<void> render(const Draw& d, Image& t) {
    if (!d.vertex.gxp || !d.fragment.gxp) return failure("a draw without programs");
    if (d.indices.size() % 3) return failure("indices of a triangle list");
    Memory memory;
    nx::vector<usse::SamplerState> samplers(nx::max<usize>(d.textures.size(), 1));
    for (u32 k = 0; k < samplers.size(); ++k) samplers[k].words[0] = k;
    Textures textures;
    textures.units = d.textures;

    Machine vsetup, fsetup;
    if (auto r = run_secondary(d.vertex, d.vertex_uniforms, d.vertex_buffers, 0x01000000u, memory, {}, vsetup); !r) return r;
    fsetup.textures = &textures;
    if (auto r = run_secondary(d.fragment, d.fragment_uniforms, d.fragment_buffers, 0x02000000u, memory, samplers, fsetup); !r)
        return r;

    // Vertices (each index once), then the default viewport: the whole target, y down, z from [-1, 1] to [0, 1].
    nx::vector<Vertex> vertices;
    nx::vector<i64> slot_of;
    u32 max_index = 0;
    for (u32 i : d.indices) max_index = nx::max(max_index, i);
    slot_of.assign(usize{max_index} + 1, -1);
    struct Screen {
        f64 x, y, z, inv_w;
    };
    nx::vector<Screen> screen;
    for (u32 i : d.indices) {
        if (slot_of[i] >= 0) continue;
        Vertex v;
        if (auto r = run_vertex(d, vsetup, i, v); !r) return r;
        if (!(v.position[3] > 0.0f))
            return failure(nx::format("a vertex behind the eye (no clipping): vertex {} at ({}, {}, {}, {})", i, v.position[0],
                                      v.position[1], v.position[2], v.position[3]));
        const f64 inv_w = 1.0 / v.position[3];
        screen.push_back(Screen{(v.position[0] * inv_w + 1.0) * 0.5 * t.width, (1.0 - v.position[1] * inv_w) * 0.5 * t.height,
                                0.5 * v.position[2] * inv_w + 0.5, inv_w});
        slot_of[i] = static_cast<i64>(vertices.size());
        vertices.push_back(std::move(v));
    }

    for (usize tri = 0; tri + 2 < d.indices.size(); tri += 3) {
        const usize s[3] = {static_cast<usize>(slot_of[d.indices[tri]]), static_cast<usize>(slot_of[d.indices[tri + 1]]),
                            static_cast<usize>(slot_of[d.indices[tri + 2]])};
        const Screen& a = screen[s[0]];
        const Screen& b = screen[s[1]];
        const Screen& c = screen[s[2]];
        f64 area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (area == 0) continue;
        const Screen* p[3] = {&a, &b, &c};
        // (edge k is opposite vertex k; its function is positive inside, whichever the winding)
        auto edge = [&](u32 k, f64 x, f64 y) {
            const Screen& e0 = *p[(k + 1) % 3];
            const Screen& e1 = *p[(k + 2) % 3];
            const f64 v = (e1.x - e0.x) * (y - e0.y) - (e1.y - e0.y) * (x - e0.x);
            return area > 0 ? v : -v;
        };
        auto edge_length = [&](u32 k) {
            const Screen& e0 = *p[(k + 1) % 3];
            const Screen& e1 = *p[(k + 2) % 3];
            return std::hypot(e1.x - e0.x, e1.y - e0.y);
        };
        // (top-left rule, y down: a top edge is horizontal with the inside below; a left edge has the inside to its
        // right)
        auto top_left = [&](u32 k) {
            const Screen& e0 = *p[(k + 1) % 3];
            const Screen& e1 = *p[(k + 2) % 3];
            f64 dx = e1.x - e0.x, dy = e1.y - e0.y;
            if (area < 0) dx = -dx, dy = -dy;
            return (dy == 0 && dx < 0) || dy < 0;
        };
        const f64 abs_area = std::fabs(area);
        const i64 x0 = nx::max<i64>(0, static_cast<i64>(std::floor(nx::min(a.x, nx::min(b.x, c.x)))) - 1);
        const i64 x1 = nx::min<i64>(t.width - 1, static_cast<i64>(std::ceil(nx::max(a.x, nx::max(b.x, c.x)))) + 1);
        const i64 y0 = nx::max<i64>(0, static_cast<i64>(std::floor(nx::min(a.y, nx::min(b.y, c.y)))) - 1);
        const i64 y1 = nx::min<i64>(t.height - 1, static_cast<i64>(std::ceil(nx::max(a.y, nx::max(b.y, c.y)))) + 1);
        for (i64 y = y0; y <= y1; ++y) {
            for (i64 x = x0; x <= x1; ++x) {
                const f64 px = static_cast<f64>(x) + 0.5, py = static_cast<f64>(y) + 0.5;
                f64 e[3];
                bool inside = true, near_edge = false;
                for (u32 k = 0; k < 3; ++k) {
                    e[k] = edge(k, px, py);
                    if (std::fabs(e[k]) / edge_length(k) < 1.0 / 64) near_edge = true;
                    if (e[k] < 0 || (e[k] == 0 && !top_left(k))) inside = false;
                }
                const usize at = static_cast<usize>(y) * t.width + static_cast<usize>(x);
                if (near_edge) t.edge[at] = 1;
                if (!inside) continue;
                // Barycentric weights (screen space), perspective-corrected for the varyings.
                const f64 l[3] = {e[0] / abs_area, e[1] / abs_area, e[2] / abs_area};
                const f64 z = l[0] * a.z + l[1] * b.z + l[2] * c.z;
                if (d.depth_test && !(z <= t.depth[at])) continue;
                const f64 pw[3] = {l[0] * a.inv_w, l[1] * b.inv_w, l[2] * c.inv_w};
                const f64 sum = pw[0] + pw[1] + pw[2];
                nx::vector<usse::VaryingValue> varyings;
                // (every vertex has the same varyings, in the same order)
                for (usize i = 0; i < vertices[s[0]].varyings.size(); ++i) {
                    usse::VaryingValue v;
                    v.varying = vertices[s[0]].varyings[i].first;
                    for (u32 k = 0; k < 4; ++k) {
                        f64 acc = 0;
                        for (u32 j = 0; j < 3; ++j) acc += pw[j] * vertices[s[j]].varyings[i].second[k];
                        v.value[k] = static_cast<f32>(acc / sum);
                    }
                    varyings.push_back(v);
                }
                usse::VaryingValue position;
                position.varying = usse::varying::kPosition;
                position.value[0] = static_cast<f32>(px), position.value[1] = static_cast<f32>(py);
                position.value[2] = static_cast<f32>(z), position.value[3] = static_cast<f32>(sum);
                varyings.push_back(position);

                Machine m = fsetup;
                if (auto r = usse::load_fragment_inputs(*d.fragment.gxp, varyings, samplers, m); !r) return r;
                if (auto r = usse::run(d.fragment.primary, ProgramKind::Primary, m); !r)
                    return failure("fragment program: " + r.error().message);
                if (m.killed) continue;
                f32 rgba[4];
                output_color(*d.fragment.gxp, m, rgba);
                if (d.blend) {
                    const u32 dst = t.rgba[at];
                    const f32 alpha = nx::clamp(rgba[3], 0.0f, 1.0f);
                    for (u32 k = 0; k < 4; ++k) {
                        const f32 dv = static_cast<f32>((dst >> (8 * k)) & 0xFF) / 255.0f;
                        rgba[k] = rgba[k] * alpha + dv * (1.0f - alpha);
                    }
                }
                t.rgba[at] = u32{unorm8(rgba[0])} | u32{unorm8(rgba[1])} << 8 | u32{unorm8(rgba[2])} << 16 |
                             u32{unorm8(rgba[3])} << 24;
                if (d.depth_test) t.depth[at] = static_cast<f32>(z);
            }
        }
    }
    return {};
}

}  // namespace psvrecore::gxm::reference
