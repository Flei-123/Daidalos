#!/usr/bin/env python3
# Runde 27c - "es fehlen auch ui buttons etc".
#
# Ein Button ist KEINE vierte Art, etwas zu zeichnen. Er ist das Image (der
# Hintergrund) und der Text (die Beschriftung) auf demselben Knoten plus das
# eine, was beiden fehlt: eine Reaktion. Deshalb ein Schalter neben den beiden,
# kein eigener Renderer - sonst haette man drei Wege, ein Rechteck mit Schrift
# zu malen, und zwei davon waeren falsch.
import io, os, sys, shutil

ROOT = os.path.dirname(os.path.abspath(__file__))

def read(p):
    with io.open(p, encoding="utf-8") as f:
        return f.read()

def write(p, s, tag):
    shutil.copyfile(p, p + ".bak_" + tag)
    with io.open(p, "w", encoding="utf-8") as f:
        f.write(s)

def sub(s, old, new, what):
    if old not in s:
        sys.exit("!! nicht gefunden: " + what)
    return s.replace(old, new, 1)

# ------------------------------------------------------------------ dai_doc.h
p = os.path.join(ROOT, "include", "dai_doc.h")
t = read(p)
OLD = """    int      image_anchor;      /* same 3x3 grid as the text                 */
    float    image_x, image_y;
"""
NEW = """    int      image_anchor;      /* same 3x3 grid as the text                 */
    float    image_x, image_y;

    /* ---- Button: the Image/Text that answers the pointer ------------------
     * A Button IS the Image (its background) and the Text (its label) on the
     * same node, plus the one thing they lack: a reaction. It brightens under
     * the pointer, darkens while held, and calls the node's script when it is
     * let go INSIDE - let go outside is a cancelled click, which is what every
     * toolkit does and what people do without thinking about it.
     *
     * `button_action` names the function to call; empty means `onClick`. The
     * rectangle is the Image's when there is one, otherwise the text block's -
     * a label with no background is still a button, it just has no frame. */
    int      button_on;
    dai_vec3 button_hover;      /* tint under the pointer, 0,0,0 -> automatic */
    dai_vec3 button_press;      /* tint while held,        0,0,0 -> automatic */
    char     button_action[64];
"""
t = sub(t, OLD, NEW, "dai_doc.h Image-Block")
write(p, t, "p137")
print("-- dai_doc.h: Button-Felder")

# ------------------------------------------------------------- dai_doc_text.cpp
p = os.path.join(ROOT, "src", "dai_doc_text.cpp")
t = read(p)
OLD = """        if (!feq(r.text_x, def.text_x) || !feq(r.text_y, def.text_y))
            put(s, "  textpos %s %s\\n", fstr(r.text_x).c_str(), fstr(r.text_y).c_str());
"""
NEW = """        if (!feq(r.text_x, def.text_x) || !feq(r.text_y, def.text_y))
            put(s, "  textpos %s %s\\n", fstr(r.text_x).c_str(), fstr(r.text_y).c_str());
        if (r.button_on != def.button_on)  put(s, "  button %d\\n", r.button_on);
        if (r.button_action[0])            put(s, "  buttonact %s\\n", r.button_action);
        if (!feq(r.button_hover.x, def.button_hover.x) ||
            !feq(r.button_hover.y, def.button_hover.y) ||
            !feq(r.button_hover.z, def.button_hover.z))
            put(s, "  buttonhover %s %s %s\\n", fstr(r.button_hover.x).c_str(),
                fstr(r.button_hover.y).c_str(), fstr(r.button_hover.z).c_str());
        if (!feq(r.button_press.x, def.button_press.x) ||
            !feq(r.button_press.y, def.button_press.y) ||
            !feq(r.button_press.z, def.button_press.z))
            put(s, "  buttonpress %s %s %s\\n", fstr(r.button_press.x).c_str(),
                fstr(r.button_press.y).c_str(), fstr(r.button_press.z).c_str());
"""
t = sub(t, OLD, NEW, "textpos beim Speichern")

