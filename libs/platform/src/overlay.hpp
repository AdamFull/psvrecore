#pragma once

// The system UI's overlay (system_ui.hpp): a Dear ImGui context, the host's events into it (from the thread pumping
// them), and its drawing (on the presenting thread) in the window's render pass, after the picture.
//
// The drawing is the overlay's own: a pipeline whose shaders are made with gxm's SPIR-V builder, vertex and index
// buffers per frame in flight, and ImGui's textures (the font atlas, its glyphs baked as text needs them) made and
// updated by copies recorded in the frame's command buffer before the pass (ImGuiBackendFlags_RendererHasTextures):
// nothing is submitted apart from the window's own frames.

#include "psvrecore/error.hpp"
#include "psvrecore/platform/system_ui.hpp"
#include "psvrecore/vulkan/device.hpp"
#include "psvrecore/vulkan/resources.hpp"
#include "core/foundation/threading/sync.h"

union SDL_Event;
struct ImGuiContext;
struct ImDrawData;
struct ImTextureData;

namespace psvrecore::platform {

class Overlay {
public:
    Overlay(vulkan::Device& device, u32 frames_in_flight);
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    void set_ui(SystemUi* ui);
    SystemUi* ui() const { return ui_; }

    // From the thread pumping events. The picture's rectangle in the window's coordinates (for the mouse and
    // touches); an event for the screens (while they take the controls); the controller's state as the game would
    // have it (SCE_CTRL_* bits), whenever it changes; the screens' controls taken or given back.
    void set_view(f32 window_w, f32 window_h, f32 x, f32 y, f32 w, f32 h);
    void event(const SDL_Event& e, bool text);
    void pad(u32 buttons, u8 lx, u8 ly);
    void captured(bool on, u32 held);

    // On the presenting thread, for a frame into a target of `extent` whose picture fills `view` (pixels): builds
    // the screens (false: nothing to draw), records the texture copies (before the render pass), then the drawing
    // (in it, made for `pass`).
    bool frame(VkExtent2D extent, f32 view_x, f32 view_y, f32 view_w, f32 view_h);
    void upload(VkCommandBuffer cmd, u32 slot);
    void draw(VkCommandBuffer cmd, u32 slot, VkRenderPass pass);

private:
    struct Texture;
    struct Slot {
        vulkan::Buffer vertices, indices, staging;
        nx::vector<Texture*> retired;  // (destroyed once this slot's frame comes round again)
    };
    Result<void> make_layout();
    Result<void> make_pipeline(VkRenderPass pass);
    void destroy_pipeline();
    void update_texture(VkCommandBuffer cmd, Slot& slot, ImTextureData* tex, usize* staged);
    void destroy_texture(Texture* t);

    vulkan::Device& device_;
    SystemUi* ui_ = nullptr;
    ImGuiContext* context_ = nullptr;
    nx::mutex lock_;  // (the context: events in, frames out)
    nx::vector<Slot> slots_;
    VkRenderPass pass_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    bool broken_ = false;
    // The frame being drawn (presenting thread).
    ImDrawData* draw_data_ = nullptr;
    VkExtent2D extent_{};
    f32 offset_x_ = 0, offset_y_ = 0, scale_ = 1;
    u64 last_ns_ = 0;
    // The controls (under lock_): held, pressed since the last frame, and held when the screens took them (ignored
    // until released).
    u32 held_ = 0, pressed_ = 0, ignored_ = 0;
    bool capturing_ = false;
    f32 window_w_ = 960, window_h_ = 544, view_x_ = 0, view_y_ = 0, view_w_ = 960, view_h_ = 544;
};

}  // namespace psvrecore::platform
