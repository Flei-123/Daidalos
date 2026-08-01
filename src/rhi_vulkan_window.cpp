// Daidalos - window and presentation (X11 + VK_KHR_swapchain).
//
// Deliberately a CONSUMER of the finished frame, not a second rendering path:
// dai_render_frame always draws into the offscreen target, and presenting is a
// blit from that target onto a swapchain image. Consequences worth having:
//
//   * the headless tests and the on screen build run the identical code
//   * resizing never invalidates the renderer, only the swapchain
//   * a Win32, Wayland or Metal window is this file again, ~300 lines, and
//     nothing else in the engine changes
//
// The cost is one full screen blit per frame. At 1080p that is well under a
// millisecond and buys the entire property above.

#include "rhi_vulkan.hpp"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <vulkan/vulkan_xlib.h>

#include <cstdio>
#include <cstring>

struct dai_window {
    dai_renderer *r = nullptr;
    Display *dpy = nullptr;
    Window win = 0;
    Atom wm_delete = 0;
    bool open = true;

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    uint32_t width = 0, height = 0;
    std::vector<VkImage> images;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE, blitted = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    // input state, filled by dai_window_poll
    bool keys[256] = {};                 // hashed keysym -> down
    int mouse_x = 0, mouse_y = 0;
    uint32_t buttons = 0;
    float    wheel = 0.0f;
};

namespace {

uint32_t key_slot(uint32_t keysym) { return (keysym ^ (keysym >> 8)) & 0xFF; }

bool create_swapchain(dai_window *w, char *err, size_t err_len) {
    dai_renderer *r = w->r;
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->phys, w->surface, &caps);

    uint32_t fmt_n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(r->phys, w->surface, &fmt_n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmt_n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(r->phys, w->surface, &fmt_n, formats.data());
    VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR }
                                                : formats[0];
    for (const auto &f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) { chosen = f; break; }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) { extent.width = w->width; extent.height = w->height; }
    if (extent.width == 0 || extent.height == 0) return false;      // minimised

    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sci.surface = w->surface;
    sci.minImageCount = count;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;       // always supported, vsynced
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = w->swapchain;

    VkSwapchainKHR nsc = VK_NULL_HANDLE;
    VkResult res = vkCreateSwapchainKHR(r->dev, &sci, nullptr, &nsc);
    if (res != VK_SUCCESS) {
        if (err && err_len) std::snprintf(err, err_len, "vkCreateSwapchainKHR failed (%d)", (int)res);
        return false;
    }
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
    if (!r->has_surface_ext) return bail("instance was created without VK_KHR_xlib_surface");
    if (!r->has_swapchain_ext) return bail("device does not support VK_KHR_swapchain");

    dai_window *w = new dai_window();
    w->r = r;
    w->width = width ? width : r->width;
    w->height = height ? height : r->height;

    w->dpy = XOpenDisplay(nullptr);
    if (!w->dpy) { delete w; return bail("cannot open X display (is DISPLAY set?)"); }

    int screen = DefaultScreen(w->dpy);
    w->win = XCreateSimpleWindow(w->dpy, RootWindow(w->dpy, screen), 0, 0, w->width, w->height, 0,
                                 BlackPixel(w->dpy, screen), BlackPixel(w->dpy, screen));
    XStoreName(w->dpy, w->win, title ? title : "Daidalos");
    XSelectInput(w->dpy, w->win, ExposureMask | KeyPressMask | KeyReleaseMask |
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask);
    w->wm_delete = XInternAtom(w->dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(w->dpy, w->win, &w->wm_delete, 1);
    XMapWindow(w->dpy, w->win);
    XFlush(w->dpy);

    VkXlibSurfaceCreateInfoKHR sci{ VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR };
    sci.dpy = w->dpy; sci.window = w->win;
    if (vkCreateXlibSurfaceKHR(r->instance, &sci, nullptr, &w->surface) != VK_SUCCESS)
        { dai_window_close(w); return bail("vkCreateXlibSurfaceKHR failed"); }

    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(r->phys, r->qfam, w->surface, &supported);
    if (!supported) { dai_window_close(w); return bail("graphics queue cannot present to this surface"); }

    char serr[128] = {0};
    if (!create_swapchain(w, serr, sizeof(serr))) { dai_window_close(w); return bail(serr[0] ? serr : "swapchain failed"); }

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
    if (r && r->instance && w->surface) vkDestroySurfaceKHR(r->instance, w->surface, nullptr);
    if (w->dpy) {
        if (w->win) XDestroyWindow(w->dpy, w->win);
        XCloseDisplay(w->dpy);
    }
    delete w;
}

