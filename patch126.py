import io

p = 'examples/editor_demo.cpp'
s = io.open(p, encoding='utf-8').read()

# ===========================================================================
# Die Zeichenbefehle der Scripts. Gesammelt waehrend frame() laeuft, gezeichnet
# wenn die UI dran ist - ein Script darf nicht mitten in die Szene malen.
# ===========================================================================
old = """static const char *hud_resolve(const char *text, void *) {"""
new = """// ---- gui: what the scripts asked to be drawn this frame -----------------
//
// COLLECTED, not drawn on the spot. frame() runs while the world is being
// stepped, long before the UI pass; drawing from there would put a label in
// the middle of the scene geometry or, worse, into last frame's vertex
// buffer. So the calls land in a list and the list is played back in one
// place, which is also what makes the coordinates mean the same thing every
// time.
struct GuiCmd {
    int kind;                 // 0 text, 1 rect, 2 image, 3 button
    float x, y, w, h;
    float size;
    uint32_t color;
    std::string text;
};
static std::vector<GuiCmd> g_gui_cmds;
static float g_gui_x = 0, g_gui_y = 0, g_gui_w = 0, g_gui_h = 0;   // the view
static float g_gui_mx = 0, g_gui_my = 0;
static int   g_gui_down = 0, g_gui_released = 0;

static uint32_t gui_col(double v) {
    // JavaScript numbers are doubles; 0xFFFFFFFF survives exactly, but a
    // negative or absurd value must not wrap into something opaque black.
    if (!(v >= 0.0)) return 0xFFFFFFFFu;
    if (v > 4294967295.0) return 0xFFFFFFFFu;
    return (uint32_t)v;
}

static void gui_text_cb(double x, double y, const char *t, double size, double rgba, void *) {
    GuiCmd c{}; c.kind = 0; c.x = (float)x; c.y = (float)y;
    c.size = size > 0 ? (float)size : 24.0f; c.color = gui_col(rgba); c.text = t ? t : "";
    g_gui_cmds.push_back(c);
}
static void gui_rect_cb(double x, double y, double w, double h, double rgba, void *) {
    GuiCmd c{}; c.kind = 1; c.x = (float)x; c.y = (float)y; c.w = (float)w; c.h = (float)h;
    c.color = gui_col(rgba);
    g_gui_cmds.push_back(c);
}
static void gui_image_cb(double x, double y, double w, double h, const char *path,
                         double rgba, void *) {
    GuiCmd c{}; c.kind = 2; c.x = (float)x; c.y = (float)y; c.w = (float)w; c.h = (float)h;
    c.color = gui_col(rgba); c.text = path ? path : "";
    g_gui_cmds.push_back(c);
}
static int gui_button_cb(double x, double y, double w, double h, const char *label, void *) {
    GuiCmd c{}; c.kind = 3; c.x = (float)x; c.y = (float)y; c.w = (float)w; c.h = (float)h;
    c.color = 0; c.text = label ? label : "";
    g_gui_cmds.push_back(c);
    // The ANSWER has to come now, in the same call, because the script uses it
    // in an if. So the hit test runs against last frame's pointer state - one
    // frame of latency on a click, which nobody can feel, in exchange for an
    // API that reads like every immediate mode UI ever written.
    float px = g_gui_x + (float)x, py = g_gui_y + (float)y;
    bool over = g_gui_mx >= px && g_gui_mx < px + (float)w &&
                g_gui_my >= py && g_gui_my < py + (float)h;
    return (over && g_gui_released) ? 1 : 0;
}
static void gui_size_cb(double *w, double *h, void *) {
    if (w) *w = g_gui_w;
    if (h) *h = g_gui_h;
}
static dai_script_gui_host g_gui_host = {
    gui_text_cb, gui_rect_cb, gui_image_cb, gui_button_cb, gui_size_cb, nullptr
};

// Plays the list back into the UI draw list, clipped to the view.
static void gui_flush(dai_ui *ui, float vx, float vy, float vw, float vh) {
    if (g_gui_cmds.empty()) return;
    dai_ui_clip_begin(ui, vx, vy, vw, vh);
    const float base = dai_ui_text_height(ui) > 0.0f ? dai_ui_text_height(ui) : 13.0f;
    for (const GuiCmd &c : g_gui_cmds) {
        float x = vx + c.x, y = vy + c.y;
        if (c.kind == 1) {
            dai_ui_rect(ui, x, y, c.w, c.h, c.color);
        } else if (c.kind == 0) {
            float k = c.size / base;
            dai_ui_text_scaled(ui, x + 1.0f, y + 1.0f, c.text.c_str(), 0xB0000000u, k);
            dai_ui_text_scaled(ui, x, y, c.text.c_str(), c.color, k);
        } else if (c.kind == 2) {
            float iw = 0, ih = 0;
            uint32_t tex = hud_image_cb(c.text.c_str(), &iw, &ih, nullptr);
            if (tex) dai_ui_image_at(ui, tex, x, y, c.w > 0 ? c.w : iw, c.h > 0 ? c.h : ih,
                                     0, 0, 1, 1, c.color);
        } else if (c.kind == 3) {
            bool over = g_gui_mx >= x && g_gui_mx < x + c.w &&
                        g_gui_my >= y && g_gui_my < y + c.h;
            const dai_ui_style *st = dai_ui_style_of(ui);
            uint32_t bg = over ? (g_gui_down ? st->button_active : st->button_hover) : st->button;
            dai_ui_rrect(ui, x, y, c.w, c.h, 4.0f, bg);
            dai_ui_rect_outline(ui, x, y, c.w, c.h, 1.0f, st->panel_border);
            float tw = dai_ui_text_width(ui, c.text.c_str());
            dai_ui_text(ui, x + (c.w - tw) * 0.5f,
                        y + (c.h - dai_ui_text_height(ui)) * 0.5f, c.text.c_str(), st->text);
        }
    }
    dai_ui_clip_end(ui);
    g_gui_cmds.clear();
}

static const char *hud_resolve(const char *text, void *) {"""
assert s.count(old) == 1, 'hud_resolve anchor not found'
s = s.replace(old, new)

