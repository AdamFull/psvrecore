#include "overlay.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "imgui.h"
#include "psvrecore/gxm/spirv.hpp"
#include "psvrecore/log.hpp"
#include "psvrecore/platform/window.hpp"
#include "psvrecore/vulkan/pipelines.hpp"
#include "spirv/unified1/spirv.h"
#include "core/foundation/platform/os.h"

namespace psvrecore::runtime::hle {
// The system fonts (generated/fonts/*.cpp in the runtime, SIL OFL 1.1): the screens' fonts too.
extern const u8 kRoboto_Regular[];
extern const usize kRoboto_RegularSize;
extern const u8 kRoboto_Bold[];
extern const usize kRoboto_BoldSize;
extern const u8 kNotoSansJP_Regular[];
extern const usize kNotoSansJP_RegularSize;
extern const u8 kNotoSansKR_Regular[];
extern const usize kNotoSansKR_RegularSize;
extern const u8 kNotoSansSC_Regular[];
extern const usize kNotoSansSC_RegularSize;
extern const u8 kNotoSansTC_Regular[];
extern const usize kNotoSansTC_RegularSize;
}  // namespace psvrecore::runtime::hle

namespace psvrecore::platform {

namespace {

using gxm::spirv::Builder;
using gxm::spirv::Id;

// SCE_CTRL_* bits.
constexpr u32 kUp = 1u << 4, kRight = 1u << 5, kDown = 1u << 6, kLeft = 1u << 7, kCircle = 1u << 13, kCross = 1u << 14;
constexpr u32 kMaxSets = 64;
constexpr f32 kFontSize = 22.0f;  // (screen units: the console's dialogs' body text)

struct Constants {
    f32 scale[2];
    f32 translate[2];
};

// The push constants: scale, then translation (positions to normalized device coordinates).
Id constants(Builder& b, Id v2) {
    const Id block = b.t_struct({v2, v2});
    b.decorate(block, SpvDecorationBlock);
    b.member_decorate(block, 0, SpvDecorationOffset, {0});
    b.member_decorate(block, 1, SpvDecorationOffset, {8});
    return b.variable(b.t_pointer(SpvStorageClassPushConstant, block), SpvStorageClassPushConstant);
}

nx::vector<u32> vertex_shader() {
    Builder b;
    b.capability(SpvCapabilityShader);
    const Id f = b.t_float(), v2 = b.t_vector(f, 2), v4 = b.t_vector(f, 4);
    const Id pc = constants(b, v2);
    auto input = [&](Id type, u32 location) {
        const Id v = b.variable(b.t_pointer(SpvStorageClassInput, type), SpvStorageClassInput);
        b.decorate(v, SpvDecorationLocation, {location});
        return v;
    };
    auto output = [&](Id type, u32 location) {
        const Id v = b.variable(b.t_pointer(SpvStorageClassOutput, type), SpvStorageClassOutput);
        b.decorate(v, SpvDecorationLocation, {location});
        return v;
    };
    const Id pos = input(v2, 0), uv = input(v2, 1), color = input(v4, 2);
    const Id color_out = output(v4, 0), uv_out = output(v2, 1);
    const Id position = b.variable(b.t_pointer(SpvStorageClassOutput, v4), SpvStorageClassOutput);
    b.decorate(position, SpvDecorationBuiltIn, {SpvBuiltInPosition});

    const Id fn = b.begin_function(b.t_void(), b.t_function(b.t_void()));
    b.block(b.new_label());
    auto member = [&](i32 i) {
        return b.op(SpvOpLoad, v2, {b.op(SpvOpAccessChain, b.t_pointer(SpvStorageClassPushConstant, v2), {pc, b.c_int(i)})});
    };
    const Id xy = b.op(SpvOpFAdd, v2, {b.op(SpvOpFMul, v2, {b.op(SpvOpLoad, v2, {pos}), member(0)}), member(1)});
    b.op0(SpvOpStore, {position, b.op(SpvOpCompositeConstruct, v4, {xy, b.c_float(0.0f), b.c_float(1.0f)})});
    b.op0(SpvOpStore, {color_out, b.op(SpvOpLoad, v4, {color})});
    b.op0(SpvOpStore, {uv_out, b.op(SpvOpLoad, v2, {uv})});
    b.op0(SpvOpReturn, {});
    b.end_function();
    const Id interface[] = {pos, uv, color, color_out, uv_out, position};
    b.entry_point(SpvExecutionModelVertex, fn, "main", interface);
    return b.module();
}

nx::vector<u32> fragment_shader() {
    Builder b;
    b.capability(SpvCapabilityShader);
    const Id f = b.t_float(), v2 = b.t_vector(f, 2), v4 = b.t_vector(f, 4);
    const Id sampled = b.t_sampled_image(b.t_image(f, SpvDim2D, 0, 0, 0, 1, SpvImageFormatUnknown));
    const Id texture = b.variable(b.t_pointer(SpvStorageClassUniformConstant, sampled), SpvStorageClassUniformConstant);
    b.decorate(texture, SpvDecorationDescriptorSet, {0});
    b.decorate(texture, SpvDecorationBinding, {0});
    const Id color = b.variable(b.t_pointer(SpvStorageClassInput, v4), SpvStorageClassInput);
    b.decorate(color, SpvDecorationLocation, {0});
    const Id uv = b.variable(b.t_pointer(SpvStorageClassInput, v2), SpvStorageClassInput);
    b.decorate(uv, SpvDecorationLocation, {1});
    const Id out = b.variable(b.t_pointer(SpvStorageClassOutput, v4), SpvStorageClassOutput);
    b.decorate(out, SpvDecorationLocation, {0});

    const Id fn = b.begin_function(b.t_void(), b.t_function(b.t_void()));
    b.block(b.new_label());
    const Id texel = b.op(SpvOpImageSampleImplicitLod, v4, {b.op(SpvOpLoad, sampled, {texture}), b.op(SpvOpLoad, v2, {uv})});
    b.op0(SpvOpStore, {out, b.op(SpvOpFMul, v4, {texel, b.op(SpvOpLoad, v4, {color})})});
    b.op0(SpvOpReturn, {});
    b.end_function();
    const Id interface[] = {color, uv, out};
    b.entry_point(SpvExecutionModelFragment, fn, "main", interface);
    b.execution_mode(fn, SpvExecutionModeOriginUpperLeft);
    return b.module();
}

// The keyboard's keys a text box uses (while the screens take typed text).
ImGuiKey text_key(SDL_Keycode k) {
    switch (k) {
        case SDLK_BACKSPACE: return ImGuiKey_Backspace;
        case SDLK_DELETE: return ImGuiKey_Delete;
        case SDLK_LEFT: return ImGuiKey_LeftArrow;
        case SDLK_RIGHT: return ImGuiKey_RightArrow;
        case SDLK_UP: return ImGuiKey_UpArrow;
        case SDLK_DOWN: return ImGuiKey_DownArrow;
        case SDLK_HOME: return ImGuiKey_Home;
        case SDLK_END: return ImGuiKey_End;
        case SDLK_RETURN: return ImGuiKey_Enter;
        case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
        case SDLK_ESCAPE: return ImGuiKey_Escape;
        case SDLK_TAB: return ImGuiKey_Tab;
        case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
        case SDLK_RCTRL: return ImGuiKey_RightCtrl;
        case SDLK_LSHIFT: return ImGuiKey_LeftShift;
        case SDLK_RSHIFT: return ImGuiKey_RightShift;
        default: return ImGuiKey_None;
    }
}

u32 swap_enter(u32 buttons, bool circle) {
    if (!circle) return buttons;
    const u32 rest = buttons & ~(kCross | kCircle);
    return rest | (buttons & kCross ? kCircle : 0) | (buttons & kCircle ? kCross : 0);
}

}  // namespace

struct Overlay::Texture {
    vulkan::Image image;
    VkDescriptorSet set = VK_NULL_HANDLE;
};

Overlay::Overlay(vulkan::Device& device, u32 frames_in_flight) : device_(device), slots_(frames_in_flight) {
    IMGUI_CHECKVERSION();
    ImGuiContext* previous = ImGui::GetCurrentContext();
    context_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(context_);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.BackendRendererName = "psvrecore";
    io.BackendPlatformName = "psvrecore";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset |
                       ImGuiBackendFlags_HasGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigNavCursorVisibleAlways = true;  // (the focus shows from the start, as on the console)
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.MouseDrawCursor = false;
    io.DisplaySize = ImVec2(static_cast<f32>(kScreenWidth), static_cast<f32>(kScreenHeight));
    // The fonts: Roboto, then Noto Sans JP, KR, SC and TC merged in for the characters it lacks; the same in bold
    // (Roboto Bold, the CJK fonts having no bold). Baked as text needs them, at the window's resolution.
    io.Fonts->TexDesiredFormat = ImTextureFormat_RGBA32;  // (the shader multiplies the vertex colour by RGBA texels)
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    using namespace runtime::hle;
    struct Data {
        const u8* bytes;
        usize size;
    };
    const Data cjk[] = {{kNotoSansJP_Regular, kNotoSansJP_RegularSize},
                        {kNotoSansKR_Regular, kNotoSansKR_RegularSize},
                        {kNotoSansSC_Regular, kNotoSansSC_RegularSize},
                        {kNotoSansTC_Regular, kNotoSansTC_RegularSize}};
    for (const Data main : {Data{kRoboto_Regular, kRoboto_RegularSize}, Data{kRoboto_Bold, kRoboto_BoldSize}}) {
        cfg.MergeMode = false;
        io.Fonts->AddFontFromMemoryTTF(const_cast<u8*>(main.bytes), static_cast<int>(main.size), kFontSize, &cfg);
        cfg.MergeMode = true;
        for (const Data& d : cjk)
            io.Fonts->AddFontFromMemoryTTF(const_cast<u8*>(d.bytes), static_cast<int>(d.size), kFontSize, &cfg);
    }
    ImGui::SetCurrentContext(previous);
}

Overlay::~Overlay() {
    const VkDevice d = device_.device();
    device_.wait_idle();
    {
        ImGuiContext* previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(context_);
        for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
            if (auto* t = static_cast<Texture*>(tex->BackendUserData)) {
                destroy_texture(t);
                tex->BackendUserData = nullptr;
                tex->SetTexID(ImTextureID_Invalid);
                tex->SetStatus(ImTextureStatus_Destroyed);
            }
        }
        ImGui::DestroyContext(context_);
        ImGui::SetCurrentContext(previous == context_ ? nullptr : previous);
    }
    for (Slot& s : slots_) {
        for (Texture* t : s.retired) destroy_texture(t);
        vulkan::destroy_buffer(device_, s.vertices);
        vulkan::destroy_buffer(device_, s.indices);
        vulkan::destroy_buffer(device_, s.staging);
    }
    destroy_pipeline();
    if (pool_) vkDestroyDescriptorPool(d, pool_, nullptr);
    if (layout_) vkDestroyPipelineLayout(d, layout_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(d, set_layout_, nullptr);
    if (sampler_) vkDestroySampler(d, sampler_, nullptr);
}

void Overlay::set_ui(SystemUi* ui) {
    nx::scoped_lock<nx::mutex> held(lock_);
    ui_ = ui;
}

void Overlay::set_view(f32 window_w, f32 window_h, f32 x, f32 y, f32 w, f32 h) {
    nx::scoped_lock<nx::mutex> held(lock_);
    window_w_ = window_w, window_h_ = window_h;
    view_x_ = x, view_y_ = y, view_w_ = w, view_h_ = h;
}

void Overlay::event(const SDL_Event& e, bool text) {
    nx::scoped_lock<nx::mutex> held(lock_);
    ImGui::SetCurrentContext(context_);
    ImGuiIO& io = ImGui::GetIO();
    // (a point of the window in the screen's units)
    auto at = [&](f32 x, f32 y) {
        const f32 sx = view_w_ > 0 ? static_cast<f32>(kScreenWidth) / view_w_ : 1.0f;
        const f32 sy = view_h_ > 0 ? static_cast<f32>(kScreenHeight) / view_h_ : 1.0f;
        return ImVec2((x - view_x_) * sx, (y - view_y_) * sy);
    };
    switch (e.type) {
        case SDL_EVENT_MOUSE_MOTION:
            if (e.motion.which == SDL_TOUCH_MOUSEID) break;  // (a touch screen's come as fingers)
            io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
            io.AddMousePosEvent(at(e.motion.x, e.motion.y).x, at(e.motion.x, e.motion.y).y);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            if (e.button.which == SDL_TOUCH_MOUSEID) break;
            const int button = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : -1;
            if (button < 0) break;
            io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
            const ImVec2 p = at(e.button.x, e.button.y);
            io.AddMousePosEvent(p.x, p.y);
            io.AddMouseButtonEvent(button, e.button.down);
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
            io.AddMouseWheelEvent(e.wheel.x, e.wheel.y);
            break;
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED: {
            const ImVec2 p = at(e.tfinger.x * window_w_, e.tfinger.y * window_h_);
            io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
            io.AddMousePosEvent(p.x, p.y);
            if (e.type == SDL_EVENT_FINGER_DOWN) io.AddMouseButtonEvent(0, true);
            else if (e.type != SDL_EVENT_FINGER_MOTION) io.AddMouseButtonEvent(0, false);
            break;
        }
        case SDL_EVENT_TEXT_INPUT:
            if (text) io.AddInputCharactersUTF8(e.text.text);
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            if (!text) break;
            if (const ImGuiKey k = text_key(e.key.key); k != ImGuiKey_None) io.AddKeyEvent(k, e.key.down);
            io.AddKeyEvent(ImGuiMod_Ctrl, (e.key.mod & SDL_KMOD_CTRL) != 0);
            io.AddKeyEvent(ImGuiMod_Shift, (e.key.mod & SDL_KMOD_SHIFT) != 0);
            break;
        default: break;
    }
}

void Overlay::pad(u32 buttons, u8 lx, u8 ly) {
    nx::scoped_lock<nx::mutex> held(lock_);
    if (!capturing_) return;
    ImGui::SetCurrentContext(context_);
    const bool circle = ui_ && ui_->enter_is_circle();
    buttons = swap_enter(buttons, circle);
    ignored_ &= buttons;  // (released: counted again)
    const u32 down = buttons & ~ignored_;
    pressed_ |= down & ~held_;
    held_ = down;
    // (ImGui's own focus moves: the d-pad and the left stick, enter, cancel)
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, (down & kUp) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, (down & kDown) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, (down & kLeft) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (down & kRight) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, (down & kCross) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, (down & kCircle) != 0);
    auto axis = [](u8 v, bool negative) {
        const f32 a = (static_cast<f32>(v) - 128.0f) / 127.0f * (negative ? -1.0f : 1.0f);
        return a < 0.5f ? 0.0f : std::min((a - 0.5f) / 0.5f, 1.0f);  // (a wide dead zone: the stick moves the focus)
    };
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, axis(lx, true) > 0, axis(lx, true));
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, axis(lx, false) > 0, axis(lx, false));
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, axis(ly, true) > 0, axis(ly, true));
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, axis(ly, false) > 0, axis(ly, false));
}

