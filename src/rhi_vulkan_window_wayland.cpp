// Daidalos - window backend #2: Wayland.
//
// Same four entry points as the X11 backend, same contract: present is a BLIT
// of the finished offscreen frame, never a second render path. Which backend
// gets compiled is a build decision (DAI_WINDOW_WAYLAND), and the engine above
// it does not know the difference - that is the whole point of keeping the
// window behind dai_render.h rather than inside the renderer.
//
// Wayland differs from X11 in ways that matter here:
//   * there is no "window position" and no server side decoration to rely on
//   * the compositor drives the lifecycle: you must ack every configure
//   * a surface has no size until the compositor says so, so the swapchain is
//     created after the first configure round trip, not before
//   * input arrives through wl_seat capabilities, which can appear late
//
// Tested headless against weston --backend=headless: no display hardware
// required, which is the same trick the X11 backend uses with Xvfb.

#include "rhi_vulkan.hpp"

#include <wayland-client.h>
#include "generated/xdg-shell-client-protocol.h"
#include <vulkan/vulkan_wayland.h>
#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

struct dai_window {
    dai_renderer *r = nullptr;

    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    wl_compositor *compositor = nullptr;
    xdg_wm_base *wm_base = nullptr;
    wl_seat *seat = nullptr;
    wl_keyboard *keyboard = nullptr;
    wl_pointer *pointer = nullptr;

    wl_surface *surface = nullptr;
    xdg_surface *xsurface = nullptr;
    xdg_toplevel *toplevel = nullptr;

    bool configured = false;
    bool open = true;
    uint32_t width = 0, height = 0;
    uint32_t pending_w = 0, pending_h = 0;

    VkSurfaceKHR vksurface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    std::vector<VkImage> images;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE, blitted = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    bool keys[256] = {};
    int mouse_x = 0, mouse_y = 0;
    uint32_t buttons = 0;
};

