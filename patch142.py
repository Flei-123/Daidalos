#!/usr/bin/env python3
# Runde 28c - acht Griffe wie in Unity statt einem, und {variablen} im Label.
import io, os, sys, shutil

ROOT = os.path.dirname(os.path.abspath(__file__))

def sub(s, old, new, what):
    if old not in s:
        sys.exit("!! nicht gefunden: " + what)
    return s.replace(old, new, 1)

p = os.path.join(ROOT, "examples", "editor_demo.cpp")
t = io.open(p, encoding="utf-8").read()

start = t.find("        // A frame around what is selected, and a handle in its corner.")
end   = t.find("        // ---- the game's own UI ------------------------------------------")
if start < 0 or end <= start:
    sys.exit("!! Drag-Block nicht gefunden")

NEW = r'''        // A frame around what is selected and EIGHT grips - four corners and
        // four edges, the way every editor that has ever let you resize a
        // rectangle does it. One corner grip meant the only way to change the
        // left edge was to change the right one and then move the whole thing
        // back, twice, until it looked right.
        //
        // Moving writes the OFFSET, not a position: the anchor is what makes
        // a HUD survive a different window size, and a drag that replaced it
        // with absolute pixels would quietly undo that.
        //
        // Resizing writes BOTH, and it has to. Where a box is drawn depends on
        // its size - an element anchored right moves left when it grows - so
        // dragging the right edge alone would drag the left one with it. The
        // offset is corrected by exactly the amount the anchor shifts it:
        // for an anchor column f (0 left, 0.5 centre, 1 right),
        //     right edge by dx  ->  w += dx,  off_x += f*dx
        //     left  edge by dx  ->  w -= dx,  off_x += (1-f)*dx
        // which is the algebra of the layout above, not a fudge factor.
        static dai_node ui_drag_node = DAI_INVALID_NODE;
        static int   ui_drag_kind = 0;         // 0 none, 1 move, 2 resize
        static int   ui_drag_ex = 0, ui_drag_ey = 0;   // -1 left/top, +1 right/bottom
        static float ui_drag_x0 = 0, ui_drag_y0 = 0;
        static float ui_drag_ox = 0, ui_drag_oy = 0, ui_drag_w0 = 0, ui_drag_h0 = 0;
        if (dai_editor_selection_count(ed) > 0) {
            dai_node sel_n = dai_editor_selected(ed, 0);
            float rx, ry, rw, rh;
            if (dai_hud_rect_of(sel_n, &rx, &ry, &rw, &rh)) {
                dai_node_desc sr{};
                int have = dai_doc_get(doc, sel_n, &sr) == DAI_OK;
                float mx2 = 0, my2 = 0;
                int mdown = 0, mpress = 0;
                dai_ui_mouse(ui, &mx2, &my2, &mdown, &mpress);

                bool img = have && sr.image_on && sr.image[0];
                // A label that is as wide as its words has nothing to resize;
                // giving it a box IS the resize, so the grips are offered and
                // the first drag writes the box the words are in now.
                bool sizable = have && (img || sr.text_on);

                const float G = 9.0f, HALF = G * 0.5f;
                struct Grip { int ex, ey; float cx, cy; };
                Grip grips[8] = {
                    { -1, -1, rx,            ry            },
                    {  0, -1, rx + rw * 0.5f, ry           },
                    { +1, -1, rx + rw,       ry            },
                    { -1,  0, rx,            ry + rh * 0.5f },
                    { +1,  0, rx + rw,       ry + rh * 0.5f },
                    { -1, +1, rx,            ry + rh       },
                    {  0, +1, rx + rw * 0.5f, ry + rh      },
                    { +1, +1, rx + rw,       ry + rh       },
                };

                dai_ui_layer_push(ui, DAI_LAYER_WINDOW + 6);
                dai_ui_rect_outline(ui, rx - 1.0f, ry - 1.0f, rw + 2.0f, rh + 2.0f, 1.0f,
                                    0xFF3D84D8u);
                if (sizable)
                    for (const Grip &g : grips) {
                        dai_ui_rect(ui, g.cx - HALF, g.cy - HALF, G, G, 0xFFFFFFFFu);
                        dai_ui_rect_outline(ui, g.cx - HALF, g.cy - HALF, G, G, 1.0f, 0xFF3D84D8u);
                    }
                dai_ui_layer_pop(ui);

                int hit = -1;
                if (sizable)
                    for (int gi = 0; gi < 8; ++gi) {
                        const Grip &g = grips[gi];
                        if (mx2 >= g.cx - HALF - 2.0f && mx2 < g.cx + HALF + 2.0f &&
                            my2 >= g.cy - HALF - 2.0f && my2 < g.cy + HALF + 2.0f) { hit = gi; break; }
                    }
                bool over_body = mx2 >= rx - 2.0f && mx2 < rx + rw + 2.0f &&
                                 my2 >= ry - 2.0f && my2 < ry + rh + 2.0f;

                if (mpress && have && (hit >= 0 || over_body)) {
                    ui_drag_node = sel_n;
                    ui_drag_x0 = mx2; ui_drag_y0 = my2;
                    ui_drag_ox = img ? sr.image_x : sr.text_x;
                    ui_drag_oy = img ? sr.image_y : sr.text_y;
                    if (hit >= 0) {
                        ui_drag_kind = 2;
                        ui_drag_ex = grips[hit].ex;
                        ui_drag_ey = grips[hit].ey;
                        // 0 means "as big as it needs to be". The honest start
                        // for a drag is the size it HAS on screen right now,
                        // not zero - otherwise the first pixel of movement
                        // collapses the box.
                        float w0 = img ? sr.image_w : sr.text_w;
                        float h0 = img ? sr.image_h : sr.text_h;
                        ui_drag_w0 = w0 > 0.0f ? w0 : rw;
                        ui_drag_h0 = h0 > 0.0f ? h0 : rh;
                    } else {
                        ui_drag_kind = 1;
                    }
                }
                if (hit >= 0) {
                    int ex = grips[hit].ex, ey = grips[hit].ey;
                    dai_ui_cursor_set(ui, ey == 0 ? DAI_CURSOR_SIZE_WE
                                        : ex == 0 ? DAI_CURSOR_SIZE_NS
                                        : (ex == ey ? DAI_CURSOR_SIZE_NWSE : DAI_CURSOR_SIZE_NESW));
                } else if (over_body && ui_drag_node == DAI_INVALID_NODE) {
                    dai_ui_cursor_set(ui, DAI_CURSOR_HAND);
                }
            }
        }
        if (ui_drag_node != DAI_INVALID_NODE) {
            float mx2 = 0, my2 = 0;
            int mdown = 0;
            dai_ui_mouse(ui, &mx2, &my2, &mdown, nullptr);
            dai_node_desc sr{};
            if (dai_doc_get(doc, ui_drag_node, &sr) == DAI_OK) {
                float dx2 = mx2 - ui_drag_x0, dy2 = my2 - ui_drag_y0;
                bool img = sr.image_on && sr.image[0];
                if (ui_drag_kind == 1) {
                    if (img) { sr.image_x = ui_drag_ox + dx2; sr.image_y = ui_drag_oy + dy2; }
                    else     { sr.text_x  = ui_drag_ox + dx2; sr.text_y  = ui_drag_oy + dy2; }
                } else {
                    int anchor = img ? sr.image_anchor : sr.text_anchor;
                    if (anchor < 0) anchor = 0;
                    if (anchor > 8) anchor = 8;
                    float fx = (float)(anchor % 3) * 0.5f;
                    float fy = (float)(anchor / 3) * 0.5f;
                    float nw = ui_drag_w0, nh = ui_drag_h0;
                    float ox = ui_drag_ox, oy = ui_drag_oy;
                    if (ui_drag_ex > 0)      { nw += dx2; ox += fx * dx2; }
                    else if (ui_drag_ex < 0) { nw -= dx2; ox += (1.0f - fx) * dx2; }
                    if (ui_drag_ey > 0)      { nh += dy2; oy += fy * dy2; }
                    else if (ui_drag_ey < 0) { nh -= dy2; oy += (1.0f - fy) * dy2; }
                    if (nw < 8.0f) nw = 8.0f;
                    if (nh < 8.0f) nh = 8.0f;
                    if (img) { sr.image_w = nw; sr.image_h = nh; sr.image_x = ox; sr.image_y = oy; }
                    else     { sr.text_w  = nw; sr.text_h  = nh; sr.text_x  = ox; sr.text_y  = oy; }
                }
                // No transaction while the button is down: a drag is ONE undo
                // step, not one per frame. It is committed on release.
                dai_doc_set(doc, ui_drag_node, &sr);
            }
            if (!mdown) {
                ui_drag_node = DAI_INVALID_NODE;
                ui_drag_kind = 0;
            }
        }

'''
t = t[:start] + NEW + t[end:]