void Overlay::captured(bool on, u32 held) {
    nx::scoped_lock<nx::mutex> lock(lock_);
    if (on == capturing_) return;
    ImGui::SetCurrentContext(context_);
    capturing_ = on;
    held_ = pressed_ = 0;
    ignored_ = on ? swap_enter(held, ui_ && ui_->enter_is_circle()) : 0;
    ImGuiIO& io = ImGui::GetIO();
    io.ClearInputKeys();
    io.ClearInputMouse();
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
}

bool Overlay::frame(VkExtent2D extent, f32 view_x, f32 view_y, f32 view_w, f32 view_h) {
    draw_data_ = nullptr;
    if (broken_) return false;
    nx::scoped_lock<nx::mutex> held(lock_);
    if (!ui_ || !ui_->visible()) {
        last_ns_ = 0;
        return false;
    }
    ImGui::SetCurrentContext(context_);
    ImGuiIO& io = ImGui::GetIO();
    const u64 now = nx::os::ticks_ns();
    io.DeltaTime = last_ns_ ? std::clamp(static_cast<f32>(now - last_ns_) / 1e9f, 1e-4f, 0.1f) : 1.0f / 60.0f;
    last_ns_ = now;
    scale_ = view_w > 0 ? view_w / static_cast<f32>(kScreenWidth) : 1.0f;
    io.DisplaySize = ImVec2(static_cast<f32>(kScreenWidth), static_cast<f32>(kScreenHeight));
    io.DisplayFramebufferScale = ImVec2(scale_, scale_);
    extent_ = extent;
    offset_x_ = view_x, offset_y_ = view_y;
    (void)view_h;
    const SystemUi::Controls controls{held_, pressed_};
    pressed_ = 0;
    ImGui::NewFrame();
    ui_->build(controls);
    ImGui::Render();
    draw_data_ = ImGui::GetDrawData();
    return draw_data_ && (draw_data_->TotalVtxCount > 0 || (draw_data_->Textures && !draw_data_->Textures->empty()));
}

