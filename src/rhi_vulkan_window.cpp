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
#include "dai_font.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>       /* XA_ATOM: the XdndAware property is a list of them */
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

    // Which part of the offscreen frame this window shows. Zero width or
    // height means "all of it", which is what every window did before torn
    // off panels needed a strip of their own.
    int src_x = 0, src_y = 0, src_w = 0, src_h = 0;

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

    // Text events from XLookupString: one code point per press, shift and
    // compose already applied - what a text field wants, unlike "held".
    uint32_t text[64] = { 0 };
    uint32_t text_head = 0, text_tail = 0;
    float    wheel = 0.0f;

    // Pointer shapes, created on first use. X11 has them built in (the "cursor
    // font"), so this costs nothing until something asks for one.
    Cursor cursors[7] = { 0, 0, 0, 0, 0, 0, 0 };
    int    cursor_now = -1;

    // XDND, receiver side. X11 has no "drop" event: a drop is a conversation
    // of ClientMessages with the dragging program, ending in a selection
    // transfer - which is why this is six atoms and a state machine instead
    // of one case label.
    Atom xdnd_aware = 0, xdnd_enter = 0, xdnd_position = 0, xdnd_status = 0;
    Atom xdnd_drop = 0, xdnd_finished = 0, xdnd_selection = 0, xdnd_action_copy = 0;
    Atom xdnd_type_list = 0, xdnd_uri_list = 0;
    Window xdnd_source = 0;
    Atom   xdnd_type = 0;        // what the source offers that we can read
    int    xdnd_version = 0;
    char     dropped[4096] = { 0 };
    uint32_t dropped_len = 0;
    uint32_t dropped_count = 0;
    int      drop_x = 0, drop_y = 0;

    // Double click: X11 does not have one. It hands out presses with a
    // millisecond timestamp and leaves the policy to the toolkit - 400 ms and
    // 4 px is what every toolkit picks.
    unsigned long last_press_time = 0;
    int  last_press_x = -999, last_press_y = -999;
    int  dbl_click = 0;
};

namespace {

uint32_t key_slot(uint32_t keysym) { return (keysym ^ (keysym >> 8)) & 0xFF; }

int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "file:///home/me/a%20file.png\r\n..." -> one absolute path per line. Only
// file:// URIs: a drag from a browser hands over an http one, and downloading
// it is not something a window backend should decide to do.
void xdnd_take_uri_list(dai_window *w, const char *text, size_t n) {
    size_t i = 0;
    while (i < n) {
        size_t e = i;
        while (e < n && text[e] != '\n' && text[e] != '\r') ++e;
        const char *line = text + i;
        size_t len = e - i;
        i = e;
        while (i < n && (text[i] == '\n' || text[i] == '\r')) ++i;
        if (!len || line[0] == '#') continue;
        const char *pfx = "file://";
        size_t pl = 7;
        if (len <= pl || std::strncmp(line, pfx, pl) != 0) continue;
        const char *p = line + pl;
        size_t rem = len - pl;
        // file://host/path is legal; we only ever see an empty host in
        // practice, and a remote one is not a path we could open anyway.
        while (rem && *p != '/') { ++p; --rem; }
        char out[1024];
        size_t o = 0;
        for (size_t k = 0; k < rem && o + 1 < sizeof(out); ++k) {
            if (p[k] == '%' && k + 2 < rem) {
                int hi = hexv(p[k + 1]), lo = hexv(p[k + 2]);
                if (hi >= 0 && lo >= 0) { out[o++] = (char)(hi * 16 + lo); k += 2; continue; }
            }
            out[o++] = p[k];
        }
        out[o] = 0;
        if (!o) continue;
        if (w->dropped_len + o + 2 >= sizeof(w->dropped)) break;
        std::memcpy(w->dropped + w->dropped_len, out, o);
        w->dropped_len += (uint32_t)o;
        w->dropped[w->dropped_len++] = '\n';
        w->dropped[w->dropped_len] = 0;
        ++w->dropped_count;
    }
}

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
    // XDND: announcing version 5 on the window is the whole of "this window
    // accepts drops". Everything else happens in the message pump.
    w->xdnd_aware       = XInternAtom(w->dpy, "XdndAware", False);
    w->xdnd_enter       = XInternAtom(w->dpy, "XdndEnter", False);
    w->xdnd_position    = XInternAtom(w->dpy, "XdndPosition", False);
    w->xdnd_status      = XInternAtom(w->dpy, "XdndStatus", False);
    w->xdnd_drop        = XInternAtom(w->dpy, "XdndDrop", False);
    w->xdnd_finished    = XInternAtom(w->dpy, "XdndFinished", False);
    w->xdnd_selection   = XInternAtom(w->dpy, "XdndSelection", False);
    w->xdnd_action_copy = XInternAtom(w->dpy, "XdndActionCopy", False);
    w->xdnd_type_list   = XInternAtom(w->dpy, "XdndTypeList", False);
    w->xdnd_uri_list    = XInternAtom(w->dpy, "text/uri-list", False);
    {
        long version = 5;
        XChangeProperty(w->dpy, w->win, w->xdnd_aware, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)&version, 1);
    }
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

