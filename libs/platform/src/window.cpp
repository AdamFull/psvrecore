#include "psvrecore/platform/window.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstring>

#include "psvrecore/log.hpp"
#include "quads.hpp"
#include "psvrecore/runtime/runtime.hpp"
#include "psvrecore/vulkan/device.hpp"
#include "psvrecore/vulkan/resources.hpp"
#include "core/foundation/strings/format.h"
#include "core/foundation/threading/sync.h"
#include "core/foundation/threading/thread.h"

namespace psvrecore::platform {

namespace {

constexpr u32 kFramesInFlight = 2;
constexpr u32 kA2B10G10R10 = 0x50000000u;  // (SCE_DISPLAY_PIXELFORMAT_A2B10G10R10; else A8B8G8R8)

VkSurfaceKHR make_surface(VkInstance instance, void* user) {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(user), instance, nullptr, &surface)) {
        nx::loge(logs::display, "display: no Vulkan surface: {}", SDL_GetError());
        return VK_NULL_HANDLE;
    }
    return surface;
}

struct Rect {
    f32 x = 0, y = 0, w = 0, h = 0;
};

// Where the screen goes in a window of w x h: as large as fits, centred, the rest black.
Rect fit(f32 w, f32 h) {
    const f32 scale = std::min(w / kScreenWidth, h / kScreenHeight);
    const f32 sw = kScreenWidth * scale, sh = kScreenHeight * scale;
    return Rect{(w - sw) * 0.5f, (h - sh) * 0.5f, sw, sh};
}

bool is_srgb(VkFormat f) {
    return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

const char* mode_name(VkPresentModeKHR m) {
    switch (m) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
        case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
        case VK_PRESENT_MODE_FIFO_KHR: return "fifo";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo relaxed";
        default: return "other";
    }
}

}  // namespace

// The presenting thread and what it draws with.
struct Window::Presenter {
    Window& window;
    nx::unique_ptr<vulkan::Device> device;
    bool vsync = true;
    std::atomic<bool> smooth{true};

    // The frame waiting to be presented (the game's thread to this one), and the window's size in pixels (from the
    // thread pumping events).
    nx::mutex lock;
    nx::cond_var wake;
    nx::vector<u8> pending;
    u32 pending_w = 0, pending_h = 0;
    bool has_pending = false, redraw = true, stop = false;
    std::atomic<u32> pixel_w{0}, pixel_h{0};
    // In the background (set_background): nothing is drawn, and there is no surface until the window is back.
    nx::mutex draw_lock;  // (held while drawing)
    std::atomic<bool> background{false};
    bool need_surface = false;  // (under draw_lock)
    nx::thread thread;

    // The presenting thread's.
    nx::vector<u8> current;  // RGBA
    u32 current_w = 0, current_h = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    struct InFlight {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        vulkan::Buffer staging;
        vulkan::Image image;
    };
    InFlight frames[kFramesInFlight];
    u32 next = 0;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    u32 made_w = 0, made_h = 0;  // the window's size the swapchain was made for
    bool out_of_date = false, can_read = false, failed = false;
    nx::vector<VkImage> images;
    nx::vector<VkSemaphore> rendered;  // (one per swapchain image: a present's wait may outlive a frame in flight)

    // The drawing (made with the device), the overlay's quads and its sprites (set from the thread pumping events,
    // under `lock`), and the atlas as an image (the presenting thread's).
    nx::unique_ptr<QuadRenderer> quads;
    nx::vector<Quad> overlay, overlay_drawn;
    nx::vector<u8> atlas_upload;  // (taken from atlas_pixels by the presenting thread)
    u32 atlas_upload_w = 0, atlas_upload_h = 0;
    nx::vector<u8> atlas_pixels;
    u32 atlas_w = 0, atlas_h = 0;
    bool atlas_changed = false;
    vulkan::Image atlas;

    // The last image presented (keep_last).
    nx::mutex readback_lock;
    std::atomic<bool> keep{false};
    vulkan::Buffer readback;
    u32 readback_w = 0, readback_h = 0;
    VkFormat readback_format = VK_FORMAT_UNDEFINED;
    VkFence readback_fence = VK_NULL_HANDLE;

