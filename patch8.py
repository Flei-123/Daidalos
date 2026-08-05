# -*- coding: utf-8 -*-
import sys, io

def rw(p): return io.open(p, encoding='utf-8').read()
def wr(p, s): io.open(p, 'w', encoding='utf-8').write(s)
def sub1(s, old, new, tag):
    if old not in s: print("MISS", tag); sys.exit(1)
    if s.count(old) != 1: print("AMBIG", tag, s.count(old)); sys.exit(1)
    print("ok", tag); return s.replace(old, new)

# ------------------------------------------------- window: report the DPI
P = 'include/dai_render.h'
s = rw(P)
s = sub1(s, "DAI_API void dai_window_caption_color(dai_window *w, uint32_t argb);",
"""DAI_API void dai_window_caption_color(dai_window *w, uint32_t argb);
/* The display's scale factor: 1.0 at 96 dpi, 1.5 at 150%, 2.0 at 200%.
 *
 * A DPI aware program renders in REAL pixels, which is what makes it sharp -
 * and also what makes a 13 px font 13 real pixels tall on a 150% display,
 * i.e. two thirds the size of every other program's text. Sharp and unreadably
 * small is not the goal; sharp at the size the user asked their desktop for
 * is. The host multiplies its interface metrics by this. */
DAI_API float dai_window_dpi_scale(dai_window *w);""", "dpi decl")
wr(P, s)

P = 'src/rhi_vulkan_window_win32.cpp'
s = rw(P)
s = sub1(s, "void dai_window_caption_color(dai_window *w, uint32_t argb) {",
"""float dai_window_dpi_scale(dai_window *w) {
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

void dai_window_caption_color(dai_window *w, uint32_t argb) {""", "dpi win32")
wr(P, s)

for P in ('src/rhi_vulkan_window.cpp', 'src/rhi_vulkan_window_wayland.cpp'):
    s = rw(P)
    s = sub1(s, "void dai_window_caption_color(dai_window *w, uint32_t argb) {",
"""float dai_window_dpi_scale(dai_window *w) {
    (void)w;
    return 1.0f;   /* X11/Wayland report scale through other channels */
}

void dai_window_caption_color(dai_window *w, uint32_t argb) {""", "dpi " + P)
    wr(P, s)

# ------------------------------------------------------------- dai_dock.cpp
P = 'src/dai_dock.cpp'
s = rw(P)
s = sub1(s,
"""const float DROP_ZONE_FRAC = 0.30f;
const float DROP_ZONE_MAX  = 120.0f;""",
"""// Wider than they look: the drop zone has to be findable while a tab is
// under the cursor hiding the pointer, and 30% of a narrow inspector column
// is 60 pixels of target for a 240 pixel gesture.
const float DROP_ZONE_FRAC = 0.34f;
const float DROP_ZONE_MAX  = 160.0f;""", "drop zone size")

