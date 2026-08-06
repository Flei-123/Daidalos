#!/usr/bin/env python3
# Runde 27b - "ich will mein UI in der Scene bearbeiten, aber der Klick
# deselektiert es einfach".
#
# Grund: der HUD wurde NUR in die Game-Ansicht gezeichnet. In der Scene gab es
# also gar kein Rechteck - der Klick fiel durch auf die 3D-Auswahl, traf dort
# nichts und dai_editor_select(..., DAI_INVALID_NODE) hat abgewaehlt. Genau der
# Klick, mit dem man das Element greifen wollte.
#
# Fix: 1. HUD wird auch in die Scene gezeichnet (Unity: die Canvas liegt IN der
#         Szene), 2. die HUD-Rechtecke sind klickbar - ein Treffer waehlt das
#         Element aus statt abzuwaehlen, ein Treffer auf das BEREITS gewaehlte
#         laesst die Auswahl in Ruhe, damit die Greifer den Zug bekommen.
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

# ------------------------------------------------------------- dai_editor_ui.cpp
p = os.path.join(ROOT, "src", "dai_editor_ui.cpp")
t = read(p)

OLD = '''struct HudRect { dai_node n; float x, y, w, h; };
static std::vector<HudRect> g_hud_rects;
static std::vector<HudRect> g_hud_rects_prev;
'''
NEW = '''struct HudRect { dai_node n; float x, y, w, h; };
static std::vector<HudRect> g_hud_rects;
static std::vector<HudRect> g_hud_rects_prev;
// The HUD is drawn more than once per frame now - once over the Scene view so
// it can be edited where the rest of the scene is edited, once into the Game
// view because that is what the player sees. Both have to end up in the SAME
// list, or the second call throws the first one's rectangles away and half the
// UI stops being clickable depending on which panel is open.
static bool g_hud_frame_open = false;

void dai_hud_frame(void) { g_hud_frame_open = false; }

// What is under the pointer, topmost first: a later node draws over an earlier
// one, so the search runs backwards. Uses the PREVIOUS frame's rectangles -
// the current frame's are still being built when input is read.
int dai_hud_pick(float mx, float my, dai_node *out) {
    for (size_t i = g_hud_rects_prev.size(); i-- > 0; ) {
        const HudRect &r = g_hud_rects_prev[i];
        if (r.w <= 0.0f || r.h <= 0.0f) continue;
        if (mx < r.x - 2.0f || mx >= r.x + r.w + 2.0f) continue;
        if (my < r.y - 2.0f || my >= r.y + r.h + 2.0f) continue;
        if (out) *out = r.n;
        return 1;
    }
    return 0;
}
'''
t = sub(t, OLD, NEW, "HudRect-Block")

OLD = '''    g_hud_rects_prev.swap(g_hud_rects);
    g_hud_rects.clear();
'''
NEW = '''    if (!g_hud_frame_open) {
        g_hud_rects_prev.swap(g_hud_rects);
        g_hud_rects.clear();
        g_hud_frame_open = true;
    }
'''
t = sub(t, OLD, NEW, "HUD-Rechteck-Tausch")

# ---- der Klick in der Scene ------------------------------------------------
OLD = '''        dai_node hit = dai_editor_pick(p->ed, mx, my);
        dai_editor_select(p->ed, hit, 0);       // empty space clears the selection
        return 1;'''
NEW = '''        // A UI element drawn over the scene is a thing you can AIM at. Before
        // this, the 3D pick ran first, found nothing behind the label - HUD
        // nodes have no body - and cleared the selection. That is exactly the
        // click you make to grab a panel and resize it, and it threw away the
        // selection instead.
        {
            dai_node hn = DAI_INVALID_NODE;
            if (dai_hud_pick(mx, my, &hn) && hn != DAI_INVALID_NODE) {
                // Already selected: hands off. The move/resize grips take this
                // same press, and re-selecting would cancel the drag.
                if (dai_editor_selection_count(p->ed) > 0 &&
                    dai_editor_selected(p->ed, 0) == hn)
                    return 1;
                dai_editor_select(p->ed, hn, 0);
                return 1;
            }
        }
        dai_node hit = dai_editor_pick(p->ed, mx, my);
        dai_editor_select(p->ed, hit, 0);       // empty space clears the selection
        return 1;'''
