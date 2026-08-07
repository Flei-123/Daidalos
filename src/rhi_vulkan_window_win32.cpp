// Daidalos - window backend #3: Win32.
//
// Same four entry points as the X11 and Wayland backends, same contract:
// present is a BLIT of the finished offscreen frame. Selected at build time
// (DAI_WINDOW=win32); nothing above it changes.
//
// HONESTY NOTE: this file is cross compiled with mingw-w64 on the build server
// and has NOT been run on Windows here - there is no Windows machine in this
// setup with a compiler and a Vulkan loader. The structure is identical to the
// two backends that ARE tested headless (X11 under Xvfb, Wayland under a
// headless weston), and everything platform specific is confined to window
// creation, the message pump and the surface call. Treat first light on real
// hardware as the remaining step.

#include "rhi_vulkan.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>          /* DragAcceptFiles / DragQueryFile: WM_DROPFILES */
#include <vulkan/vulkan_win32.h>

#include <cstdio>
#include <cstring>

struct dai_window {
    dai_renderer *r = nullptr;
    float wheel = 0.0f;
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    bool open = true;
    uint32_t width = 0, height = 0;
    bool resized = false;

    // Which part of the offscreen frame this window shows; 0 width or height
    // means all of it - what every window did before torn off panels needed a
    // strip of their own.
    int src_x = 0, src_y = 0, src_w = 0, src_h = 0;

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    std::vector<VkImage> images;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE, blitted = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    bool keys[256] = {};
    int mouse_x = 0, mouse_y = 0;
    uint32_t buttons = 0;

    // Text events, as they arrived: TranslateMessage turns WM_KEYDOWN into
    // WM_CHAR with shift, caps lock and dead keys already applied - exactly
    // what a text field wants, and nothing like "is the key held".
    uint32_t text[64] = { 0 };
    uint32_t text_head = 0, text_tail = 0;   // ring: head writes, tail reads

    // The pointer shape. Windows resets it to the window class's cursor on
    // every mouse move unless WM_SETCURSOR is answered, which is why this is a
    // stored handle and not a one off SetCursor call.
    HCURSOR cursor = nullptr;
    int     cursor_id = -1;
    int     dbl_click = 0;

    // What Explorer dropped, held until the host asks for it. A fixed buffer
    // rather than a vector of strings: a drop is a handful of paths, this file
    // allocates nothing anywhere else, and a queue that cannot grow cannot be
    // a memory bug in a message handler.
    char     dropped[4096] = { 0 };
    uint32_t dropped_len = 0;
    uint32_t dropped_count = 0;
    int      drop_x = 0, drop_y = 0;
};

