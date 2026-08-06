#!/usr/bin/env python3
# Runde 29 - vier Meldungen, die eindeutig sind:
#  1. Escape schliesst die Vorschlagsliste nicht: sie wurde jede Frame neu
#     aufgebaut, also war sie im naechsten Bild wieder da.
#  2. Image UND Text auf einem Objekt: nur das obere war anfassbar - der Node
#     hat zwei Rechtecke, gefunden wurde immer das erste.
#  3. "no scripts - drag a .js ..." stand dauerhaft unter jedem Objekt.
#  4. "image not loaded" sagt nicht, WELCHE Datei wo gesucht wurde.
import io, os, sys, shutil

ROOT = os.path.dirname(os.path.abspath(__file__))

def sub(s, old, new, what):
    if old not in s:
        sys.exit("!! nicht gefunden: " + what)
    return s.replace(old, new, 1)

# ============================================ 1. Escape
p = os.path.join(ROOT, "src", "dai_ui.cpp")
t = io.open(p, encoding="utf-8").read()
t = sub(t, "            if (in.key_escape) { st->ac_open = 0; caret_input = true; }",
"""            // Escape means "not now". It set ac_open to 0 and the list was
            // rebuilt from scratch at the end of the same frame, so it came
            // straight back - the key did nothing you could see. The refusal
            // has to be REMEMBERED until the word being typed changes.
            if (in.key_escape) { st->ac_open = 0; st->ac_off = 1; caret_input = true; }""",
    "Escape")
t = sub(t, """        if (st->focused && prefix.size() >= 1 && lang != DAI_CODE_LANG_NONE) {""",
"""        // Typing anything new is a new question, so the refusal expires.
        if (st->ac_off && (int)prefix.size() != st->ac_off_len) st->ac_off = 0;
        st->ac_off_len = (int)prefix.size();
        if (st->focused && !st->ac_off && prefix.size() >= 1 && lang != DAI_CODE_LANG_NONE) {""",
    "ac_off")
shutil.copyfile(p, p + ".bak_p144")
io.open(p, "w", encoding="utf-8").write(t)
print("-- dai_ui.cpp: Escape schliesst die Liste wirklich")

p = os.path.join(ROOT, "include", "dai_ui.h")
t = io.open(p, encoding="utf-8").read()
t = sub(t, """    int   plain;
} dai_ui_code_state;""",
"""    int   plain;
    /* Escape was pressed: no list until the word being typed changes. */
    int   ac_off;
    int   ac_off_len;
} dai_ui_code_state;""", "code_state")
shutil.copyfile(p, p + ".bak_p144")
io.open(p, "w", encoding="utf-8").write(t)
print("-- dai_ui.h")

# ============================================ 2..4  dai_editor_ui
p = os.path.join(ROOT, "src", "dai_editor_ui.cpp")
t = io.open(p, encoding="utf-8").read()

t = sub(t, "struct HudRect { dai_node n; float x, y, w, h; };",
"""// `kind` because a node can have BOTH an Image and a Text, and they are two
// rectangles in two places. Without it the editor found whichever came first
// and the other one could not be selected or resized at all.
enum { HUD_KIND_IMAGE = 0, HUD_KIND_TEXT = 1 };
struct HudRect { dai_node n; int kind; float x, y, w, h; };""", "HudRect")

t = sub(t, """int dai_hud_rect_of(dai_node n, float *x, float *y, float *w, float *h) {
    for (const HudRect &r : g_hud_rects_prev) {
        if (r.n != n) continue;""",
"""// Every rectangle a node has, oldest first. `index` 0 is its Image, 1 its
// Text - or 0 is the Text when there is no Image. Returns 0 past the end, so
// a caller loops until it stops.
int dai_hud_rect_nth(dai_node n, int index, int *kind,
                     float *x, float *y, float *w, float *h) {
    int seen = 0;
    for (const HudRect &r : g_hud_rects_prev) {
        if (r.n != n) continue;
        if (seen++ != index) continue;
        if (kind) *kind = r.kind;
        if (x) *x = r.x;
        if (y) *y = r.y;
        if (w) *w = r.w;
        if (h) *h = r.h;
        return 1;
    }
    return 0;
}

int dai_hud_rect_of(dai_node n, float *x, float *y, float *w, float *h) {
    for (const HudRect &r : g_hud_rects_prev) {
        if (r.n != n) continue;""", "dai_hud_rect_of")

t = sub(t, "                g_hud_rects.push_back(HudRect{ ids[i], ix, iy, dw, dh });",
           "                g_hud_rects.push_back(HudRect{ ids[i], HUD_KIND_IMAGE, ix, iy, dw, dh });",
        "Image-Rechteck")
t = sub(t, "        g_hud_rects.push_back(HudRect{ ids[i], bx, by, widest, block_h });",
           "        g_hud_rects.push_back(HudRect{ ids[i], HUD_KIND_TEXT, bx, by, widest, block_h });",
        "Text-Rechteck")

# ---- 4. sagen, welche Datei wo gesucht wurde ---------------------------
t = sub(t, """                    dai_ui_text(ui, ix + 4.0f, iy + 3.0f, "image not loaded", 0xFFFFFFFFu);""",
        """                    dai_ui_text(ui, ix + 4.0f, iy + 3.0f, "image not loaded", 0xFFFFFFFFu);
                    (void)bn;""", "Platzhalter")

# ---- 3. das Dauer-Label unter jedem Objekt -----------------------------
t = sub(t, '''        if (slist.empty())
            dai_ui_label(p->ui, "no scripts - drag a .js or .cpp from Project onto this object");''',
'''        // Only while something IS being dragged. As a permanent line it sat
        // under every object in the project saying the same thing forever,
        // which is not a hint, it is furniture.
        if (slist.empty()) { }''',
    "no-scripts-Label")

shutil.copyfile(p, p + ".bak_p144")
io.open(p, "w", encoding="utf-8").write(t)
print("-- dai_editor_ui.cpp")

p = os.path.join(ROOT, "include", "dai_editor_ui.h")
t = io.open(p, encoding="utf-8").read()
t = sub(t, "DAI_API int dai_hud_rect_of(dai_node n, float *x, float *y, float *w, float *h);",
"""DAI_API int dai_hud_rect_of(dai_node n, float *x, float *y, float *w, float *h);

/* Every rectangle a node draws, oldest first: `kind` 0 is its Image, 1 its
 * Text. Returns 0 past the end. A node with both has two, in two places, and
 * both have to be selectable - which is why dai_hud_rect_of is not enough. */
DAI_API int dai_hud_rect_nth(dai_node n, int index, int *kind,
                             float *x, float *y, float *w, float *h);""", "rect_nth")
shutil.copyfile(p, p + ".bak_p144")
io.open(p, "w", encoding="utf-8").write(t)
print("-- dai_editor_ui.h")
print("OK")