int dai_window_poll(dai_window *w) {
    if (!w || !w->open) return 0;
    while (XPending(w->dpy)) {
        XEvent e;
        XNextEvent(w->dpy, &e);
        switch (e.type) {
        case ClientMessage:
            if ((Atom)e.xclient.data.l[0] == w->wm_delete) w->open = false;
            break;
        case KeyPress: case KeyRelease: {
            KeySym ks = XLookupKeysym(&e.xkey, 0);
            w->keys[key_slot((uint32_t)ks)] = (e.type == KeyPress);
            if (e.type == KeyPress && ks == XK_Escape) w->open = false;
            break;
        }
        case ButtonPress:
            // X11 reports the wheel as buttons 4 and 5. They must not land in
            // the button mask, or "middle drag" would trigger on every scroll.
            if (e.xbutton.button == 4) w->wheel += 1.0f;
            else if (e.xbutton.button == 5) w->wheel -= 1.0f;
            else w->buttons |= (1u << e.xbutton.button);
            break;
        case ButtonRelease:
            if (e.xbutton.button != 4 && e.xbutton.button != 5)
                w->buttons &= ~(1u << e.xbutton.button);
            break;
        case MotionNotify:  w->mouse_x = e.xmotion.x; w->mouse_y = e.xmotion.y; break;
        case ConfigureNotify:
            if ((uint32_t)e.xconfigure.width != w->width || (uint32_t)e.xconfigure.height != w->height) {
                w->width = (uint32_t)e.xconfigure.width;
                w->height = (uint32_t)e.xconfigure.height;
                vkDeviceWaitIdle(w->r->dev);
                create_swapchain(w, nullptr, 0);
            }
            break;
        default: break;
        }
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
        if (!create_swapchain(w, nullptr, 0)) return DAI_ERR_STATE;
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

    // the offscreen target is already in TRANSFER_SRC_OPTIMAL after a frame
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
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
        vkDeviceWaitIdle(r->dev);
        create_swapchain(w, nullptr, 0);
    } else if (res != VK_SUCCESS) {
        return DAI_ERR_STATE;
    }
    return DAI_OK;
}

int dai_window_key_down(dai_window *w, uint32_t keysym) {
    return (w && w->keys[key_slot(keysym)]) ? 1 : 0;
}

float dai_window_wheel(dai_window *w) {
    if (!w) return 0.0f;
    float v = w->wheel;
    w->wheel = 0.0f;
    return v;
}

int dai_window_mouse(dai_window *w, int *x, int *y, uint32_t *buttons) {
    if (!w) return 0;
    // The finished frame is BLITTED onto the window, stretched from the
    // renderer's resolution to whatever size the window happens to be. So a
    // pointer position in window pixels does not address the picture the host
    // drew - and a host that hit tests a gizmo against it misses by exactly the
    // stretch factor. Resize the window and the miss grows.
    //
    // Handing back window pixels and expecting every caller to divide is how
    // that bug gets written once per program. The mouse is reported in the
    // same space as the frame.
    const dai_renderer *r = w->r;
    const double sx = (w->width  && r->width)  ? (double)r->width  / (double)w->width  : 1.0;
    const double sy = (w->height && r->height) ? (double)r->height / (double)w->height : 1.0;
    if (x) *x = (int)((double)w->mouse_x * sx);
    if (y) *y = (int)((double)w->mouse_y * sy);
    if (buttons) *buttons = w->buttons;
    return 1;
}

void dai_window_size(dai_window *w, uint32_t *width, uint32_t *height) {
    if (!w) return;
    if (width) *width = w->width;
    if (height) *height = w->height;
}

} // extern "C"