namespace {

uint32_t key_slot(uint32_t code) { return (code ^ (code >> 8)) & 0xFF; }

// Windows hands out virtual key codes; the engine's API is dai_key. Translating
// here rather than in the host is the whole point - otherwise every program
// would need an #ifdef around every key it cares about.
//
// Letters and digits are the easy half: VK_A..VK_Z are the ASCII capitals, so
// lower casing them lands exactly on DAI_KEY_A..Z. The rest is a small table.
uint32_t dai_key_from_vk(WPARAM vk) {
    if (vk >= 'A' && vk <= 'Z') return (uint32_t)vk + 0x20;   // -> lower case
    if (vk >= '0' && vk <= '9') return (uint32_t)vk;
    switch (vk) {
    case VK_SPACE:   return DAI_KEY_SPACE;
    case VK_ESCAPE:  return DAI_KEY_ESCAPE;
    case VK_TAB:     return DAI_KEY_TAB;
    case VK_RETURN:  return DAI_KEY_RETURN;
    case VK_BACK:    return DAI_KEY_BACKSPACE;
    case VK_DELETE:  return DAI_KEY_DELETE;
    case VK_LEFT:    return DAI_KEY_LEFT;
    case VK_UP:      return DAI_KEY_UP;
    case VK_RIGHT:   return DAI_KEY_RIGHT;
    case VK_DOWN:    return DAI_KEY_DOWN;
    case VK_HOME:    return DAI_KEY_HOME;
    case VK_END:     return DAI_KEY_END;
    case VK_F1:      return DAI_KEY_F1;
    case VK_F2:      return DAI_KEY_F2;
    case VK_F3:      return DAI_KEY_F3;
    case VK_F4:      return DAI_KEY_F4;
    case VK_F5:      return DAI_KEY_F5;
    case VK_LSHIFT:  return DAI_KEY_SHIFT_L;
    case VK_RSHIFT:  return DAI_KEY_SHIFT_R;
    case VK_LCONTROL:return DAI_KEY_CTRL_L;
    case VK_RCONTROL:return DAI_KEY_CTRL_R;
    case VK_LMENU:   return DAI_KEY_ALT_L;
    case VK_RMENU:   return DAI_KEY_ALT_R;
    default: return 0;
    }
}

// A plain WM_KEYDOWN for shift/ctrl/alt reports the side-less VK_SHIFT and
// friends, so both sides get set. A host asking "either shift" then works
// without knowing which key the user actually pressed.
void set_key(dai_window *w, WPARAM vk, bool down);

void set_key(dai_window *w, WPARAM vk, bool down) {
    // The side-less modifiers: set both, so "is shift held" is one question.
    switch (vk) {
    case VK_SHIFT:
        w->keys[key_slot(DAI_KEY_SHIFT_L)] = down;
        w->keys[key_slot(DAI_KEY_SHIFT_R)] = down;
        return;
    case VK_CONTROL:
        w->keys[key_slot(DAI_KEY_CTRL_L)] = down;
        w->keys[key_slot(DAI_KEY_CTRL_R)] = down;
        return;
    case VK_MENU:
        w->keys[key_slot(DAI_KEY_ALT_L)] = down;
        w->keys[key_slot(DAI_KEY_ALT_R)] = down;
        return;
    default: break;
    }
    uint32_t k = dai_key_from_vk(vk);
    if (k) w->keys[key_slot(k)] = down;
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    dai_window *w = (dai_window *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!w) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_CLOSE: case WM_DESTROY: w->open = false; return 0;
    case WM_SIZE:
        if (LOWORD(lp) && HIWORD(lp)) {
            w->width = LOWORD(lp); w->height = HIWORD(lp); w->resized = true;
        }
        return 0;
    // Virtual key codes are what the host gets here, mapped through the same
    // hash the other backends use, so dai_window_key_down stays one function.
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        set_key(w, wp, true);
        // Escape is NOT quit. It cancels a menu, a rename, a drag - and an
        // editor that shuts down when you back out of a text field loses
        // work for a living.
        return 0;
    case WM_KEYUP: case WM_SYSKEYUP:   set_key(w, wp, false); return 0;
    // Focus left: every key and every button is released as far as this
    // window is concerned. It will never see the KEYUP that follows, and a
    // key stuck down is an editor that walks by itself and eats every
    // shortcut - which is exactly how "I suddenly cannot move" happens.
    case WM_KILLFOCUS:
        std::memset(w->keys, 0, sizeof(w->keys));
        w->buttons = 0;
        if (GetCapture() == hwnd) ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        w->buttons = 0;
        return 0;
    case WM_CHAR: {
        // Control characters arrive here too (backspace, tab, enter) - the
        // fields handle those as keys, so only printable code points go into
        // the text stream.
        uint32_t cp = (uint32_t)wp;
        if (cp >= 0x20 && cp != 0x7F) {
            uint32_t next = (w->text_head + 1) % 64;
            if (next != w->text_tail) {     // a full ring drops the oldest input
                w->text[w->text_head] = cp;
                w->text_head = next;
            }
        }
        return 0;
    }
    case WM_SETCURSOR:
        // Only inside the client area: the frame's own resize cursors are the
        // window manager's business.
        if (LOWORD(lp) == HTCLIENT && w->cursor) { SetCursor(w->cursor); return TRUE; }
        break;
    case WM_MOUSEMOVE:  w->mouse_x = (int)(short)LOWORD(lp); w->mouse_y = (int)(short)HIWORD(lp); return 0;
    // SetCapture while any button is held: without it a drag that leaves the
    // client area never sees its own button-up, and the editor is left
    // believing the button is still down for ever after.
    case WM_LBUTTONDBLCLK: w->buttons |= 1u << 1; w->dbl_click = 1; SetCapture(hwnd); return 0;
    case WM_LBUTTONDOWN: w->buttons |= 1u << 1; SetCapture(hwnd); return 0;
    case WM_LBUTTONUP:   w->buttons &= ~(1u << 1); if (!w->buttons && GetCapture() == hwnd) ReleaseCapture(); return 0;
    case WM_MBUTTONDOWN: w->buttons |= 1u << 2; SetCapture(hwnd); return 0;
    case WM_MBUTTONUP:   w->buttons &= ~(1u << 2); if (!w->buttons && GetCapture() == hwnd) ReleaseCapture(); return 0;
    case WM_RBUTTONDOWN: w->buttons |= 1u << 3; SetCapture(hwnd); return 0;
    case WM_RBUTTONUP:   w->buttons &= ~(1u << 3); if (!w->buttons && GetCapture() == hwnd) ReleaseCapture(); return 0;
    case WM_MOUSEWHEEL:  w->wheel += (float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA; return 0;
    case WM_DROPFILES: {
        HDROP hd = (HDROP)wp;
        POINT pt{ 0, 0 };
        DragQueryPoint(hd, &pt);            // already client coordinates
        w->drop_x = pt.x; w->drop_y = pt.y;
        UINT n = DragQueryFileW(hd, 0xFFFFFFFFu, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            wchar_t wpath[MAX_PATH * 2];
            UINT len = DragQueryFileW(hd, i, wpath, (UINT)(sizeof(wpath) / sizeof(wpath[0])));
            if (!len) continue;
            char u8[MAX_PATH * 4];
            int got = WideCharToMultiByte(CP_UTF8, 0, wpath, (int)len, u8,
                                          (int)sizeof(u8) - 1, nullptr, nullptr);
            if (got <= 0) continue;
            u8[got] = 0;
            // Windows hands out backslashes; everything above this line speaks
            // '/'. Normalising here means the editor never has to care which
            // window system a path came from.
            for (int c = 0; c < got; ++c) if (u8[c] == '\\') u8[c] = '/';
            uint32_t need = (uint32_t)got + 1;
            if (w->dropped_len + need >= sizeof(w->dropped)) break;
            std::memcpy(w->dropped + w->dropped_len, u8, (size_t)got);
            w->dropped_len += (uint32_t)got;
            w->dropped[w->dropped_len++] = '\n';
            w->dropped[w->dropped_len] = 0;
            ++w->dropped_count;
        }
        DragFinish(hd);
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool create_swapchain(dai_window *w) {
    dai_renderer *r = w->r;
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->phys, w->surface, &caps);

    uint32_t fmt_n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(r->phys, w->surface, &fmt_n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmt_n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(r->phys, w->surface, &fmt_n, formats.data());
    VkSurfaceFormatKHR chosen = formats.empty()
        ? VkSurfaceFormatKHR{ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR } : formats[0];
    for (const auto &f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) { chosen = f; break; }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) { extent.width = w->width; extent.height = w->height; }
    if (!extent.width || !extent.height) return false;      // minimised

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
    if (!r->has_surface_ext) return bail("instance was created without VK_KHR_win32_surface");
    if (!r->has_swapchain_ext) return bail("device does not support VK_KHR_swapchain");

    // Per monitor DPI awareness, before the first window exists.
    //
    // Without it Windows renders the window at 100% and stretches the result to
    // the monitor's scaling - on a 150% display every pixel of a 13 px font
    // becomes 1.5 pixels of blur, and the interface looks twice as large as it
    // was laid out to be. It is also why the editor looked "not compact" no
    // matter how small the font was set: it was not the font, it was a
    // magnifying glass over the whole window.
    //
    // Resolved dynamically because SetProcessDpiAwarenessContext only exists on
    // Windows 10 1703 and later, and linking it statically would refuse to
    // start on anything older.
    {
        static bool dpi_done = false;
        if (!dpi_done) {
            dpi_done = true;
            using SetCtxFn = BOOL(WINAPI *)(void *);
            if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
                auto set_ctx = (SetCtxFn)(void *)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
                // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
                if (!set_ctx || !set_ctx((void *)(intptr_t)-4)) {
                    using SetAwareFn = BOOL(WINAPI *)(void);
                    if (auto legacy = (SetAwareFn)(void *)GetProcAddress(user32, "SetProcessDPIAware"))
                        legacy();
                }
            }
        }
    }

    dai_window *w = new dai_window();
    w->r = r;
    w->width = width ? width : r->width;
    w->height = height ? height : r->height;
    w->inst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = w->inst;
    wc.style |= CS_DBLCLKS;                                 // WM_LBUTTONDBLCLK at all
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);   // IDC_* are MAKEINTRESOURCE ordinals
    // Icon 1 in the exe's resources, when the host embedded one (editor_demo
    // links daidalos.res). No resource, no icon - LoadIcon fails soft.
    HICON app_icon = LoadIconW(w->inst, MAKEINTRESOURCEW(1));
    if (app_icon) { wc.hIcon = app_icon; wc.hIconSm = app_icon; }
    wc.lpszClassName = L"DaidalosWindow";
    RegisterClassExW(&wc);          // duplicate registration is harmless

    RECT rect{ 0, 0, (LONG)w->width, (LONG)w->height };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    wchar_t wtitle[128];
    MultiByteToWideChar(CP_UTF8, 0, title ? title : "Daidalos", -1, wtitle, 128);
    w->hwnd = CreateWindowExW(0, L"DaidalosWindow", wtitle, WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              rect.right - rect.left, rect.bottom - rect.top,
                              nullptr, nullptr, w->inst, nullptr);
    if (!w->hwnd) { delete w; return bail("CreateWindowEx failed"); }
    SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, (LONG_PTR)w);
    // "This window takes files from Explorer." Without it WM_DROPFILES is
    // never sent and the drop handler below is dead code.
    DragAcceptFiles(w->hwnd, TRUE);
    ShowWindow(w->hwnd, SW_SHOW);

