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
    case VK_F1:      return DAI_KEY_F1;
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
        if (wp == VK_ESCAPE) w->open = false;
        return 0;
    case WM_KEYUP: case WM_SYSKEYUP:   set_key(w, wp, false); return 0;
    case WM_MOUSEMOVE:  w->mouse_x = (int)(short)LOWORD(lp); w->mouse_y = (int)(short)HIWORD(lp); return 0;
    case WM_LBUTTONDOWN: w->buttons |= 1u << 1; return 0;
    case WM_LBUTTONUP:   w->buttons &= ~(1u << 1); return 0;
    case WM_MBUTTONDOWN: w->buttons |= 1u << 2; return 0;
    case WM_MBUTTONUP:   w->buttons &= ~(1u << 2); return 0;
    case WM_RBUTTONDOWN: w->buttons |= 1u << 3; return 0;
    case WM_RBUTTONUP:   w->buttons &= ~(1u << 3); return 0;
    case WM_MOUSEWHEEL:  w->wheel += (float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA; return 0;
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
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);   // IDC_* are MAKEINTRESOURCE ordinals
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
        create_swapchain(w);
    } else if (res != VK_SUCCESS) return DAI_ERR_STATE;
    return DAI_OK;
}

int dai_window_key_down(dai_window *w, uint32_t code) { return (w && w->keys[key_slot(code)]) ? 1 : 0; }

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