# --- binden, wenn ein Script startet ---------------------------------------
old = """            dai_script_bind_play(s, &g_play_host);"""
new = """            dai_script_bind_play(s, &g_play_host);
            dai_script_bind_gui(s, &g_gui_host);"""
assert s.count(old) == 1, 'bind_play call not found'
s = s.replace(old, new)

# --- Zeigerzustand und Ausgabe ---------------------------------------------
old = """        // ---- the game's own UI ------------------------------------------"""
new = """        // What the script GUI needs to answer a button: where the pointer is
        // and whether it was let go this frame, in the view the game is shown
        // in. Taken here, once, before anything is drawn.
        {
            float gx2, gy2, gw2, gh2;
            if (!dai_editor_ui_game_view_rect(panels, &gx2, &gy2, &gw2, &gh2))
                dai_editor_ui_viewport_rect(panels, &gx2, &gy2, &gw2, &gh2);
            g_gui_x = gx2; g_gui_y = gy2; g_gui_w = gw2; g_gui_h = gh2;
            int down = 0;
            dai_ui_mouse(ui, &g_gui_mx, &g_gui_my, &down, nullptr);
            g_gui_released = (!down && g_gui_down) ? 1 : 0;
            g_gui_down = down;
        }

        // ---- the game's own UI ------------------------------------------"""
assert s.count(old) == 1, 'hud draw anchor not found'
s = s.replace(old, new)

old = """            if (dai_editor_ui_game_view_rect(panels, &hx, &hy, &hw, &hh))
                dai_hud_draw(ui, doc, hx, hy, hw, hh, 1.0f, hud_resolve, nullptr);"""
new = """            if (dai_editor_ui_game_view_rect(panels, &hx, &hy, &hw, &hh)) {
                dai_hud_draw(ui, doc, hx, hy, hw, hh, 1.0f, hud_resolve, nullptr);
                gui_flush(ui, hx, hy, hw, hh);
            } else {
                // No Game panel: the script's UI still has to go somewhere, or
                // a menu drawn from code is invisible until you dock one.
                float vx2, vy2, vw2, vh2;
                dai_editor_ui_viewport_rect(panels, &vx2, &vy2, &vw2, &vh2);
                if (vw2 > 0.0f && vh2 > 0.0f) gui_flush(ui, vx2, vy2, vw2, vh2);
            }"""
assert s.count(old) == 1, 'hud draw call not found'
s = s.replace(old, new)

# Stop raeumt die Liste ab, sonst bleibt das letzte Bild stehen.
old = """static void scripts_stop() {"""
new = """static void scripts_stop() {
    g_gui_cmds.clear();     // or the last frame's menu stays on the screen"""
assert s.count(old) == 1, 'scripts_stop not found'
s = s.replace(old, new)

io.open(p, 'w', encoding='utf-8').write(s)
print('host: script gui collected and flushed')
