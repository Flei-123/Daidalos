#!/usr/bin/env python3
# patch29 - the input, picking, scrolling and asset fixes.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))

def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p29'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# =========================================================== 1. win32 input
# Alt+Tab away with W held and the key never comes back up: the window stops
# getting WM_KEYUP the moment it loses focus, so the editor kept "walking"
# into a direction nobody was holding - or, worse, kept a mouse button down
# and every later click was read as a drag. That is the "sometimes I could
# not move" report.
s = rd('src/rhi_vulkan_window_win32.cpp')
s = sub1(s,
"""    case WM_KEYUP: case WM_SYSKEYUP:   set_key(w, wp, false); return 0;""",
"""    case WM_KEYUP: case WM_SYSKEYUP:   set_key(w, wp, false); return 0;
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
        return 0;""",
'win32 killfocus')

# Capture, so a drag that leaves the window still ends when the button does.
s = sub1(s,
"""    case WM_LBUTTONDBLCLK: w->buttons |= 1u << 1; w->dbl_click = 1; return 0;
    case WM_LBUTTONDOWN: w->buttons |= 1u << 1; return 0;
    case WM_LBUTTONUP:   w->buttons &= ~(1u << 1); return 0;
    case WM_MBUTTONDOWN: w->buttons |= 1u << 2; return 0;
    case WM_MBUTTONUP:   w->buttons &= ~(1u << 2); return 0;
    case WM_RBUTTONDOWN: w->buttons |= 1u << 3; return 0;
    case WM_RBUTTONUP:   w->buttons &= ~(1u << 3); return 0;""",
"""    // SetCapture while any button is held: without it a drag that leaves the
    // client area never sees its own button-up, and the editor is left
    // believing the button is still down for ever after.
    case WM_LBUTTONDBLCLK: w->buttons |= 1u << 1; w->dbl_click = 1; SetCapture(hwnd); return 0;
    case WM_LBUTTONDOWN: w->buttons |= 1u << 1; SetCapture(hwnd); return 0;
    case WM_LBUTTONUP:   w->buttons &= ~(1u << 1); if (!w->buttons && GetCapture() == hwnd) ReleaseCapture(); return 0;
    case WM_MBUTTONDOWN: w->buttons |= 1u << 2; SetCapture(hwnd); return 0;
    case WM_MBUTTONUP:   w->buttons &= ~(1u << 2); if (!w->buttons && GetCapture() == hwnd) ReleaseCapture(); return 0;
    case WM_RBUTTONDOWN: w->buttons |= 1u << 3; SetCapture(hwnd); return 0;
    case WM_RBUTTONUP:   w->buttons &= ~(1u << 3); if (!w->buttons && GetCapture() == hwnd) ReleaseCapture(); return 0;""",
'win32 capture')
wr('src/rhi_vulkan_window_win32.cpp', s)

# ====================================================== 2. picking a camera
# A camera, a light and an empty have nothing to hit, so they got a fixed
# 0.12 m box. Ten metres away that is still a target you can hit by accident
# while clicking at the sky - "I clicked nothing and selected the camera".
# A handle that is a constant number of PIXELS cannot: at distance it shrinks
# with everything else.
s = rd('src/dai_editor.cpp')
s = sub1(s,
"""        if (half.x <= 0.0f && half.y <= 0.0f && half.z <= 0.0f)
            half = dai_vec3{ 0.12f, 0.12f, 0.12f };""",
"""        if (half.x <= 0.0f && half.y <= 0.0f && half.z <= 0.0f) {
            // ~9 px on screen, whatever the distance: the handle is for
            // clicking the icon, not the quadrant of sky it hangs in.
            float dx = wp.x - o.x, dy = wp.y - o.y, dz = wp.z - o.z;
            float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            float per_px = 2.0f * dist * std::tan(e->fov * 3.14159265f / 360.0f) /
                           (e->vh > 1.0f ? e->vh : 1.0f);
            float hs = per_px * 9.0f;
            if (hs < 0.02f) hs = 0.02f;
            if (hs > 0.5f)  hs = 0.5f;
            half = dai_vec3{ hs, hs, hs };
        }""",
'pick handle')
wr('src/dai_editor.cpp', s)