    explicit Presenter(Window& w) : window(w) {}

    ~Presenter() {
        {
            nx::scoped_lock<nx::mutex> held(lock);
            stop = true;
        }
        wake.notify_all();
        if (thread.joinable()) thread.join();
        if (!device) return;
        device->wait_idle();
        const VkDevice d = device->device();
        destroy_swapchain();
        for (InFlight& f : frames) {
            vulkan::destroy_buffer(*device, f.staging);
            vulkan::destroy_image(*device, f.image);
            if (f.fence) vkDestroyFence(d, f.fence, nullptr);
            if (f.acquired) vkDestroySemaphore(d, f.acquired, nullptr);
        }
        vulkan::destroy_buffer(*device, readback);
        vulkan::destroy_image(*device, atlas);
        quads.reset();
        if (pool) vkDestroyCommandPool(d, pool, nullptr);
    }

    Result<void> init() {
        const VkDevice d = device->device();
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = device->queue_family();
        if (VkResult r = vkCreateCommandPool(d, &pi, nullptr, &pool); r != VK_SUCCESS)
            return nx::err(vulkan::vk_error("vkCreateCommandPool", r));
        for (InFlight& f : frames) {
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = pool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            if (VkResult r = vkAllocateCommandBuffers(d, &ai, &f.cmd); r != VK_SUCCESS)
                return nx::err(vulkan::vk_error("vkAllocateCommandBuffers", r));
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (VkResult r = vkCreateFence(d, &fi, nullptr, &f.fence); r != VK_SUCCESS)
                return nx::err(vulkan::vk_error("vkCreateFence", r));
            VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            if (VkResult r = vkCreateSemaphore(d, &si, nullptr, &f.acquired); r != VK_SUCCESS)
                return nx::err(vulkan::vk_error("vkCreateSemaphore", r));
        }
        quads = nx::make_unique<QuadRenderer>(*device);
        thread = nx::thread([this] { run(); });
        return {};
    }

    void request_redraw() {
        {
            nx::scoped_lock<nx::mutex> held(lock);
            redraw = true;
        }
        wake.notify_one();
    }

    void run() {
        nx::this_thread::set_name("psvrecore display");
        while (true) {
            {
                nx::scoped_lock<nx::mutex> held(lock);
                wake.wait(lock, [&] { return has_pending || redraw || stop; });
                if (stop) return;
                if (has_pending) {
                    std::swap(pending, current);
                    current_w = pending_w, current_h = pending_h;
                    has_pending = false;
                }
                overlay_drawn = overlay;
                if (atlas_changed) {
                    atlas_upload = std::move(atlas_pixels);
                    atlas_upload_w = atlas_w, atlas_upload_h = atlas_h;
                    atlas_changed = false;
                }
                redraw = false;
            }
            if (failed) continue;
            nx::scoped_lock<nx::mutex> drawing(draw_lock);
            if (background.load()) continue;
            // (a swapchain out of date: made again and drawn again at once)
            for (u32 attempt = 0; attempt < 3 && !draw(); ++attempt) {
            }
        }
    }