namespace {

uint32_t key_slot(uint32_t keysym) { return (keysym ^ (keysym >> 8)) & 0xFF; }

// ---- listeners

void wm_ping(void *, xdg_wm_base *base, uint32_t serial) { xdg_wm_base_pong(base, serial); }
const xdg_wm_base_listener kWmListener = { wm_ping };

void xsurf_configure(void *data, xdg_surface *xs, uint32_t serial) {
    dai_window *w = (dai_window *)data;
    xdg_surface_ack_configure(xs, serial);
    w->configured = true;
}
const xdg_surface_listener kXSurfListener = { xsurf_configure };

void top_configure(void *data, xdg_toplevel *, int32_t width, int32_t height, wl_array *) {
    dai_window *w = (dai_window *)data;
    if (width > 0 && height > 0) { w->pending_w = (uint32_t)width; w->pending_h = (uint32_t)height; }
}
void top_close(void *data, xdg_toplevel *) { ((dai_window *)data)->open = false; }
const xdg_toplevel_listener kTopListener = { .configure = top_configure, .close = top_close };

void kb_keymap(void *, wl_keyboard *, uint32_t format, int fd, uint32_t size) { close(fd); }
void kb_enter(void *, wl_keyboard *, uint32_t, wl_surface *, wl_array *) {}
void kb_leave(void *, wl_keyboard *, uint32_t, wl_surface *) {}
void kb_key(void *data, wl_keyboard *, uint32_t, uint32_t, uint32_t key, uint32_t state) {
    dai_window *w = (dai_window *)data;
    // evdev code + 8 is the X11 keycode; without a keymap we cannot resolve a
    // keysym, so the raw code is what the host gets. Documented, not hidden.
    w->keys[key_slot(key + 8)] = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    if (key == KEY_ESC && state == WL_KEYBOARD_KEY_STATE_PRESSED) w->open = false;
}
void kb_modifiers(void *, wl_keyboard *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
void kb_repeat(void *, wl_keyboard *, int32_t, int32_t) {}
const wl_keyboard_listener kKbListener = { .keymap = kb_keymap, .enter = kb_enter, .leave = kb_leave,
                                           .key = kb_key, .modifiers = kb_modifiers, .repeat_info = kb_repeat };

void pt_enter(void *, wl_pointer *, uint32_t, wl_surface *, wl_fixed_t, wl_fixed_t) {}
void pt_leave(void *, wl_pointer *, uint32_t, wl_surface *) {}
void pt_motion(void *data, wl_pointer *, uint32_t, wl_fixed_t x, wl_fixed_t y) {
    dai_window *w = (dai_window *)data;
    w->mouse_x = wl_fixed_to_int(x);
    w->mouse_y = wl_fixed_to_int(y);
}
void pt_button(void *data, wl_pointer *, uint32_t, uint32_t, uint32_t button, uint32_t state) {
    dai_window *w = (dai_window *)data;
    uint32_t bit = 1u << (button == BTN_LEFT ? 1 : button == BTN_MIDDLE ? 2 : 3);
    if (state) w->buttons |= bit; else w->buttons &= ~bit;
}
void pt_axis(void *, wl_pointer *, uint32_t, uint32_t, wl_fixed_t) {}
void pt_frame(void *, wl_pointer *) {}
void pt_axis_source(void *, wl_pointer *, uint32_t) {}
void pt_axis_stop(void *, wl_pointer *, uint32_t, uint32_t) {}
void pt_axis_discrete(void *, wl_pointer *, uint32_t, int32_t) {}
// wl_pointer gained members over time; designated initialisers keep this
// compiling against whichever libwayland version the distribution ships
const wl_pointer_listener kPtListener = {
    .enter = pt_enter, .leave = pt_leave, .motion = pt_motion, .button = pt_button,
    .axis = pt_axis, .frame = pt_frame, .axis_source = pt_axis_source,
    .axis_stop = pt_axis_stop, .axis_discrete = pt_axis_discrete,
};

void seat_caps(void *data, wl_seat *seat, uint32_t caps) {
    dai_window *w = (dai_window *)data;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !w->keyboard) {
        w->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(w->keyboard, &kKbListener, w);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !w->pointer) {
        w->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(w->pointer, &kPtListener, w);
    }
}
void seat_name(void *, wl_seat *, const char *) {}
const wl_seat_listener kSeatListener = { seat_caps, seat_name };

void reg_global(void *data, wl_registry *reg, uint32_t id, const char *iface, uint32_t version) {
    dai_window *w = (dai_window *)data;
    if (!std::strcmp(iface, wl_compositor_interface.name))
        w->compositor = (wl_compositor *)wl_registry_bind(reg, id, &wl_compositor_interface, version < 4 ? version : 4);
    else if (!std::strcmp(iface, xdg_wm_base_interface.name)) {
        w->wm_base = (xdg_wm_base *)wl_registry_bind(reg, id, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(w->wm_base, &kWmListener, w);
    } else if (!std::strcmp(iface, wl_seat_interface.name)) {
        w->seat = (wl_seat *)wl_registry_bind(reg, id, &wl_seat_interface, version < 5 ? version : 5);
        wl_seat_add_listener(w->seat, &kSeatListener, w);
    }
}
void reg_remove(void *, wl_registry *, uint32_t) {}
const wl_registry_listener kRegListener = { reg_global, reg_remove };

bool create_swapchain(dai_window *w) {
    dai_renderer *r = w->r;
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->phys, w->vksurface, &caps);

    uint32_t fmt_n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(r->phys, w->vksurface, &fmt_n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmt_n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(r->phys, w->vksurface, &fmt_n, formats.data());
    VkSurfaceFormatKHR chosen = formats.empty()
        ? VkSurfaceFormatKHR{ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR } : formats[0];
    for (const auto &f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) { chosen = f; break; }

    // Wayland reports 0xFFFFFFFF here far more often than X11 does: the client
    // picks its own size, the compositor only suggests one through configure.
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) { extent.width = w->width; extent.height = w->height; }
    if (!extent.width || !extent.height) return false;

    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sci.surface = w->vksurface;
    sci.minImageCount = count;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = w->swapchain;

    VkSwapchainKHR nsc = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(r->dev, &sci, nullptr, &nsc) != VK_SUCCESS) return false;
    if (w->swapchain) vkDestroySwapchainKHR(r->dev, w->swapchain, nullptr);
    w->swapchain = nsc;
    w->format = chosen.format;
    w->width = extent.width;
    w->height = extent.height;