# the dock cross: show the four splits + centre as real targets
s = sub1(s,
"""    dai_ui_layer_push(ui, DAI_LAYER_DOCK_PREVIEW);
    uint32_t tint = (st->accent & 0x00FFFFFFu) | 0x60000000u;
    dai_ui_rect(ui, d->drag_preview.x, d->drag_preview.y, d->drag_preview.w, d->drag_preview.h, tint);
    dai_ui_rect_outline(ui, d->drag_preview.x, d->drag_preview.y, d->drag_preview.w,
                        d->drag_preview.h, 2.0f, st->accent);""",
"""    dai_ui_layer_push(ui, DAI_LAYER_DOCK_PREVIEW);
    uint32_t tint = (st->accent & 0x00FFFFFFu) | 0x60000000u;
    dai_ui_rect(ui, d->drag_preview.x, d->drag_preview.y, d->drag_preview.w, d->drag_preview.h, tint);
    dai_ui_rect_outline(ui, d->drag_preview.x, d->drag_preview.y, d->drag_preview.w,
                        d->drag_preview.h, 2.0f, st->accent);

    // The dock cross, over the leaf the pointer is on. Without it the four
    // split zones are invisible geography you have to find by waving the
    // mouse - which is why "I still cannot put the console under the
    // inspector" is a UI bug and not a missing feature: the feature was
    // there, the target was not.
    if (d->drop_node) {
        Rect r = d->drop_node->rect;
        float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
        const float B = 26.0f, G = 3.0f;         // button size, gap
        struct Slot { float x, y; int kind; };
        const Slot slots[5] = {
            { cx - B * 0.5f,           cy - B * 0.5f,           0 },   // centre: as a tab
            { cx - B * 1.5f - G,       cy - B * 0.5f,           1 },   // left
            { cx + B * 0.5f + G,       cy - B * 0.5f,           2 },   // right
            { cx - B * 0.5f,           cy - B * 1.5f - G,       3 },   // top
            { cx - B * 0.5f,           cy + B * 0.5f + G,       4 },   // bottom
        };
        for (const Slot &sl : slots) {
            bool on = d->drag_kind == sl.kind;
            uint32_t bg = on ? st->accent : ((st->chrome & 0x00FFFFFFu) | 0xD8000000u);
            dai_ui_rrect(ui, sl.x, sl.y, B, B, 4.0f, bg);
            dai_ui_rect_outline(ui, sl.x, sl.y, B, B, 1.0f, on ? 0xFFFFFFFFu : st->panel_border);
            // A little picture of where the panel would land inside the leaf.
            uint32_t fg = on ? 0xFF101010u : st->text_dim;
            float ix = sl.x + 5.0f, iy = sl.y + 5.0f, iw = B - 10.0f, ih = B - 10.0f;
            switch (sl.kind) {
            case 1: dai_ui_rect(ui, ix, iy, iw * 0.5f, ih, fg); break;
            case 2: dai_ui_rect(ui, ix + iw * 0.5f, iy, iw * 0.5f, ih, fg); break;
            case 3: dai_ui_rect(ui, ix, iy, iw, ih * 0.5f, fg); break;
            case 4: dai_ui_rect(ui, ix, iy + ih * 0.5f, iw, ih * 0.5f, fg); break;
            default: dai_ui_rect(ui, ix, iy, iw, 3.0f, fg);
                     dai_ui_rect_outline(ui, ix, iy, iw, ih, 1.0f, fg); break;
            }
        }
    }""", "dock cross")
wr(P, s)

# ------------------------------------------------------- editor_demo.cpp
P = 'examples/editor_demo.cpp'
s = rw(P)
s = sub1(s,
"""        if (ww != dai_render_width(r) || wh != dai_render_height(r)) {
            if (dai_render_resize(r, ww, wh) == DAI_OK)
                dai_editor_camera_viewport(ed, (float)ww, (float)wh);
        }""",
"""        if (ww != dai_render_width(r) || wh != dai_render_height(r)) {
            if (dai_render_resize(r, ww, wh) == DAI_OK)
                dai_editor_camera_viewport(ed, (float)ww, (float)wh);
            // The pointer is reported in RENDER pixels, and that scale just
            // changed - so the same physical mouse position is a different
            // number this frame. A resize is done with the button held, so
            // without re-anchoring the camera spins while the window grows.
            int amx = 0, amy = 0;
            dai_window_mouse(win, &amx, &amy, nullptr);
            dai_editor_cam_anchor(ed, (float)amx, (float)amy);
        }""", "cam anchor on resize")

# DPI: scale the interface to the desktop's scale factor, once, unless the
# user already chose a size.
s = sub1(s,
"""    g_ui_scale_out = &prefs.ui_scale;""",
"""    // A 150% desktop asks for 150% interface. The renderer already draws in
    // real pixels (that is what makes it sharp); without this the whole editor
    // is 2/3 the size of every other window on the machine, which reads as
    // "the resolution is halved and it looks blurry".
    {
        float dpi = dai_window_dpi_scale(win);
        if (dpi > 1.02f && prefs.ui_scale > 0.98f && prefs.ui_scale < 1.02f) {
            prefs.ui_scale = dpi;
            std::printf("dpi: display at %.0f%%, ui scaled to match\\n", dpi * 100.0f);
        }
    }
    g_ui_scale_out = &prefs.ui_scale;""", "dpi ui scale")
wr(P, s)
print("patch8 done")