void Overlay::destroy_texture(Texture* t) {
    if (!t) return;
    vulkan::destroy_image(device_, t->image);
    if (t->set && pool_) vkFreeDescriptorSets(device_.device(), pool_, 1, &t->set);
    delete t;
}

Result<void> Overlay::make_layout() {
    const VkDevice d = device_.device();
    {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.maxLod = 0.0f;
        if (VkResult r = vkCreateSampler(d, &si, nullptr, &sampler_); r != VK_SUCCESS)
            return nx::err(vulkan::vk_error("vkCreateSampler", r));
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                             nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1;
        li.pBindings = &binding;
        if (VkResult r = vkCreateDescriptorSetLayout(d, &li, nullptr, &set_layout_); r != VK_SUCCESS)
            return nx::err(vulkan::vk_error("vkCreateDescriptorSetLayout", r));
        const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Constants)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &set_layout_;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &push;
        if (VkResult r = vkCreatePipelineLayout(d, &pli, nullptr, &layout_); r != VK_SUCCESS)
            return nx::err(vulkan::vk_error("vkCreatePipelineLayout", r));
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxSets};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pi.maxSets = kMaxSets;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &size;
        if (VkResult r = vkCreateDescriptorPool(d, &pi, nullptr, &pool_); r != VK_SUCCESS)
            return nx::err(vulkan::vk_error("vkCreateDescriptorPool", r));
    }
    return {};
}