# ============================================ 3. a click outside the scene
# Every press that was not claimed by a widget used to reach the picker, no
# matter where in the window it happened - the empty end of a tab bar, the
# toolbar's background, the gap between two panels. Selecting an object and
# then switching a tab cleared the selection for exactly that reason.
s = rd('src/dai_editor_ui.cpp')
s = sub1(s,
"""    // A press that started over a panel must not fall through to the scene, or
    // clicking a button would also deselect whatever was selected.
    if (over_ui) return 0;""",
"""    // A press that started over a panel must not fall through to the scene, or
    // clicking a button would also deselect whatever was selected.
    if (over_ui) return 0;
    // Nor may a press that is not IN the scene view at all. "Not over a
    // widget" is not the same thing as "in the 3D view": tab bars, the
    // toolbar and the splitters between panels are all neither, and a click
    // on any of them used to pick - which is why changing tabs deselected.
    if (p->view_w > 0.0f && p->view_h > 0.0f &&
        (mx < p->view_x || mx >= p->view_x + p->view_w ||
         my < p->view_y || my >= p->view_y + p->view_h))
        return 0;""",
'viewport bounds')
wr('src/dai_editor_ui.cpp', s)

# ================================================================ 4. scroll
# Trailing spacing counted as content, so a panel whose contents FIT still
# reported a few pixels of overflow: the wheel moved it those few pixels and
# the clamp pulled it back. That is the "scrolling does nothing but jitter".
s = rd('src/dai_ui.cpp')
s = sub1(s,
"""    float &off = ui->scroll_of(f.id);
    float content = (ui->cursor_y + off) - f.start_y;
    float max_off = content - f.h;
    if (max_off < 0.0f) max_off = 0.0f;""",
"""    float &off = ui->scroll_of(f.id);
    // The cursor sits one `spacing` past the last widget - that gap is not
    // content, and counting it made a panel that fits exactly report a few
    // pixels of overflow. The wheel then moved those few pixels and the
    // clamp put them straight back: scrolling that only ever jittered.
    float content = (ui->cursor_y + off) - f.start_y - ui->style.spacing;
    float max_off = content - f.h;
    if (max_off < 2.0f) max_off = 0.0f;   // under two pixels is not scrolling""",
'scroll content')

# A scrollbar you can drag. A 4 px strip that only ever reported the position
# is a widget that looks interactive and is not.
s = sub1(s,
"""    if (max_off > 0.0f) {
        float track_x = f.x + f.w - 4.0f;
        float frac = f.h / (content > 0 ? content : 1.0f);
        float bar_h = f.h * (frac > 1 ? 1 : frac);
        if (bar_h < 16.0f) bar_h = 16.0f;
        float t = off / max_off;
        dai_ui_rect(ui, track_x, f.y, 4.0f, f.h, ui->style.track);
        dai_ui_rect(ui, track_x, f.y + (f.h - bar_h) * t, 4.0f, bar_h, ui->style.button_hover);
    }""",
"""    if (max_off > 0.0f) {
        float track_x = f.x + f.w - 7.0f;
        float track_w = 6.0f;
        float frac = f.h / (content > 0 ? content : 1.0f);
        float bar_h = f.h * (frac > 1 ? 1 : frac);
        if (bar_h < 20.0f) bar_h = 20.0f;
        float t = off / max_off;
        float bar_y = f.y + (f.h - bar_h) * t;
        // Draggable: press anywhere on the bar and the offset follows the
        // pointer, press the track and it jumps there. Both are what every
        // other scrollbar does, and neither existed.
        float mx = ui->input.mouse_x, my = ui->input.mouse_y;
        bool over = mx >= track_x - 3.0f && mx < track_x + track_w + 3.0f &&
                    my >= f.y && my < f.y + f.h;
        bool pressed = ui->input.mouse_down && !ui->prev.mouse_down;
        if (over && pressed && inside_chk(ui, f.x, f.y, f.w, f.h)) {
            ui->scroll_drag_id = f.id;
            ui->scroll_grab = (my >= bar_y && my < bar_y + bar_h) ? (my - bar_y) : bar_h * 0.5f;
        }
        if (!ui->input.mouse_down) ui->scroll_drag_id = 0;
        if (ui->scroll_drag_id == f.id && ui->input.mouse_down) {
            float span = f.h - bar_h;
            float rel = span > 0.5f ? (my - ui->scroll_grab - f.y) / span : 0.0f;
            if (rel < 0.0f) rel = 0.0f;
            if (rel > 1.0f) rel = 1.0f;
            off = rel * max_off;
            bar_y = f.y + span * rel;
            ui->mouse_over_ui = true;
        }
        uint32_t bc = (over || ui->scroll_drag_id == f.id) ? ui->style.accent
                                                           : ui->style.button_hover;
        dai_ui_rrect(ui, track_x, f.y, track_w, f.h, 3.0f, ui->style.track);
        dai_ui_rrect(ui, track_x, bar_y, track_w, bar_h, 3.0f, bc);
    } else if (ui->scroll_drag_id == f.id) {
        ui->scroll_drag_id = 0;
    }""",
'scrollbar drag')