OLD = """        else if (key == "imagepos")   { ok = parse_floats(after, &rec.image_x, 2); }
"""
NEW = """        else if (key == "imagepos")   { ok = parse_floats(after, &rec.image_x, 2); }
        else if (key == "button")     { ok = parse_i32(after, &rec.button_on); }
        else if (key == "buttonhover"){ ok = parse_floats(after, &rec.button_hover.x, 3); }
        else if (key == "buttonpress"){ ok = parse_floats(after, &rec.button_press.x, 3); }
        else if (key == "buttonact") {
            std::string v = after;
            while (!v.empty() && (v.front() == ' ' || v.front() == '\\t')) v.erase(0, 1);
            while (!v.empty() && (v.back() == ' ' || v.back() == '\\t' || v.back() == '\\r')) v.pop_back();
            if (v.size() >= sizeof(rec.button_action)) ok = false;
            else std::snprintf(rec.button_action, sizeof(rec.button_action), "%s", v.c_str()); }
"""
t = sub(t, OLD, NEW, "imagepos beim Laden")
write(p, t, "p137")
print("-- dai_doc_text.cpp: Button wird gespeichert und geladen")

# ------------------------------------------------------------ dai_editor_ui.cpp
p = os.path.join(ROOT, "src", "dai_editor_ui.cpp")
t = read(p)

# ---- Zustand + Helfer, direkt hinter dai_hud_pick ------------------------
OLD = """static dai_hud_image_fn g_hud_image = nullptr;"""
NEW = """// ---- buttons ---------------------------------------------------------
// Only ON while the game view is being drawn during Play. The same HUD is
// drawn a second time over the Scene view for editing, and a button that
// answered the pointer there would fire every time you tried to move it.
static int   g_hud_interactive = 0;
static dai_node g_hud_held = DAI_INVALID_NODE;
static std::vector<dai_node> g_hud_clicks;

void dai_hud_interactive(int on) { g_hud_interactive = on ? 1 : 0; }

uint32_t dai_hud_take_clicks(dai_node *out, uint32_t max) {
    uint32_t n = (uint32_t)g_hud_clicks.size();
    if (n > max) n = max;
    if (out) for (uint32_t i = 0; i < n; ++i) out[i] = g_hud_clicks[i];
    g_hud_clicks.clear();
    return n;
}

static uint32_t hud_rgb_of(const dai_vec3 &v) {
    auto ch = [](float x) -> uint32_t {
        float c = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
        return (uint32_t)(c * 255.0f + 0.5f);
    };
    return 0xFF000000u | (ch(v.z) << 16) | (ch(v.y) << 8) | ch(v.x);
}
static uint32_t hud_tint_mul(uint32_t c, float f) {
    auto ch = [&](int sh) -> uint32_t {
        float v = (float)((c >> sh) & 0xFFu) * f;
        if (v > 255.0f) v = 255.0f;
        if (v < 0.0f) v = 0.0f;
        return (uint32_t)(v + 0.5f) << sh;
    };
    return (c & 0xFF000000u) | ch(0) | ch(8) | ch(16);
}
// 0 idle, 1 hovered, 2 held. Registers the click on RELEASE INSIDE - letting
// go somewhere else is a cancelled click, and that is not a detail: it is the
// only way out once a button has been pressed by accident.
static int hud_button_state(dai_ui *ui, dai_node id,
                            float bx, float by, float bw, float bh) {
    if (!g_hud_interactive) return 0;
    float mx = 0.0f, my = 0.0f;
    int down = 0, press = 0;
    dai_ui_mouse(ui, &mx, &my, &down, &press);
    bool over = mx >= bx && mx < bx + bw && my >= by && my < by + bh;
    if (press && over) g_hud_held = id;
    if (!down && g_hud_held == id) {
        if (over) g_hud_clicks.push_back(id);
        g_hud_held = DAI_INVALID_NODE;
    }
    if (!over) return 0;
    return (down && g_hud_held == id) ? 2 : 1;
}
// The tint a button wants, given the one it would have had. Without colours of
// its own it brightens under the pointer and darkens while held - what every
// toolkit does, and what nobody has to be told.
static uint32_t hud_button_tint(const dai_node_desc &r, int state, uint32_t base) {
    if (state == 1) {
        if (r.button_hover.x || r.button_hover.y || r.button_hover.z)
            return hud_rgb_of(r.button_hover);
        return hud_tint_mul(base, 1.18f);
    }
    if (state == 2) {
        if (r.button_press.x || r.button_press.y || r.button_press.z)
            return hud_rgb_of(r.button_press);
        return hud_tint_mul(base, 0.78f);
    }
    return base;
}

static dai_hud_image_fn g_hud_image = nullptr;"""
t = sub(t, OLD, NEW, "g_hud_image-Definition")