Result<void> Overlay::make_pipeline(VkRenderPass pass) {
    const VkDevice d = device_.device();
    if (!layout_) {
        if (auto r = make_layout(); !r) return r;
    }
    destroy_pipeline();
    const nx::vector<u32> vs_words = vertex_shader(), fs_words = fragment_shader();
    auto vs = vulkan::create_shader(device_, vs_words);
    if (!vs) return nx::err(vs.error());
    auto fs = vulkan::create_shader(device_, fs_words);
    if (!fs) {
        vkDestroyShaderModule(d, *vs, nullptr);
        return nx::err(fs.error());
    }
    VkPipelineShaderStageCreateInfo stages[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                              {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = *vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = *fs;
    stages[1].pName = "main";
    const VkVertexInputBindingDescription binding{0, sizeof(ImDrawVert), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[3] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<u32>(offsetof(ImDrawVert, pos))},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<u32>(offsetof(ImDrawVert, uv))},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, static_cast<u32>(offsetof(ImDrawVert, col))}};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dynamic;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds;
    gp.layout = layout_;
    gp.renderPass = pass;
    const VkResult r = vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline_);
    vkDestroyShaderModule(d, *vs, nullptr);
    vkDestroyShaderModule(d, *fs, nullptr);
    if (r != VK_SUCCESS) return nx::err(vulkan::vk_error("vkCreateGraphicsPipelines", r));
    pass_ = pass;
    return {};
}