    VkWin32SurfaceCreateInfoKHR sci{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    sci.hinstance = w->inst;
    sci.hwnd = w->hwnd;
    if (vkCreateWin32SurfaceKHR(r->instance, &sci, nullptr, &w->surface) != VK_SUCCESS)
        { dai_window_close(w); return bail("vkCreateWin32SurfaceKHR failed"); }

    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(r->phys, r->qfam, w->surface, &supported);
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
    if (r && r->instance && w->surface) vkDestroySurfaceKHR(r->instance, w->surface, nullptr);
    if (w->hwnd) DestroyWindow(w->hwnd);
    delete w;
}

void dai_window_keep_open(dai_window *w) { if (w) w->open = true; }

int dai_window_poll(dai_window *w) {
    if (!w || !w->open) return 0;
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (w->resized) {
        w->resized = false;
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
    // Only the part of the frame this window shows - see dai_window_source_rect.
    // Clamped into the frame that exists, because a blit that reads past the
    // offscreen target is a device loss, not an error code.
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
        create_swapchain(w);
    } else if (res != VK_SUCCESS) return DAI_ERR_STATE;
    return DAI_OK;
}

int dai_window_key_down(dai_window *w, uint32_t code) { return (w && w->keys[key_slot(code)]) ? 1 : 0; }

// The OS clipboard, as UTF-8 out here and UTF-16 towards Windows - the editor
// keeps its own clipboard for kinds (node vs component vs log line), but copy
// that cannot leave the process is copy you have to retype.
int dai_window_clipboard_set(dai_window *w, const char *utf8) {
    if (!w || !utf8) return 0;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (wlen <= 0) return 0;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)wlen * sizeof(wchar_t));
    if (!mem) return 0;
    wchar_t *dst = (wchar_t *)GlobalLock(mem);
    if (!dst) { GlobalFree(mem); return 0; }
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, dst, wlen);
    GlobalUnlock(mem);
    if (!OpenClipboard(nullptr)) { GlobalFree(mem); return 0; }
    EmptyClipboard();
    HANDLE put = SetClipboardData(CF_UNICODETEXT, mem);
    CloseClipboard();
    if (!put) { GlobalFree(mem); return 0; }
    return 1;
}