# ---- {variablen}: der Host loest sie im Skript DES KNOTENS auf ----------
t = sub(t, "static uint32_t hud_image_cb(const char *path, float *out_w, float *out_h, void *) {",
r'''// {score} in a HUD label -> what the script on that node says right now.
//
// Looked up as a global first and then on `state`, because both spellings are
// real: `var score = 0` at the top of a behaviour is a global, and
// `state.score` is what survives a hot reload. A name that is neither is left
// alone - the placeholder stays on screen, which is exactly what you want
// while you are still placing the label.
//
// The name is checked before it is pasted into source: letters, digits and
// underscore only. "{a; drop()}" is a label, not a command.
static int hud_var_cb(dai_node n, const char *name, char *out, size_t out_size, void *) {
#ifdef DAI_WITH_SCRIPT
    if (!name || !name[0] || !out || out_size < 2) return 0;
    for (const char *q = name; *q; ++q)
        if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
              (*q >= '0' && *q <= '9') || *q == '_')) return 0;
    for (RunningScript &rs : g_running) {
        if (rs.node != n) continue;
        char js[512];
        std::snprintf(js, sizeof(js),
            "state.__hvok = 0;"
            "try { var __t = (typeof %s !== 'undefined') ? %s"
            "               : (state && state.%s !== undefined ? state.%s : undefined);"
            "      if (__t !== undefined && __t !== null) {"
            "          state.__hv = String(__t); state.__hvok = 1; } } catch (e) {}",
            name, name, name, name);
        char err[128] = { 0 };
        if (dai_script_eval(rs.s, js, "hudvar", err, sizeof(err)) != DAI_OK) continue;
        if (dai_script_get_number(rs.s, "__hvok", 0.0) < 0.5) continue;
        if (!dai_script_get_string(rs.s, "__hv", out, out_size)) continue;
        return 1;
    }
#else
    (void)n; (void)name; (void)out; (void)out_size;
#endif
    return 0;
}

static uint32_t hud_image_cb(const char *path, float *out_w, float *out_h, void *) {''',
    "hud_image_cb")