# ---- pro Knoten einmal merken -------------------------------------------
OLD = """        dai_node_desc r{};
        if (dai_doc_get(doc, ids[i], &r) != DAI_OK) continue;
        // `disabled` is the OBJECT: off means off, label included."""
NEW = """        dai_node_desc r{};
        if (dai_doc_get(doc, ids[i], &r) != DAI_OK) continue;
        // Worked out at the Image and reused by the Text, so a button with a
        // background AND a label reacts as ONE thing: both tint together, and
        // the rectangle that answers the pointer is the background's.
        int btn = 0;
        // `disabled` is the OBJECT: off means off, label included."""
t = sub(t, OLD, NEW, "Knotenschleife im HUD")

OLD = """                dai_ui_image_at(ui, tex, ix, iy, dw, dh, 0, 0, 1, 1, tint);
                g_hud_rects.push_back(HudRect{ ids[i], ix, iy, dw, dh });"""
NEW = """                if (r.button_on && !r.disabled) {
                    btn = hud_button_state(ui, ids[i], ix, iy, dw, dh);
                    tint = hud_button_tint(r, btn, tint);
                }
                dai_ui_image_at(ui, tex, ix, iy, dw, dh, 0, 0, 1, 1, tint);
                g_hud_rects.push_back(HudRect{ ids[i], ix, iy, dw, dh });"""
t = sub(t, OLD, NEW, "Image-Zeichnung")

OLD = """        g_hud_rects.push_back(HudRect{ ids[i], bx, by, widest, block_h });"""
NEW = """        g_hud_rects.push_back(HudRect{ ids[i], bx, by, widest, block_h });
        // A label with no background is still a button; the words ARE the
        // rectangle. Only worked out here when the Image did not already do it.
        if (r.button_on && !r.disabled) {
            if (!r.image_on || !r.image[0])
                btn = hud_button_state(ui, ids[i], bx, by, widest, block_h);
            col32 = hud_button_tint(r, btn, col32);
        }"""
t = sub(t, OLD, NEW, "Text-Rechteck")

# ---- Inspector: Abschnitt "Button (UI)" ---------------------------------
OLD = """    // ---- Text ---------------------------------------------------------------"""
NEW = """    // ---- Button (UI) --------------------------------------------------------
    // Deliberately AFTER Image and before Text: it is the thing that makes
    // those two answer the pointer, and reading the inspector top to bottom
    // is then the same order as building one - a background, a reaction, a
    // label.
    if (r.button_on) {
        int on = 1;
        {
            int hrc = dai_ui_header_icon_col(p->ui, DAI_ICON_C_SPRITE, rgba(0x7E, 0xD3, 0x9B, 255),
                                             "Button (UI)", &p->fold_button, &on);
            if (hrc == 2 && !on) r.button_on = 0;
            else if (hrc == 3) {
                p->comp_menu_target = 9;
                float cmx = 0, cmy = 0;
                dai_ui_mouse(p->ui, &cmx, &cmy, nullptr, nullptr);
                dai_ui_popup_open(&p->menu_comp, cmx, cmy);
            }
        }
        if (p->fold_button && r.button_on) {
            if (!r.image_on && !r.text_on)
                dai_ui_label(p->ui, "add an Image or a Text - a button needs something to be");
            dai_ui_text_field(p->ui, "On click", r.button_action, sizeof(r.button_action), "btnact");
            dai_ui_help(p->ui, "The function in this object's script. Empty = onClick().");
            {
                float hc[3] = { r.button_hover.x, r.button_hover.y, r.button_hover.z };
                if (dai_ui_color(p->ui, "Hover", hc, "btnhov"))
                    r.button_hover = dai_vec3{ hc[0], hc[1], hc[2] };
            }
            dai_ui_help(p->ui, "black = automatic: 18% brighter under the pointer.");
            {
                float pc[3] = { r.button_press.x, r.button_press.y, r.button_press.z };
                if (dai_ui_color(p->ui, "Pressed", pc, "btnprs"))
                    r.button_press = dai_vec3{ pc[0], pc[1], pc[2] };
            }
            dai_ui_help(p->ui, "black = automatic: 22% darker while held.");
            dai_ui_label(p->ui, "fires on release INSIDE - let go outside cancels");
        }
    }

    // ---- Text ---------------------------------------------------------------"""
