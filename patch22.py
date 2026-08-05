# -*- coding: utf-8 -*-
import sys, io

def rw(p): return io.open(p, encoding='utf-8').read()
def wr(p, s): io.open(p, 'w', encoding='utf-8').write(s)
def sub1(s, old, new, tag):
    if old not in s: print("MISS", tag); sys.exit(1)
    if s.count(old) != 1: print("AMBIG", tag, s.count(old)); sys.exit(1)
    print("ok", tag); return s.replace(old, new)

P = 'src/dai_dock.cpp'
s = rw(P)

# ---- #40: dragging a tab within its own bar reorders -----------------------
s = sub1(s,
"""    bool  drag_armed = false;      // pressed on a tab, not yet past the threshold""",
"""    bool  drag_armed = false;      // pressed on a tab, not yet past the threshold
    Node *drag_home = nullptr;     // the leaf the drag started in (reorder!)
    int   reorder_at = -1;         // insertion index while hovering the home bar""", "dock reorder state")

s = sub1(s,
"""        hit_leaf->selected = hit_tab;
        d->drag_armed = true;
        d->drag_title = hit_leaf->tabs[(size_t)hit_tab];""",
"""        hit_leaf->selected = hit_tab;
        d->drag_armed = true;
        d->drag_home = hit_leaf;
        d->drag_title = hit_leaf->tabs[(size_t)hit_tab];""", "remember home")

s = sub1(s,
"""    if (d->dragging && down) {
        // The target is recomputed while the button is held and must SURVIVE
        // into the release frame - clearing it here every frame meant the
        // drop handler always saw "nowhere" and the tab never docked.
        d->drag_kind = -1;
        d->drop_node = nullptr;""",
"""    if (d->dragging && down) {
        // The target is recomputed while the button is held and must SURVIVE
        // into the release frame - clearing it here every frame meant the
        // drop handler always saw "nowhere" and the tab never docked.
        d->drag_kind = -1;
        d->drop_node = nullptr;
        d->reorder_at = -1;
        // Reorder FIRST: hovering the tab bar the drag started in, on the
        // bar's strip, is the one gesture that means "put it between these
        // two". Everything else still means dock/split/float.
        if (d->drag_home) {
            Rect hr = d->drag_home->rect;
            bool on_home_bar = my >= hr.y && my < hr.y + TAB_H &&
                               mx >= hr.x && mx < hr.x + hr.w;
            if (on_home_bar && (int)d->drag_home->tabs.size() > 1) {
                float cx = hr.x;
                int at = (int)d->drag_home->tabs.size();
                for (size_t i = 0; i < d->drag_home->tabs.size(); ++i) {
                    float tw = tab_width(ui, d->drag_home->tabs[i]);
                    if (mx < cx + tw * 0.5f) { at = (int)i; break; }
                    cx += tw;
                }
                d->reorder_at = at;
                d->drop_node = d->drag_home;
                // the insertion line, like every file manager's
                float ix = hr.x;
                for (int i = 0; i < at && i < (int)d->drag_home->tabs.size(); ++i)
                    ix += tab_width(ui, d->drag_home->tabs[(size_t)i]);
                dai_ui_layer_push(ui, DAI_LAYER_DOCK_PREVIEW);
                dai_ui_rect(ui, ix - 1.0f, hr.y + 3.0f, 2.0f, TAB_H - 4.0f,
                            dai_ui_style_of(ui)->accent);
                dai_ui_layer_pop(ui);
            }
        }
        if (d->reorder_at >= 0) goto drag_target_done;""", "reorder detect")

s = sub1(s,
"""        } else {
            d->drag_kind = 5;                            // nowhere: a new window
            d->drag_preview = Rect{ mx - 90.0f, my - 10.0f, 180.0f, 120.0f };
        }
    }""",
"""        } else {
            d->drag_kind = 5;                            // nowhere: a new window
            d->drag_preview = Rect{ mx - 90.0f, my - 10.0f, 180.0f, 120.0f };
        }
    }
drag_target_done: (void)0;""", "reorder goto end")

s = sub1(s,
"""    if (d->dragging && released) {
        std::string title = d->drag_title;
        int kind = d->drag_kind;
        Node *target = d->drop_node;""",
"""    if (d->dragging && released) {
        std::string title = d->drag_title;
        int kind = d->drag_kind;
        Node *target = d->drop_node;
        // A drop that stayed on the home bar is a REORDER, not a dock move:
        // the tab comes out and goes back in at the insertion index. This
        // must run before the degenerate check, or a single-tab leaf would
        // swallow it.
        if (d->reorder_at >= 0 && d->drag_home) {
            Node *home = d->drag_home;
            int from = -1;
            for (size_t i = 0; i < home->tabs.size(); ++i)
                if (home->tabs[i] == title) { from = (int)i; break; }
            if (from >= 0) {
                int at = d->reorder_at;
                if (at > from) at--;              // removing shifts the index
                if (at < 0) at = 0;
                if (at > (int)home->tabs.size() - 1) at = (int)home->tabs.size() - 1;
                if (at != from) {
                    home->tabs.erase(home->tabs.begin() + from);
                    home->tabs.insert(home->tabs.begin() + at, title);
                }
                home->selected = at;
            }
            d->dragging = false;
            d->drag_armed = false;
            d->reorder_at = -1;
            d->drag_home = nullptr;
            return;
        }""", "reorder drop")

s = sub1(s,
"""    if (!down) { d->dragging = false; d->drag_armed = false; d->drag_kind = -1; d->drop_node = nullptr; }""",
"""    if (!down) { d->dragging = false; d->drag_armed = false; d->drag_kind = -1;
                 d->drop_node = nullptr; d->drag_home = nullptr; d->reorder_at = -1; }""", "reorder reset")

wr(P, s)
print("patch22 done")