t = sub(t, "    dai_hud_images(hud_image_cb, nullptr);",
           "    dai_hud_images(hud_image_cb, nullptr);\n    dai_hud_vars(hud_var_cb, nullptr);",
        "dai_hud_images-Aufruf")

shutil.copyfile(p, p + ".bak_p142")
io.open(p, "w", encoding="utf-8").write(t)
print("-- editor_demo.cpp: 8 Griffe + {variablen}")

# ---- dai_script_get_string ---------------------------------------------
p = os.path.join(ROOT, "src", "dai_script.cpp")
t = io.open(p, encoding="utf-8").read()
t = sub(t, "double dai_script_get_number(dai_script *s, const char *name, double fallback) {",
r'''/* The string twin of dai_script_get_number. A HUD label wants "3 lives" as
 * readily as it wants 3, and formatting a number the script already turned
 * into words is a round trip that loses. */
int dai_script_get_string(dai_script *s, const char *name, char *out, size_t out_size) {
    if (!s || !name || !out || out_size < 2) return 0;
    JSValue g = JS_GetGlobalObject(s->ctx);
    JSValue st = JS_GetPropertyStr(s->ctx, g, "state");
    JSValue v = JS_GetPropertyStr(s->ctx, st, name);
    int ok = 0;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        std::string sv = str(s->ctx, v);
        std::snprintf(out, out_size, "%s", sv.c_str());
        ok = 1;
    }
    JS_FreeValue(s->ctx, v);
    JS_FreeValue(s->ctx, st);
    JS_FreeValue(s->ctx, g);
    return ok;
}

double dai_script_get_number(dai_script *s, const char *name, double fallback) {''',
    "get_number")
shutil.copyfile(p, p + ".bak_p142")
io.open(p, "w", encoding="utf-8").write(t)
print("-- dai_script.cpp: dai_script_get_string")

p = os.path.join(ROOT, "include", "dai_script.h")
t = io.open(p, encoding="utf-8").read()
old = "DAI_API double dai_script_get_number(dai_script *s, const char *name, double fallback);"
if old not in t:
    sys.exit("!! get_number-Deklaration nicht gefunden")
t = t.replace(old, old + """

/* state.<name> as text. 1 when it exists and is not null. */
DAI_API int dai_script_get_string(dai_script *s, const char *name,
                                  char *out, size_t out_size);""", 1)
shutil.copyfile(p, p + ".bak_p142")
io.open(p, "w", encoding="utf-8").write(t)
print("-- dai_script.h")
print("OK")