t = sub(t, OLD, NEW, "Text-Abschnitt im Inspector")

# fold_button
OLD = "    int fold_transform = 1, fold_body = 1, fold_collider = 1, fold_render = 1;"
NEW = "    int fold_transform = 1, fold_body = 1, fold_collider = 1, fold_render = 1;\n    int fold_button = 1;"
t = sub(t, OLD, NEW, "fold-Felder")

# comp_menu_target Kommentar
t = t.replace("// 0 transform 1 rigidbody 2 collider 3 camera 4 light 5 sprite 6 audio 7 text",
              "// 0 transform 1 rigidbody 2 collider 3 camera 4 light 5 sprite 6 audio 7 text 8 image 9 button", 1)

# ---- Add Component ------------------------------------------------------
OLD = """            if (!ar2.image_on)    entries.push_back({ "Image (UI)", "Rendering", 8, "" });"""
NEW = """            if (!ar2.image_on)    entries.push_back({ "Image (UI)", "Rendering", 8, "" });
            if (!ar2.button_on)   entries.push_back({ "Button (UI)", "Rendering", 9, "" });"""
t = sub(t, OLD, NEW, "Add-Component-Liste")

OLD = """                case 8: ar2.image_on = 1;"""
NEW = """                case 9: ar2.button_on = 1;
                        // A button with nothing to show is a button you cannot
                        // find. Unity's Add > UI > Button does the same: it
                        // arrives with a background and a label already on it.
                        if (!ar2.image_on && !ar2.text_on) {
                            ar2.text_on = 1;
                            if (!ar2.text[0]) std::snprintf(ar2.text, sizeof(ar2.text), "Button");
                        }
                        break;
                case 8: ar2.image_on = 1;"""
t = sub(t, OLD, NEW, "Add-Component-Schalter")

# ---- Remove Component ---------------------------------------------------
OLD = """                    else if (target == 8) ar.image_on = 0;"""
NEW = """                    else if (target == 8) ar.image_on = 0;
                    else if (target == 9) ar.button_on = 0;"""
t = sub(t, OLD, NEW, "Remove-Component")

write(p, t, "p137")
print("-- dai_editor_ui.cpp: Button-Komponente, Hover/Press, Klick-Sammler")

# ------------------------------------------------------------ dai_editor_ui.h
p = os.path.join(ROOT, "include", "dai_editor_ui.h")
t = read(p)
OLD = "/* The UI node under the pointer, topmost first. 1 when there is one. */\nDAI_API int dai_hud_pick(float mx, float my, dai_node *out);"
NEW = """/* The UI node under the pointer, topmost first. 1 when there is one. */
DAI_API int dai_hud_pick(float mx, float my, dai_node *out);

/* Buttons answer the pointer only while this is on. The host turns it on for
 * the Game view during Play and off again straight after - the same HUD is
 * drawn a second time over the Scene view, and a button that fired there
 * would go off every time you tried to move it. */
DAI_API void dai_hud_interactive(int on);

/* The buttons clicked since the last call, and forgets them. */
DAI_API uint32_t dai_hud_take_clicks(dai_node *out, uint32_t max);"""
t = sub(t, OLD, NEW, "dai_hud_pick-Deklaration")
write(p, t, "p137")
print("-- dai_editor_ui.h: Button-API")
print("OK")