    void destroy_swapchain() {
        const VkDevice d = device->device();
        if (quads) (void)quads->set_targets({}, {});
        for (VkSemaphore s : rendered) vkDestroySemaphore(d, s, nullptr);
        rendered.clear();
        images.clear();
        if (swapchain) vkDestroySwapchainKHR(d, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }

    // Makes the swapchain for a window of w x h pixels; false if there is nothing to present to (minimized).
    bool recreate(u32 w, u32 h) {
        device->wait_idle();
        const VkPhysicalDevice p = device->physical();
        const VkSurfaceKHR surface = device->surface();
        const VkDevice d = device->device();
        VkSurfaceCapabilitiesKHR caps{};
        if (VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(p, surface, &caps); r != VK_SUCCESS) {
            nx::loge(logs::display, "display: vkGetPhysicalDeviceSurfaceCapabilitiesKHR: {}", vulkan::result_name(r));
            return false;
        }
        VkExtent2D e = caps.currentExtent;
        if (e.width == 0xFFFFFFFFu) {  // (the swapchain decides the window's size)
            e.width = std::clamp(w, caps.minImageExtent.width, caps.maxImageExtent.width);
            e.height = std::clamp(h, caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        if (!e.width || !e.height) return false;
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
            nx::loge(logs::display, "display: the window's images cannot be drawn into: nothing is shown");
            failed = true;
            return false;
        }

        u32 n = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(p, surface, &n, nullptr);
        nx::vector<VkSurfaceFormatKHR> formats(n);
        vkGetPhysicalDeviceSurfaceFormatsKHR(p, surface, &n, formats.data());
        if (formats.empty()) return false;
        // (UNORM, so the guest's bytes go to the display as they are, as on the console; an sRGB swapchain gets sRGB
        // textures, sampling then decoding what drawing encodes again)
        VkSurfaceFormatKHR chosen = formats[0];
        for (const VkSurfaceFormatKHR& f : formats) {
            if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM ||
                 f.format == VK_FORMAT_A8B8G8R8_UNORM_PACK32) &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                chosen = f;
                break;
            }
        }

        vkGetPhysicalDeviceSurfacePresentModesKHR(p, surface, &n, nullptr);
        nx::vector<VkPresentModeKHR> modes(n);
        vkGetPhysicalDeviceSurfacePresentModesKHR(p, surface, &n, modes.data());
        auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
        VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;  // (always there)
        if (vsync ? has(VK_PRESENT_MODE_MAILBOX_KHR) : has(VK_PRESENT_MODE_IMMEDIATE_KHR))
            mode = vsync ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;
        else if (!vsync && has(VK_PRESENT_MODE_MAILBOX_KHR))
            mode = VK_PRESENT_MODE_MAILBOX_KHR;

        u32 count = std::max(caps.minImageCount + 1, 3u);
        if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);
        can_read = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;

        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface = surface;
        ci.minImageCount = count;
        ci.imageFormat = chosen.format;
        ci.imageColorSpace = chosen.colorSpace;
        ci.imageExtent = e;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (can_read ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        // (the compositor rotates: Android's pre-rotation is not done)
        ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                              ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                              : caps.currentTransform;
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) {
            for (u32 bit = 1; bit; bit <<= 1) {
                if (caps.supportedCompositeAlpha & bit) {
                    ci.compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(bit);
                    break;
                }
            }
        }
        ci.presentMode = mode;
        ci.clipped = VK_TRUE;
        ci.oldSwapchain = swapchain;
        VkSwapchainKHR made = VK_NULL_HANDLE;
        if (VkResult r = vkCreateSwapchainKHR(d, &ci, nullptr, &made); r != VK_SUCCESS) {
            nx::loge(logs::display, "display: vkCreateSwapchainKHR: {}", vulkan::result_name(r));
            return false;
        }
        destroy_swapchain();
        swapchain = made;
        vkGetSwapchainImagesKHR(d, swapchain, &n, nullptr);
        images.resize(n);
        vkGetSwapchainImagesKHR(d, swapchain, &n, images.data());
        for (u32 i = 0; i < n; ++i) {
            VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkSemaphore s = VK_NULL_HANDLE;
            vkCreateSemaphore(d, &si, nullptr, &s);
            rendered.push_back(s);
        }
        if (auto r = quads->init(chosen.format, kFramesInFlight * 2); !r) {
            nx::loge(logs::display, "display: {}", r.error().message);
            failed = true;
            return false;
        }
        if (auto r = quads->set_targets(images, e); !r) {
            nx::loge(logs::display, "display: {}", r.error().message);
            return false;
        }
        if (format != chosen.format || extent.width != e.width || extent.height != e.height)
            nx::logi(logs::display, "display: {} x {}, format {}, {} images, {}", e.width, e.height,
                     static_cast<i32>(chosen.format), n, mode_name(mode));
        format = chosen.format;
        extent = e;
        made_w = w, made_h = h;
        out_of_date = false;
        return true;
    }

    // To the background: the drawing done, the swapchain and the surface gone (the window's goes with it).
    void enter_background() {
        background.store(true);
        nx::scoped_lock<nx::mutex> drawing(draw_lock);
        device->wait_idle();
        destroy_swapchain();
        device->replace_surface(VK_NULL_HANDLE);
        need_surface = true;
    }

    void leave_background() {
        background.store(false);
        request_redraw();
    }

    // Presents the current frame; false if the swapchain was out of date (to be drawn again).
    bool draw() {
        if (need_surface) {
            // (back from the background: a surface for the window as it is now; until it has one, nothing)
            // (the window may not have its new one yet: tried again at the next frame, without a word)
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            if (!SDL_Vulkan_CreateSurface(window.window_, device->instance(), nullptr, &surface)) return true;
            device->replace_surface(surface);
            need_surface = false;
            out_of_date = true;
        }
        const u32 w = pixel_w.load(), h = pixel_h.load();
        if (!w || !h) return true;  // (minimized)
        if (!swapchain || out_of_date || w != made_w || h != made_h) {
            if (!recreate(w, h)) return true;
        }
        const VkDevice d = device->device();
        InFlight& f = frames[next];
        next = (next + 1) % kFramesInFlight;
        vkWaitForFences(d, 1, &f.fence, VK_TRUE, UINT64_MAX);
        u32 index = 0;
        VkResult r = vkAcquireNextImageKHR(d, swapchain, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &index);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            out_of_date = true;
            return false;
        }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            nx::loge(logs::display, "display: vkAcquireNextImageKHR: {}", vulkan::result_name(r));
            return true;
        }

        // The overlay's atlas, when it is new (rarely: the frames in flight done with the old one first).
        const VkFormat texture_format = is_srgb(format) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        if (!atlas_upload.empty()) {
            device->wait_idle();
            vulkan::destroy_image(*device, atlas);
            vulkan::ImageDesc desc;
            desc.format = texture_format;
            desc.width = atlas_upload_w, desc.height = atlas_upload_h;
            if (auto image = vulkan::create_image(*device, desc)) {
                atlas = *image;
                if (auto r = vulkan::upload_image(*device, atlas, atlas_upload); !r)
                    nx::loge(logs::display, "display: the overlay: {}", r.error().message);
            }
            atlas_upload.clear();
        }

        // The frame into this slot's image.
        const bool have = current_w && current_h;
        if (have) {
            const VkDeviceSize bytes = VkDeviceSize{current_w} * current_h * 4;
            if (f.staging.size < bytes) {
                vulkan::destroy_buffer(*device, f.staging);
                auto b = vulkan::create_host_buffer(*device, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
                if (!b) {
                    nx::loge(logs::display, "display: {}", b.error().message);
                    return true;
                }
                f.staging = *b;
            }
            std::memcpy(f.staging.mapped, current.data(), bytes);
            vulkan::flush(*device, f.staging);
            if (!f.image.image || f.image.desc.width != current_w || f.image.desc.height != current_h ||
                f.image.desc.format != texture_format) {
                vulkan::destroy_image(*device, f.image);
                vulkan::ImageDesc desc;
                desc.format = texture_format;
                desc.width = current_w, desc.height = current_h;
                auto image = vulkan::create_image(*device, desc);
                if (!image) {
                    nx::loge(logs::display, "display: {}", image.error().message);
                    return true;
                }
                f.image = *image;
            }
        }

        nx::scoped_lock<nx::mutex> read_held(readback_lock);
        const bool read = keep.load() && can_read;
        if (read) {
            const VkDeviceSize bytes = VkDeviceSize{extent.width} * extent.height * 4;
            if (readback.size < bytes) {
                vulkan::destroy_buffer(*device, readback);
                auto b = vulkan::create_host_buffer(*device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
                if (b) readback = *b;
            }
        }

        vkResetFences(d, 1, &f.fence);
        vkResetCommandBuffer(f.cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(f.cmd, &bi);
        if (have) {
            vulkan::transition(f.cmd, f.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {current_w, current_h, 1};
            vkCmdCopyBufferToImage(f.cmd, f.staging.buffer, f.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &copy);
            vulkan::transition(f.cmd, f.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        const u32 slot = static_cast<u32>(&f - frames);
        quads->begin(f.cmd, index);
        if (have) {
            // (the screen as large as fits, centred; sharp pixels at 1:1)
            const Rect v = fit(static_cast<f32>(extent.width), static_cast<f32>(extent.height));
            Quad q;
            q.x0 = std::round(v.x), q.y0 = std::round(v.y);
            q.x1 = std::round(v.x + v.w), q.y1 = std::round(v.y + v.h);
            const bool exact = q.x1 - q.x0 == static_cast<f32>(current_w) && q.y1 - q.y0 == static_cast<f32>(current_h);
            quads->draw(f.cmd, quads->texture(slot * 2, f.image.view, smooth && !exact), q, false);
        }
        if (atlas.image && !overlay_drawn.empty()) {
            const VkDescriptorSet sprites = quads->texture(slot * 2 + 1, atlas.view, true);
            for (const Quad& q : overlay_drawn) quads->draw(f.cmd, sprites, q, true);
        }
        quads->end(f.cmd);
        if (read && readback.buffer) {
            auto layout_barrier = [&](VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
                                      VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
                VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                b.srcAccessMask = src;
                b.dstAccessMask = dst;
                b.oldLayout = from;
                b.newLayout = to;
                b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = images[index];
                b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdPipelineBarrier(f.cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
            };
            layout_barrier(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {extent.width, extent.height, 1};
            vkCmdCopyImageToBuffer(f.cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
            VkBufferMemoryBarrier hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            hb.buffer = readback.buffer;
            hb.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                                 &hb, 0, nullptr);
            layout_barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                           VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            readback_w = extent.width, readback_h = extent.height;
            readback_format = format;
            readback_fence = f.fence;
        }
        vkEndCommandBuffer(f.cmd);

        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f.acquired;
        si.pWaitDstStageMask = &wait_stage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &f.cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &rendered[index];
        if (VkResult sr = device->submit(si, f.fence); sr != VK_SUCCESS) {
            nx::loge(logs::display, "display: vkQueueSubmit: {}", vulkan::result_name(sr));
            return true;
        }
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &rendered[index];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &index;
        {
            nx::scoped_lock<nx::mutex> queue_held(device->queue_lock());
            r = vkQueuePresentKHR(device->queue(), &pi);
        }
        // (SUBOPTIMAL is not acted on: Android reports it for every present the compositor rotates)
        if (r == VK_ERROR_OUT_OF_DATE_KHR) out_of_date = true;
        else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
            nx::loge(logs::display, "display: vkQueuePresentKHR: {}", vulkan::result_name(r));
        window.presented_.fetch_add(1);
        return true;
    }
};

Result<nx::unique_ptr<Window>> Window::open(const Options& options) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
        return failure(nx::format("display: SDL: {}", SDL_GetError()));
    nx::unique_ptr<Window> w = nx::make_unique<Window>();
    w->options_ = options;
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (options.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
#if defined(__ANDROID__)
    // (the whole screen, held sideways as the console is: SDL lets a resizable window turn any way otherwise)
    flags |= SDL_WINDOW_FULLSCREEN;
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");  // (Back is a key: the settings, not leaving the game)
#endif
    if (options.hidden) flags |= SDL_WINDOW_HIDDEN;
    const u32 scale = std::max(options.scale, 1u);
    w->window_ = SDL_CreateWindow(options.title.c_str(), static_cast<int>(kScreenWidth * scale),
                                  static_cast<int>(kScreenHeight * scale), flags);
    if (!w->window_) return failure(nx::format("display: no window: {}", SDL_GetError()));  // (as below)
    SDL_SetWindowMinimumSize(w->window_, static_cast<int>(kScreenWidth / 4), static_cast<int>(kScreenHeight / 4));

    vulkan::DeviceOptions device_options;
    device_options.gpu = options.gpu;
    device_options.validation = options.validation;
    Uint32 count = 0;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    for (Uint32 i = 0; i < count; ++i) device_options.instance_extensions.push_back(extensions[i]);
    device_options.make_surface = make_surface;
    device_options.surface_user = w->window_;
    auto device = vulkan::Device::create(device_options);
    if (!device) return failure(nx::format("display: {}", device.error().message));  // (the destructor closes it)
    w->presenter_ = nx::make_unique<Presenter>(*w);
    w->presenter_->device = std::move(*device);
    w->presenter_->vsync = options.vsync;
    w->presenter_->smooth = options.smooth;
    // (the on-screen controls: their atlas once, their quads as they change)
    TouchControls& controls = w->input_.controls();
    controls.set_mode(options.controls);
    controls.set_opacity(options.controls_opacity);
    controls.set_scale(options.controls_scale);
#if defined(__ANDROID__)
    controls.touched();  // (a touch screen: shown from the start)
#endif
    w->update_view();
    if (auto r = w->presenter_->init(); !r) return failure(nx::format("display: {}", r.error().message));
    {
        nx::vector<u8> atlas;
        u32 aw = 0, ah = 0;
        TouchControls::atlas(&atlas, &aw, &ah);
        w->set_overlay_atlas(atlas.data(), aw, ah);
        w->update_overlay();
    }
    nx::logi(logs::display, "display: window on {}", w->presenter_->device->properties().deviceName);
    return w;
}

Window::~Window() {
    presenter_.reset();
    for (SDL_Gamepad* g : gamepads_) SDL_CloseGamepad(g);
    if (window_) SDL_DestroyWindow(window_);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
}

void Window::update_view() {
    int w = 0, h = 0, pw = 0, ph = 0;
    SDL_GetWindowSize(window_, &w, &h);
    SDL_GetWindowSizeInPixels(window_, &pw, &ph);
    const Rect v = fit(static_cast<f32>(w), static_cast<f32>(h));
    input_.set_view(static_cast<f32>(w), static_cast<f32>(h), v.x, v.y, v.w, v.h);
    SDL_Rect safe{0, 0, w, h};
    if (!SDL_GetWindowSafeArea(window_, &safe)) safe = SDL_Rect{0, 0, w, h};
    input_.controls().layout(static_cast<f32>(w), static_cast<f32>(h), w > 0 ? static_cast<f32>(pw) / w : 1.0f,
                             static_cast<f32>(safe.x), static_cast<f32>(safe.y), static_cast<f32>(safe.w),
                             static_cast<f32>(safe.h));
    update_overlay();
    if (presenter_) {
        presenter_->pixel_w.store(static_cast<u32>(std::max(pw, 0)));
        presenter_->pixel_h.store(static_cast<u32>(std::max(ph, 0)));
        presenter_->request_redraw();
    }
}

bool Window::pump(runtime::Runtime* rt, u32 wait_ms) {
    SDL_Event e;
    bool got = wait_ms ? SDL_WaitEventTimeout(&e, static_cast<Sint32>(wait_ms)) : SDL_PollEvent(&e);
    while (got) {
        handle(e, rt);
        got = SDL_PollEvent(&e);
    }
    update_overlay();
    return !closed_;
}

void Window::update_overlay() {
    const TouchControls& controls = input_.controls();
    if (!presenter_ || controls.version() == overlay_version_) return;
    overlay_version_ = controls.version();
    set_overlay(controls.quads());
}

void Window::handle(const SDL_Event& e, runtime::Runtime* rt) {
    switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: closed_ = true; break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_SHOWN:
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED: update_view(); break;
        case SDL_EVENT_GAMEPAD_ADDED:
            // (a gamepad's events come once it is opened)
            if (SDL_Gamepad* g = SDL_OpenGamepad(e.gdevice.which)) {
                gamepads_.push_back(g);
                nx::logi(logs::input, "input: gamepad '{}'", SDL_GetGamepadName(g));
            }
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            for (usize i = 0; i < gamepads_.size(); ++i) {
                if (SDL_GetGamepadID(gamepads_[i]) == e.gdevice.which) {
                    SDL_CloseGamepad(gamepads_[i]);
                    gamepads_.erase(gamepads_.begin() + static_cast<isize>(i));
                    break;
                }
            }
            break;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.key == SDLK_F11 && !e.key.repeat)
                SDL_SetWindowFullscreen(window_, !(SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN));
            break;
        default: break;
    }
    if (rt) input_.handle(e, rt->input(), rt->kernel().clock_ns());
}

void Window::show(const gxm::Frame& f) {
    // (drawn at a higher internal resolution: that picture)
    if (f.scaled) return show_pixels(f.scaled, f.scaled_width, f.scaled_height, f.scaled_width, f.pixel_format);
    const u8* base = runtime::runtime().memory().host(f.address);
    if (!base) return;
    show_pixels(base, f.width, f.height, f.pitch, f.pixel_format);
}

void Window::show_pixels(const u8* pixels, u32 width, u32 height, u32 pitch, u32 pixel_format) {
    if (!presenter_ || !width || !height) return;
    Presenter& p = *presenter_;
    {
        nx::scoped_lock<nx::mutex> held(p.lock);
        p.pending.resize(usize{width} * height * 4);
        u8* out = p.pending.data();
        for (u32 y = 0; y < height; ++y) {
            const u8* row = pixels + usize{y} * pitch * 4;
            u8* dst = out + usize{y} * width * 4;
            if (pixel_format == kA2B10G10R10) {
                // (10 bits a channel, R in the low bits, to 8)
                for (u32 x = 0; x < width; ++x) {
                    u32 v;
                    std::memcpy(&v, row + x * 4, 4);
                    dst[x * 4 + 0] = static_cast<u8>((v >> 2) & 0xFF);
                    dst[x * 4 + 1] = static_cast<u8>((v >> 12) & 0xFF);
                    dst[x * 4 + 2] = static_cast<u8>((v >> 22) & 0xFF);
                    dst[x * 4 + 3] = 0xFF;
                }
            } else {
                std::memcpy(dst, row, usize{width} * 4);
            }
        }
        p.pending_w = width, p.pending_h = height;
        p.has_pending = true;
    }
    p.wake.notify_one();
}

void Window::set_overlay(nx::vector<Quad> quads) {
    if (!presenter_) return;
    {
        nx::scoped_lock<nx::mutex> held(presenter_->lock);
        presenter_->overlay = std::move(quads);
        presenter_->redraw = true;
    }
    presenter_->wake.notify_one();
}

void Window::set_overlay_atlas(const u8* rgba, u32 width, u32 height) {
    if (!presenter_) return;
    {
        nx::scoped_lock<nx::mutex> held(presenter_->lock);
        presenter_->atlas_pixels.assign(rgba, rgba + usize{width} * height * 4);
        presenter_->atlas_w = width, presenter_->atlas_h = height;
        presenter_->atlas_changed = true;
        presenter_->redraw = true;
    }
    presenter_->wake.notify_one();
}

void Window::set_smooth(bool smooth) {
    if (!presenter_) return;
    presenter_->smooth.store(smooth);
    presenter_->request_redraw();
}

void Window::set_background(bool background) {
    if (!presenter_) return;
    if (background) presenter_->enter_background();
    else presenter_->leave_background();
}

vulkan::Device* Window::device() const { return presenter_ ? presenter_->device.get() : nullptr; }

bool Window::wait_presented(u64 count, u32 timeout_ms) {
    const u64 until = SDL_GetTicks() + timeout_ms;
    while (presented_.load() < count) {
        if (SDL_GetTicks() >= until) return false;
        SDL_Delay(1);
    }
    return true;
}

void Window::keep_last(bool keep) {
    if (presenter_) presenter_->keep.store(keep);
}

bool Window::last_image(nx::vector<u8>* rgba, u32* width, u32* height) {
    if (!presenter_) return false;
    Presenter& p = *presenter_;
    nx::scoped_lock<nx::mutex> held(p.readback_lock);
    if (!p.readback_fence || !p.readback.buffer) return false;
    vkWaitForFences(p.device->device(), 1, &p.readback_fence, VK_TRUE, UINT64_MAX);
    vulkan::invalidate(*p.device, p.readback);
    const usize n = usize{p.readback_w} * p.readback_h * 4;
    rgba->resize(n);
    std::memcpy(rgba->data(), p.readback.mapped, n);
    if (p.readback_format == VK_FORMAT_B8G8R8A8_UNORM || p.readback_format == VK_FORMAT_B8G8R8A8_SRGB) {
        for (usize i = 0; i < n; i += 4) std::swap((*rgba)[i], (*rgba)[i + 2]);
    }
    *width = p.readback_w, *height = p.readback_h;
    return true;
}

}  // namespace psvrecore::platform