t = sub(t, OLD, NEW, "Scene-Pick")

write(p, t, "p136")
print("-- dai_editor_ui.cpp: HUD sammelt ueber die ganze Frame, HUD ist pickbar")

# ------------------------------------------------------------- Header
p = os.path.join(ROOT, "include", "dai_editor_ui.h")
t = read(p)
OLD = "DAI_API int dai_hud_rect_of(dai_node n, float *x, float *y, float *w, float *h);"
NEW = '''DAI_API int dai_hud_rect_of(dai_node n, float *x, float *y, float *w, float *h);

/* Once per frame, BEFORE the first dai_hud_draw of that frame. Tells the HUD
 * that a new frame started, so several draws (Scene view and Game view) add to
 * one list of rectangles instead of each throwing away the last one's. */
DAI_API void dai_hud_frame(void);

/* The UI node under the pointer, topmost first. 1 when there is one. */
DAI_API int dai_hud_pick(float mx, float my, dai_node *out);'''
t = sub(t, OLD, NEW, "dai_hud_rect_of-Deklaration")
write(p, t, "p136")
print("-- dai_editor_ui.h: dai_hud_frame + dai_hud_pick")

# ------------------------------------------------------------- editor_demo.cpp
p = os.path.join(ROOT, "examples", "editor_demo.cpp")
t = read(p)
OLD = '''        {
            float hx, hy, hw, hh;
            if (dai_editor_ui_game_view_rect(panels, &hx, &hy, &hw, &hh)) {
                dai_hud_draw(ui, doc, hx, hy, hw, hh, 1.0f, hud_resolve, nullptr);
                gui_flush(ui, hx, hy, hw, hh);
            } else {
                // No Game panel: the script's UI still has to go somewhere, or
                // a menu drawn from code is invisible until you dock one.
                float vx2, vy2, vw2, vh2;
                dai_editor_ui_viewport_rect(panels, &vx2, &vy2, &vw2, &vh2);
                if (vw2 > 0.0f && vh2 > 0.0f) gui_flush(ui, vx2, vy2, vw2, vh2);
            }
        }'''
NEW = '''        {
            dai_hud_frame();
            float vx2, vy2, vw2, vh2;
            dai_editor_ui_viewport_rect(panels, &vx2, &vy2, &vw2, &vh2);
            float hx, hy, hw, hh;
            int has_game = dai_editor_ui_game_view_rect(panels, &hx, &hy, &hw, &hh);

            // The Scene view draws the game's UI too. Unity's canvas lives IN
            // the scene, and a HUD you can only see in the Game tab is a HUD
            // you place by trial and error - which is what it was. Drawn
            // FIRST, so dai_hud_rect_of finds the scene copy and the move and
            // resize grips appear where the editing happens.
            if (dai_editor_ui_view(panels) == DAI_VIEW_SCENE && vw2 > 0.0f && vh2 > 0.0f)
                dai_hud_draw(ui, doc, vx2, vy2, vw2, vh2, 1.0f, hud_resolve, nullptr);

            if (has_game) {
                dai_hud_draw(ui, doc, hx, hy, hw, hh, 1.0f, hud_resolve, nullptr);
                gui_flush(ui, hx, hy, hw, hh);
            } else if (vw2 > 0.0f && vh2 > 0.0f) {
                // No Game panel: the script's UI still has to go somewhere, or
                // a menu drawn from code is invisible until you dock one.
                gui_flush(ui, vx2, vy2, vw2, vh2);
            }
        }'''
t = sub(t, OLD, NEW, "HUD-Zeichenblock")
write(p, t, "p136")
print("-- editor_demo.cpp: HUD auch in der Scene-Ansicht")
print("OK")