void Overlay::destroy_pipeline() {
    if (pipeline_) vkDestroyPipeline(device_.device(), pipeline_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    pass_ = VK_NULL_HANDLE;
}

void Overlay::update_texture(VkCommandBuffer cmd, Slot& slot, ImTextureData* tex, usize* staged) {
    auto* t = static_cast<Texture*>(tex->BackendUserData);
    if (tex->Status == ImTextureStatus_WantDestroy) {
        // (unused for as many frames as are in flight: no frame still drawing uses it)
        if (tex->UnusedFrames < static_cast<int>(slots_.size())) return;
        slot.retired.push_back(t);
        tex->BackendUserData = nullptr;
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
        return;
    }
    if (tex->Status == ImTextureStatus_WantCreate) {
        t = new Texture;
        vulkan::ImageDesc desc;
        desc.format = VK_FORMAT_R8G8B8A8_UNORM;  // (RGBA32 only: TexDesiredFormat)
        desc.width = static_cast<u32>(tex->Width), desc.height = static_cast<u32>(tex->Height);
        auto image = vulkan::create_image(device_, desc);
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout_;
        if (!image || vkAllocateDescriptorSets(device_.device(), &ai, &t->set) != VK_SUCCESS) {
            nx::loge(logs::display, "system UI: a texture of {} x {} could not be made", tex->Width, tex->Height);
            if (image) t->image = *image;
            destroy_texture(t);
            broken_ = true;
            return;
        }
        t->image = *image;
        const VkDescriptorImageInfo info{sampler_, t->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = t->set;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &info;
        vkUpdateDescriptorSets(device_.device(), 1, &w, 0, nullptr);
        tex->BackendUserData = t;
        tex->SetTexID(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(t)));
    }
    if (!t) return;
    // The rectangles to copy: the whole texture when new, else its updates.
    nx::vector<ImTextureRect> rects;
    if (tex->Status == ImTextureStatus_WantCreate) {
        rects.push_back(ImTextureRect{0, 0, static_cast<unsigned short>(tex->Width), static_cast<unsigned short>(tex->Height)});
    } else {
        for (const ImTextureRect& r : tex->Updates) rects.push_back(r);
        if (rects.empty()) rects.push_back(tex->UpdateRect);
    }
    const usize bpp = static_cast<usize>(tex->BytesPerPixel);
    nx::vector<VkBufferImageCopy> copies;
    for (const ImTextureRect& r : rects) {
        if (!r.w || !r.h) continue;
        const usize bytes = usize{r.w} * r.h * bpp;
        u8* out = static_cast<u8*>(slot.staging.mapped) + *staged;
        for (u32 y = 0; y < r.h; ++y)
            std::memcpy(out + y * r.w * bpp, tex->GetPixelsAt(r.x, r.y + static_cast<int>(y)), r.w * bpp);
        VkBufferImageCopy c{};
        c.bufferOffset = *staged;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageOffset = {r.x, r.y, 0};
        c.imageExtent = {r.w, r.h, 1};
        copies.push_back(c);
        *staged += (bytes + 15) & ~usize{15};
    }
    if (!copies.empty()) {
        vulkan::transition(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(cmd, slot.staging.buffer, t->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<u32>(copies.size()), copies.data());
        vulkan::transition(cmd, t->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    tex->SetStatus(ImTextureStatus_OK);
}

void Overlay::upload(VkCommandBuffer cmd, u32 index) {
    Slot& slot = slots_[index];
    for (Texture* t : slot.retired) destroy_texture(t);  // (this slot's last frame is done)
    slot.retired.clear();
    if (!draw_data_ || !draw_data_->Textures || broken_) return;
    if (!layout_) {
        if (auto r = make_layout(); !r) {
            nx::loge(logs::display, "system UI: {}", r.error().message);
            broken_ = true;
            return;
        }
    }
    // (the bytes this frame copies, so the staging buffer is large enough first)
    usize need = 0;
    for (ImTextureData* tex : *draw_data_->Textures) {
        if (tex->Status == ImTextureStatus_WantCreate) need += usize{static_cast<usize>(tex->Width)} * tex->Height * tex->BytesPerPixel + 16;
        else if (tex->Status == ImTextureStatus_WantUpdates) {
            for (const ImTextureRect& r : tex->Updates) need += usize{r.w} * r.h * tex->BytesPerPixel + 16;
            need += usize{tex->UpdateRect.w} * tex->UpdateRect.h * tex->BytesPerPixel + 16;
        }
    }
    if (need > slot.staging.size) {
        vulkan::destroy_buffer(device_, slot.staging);
        auto b = vulkan::create_host_buffer(device_, std::max<usize>(need, 1u << 20), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        if (!b) {
            nx::loge(logs::display, "system UI: {}", b.error().message);
            broken_ = true;
            return;
        }
        slot.staging = *b;
    }
    usize staged = 0;
    for (ImTextureData* tex : *draw_data_->Textures)
        if (tex->Status != ImTextureStatus_OK && tex->Status != ImTextureStatus_Destroyed) update_texture(cmd, slot, tex, &staged);
    if (staged) vulkan::flush(device_, slot.staging);
}

void Overlay::draw(VkCommandBuffer cmd, u32 index, VkRenderPass pass) {
    if (!draw_data_ || broken_) return;
    if (pass != pass_) {
        if (auto r = make_pipeline(pass); !r) {
            nx::loge(logs::display, "system UI: {}", r.error().message);
            broken_ = true;
            return;
        }
    }
    const ImDrawData& dd = *draw_data_;
    if (dd.TotalVtxCount <= 0) return;
    Slot& slot = slots_[index];
    const usize vbytes = usize{static_cast<usize>(dd.TotalVtxCount)} * sizeof(ImDrawVert);
    const usize ibytes = usize{static_cast<usize>(dd.TotalIdxCount)} * sizeof(ImDrawIdx);
    auto ensure = [&](vulkan::Buffer& b, usize bytes, VkBufferUsageFlags usage) {
        if (b.size >= bytes) return true;
        vulkan::destroy_buffer(device_, b);
        auto made = vulkan::create_host_buffer(device_, std::max<usize>(bytes * 2, 64 * 1024), usage);
        if (!made) {
            nx::loge(logs::display, "system UI: {}", made.error().message);
            return false;
        }
        b = *made;
        return true;
    };
    if (!ensure(slot.vertices, vbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
        !ensure(slot.indices, ibytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT))
        return;
    auto* vout = static_cast<ImDrawVert*>(slot.vertices.mapped);
    auto* iout = static_cast<ImDrawIdx*>(slot.indices.mapped);
    for (const ImDrawList* list : dd.CmdLists) {
        std::memcpy(vout, list->VtxBuffer.Data, static_cast<usize>(list->VtxBuffer.Size) * sizeof(ImDrawVert));
        std::memcpy(iout, list->IdxBuffer.Data, static_cast<usize>(list->IdxBuffer.Size) * sizeof(ImDrawIdx));
        vout += list->VtxBuffer.Size;
        iout += list->IdxBuffer.Size;
    }
    vulkan::flush(device_, slot.vertices);
    vulkan::flush(device_, slot.indices);

    const f32 w = static_cast<f32>(extent_.width), h = static_cast<f32>(extent_.height);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    const VkViewport viewport{0, 0, w, h, 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    // (screen units to pixels: times the scale, plus where the picture starts; then to -1..1)
    const Constants c{{2.0f * scale_ / w, 2.0f * scale_ / h}, {-1.0f + 2.0f * offset_x_ / w, -1.0f + 2.0f * offset_y_ / h}};
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof c, &c);
    const VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &slot.vertices.buffer, &zero);
    vkCmdBindIndexBuffer(cmd, slot.indices.buffer, 0, sizeof(ImDrawIdx) == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    u32 vtx_base = 0, idx_base = 0;
    VkDescriptorSet bound = VK_NULL_HANDLE;
    for (const ImDrawList* list : dd.CmdLists) {
        for (const ImDrawCmd& dc : list->CmdBuffer) {
            if (dc.UserCallback) continue;  // (none are used)
            const f32 x0 = std::max((dc.ClipRect.x - dd.DisplayPos.x) * scale_ + offset_x_, 0.0f);
            const f32 y0 = std::max((dc.ClipRect.y - dd.DisplayPos.y) * scale_ + offset_y_, 0.0f);
            const f32 x1 = std::min((dc.ClipRect.z - dd.DisplayPos.x) * scale_ + offset_x_, w);
            const f32 y1 = std::min((dc.ClipRect.w - dd.DisplayPos.y) * scale_ + offset_y_, h);
            if (x1 <= x0 || y1 <= y0) continue;
            const VkRect2D scissor{{static_cast<i32>(x0), static_cast<i32>(y0)},
                                   {static_cast<u32>(x1 - std::floor(x0)), static_cast<u32>(y1 - std::floor(y0))}};
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            auto* t = reinterpret_cast<Texture*>(static_cast<uintptr_t>(dc.GetTexID()));
            if (!t || !t->set) continue;
            if (t->set != bound) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &t->set, 0, nullptr);
                bound = t->set;
            }
            vkCmdDrawIndexed(cmd, dc.ElemCount, 1, dc.IdxOffset + idx_base, static_cast<i32>(dc.VtxOffset + vtx_base), 0);
        }
        vtx_base += static_cast<u32>(list->VtxBuffer.Size);
        idx_base += static_cast<u32>(list->IdxBuffer.Size);
    }
}

}  // namespace psvrecore::platform