    uint32_t img_n = 0;
    vkGetSwapchainImagesKHR(r->dev, w->swapchain, &img_n, nullptr);
    w->images.resize(img_n);
    vkGetSwapchainImagesKHR(r->dev, w->swapchain, &img_n, w->images.data());
    return true;
}

} // namespace

extern "C" {

dai_window *dai_window_open(dai_renderer *r, const char *title, uint32_t width, uint32_t height,
                            char *err, size_t err_len) {
    auto bail = [&](const char *m) -> dai_window * {
        if (err && err_len) std::snprintf(err, err_len, "%s", m);
        return nullptr;
    };
    if (!r) return bail("no renderer");
    if (!r->has_surface_ext) return bail("instance was created without VK_KHR_wayland_surface");
    if (!r->has_swapchain_ext) return bail("device does not support VK_KHR_swapchain");

    dai_window *w = new dai_window();
    w->r = r;
    w->width = width ? width : r->width;
    w->height = height ? height : r->height;

    w->display = wl_display_connect(nullptr);
    if (!w->display) { delete w; return bail("cannot connect to a Wayland display (WAYLAND_DISPLAY set?)"); }

    w->registry = wl_display_get_registry(w->display);
    wl_registry_add_listener(w->registry, &kRegListener, w);
    wl_display_roundtrip(w->display);        // globals
    wl_display_roundtrip(w->display);        // seat capabilities

    if (!w->compositor || !w->wm_base) { dai_window_close(w); return bail("compositor lacks wl_compositor or xdg_wm_base"); }

    w->surface = wl_compositor_create_surface(w->compositor);
    w->xsurface = xdg_wm_base_get_xdg_surface(w->wm_base, w->surface);
    xdg_surface_add_listener(w->xsurface, &kXSurfListener, w);
    w->toplevel = xdg_surface_get_toplevel(w->xsurface);
    xdg_toplevel_add_listener(w->toplevel, &kTopListener, w);
    xdg_toplevel_set_title(w->toplevel, title ? title : "Daidalos");
    xdg_toplevel_set_app_id(w->toplevel, "daidalos");
    wl_surface_commit(w->surface);

    // the surface has no size until the compositor has configured it
    while (!w->configured && wl_display_dispatch(w->display) != -1) {}
    if (w->pending_w && w->pending_h) { w->width = w->pending_w; w->height = w->pending_h; }
    wl_surface_commit(w->surface);

    VkWaylandSurfaceCreateInfoKHR sci{ VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR };
    sci.display = w->display;
    sci.surface = w->surface;
    if (vkCreateWaylandSurfaceKHR(r->instance, &sci, nullptr, &w->vksurface) != VK_SUCCESS)
        { dai_window_close(w); return bail("vkCreateWaylandSurfaceKHR failed"); }

    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(r->phys, r->qfam, w->vksurface, &supported);
    if (!supported) { dai_window_close(w); return bail("graphics queue cannot present to this surface"); }
    if (!create_swapchain(w)) { dai_window_close(w); return bail("swapchain creation failed"); }

    VkCommandBufferAllocateInfo cbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbi.commandPool = r->pool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
    vkAllocateCommandBuffers(r->dev, &cbi, &w->cmd);
    VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    vkCreateSemaphore(r->dev, &si, nullptr, &w->acquired);
    vkCreateSemaphore(r->dev, &si, nullptr, &w->blitted);
    VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    vkCreateFence(r->dev, &fi, nullptr, &w->fence);
    return w;
}

void dai_window_close(dai_window *w) {
    if (!w) return;
    dai_renderer *r = w->r;
    if (r && r->dev) {
        vkDeviceWaitIdle(r->dev);
        if (w->fence) vkDestroyFence(r->dev, w->fence, nullptr);
        if (w->acquired) vkDestroySemaphore(r->dev, w->acquired, nullptr);
        if (w->blitted) vkDestroySemaphore(r->dev, w->blitted, nullptr);
        if (w->cmd) vkFreeCommandBuffers(r->dev, r->pool, 1, &w->cmd);
        if (w->swapchain) vkDestroySwapchainKHR(r->dev, w->swapchain, nullptr);
    }
    if (r && r->instance && w->vksurface) vkDestroySurfaceKHR(r->instance, w->vksurface, nullptr);
    if (w->toplevel) xdg_toplevel_destroy(w->toplevel);
    if (w->xsurface) xdg_surface_destroy(w->xsurface);
    if (w->surface) wl_surface_destroy(w->surface);
    if (w->keyboard) wl_keyboard_destroy(w->keyboard);
    if (w->pointer) wl_pointer_destroy(w->pointer);
    if (w->seat) wl_seat_destroy(w->seat);
    if (w->wm_base) xdg_wm_base_destroy(w->wm_base);
    if (w->compositor) wl_compositor_destroy(w->compositor);
    if (w->registry) wl_registry_destroy(w->registry);
    if (w->display) wl_display_disconnect(w->display);
    delete w;
}

int dai_window_poll(dai_window *w) {
    if (!w || !w->open) return 0;
    // non blocking: dispatch what has arrived, never wait for an event
    wl_display_dispatch_pending(w->display);
    wl_display_flush(w->display);
    if (w->pending_w && (w->pending_w != w->width || w->pending_h != w->height)) {
        w->width = w->pending_w; w->height = w->pending_h;
        vkDeviceWaitIdle(w->r->dev);
        create_swapchain(w);
    }
    return w->open ? 1 : 0;
}

dai_result dai_window_present(dai_window *w) {
    if (!w || !w->open) return DAI_ERR_STATE;
    dai_renderer *r = w->r;
    if (!r->have_frame) return DAI_ERR_STATE;

    uint32_t index = 0;
    VkResult res = vkAcquireNextImageKHR(r->dev, w->swapchain, UINT64_MAX, w->acquired, VK_NULL_HANDLE, &index);
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
        vkDeviceWaitIdle(r->dev);
        if (!create_swapchain(w)) return DAI_ERR_STATE;
        res = vkAcquireNextImageKHR(r->dev, w->swapchain, UINT64_MAX, w->acquired, VK_NULL_HANDLE, &index);
    }
    if (res != VK_SUCCESS) return DAI_ERR_STATE;

    vkResetFences(r->dev, 1, &w->fence);
    vkResetCommandBuffer(w->cmd, 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(w->cmd, &bi);

    VkImage dst = w->images[index];
    vk_barrier(w->cmd, dst, VK_IMAGE_ASPECT_COLOR_BIT,
               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
               VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkImageBlit blit{};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[1] = { (int32_t)r->width, (int32_t)r->height, 1 };
    blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.dstOffsets[1] = { (int32_t)w->width, (int32_t)w->height, 1 };
    vkCmdBlitImage(w->cmd, r->color_rt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

    vk_barrier(w->cmd, dst, VK_IMAGE_ASPECT_COLOR_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
               VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
    vkEndCommandBuffer(w->cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &w->acquired; si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1; si.pCommandBuffers = &w->cmd;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &w->blitted;
    if (vkQueueSubmit(r->queue, 1, &si, w->fence) != VK_SUCCESS) return DAI_ERR_STATE;

    VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &w->blitted;
    pi.swapchainCount = 1; pi.pSwapchains = &w->swapchain; pi.pImageIndices = &index;
    res = vkQueuePresentKHR(r->queue, &pi);
    vkWaitForFences(r->dev, 1, &w->fence, VK_TRUE, UINT64_MAX);
    wl_display_flush(w->display);
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
        vkDeviceWaitIdle(r->dev);
        create_swapchain(w);
    } else if (res != VK_SUCCESS) return DAI_ERR_STATE;
    return DAI_OK;
}

int dai_window_key_down(dai_window *w, uint32_t keysym) { return (w && w->keys[key_slot(keysym)]) ? 1 : 0; }

int dai_window_mouse(dai_window *w, int *x, int *y, uint32_t *buttons) {
    if (!w) return 0;
    if (x) *x = w->mouse_x;
    if (y) *y = w->mouse_y;
    if (buttons) *buttons = w->buttons;
    return 1;
}

void dai_window_size(dai_window *w, uint32_t *width, uint32_t *height) {
    if (!w) return;
    if (width) *width = w->width;
    if (height) *height = w->height;
}

} // extern "C"