s = sub1(s, "    std::vector<ScrollFrame> scroll_stack;\n    float out_scale",
            "    std::vector<ScrollFrame> scroll_stack;\n    uint64_t scroll_drag_id = 0;   // the bar the pointer is holding\n    float    scroll_grab = 0.0f;\n    float out_scale",
         'scroll drag state')
wr('src/dai_ui.cpp', s)

# ================================================= 5. what the browser shows
# The Project window listed .glb, .gltf and .js and nothing else - so a C++
# behaviour, once created, was invisible, and a prefab saved out of the
# hierarchy could never be found again. Both were reported as "does not work",
# and both were only ever "cannot be seen".
s = rd('src/dai_assets.cpp')
s = sub1(s,
"""            return e == "glb" || e == "gltf" || e == "js";""",
"""            // Placeable (models), attachable (scripts), instantiable
            // (prefabs) and openable (text, images, sound). A browser that
            // hides a file the editor just wrote is a browser that lies.
            return e == "glb" || e == "gltf" || e == "js" || e == "cpp" ||
                   e == "daidalos" || e == "hpp" || e == "h" || e == "json" ||
                   e == "txt" || e == "md" || e == "png" || e == "jpg" ||
                   e == "jpeg" || e == "wav" || e == "ogg" || e == "glsl";""",
'assets loadable')
wr('src/dai_assets.cpp', s)

# ============================================ 6. the host: scripts, prefabs
s = rd('examples/editor_demo.cpp')
# A .js that already exists was silently overwritten - the editor asks for
# NewScript, NewScript (1), NewScript (2) and takes the first name the host
# accepts, so "yes, done" on an existing file meant the second script ate the
# first one.
s = sub1(s,
"""    std::snprintf(path, sizeof(path), "%s/%s.js", g_assets_dir, name);
    make_parent_dirs(path);""",
"""    std::snprintf(path, sizeof(path), "%s/%s.js", g_assets_dir, name);
    // Refuse a name that is taken. The editor walks NewScript, NewScript (1),
    // ... and takes the first one the host accepts; saying yes to a file that
    // exists meant the new script silently replaced the old one.
    if (FILE *ex = std::fopen(path, "rb")) { std::fclose(ex); return 0; }
    make_parent_dirs(path);""",
'js exists check')

# A prefab picked in the Project window is INSTANTIATED, not handed to the
# model loader, which has never heard of .daidalos.
s = sub1(s,
"""            if (dai_editor_ui_take_asset(panels, &pick, &as_tree) && pick) {
                if (dai_assets_model_blocking(assets, pick)) {""",
"""            if (dai_editor_ui_take_asset(panels, &pick, &as_tree) && pick) {
                size_t pl = std::strlen(pick);
                if (pl > 9 && std::strcmp(pick + pl - 9, ".daidalos") == 0) {
                    // A prefab is a scene file: instantiate it under the
                    // current selection, select the copy, done. The model
                    // loader has never heard of one, which is why dropping
                    // a prefab in used to do nothing at all.
                    char full[700];
                    std::snprintf(full, sizeof(full), "%s/%s", g_assets_dir, pick);
                    char perr[256] = { 0 };
                    dai_node made = dai_doc_prefab_instantiate(doc, full, DAI_INVALID_NODE,
                                                               perr, sizeof(perr));
                    if (made) {
                        dai_doc_sync_apply(sync);
                        dai_editor_select(ed, made, 0);
                        dai_editor_ui_toast(panels, "prefab placed", 1.5f);
                    } else {
                        char m[400];
                        std::snprintf(m, sizeof(m), "prefab failed: %s",
                                      perr[0] ? perr : "unreadable");
                        dai_editor_ui_log(panels, 2, m);
                    }
                } else if (dai_assets_model_blocking(assets, pick)) {""",
'prefab instantiate')
wr('examples/editor_demo.cpp', s)
print('patch29 ok')