uint32_t dai_window_clipboard_get(dai_window *w, char *out, uint32_t max) {
    if (!w || !out || !max) return 0;
    out[0] = 0;
    if (!OpenClipboard(nullptr)) return 0;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    uint32_t n = 0;
    if (h) {
        const wchar_t *ws = (const wchar_t *)GlobalLock(h);
        if (ws) {
            int need = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
            if (need > 0 && (uint32_t)need <= max)
                n = (uint32_t)WideCharToMultiByte(CP_UTF8, 0, ws, -1, out, (int)max, nullptr, nullptr) - 1;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return n;
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

void dai_window_cursor(dai_window *w, int cursor) {
    if (!w) return;
    if (cursor < 0 || cursor > 6) cursor = 0;
    if (cursor == w->cursor_id) return;
    static const LPCWSTR SHAPE[7] = {
        (LPCWSTR)IDC_ARROW, (LPCWSTR)IDC_IBEAM, (LPCWSTR)IDC_SIZEWE, (LPCWSTR)IDC_SIZENS,
        (LPCWSTR)IDC_SIZENWSE, (LPCWSTR)IDC_SIZENESW, (LPCWSTR)IDC_HAND
    };
    HCURSOR h = LoadCursorW(nullptr, SHAPE[cursor]);
    if (!h) return;
    w->cursor = h;
    w->cursor_id = cursor;
    SetCursor(h);
}

float dai_window_dpi_scale(dai_window *w) {
    if (!w || !w->hwnd) return 1.0f;
    // GetDpiForWindow is Windows 10 1607+. Resolved dynamically for the same
    // reason SetProcessDpiAwarenessContext is: linking it would refuse to
    // start on anything older, and older is exactly where 96 dpi is right.
    UINT dpi = 0;
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        typedef UINT (WINAPI *GetDpiFn)(HWND);
        if (auto fn = (GetDpiFn)(void *)GetProcAddress(user32, "GetDpiForWindow"))
            dpi = fn(w->hwnd);
    }
    if (!dpi) {
        HDC dc = GetDC(w->hwnd);
        if (dc) { dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(w->hwnd, dc); }
    }
    if (!dpi) return 1.0f;
    float sc = (float)dpi / 96.0f;
    if (sc < 0.5f) sc = 0.5f;
    if (sc > 4.0f) sc = 4.0f;
    return sc;
}

void dai_window_caption_color(dai_window *w, uint32_t argb) {
    if (!w || !w->hwnd) return;
    // Windows 11's DWMWA_CAPTION_COLOR wants a COLORREF (0x00BBGGRR); the UI
    // packs 0xAABBGGRR, so it is the low three bytes as they are. Attribute
    // 20 is the dark-mode switch, 35 the caption colour, 36 the text colour -
    // named constants exist only in very new SDKs, the numbers are stable.
    COLORREF caption = argb & 0x00FFFFFFu;
    // Pick caption text that stays readable on whatever the theme chose.
    uint32_t r = argb & 0xFF, g = (argb >> 8) & 0xFF, b = (argb >> 16) & 0xFF;
    uint32_t lum = (r * 299 + g * 587 + b * 114) / 1000;
    COLORREF text = lum > 140 ? 0x00000000 : 0x00FFFFFF;
    BOOL dark = lum > 140 ? FALSE : TRUE;
    HMODULE dwm = GetModuleHandleW(L"dwmapi.dll");
    if (!dwm) dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    typedef HRESULT (WINAPI *SetAttr)(HWND, DWORD, LPCVOID, DWORD);
    SetAttr set_attr = (SetAttr)GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (!set_attr) return;
    set_attr(w->hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    set_attr(w->hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
    set_attr(w->hwnd, 36 /*DWMWA_TEXT_COLOR*/, &text, sizeof(text));
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

void dai_window_source_rect(dai_window *w, int x, int y, int width, int height) {
    if (!w) return;
    w->src_x = x; w->src_y = y; w->src_w = width; w->src_h = height;
}

int dai_window_move(dai_window *w, int x, int y) {
    if (!w || !w->hwnd) return 0;
    // SWP_NOZORDER: moving a panel must not raise it over the editor, or
    // dragging one across the screen shuffles the whole stack behind it.
    SetWindowPos(w->hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    return 1;
}

int dai_window_position(dai_window *w, int *x, int *y) {
    if (!w || !w->hwnd) return 0;
    RECT rc{};
    if (!GetWindowRect(w->hwnd, &rc)) return 0;
    if (x) *x = rc.left;
    if (y) *y = rc.top;
    return 1;
}

void dai_window_resize(dai_window *w, uint32_t width, uint32_t height) {
    if (!w || !w->hwnd || !width || !height) return;
    // The CLIENT area is what the caller means; a window styled with a frame
    // is larger than the picture it holds, and sizing to the outside makes
    // every torn off panel a few pixels short.
    RECT rc{ 0, 0, (LONG)width, (LONG)height };
    DWORD style = (DWORD)GetWindowLongPtrW(w->hwnd, GWL_STYLE);
    DWORD ex    = (DWORD)GetWindowLongPtrW(w->hwnd, GWL_EXSTYLE);
    AdjustWindowRectEx(&rc, style, FALSE, ex);
    SetWindowPos(w->hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void dai_window_tool_style(dai_window *w, int on) {
    if (!w || !w->hwnd) return;
    // WS_EX_TOOLWINDOW: no task bar button, thin caption - Windows' own name
    // for a floating palette. WS_EX_TOPMOST keeps it over the editor, which is
    // the behaviour anyone who has torn a panel off in Unity expects.
    LONG_PTR ex = GetWindowLongPtrW(w->hwnd, GWL_EXSTYLE);
    if (on) ex |= WS_EX_TOOLWINDOW; else ex &= ~(LONG_PTR)WS_EX_TOOLWINDOW;
    SetWindowLongPtrW(w->hwnd, GWL_EXSTYLE, ex);
    SetWindowPos(w->hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void dai_window_size(dai_window *w, uint32_t *width, uint32_t *height) {
    if (!w) return;
    if (width) *width = w->width;
    if (height) *height = w->height;
}

} // extern "C"