void dai_window_keep_open(dai_window *w) { if (w) w->open = true; }

int dai_window_poll(dai_window *w) {
    if (!w || !w->open) return 0;
    while (XPending(w->dpy)) {
        XEvent e;
        XNextEvent(w->dpy, &e);
        switch (e.type) {
        case SelectionNotify:
            // The answer to the XConvertSelection sent from XdndDrop: a
            // text/uri-list, which is "file:///path" lines with the awkward
            // characters percent encoded.
            if (e.xselection.property == None) break;
            if (e.xselection.selection != w->xdnd_selection) break;
            {
                Atom type = 0; int fmt = 0;
                unsigned long items = 0, after = 0;
                unsigned char *data = nullptr;
                if (XGetWindowProperty(w->dpy, w->win, w->xdnd_selection, 0, 65536, True,
                                       AnyPropertyType, &type, &fmt, &items, &after,
                                       &data) == Success && data) {
                    xdnd_take_uri_list(w, (const char *)data, (size_t)items);
                    XFree(data);
                }
                if (w->xdnd_source) {
                    XEvent r{};
                    r.xclient.type = ClientMessage;
                    r.xclient.display = w->dpy;
                    r.xclient.window = w->xdnd_source;
                    r.xclient.message_type = w->xdnd_finished;
                    r.xclient.format = 32;
                    r.xclient.data.l[0] = (long)w->win;
                    r.xclient.data.l[1] = 1;
                    r.xclient.data.l[2] = (long)w->xdnd_action_copy;
                    XSendEvent(w->dpy, w->xdnd_source, False, NoEventMask, &r);
                    XFlush(w->dpy);
                }
                w->xdnd_source = 0;
            }
            break;
        case ClientMessage:
            if ((Atom)e.xclient.data.l[0] == w->wm_delete) { w->open = false; break; }
            if (e.xclient.message_type == w->xdnd_enter) {
                w->xdnd_source = (Window)e.xclient.data.l[0];
                w->xdnd_version = (int)(((unsigned long)e.xclient.data.l[1]) >> 24);
                w->xdnd_type = 0;
                if (e.xclient.data.l[1] & 1) {
                    // More than three types: they are in a property instead.
                    Atom type = 0; int fmt = 0;
                    unsigned long items = 0, after = 0;
                    unsigned char *data = nullptr;
                    if (XGetWindowProperty(w->dpy, w->xdnd_source, w->xdnd_type_list, 0, 64,
                                           False, XA_ATOM, &type, &fmt, &items, &after,
                                           &data) == Success && data) {
                        Atom *ats = (Atom *)data;
                        for (unsigned long i = 0; i < items; ++i)
                            if (ats[i] == w->xdnd_uri_list) { w->xdnd_type = ats[i]; break; }
                        XFree(data);
                    }
                } else {
                    for (int i = 2; i <= 4; ++i)
                        if ((Atom)e.xclient.data.l[i] == w->xdnd_uri_list)
                            { w->xdnd_type = w->xdnd_uri_list; break; }
                }
                break;
            }
            if (e.xclient.message_type == w->xdnd_position) {
                // Root coordinates on the wire; the editor thinks in client
                // ones, so translate before storing.
                int rx = (int)(e.xclient.data.l[2] >> 16);
                int ry = (int)(e.xclient.data.l[2] & 0xFFFF);
                int cx = 0, cy = 0; Window child = 0;
                XTranslateCoordinates(w->dpy, DefaultRootWindow(w->dpy), w->win,
                                      rx, ry, &cx, &cy, &child);
                w->drop_x = cx; w->drop_y = cy;
                XEvent r{};
                r.xclient.type = ClientMessage;
                r.xclient.display = w->dpy;
                r.xclient.window = (Window)e.xclient.data.l[0];
                r.xclient.message_type = w->xdnd_status;
                r.xclient.format = 32;
                r.xclient.data.l[0] = (long)w->win;
                r.xclient.data.l[1] = w->xdnd_type ? 1 : 0;   // bit 0: we accept
                r.xclient.data.l[2] = 0;                      // no "silent" rect
                r.xclient.data.l[3] = 0;
                r.xclient.data.l[4] = (long)w->xdnd_action_copy;
                XSendEvent(w->dpy, (Window)e.xclient.data.l[0], False, NoEventMask, &r);
                XFlush(w->dpy);
                break;
            }
            if (e.xclient.message_type == w->xdnd_drop) {
                w->xdnd_source = (Window)e.xclient.data.l[0];
                if (!w->xdnd_type) {
                    XEvent r{};
                    r.xclient.type = ClientMessage;
                    r.xclient.display = w->dpy;
                    r.xclient.window = w->xdnd_source;
                    r.xclient.message_type = w->xdnd_finished;
                    r.xclient.format = 32;
                    r.xclient.data.l[0] = (long)w->win;
                    XSendEvent(w->dpy, w->xdnd_source, False, NoEventMask, &r);
                    w->xdnd_source = 0;
                } else {
                    Time t = (Time)e.xclient.data.l[2];
                    XConvertSelection(w->dpy, w->xdnd_selection, w->xdnd_type,
                                      w->xdnd_selection, w->win, t);
                }
                break;
            }
            break;
        case KeyPress: case KeyRelease: {
            KeySym ks = XLookupKeysym(&e.xkey, 0);
            w->keys[key_slot((uint32_t)ks)] = (e.type == KeyPress);
            if (e.type == KeyPress && ks == XK_Escape) w->open = false;
            if (e.type == KeyPress) {
                // The text half of the same press. Control keys (backspace,
                // tab, enter) stay out of the stream - the fields read those
                // as keys.
                char buf[16];
                KeySym sym = 0;
                int n = XLookupString(&e.xkey, buf, (int)sizeof(buf) - 1, &sym, nullptr);
                if (n > 0) {
                    // XLookupString answers in the locale's encoding - on a
                    // UTF-8 locale that is UTF-8, and the decoder the font
                    // loader already has eats it directly.
                    uint32_t off = 0;
                    while (off < (uint32_t)n) {
                        uint32_t cp = dai_utf8_next(buf, &off);
                        if (!cp) break;
                        if (cp >= 0x20 && cp != 0x7F) {
                            uint32_t next = (w->text_head + 1) % 64;
                            if (next != w->text_tail) {
                                w->text[w->text_head] = cp;
                                w->text_head = next;
                            }
                        }
                    }
                }
            }
            break;
        }
        case ButtonPress:
            // X11 reports the wheel as buttons 4 and 5. They must not land in
            // the button mask, or "middle drag" would trigger on every scroll.
            if (e.xbutton.button == 4) w->wheel += 1.0f;
            else if (e.xbutton.button == 5) w->wheel -= 1.0f;
            else {
                w->buttons |= (1u << e.xbutton.button);
                if (e.xbutton.button == 1) {
                    long dt = (long)(e.xbutton.time - w->last_press_time);
                    int dx = e.xbutton.x - w->last_press_x, dy = e.xbutton.y - w->last_press_y;
                    if (dt > 0 && dt < 400 && dx * dx + dy * dy <= 16) w->dbl_click = 1;
                    w->last_press_time = e.xbutton.time;
                    w->last_press_x = e.xbutton.x;
                    w->last_press_y = e.xbutton.y;
                }
            }
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
    // The source rectangle, clamped into the frame that actually exists: a
    // host that shrank the offscreen target must not hand Vulkan a blit that
    // reads past it, and a device loss is a bad way to learn that.
    int sx = w->src_x, sy = w->src_y, sw = w->src_w, sh = w->src_h;
    if (sw <= 0 || sh <= 0) { sx = 0; sy = 0; sw = (int)r->width; sh = (int)r->height; }
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sx + sw > (int)r->width)  sw = (int)r->width - sx;
    if (sy + sh > (int)r->height) sh = (int)r->height - sy;
    if (sw <= 0 || sh <= 0) { sx = 0; sy = 0; sw = (int)r->width; sh = (int)r->height; }
    blit.srcOffsets[0] = { sx, sy, 0 };
    blit.srcOffsets[1] = { sx + sw, sy + sh, 1 };
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

void dai_window_source_rect(dai_window *w, int x, int y, int width, int height) {
    if (!w) return;
    w->src_x = x; w->src_y = y; w->src_w = width; w->src_h = height;
}

int dai_window_move(dai_window *w, int x, int y) {
    if (!w || !w->dpy || !w->win) return 0;
    XMoveWindow(w->dpy, w->win, x, y);
    XFlush(w->dpy);
    return 1;
}

int dai_window_position(dai_window *w, int *x, int *y) {
    if (!w || !w->dpy || !w->win) return 0;
    // XGetGeometry answers in the PARENT's coordinates, and a window managed
    // by a window manager has been reparented into a frame - so it answers
    // the offset inside the title bar decoration, not the desktop. Translating
    // to the root window is the question that was actually asked.
    Window child = 0;
    int rx = 0, ry = 0;
    if (!XTranslateCoordinates(w->dpy, w->win, DefaultRootWindow(w->dpy), 0, 0, &rx, &ry, &child))
        return 0;
    if (x) *x = rx;
    if (y) *y = ry;
    return 1;
}

void dai_window_resize(dai_window *w, uint32_t width, uint32_t height) {
    if (!w || !w->dpy || !w->win || !width || !height) return;
    XResizeWindow(w->dpy, w->win, width, height);
    XFlush(w->dpy);
}

void dai_window_tool_style(dai_window *w, int on) {
    if (!w || !w->dpy || !w->win) return;
    // _NET_WM_WINDOW_TYPE_UTILITY is the freedesktop way to say "this is a
    // tool palette": no task bar entry, kept above its owner. Setting the
    // override-redirect bit instead would take the window away from the
    // window manager entirely, which also takes away moving it with the
    // keyboard and putting it on another workspace.
    Atom type = XInternAtom(w->dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom util = XInternAtom(w->dpy, on ? "_NET_WM_WINDOW_TYPE_UTILITY"
                                       : "_NET_WM_WINDOW_TYPE_NORMAL", False);
    XChangeProperty(w->dpy, w->win, type, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&util, 1);
    XFlush(w->dpy);
}

uint32_t dai_window_text(dai_window *w, uint32_t *out, uint32_t max) {
    if (!w || !out || !max) return 0;
    uint32_t n = 0;
    while (w->text_tail != w->text_head && n + 1 < max) {
        out[n++] = w->text[w->text_tail];
        w->text_tail = (w->text_tail + 1) % 64;
    }
    out[n] = 0;
    return n;
}

int dai_window_key_down(dai_window *w, uint32_t keysym) {
    return (w && w->keys[key_slot(keysym)]) ? 1 : 0;
}

// X11 selection ownership means answering SelectionRequest events from the
// event loop - that bridge is not built yet. Returning 0 keeps the editor's
// internal clipboard (which is what Windows had before the bridge too).
int dai_window_clipboard_set(dai_window *w, const char *utf8) { (void)w; (void)utf8; return 0; }
uint32_t dai_window_clipboard_get(dai_window *w, char *out, uint32_t max) {
    (void)w; if (out && max) out[0] = 0; return 0;
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

float dai_window_dpi_scale(dai_window *w) {
    (void)w;
    return 1.0f;   /* X11/Wayland report scale through other channels */
}

void dai_window_caption_color(dai_window *w, uint32_t argb) {
    // X11/Wayland paint their own server-side decorations; there is nothing
    // here to recolour. The call exists so hosts compile unchanged.
    (void)w; (void)argb;
}

uint32_t dai_window_dropped_files(dai_window *w, char *out, uint32_t max, int *x, int *y) {
    if (!w || !out || !max) return 0;
    out[0] = 0;
    if (!w->dropped_count) return 0;
    uint32_t n = w->dropped_len;
    if (n >= max) n = max - 1;
    std::memcpy(out, w->dropped, n);
    out[n] = 0;
    if (x) *x = w->drop_x;
    if (y) *y = w->drop_y;
    uint32_t c = w->dropped_count;
    w->dropped_len = 0; w->dropped_count = 0; w->dropped[0] = 0;
    return c;
}

int dai_window_double_click(dai_window *w) {
    if (!w) return 0;
    int v = w->dbl_click;
    w->dbl_click = 0;
    return v;
}

void dai_window_cursor(dai_window *w, int cursor) {
    if (!w || !w->dpy) return;
    if (cursor < 0 || cursor > 6) cursor = 0;
    if (cursor == w->cursor_now) return;          // only talk to X when it changes
    if (!w->cursors[cursor]) {
        // X11 cursor font shapes: XC_left_ptr 68, XC_xterm 152, XC_sb_h_double_arrow
        // 108, XC_sb_v_double_arrow 116, XC_bottom_right_corner 14,
        // XC_bottom_left_corner 12, XC_hand2 60. Spelled as numbers so this
        // file does not need X11/cursorfont.h.
        static const unsigned int SHAPE[7] = { 68, 152, 108, 116, 14, 12, 60 };
        w->cursors[cursor] = XCreateFontCursor(w->dpy, SHAPE[cursor]);
    }
    XDefineCursor(w->dpy, w->win, w->cursors[cursor]);
    w->cursor_now = cursor;
}

void dai_window_size(dai_window *w, uint32_t *width, uint32_t *height) {
    if (!w) return;
    if (width) *width = w->width;
    if (height) *height = w->height;
}

} // extern "C"
