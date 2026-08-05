#!/usr/bin/env python3
# patch40 - docking anywhere: the cross buttons ARE the target, the edge
# wedges are proportional instead of a fixed band, and the whole layout has
# four outer buttons of its own.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p40'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

s = rd('src/dai_dock.cpp')

# ================================================= 1. the geometry, shared
# The cross used to be drawn in dai_dock_end from numbers that existed only
# there, while dai_dock_begin decided the drop from a completely different set
# of numbers - fixed pixel bands. The picture and the target were two
# different things, which is the whole bug: you aim at a button and land
# somewhere else. One function now, called by both.
s = sub1(s,
"""// Wider than they look: the drop zone has to be findable while a tab is
// under the cursor hiding the pointer, and 30% of a narrow inspector column
// is 60 pixels of target for a 240 pixel gesture.
const float DROP_ZONE_FRAC = 0.42f;
const float DROP_ZONE_MAX  = 220.0f;

struct Rect { float x = 0, y = 0, w = 0, h = 0; };

bool hit(const Rect &r, float x, float y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}""",
"""// The middle square of a leaf means "as a tab"; outside it, the pointer
// belongs to whichever EDGE it is proportionally nearest. Proportionally is
// the point. The old rule was a band of min(42%, 220px) per side checked top,
// bottom, left, right in that order - and on a short wide panel (the Project
// window across the bottom of the editor, 200 px tall) top and bottom claimed
// 84% of the height between them, so "right of Project" existed in a 30 pixel
// stripe you had to find blind. Normalised distance has no such blind spot:
// every edge of every panel is reachable at every aspect ratio.
const float DROP_CORE = 0.24f;

// The dock cross. These buttons ARE the drop target, not a picture of one.
const float CROSS_B = 26.0f;   // button size
const float CROSS_G = 3.0f;    // gap between them
const float EDGE_B  = 30.0f;   // the outer buttons: dock against the LAYOUT
const float EDGE_M  = 10.0f;   // how far in from the border they sit

struct Rect { float x = 0, y = 0, w = 0, h = 0; };

bool hit(const Rect &r, float x, float y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

struct Slot { Rect r; int kind; };

// Over the leaf under the pointer: centre, left, right, top, bottom.
void cross_slots(const Rect &r, Slot out[5]) {
    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    const float B = CROSS_B, G = CROSS_G;
    out[0] = Slot{ Rect{ cx - B * 0.5f,     cy - B * 0.5f,     B, B }, 0 };
    out[1] = Slot{ Rect{ cx - B * 1.5f - G, cy - B * 0.5f,     B, B }, 1 };
    out[2] = Slot{ Rect{ cx + B * 0.5f + G, cy - B * 0.5f,     B, B }, 2 };
    out[3] = Slot{ Rect{ cx - B * 0.5f,     cy - B * 1.5f - G, B, B }, 3 };
    out[4] = Slot{ Rect{ cx - B * 0.5f,     cy + B * 0.5f + G, B, B }, 4 };
}

// Against the edges of the whole dock area: a full height column down one
// side, a full width strip along the top or the bottom. Splitting a LEAF can
// never produce those - a panel dropped left of the Hierarchy is only as tall
// as the Hierarchy - and "put this down the entire left edge" is a thing
// people want often enough that hunting for the one leaf that spans the
// height is not an answer. Kinds 6..9.
void edge_slots(const Rect &a, Slot out[4]) {
    float cx = a.x + a.w * 0.5f, cy = a.y + a.h * 0.5f;
    const float B = EDGE_B, M = EDGE_M;
    out[0] = Slot{ Rect{ a.x + M,           cy - B * 0.5f,   B, B }, 6 };
    out[1] = Slot{ Rect{ a.x + a.w - M - B, cy - B * 0.5f,   B, B }, 7 };
    out[2] = Slot{ Rect{ cx - B * 0.5f,     a.y + M,         B, B }, 8 };
    out[3] = Slot{ Rect{ cx - B * 0.5f,     a.y + a.h - M - B, B, B }, 9 };
}

// What a drop of this kind would cover, so the preview and the result agree.
Rect drop_preview_rect(const Rect &r, const Rect &area, int kind) {
    switch (kind) {
    case 1: return Rect{ r.x, r.y, r.w * 0.4f, r.h };
    case 2: return Rect{ r.x + r.w * 0.6f, r.y, r.w * 0.4f, r.h };
    case 3: return Rect{ r.x, r.y, r.w, r.h * 0.4f };
    case 4: return Rect{ r.x, r.y + r.h * 0.6f, r.w, r.h * 0.4f };
    case 6: return Rect{ area.x, area.y, area.w * 0.25f, area.h };
    case 7: return Rect{ area.x + area.w * 0.75f, area.y, area.w * 0.25f, area.h };
    case 8: return Rect{ area.x, area.y, area.w, area.h * 0.25f };
    case 9: return Rect{ area.x, area.y + area.h * 0.75f, area.w, area.h * 0.25f };
    default: return r;
    }
}""",
    'geometry helpers')

# ============================================== 2. the target, in dock_begin
s = sub1(s,
"""        // Find the drop target: tab bars first, then the edges of a body.
        Node *target = nullptr;
        for (auto it = order.rbegin(); it != order.rend() && !target; ++it) {
            if (!hit(it->second->rect, mx, my)) continue;
            std::vector<Node *> fl;
            collect_leaves(it->second->root, &fl);
            for (Node *leaf : fl) if (hit(leaf->rect, mx, my)) { target = leaf; break; }
        }
        if (!target)
            for (Node *leaf : leaves) if (hit(leaf->rect, mx, my)) { target = leaf; break; }

        if (target) {
            d->drop_node = target;
            Rect r = target->rect;
            if (my < r.y + TAB_H + 4.0f) {
                d->drag_kind = 0;                        // into this tab bar
                d->drag_preview = Rect{ r.x, r.y, r.w, TAB_H };
            } else {
                float zw = std::min(r.w * DROP_ZONE_FRAC, DROP_ZONE_MAX);
                float zh = std::min(r.h * DROP_ZONE_FRAC, DROP_ZONE_MAX);
                // Top and bottom are checked FIRST: in a corner both zones
                // claim the pointer, and the corner square is where you drop
                // a panel to sit UNDER the Inspector or Hierarchy (between
                // the side strip and the bottom strip) - left/right used to
                // win and the panel landed as a thin column at the window
                // edge instead.
                if (my < r.y + zh)              { d->drag_kind = 3; d->drag_preview = Rect{ r.x, r.y, r.w, r.h * 0.4f }; }
                else if (my > r.y + r.h - zh)   { d->drag_kind = 4; d->drag_preview = Rect{ r.x, r.y + r.h * 0.6f, r.w, r.h * 0.4f }; }
                else if (mx < r.x + zw)         { d->drag_kind = 1; d->drag_preview = Rect{ r.x, r.y, r.w * 0.4f, r.h }; }
                else if (mx > r.x + r.w - zw)   { d->drag_kind = 2; d->drag_preview = Rect{ r.x + r.w * 0.6f, r.y, r.w * 0.4f, r.h }; }
                else                            { d->drag_kind = 0; d->drag_preview = Rect{ r.x, r.y, r.w, r.h }; }
            }
        } else {
            d->drag_kind = 5;                            // nowhere: a new window
            d->drag_preview = Rect{ mx - 90.0f, my - 10.0f, 180.0f, 120.0f };
        }""",
"""        // Find the leaf under the pointer. A floating window is checked
        // first and, once the pointer is inside one, the layout underneath is
        // not a candidate at all - dropping "into" a window you cannot see is
        // never what the gesture meant.
        Node *target = nullptr;
        bool over_float = false;
        for (auto it = order.rbegin(); it != order.rend() && !target; ++it) {
            if (!hit(it->second->rect, mx, my)) continue;
            over_float = true;
            std::vector<Node *> fl;
            collect_leaves(it->second->root, &fl);
            for (Node *leaf : fl) if (hit(leaf->rect, mx, my)) { target = leaf; break; }
        }
        if (!target && !over_float)
            for (Node *leaf : leaves) if (hit(leaf->rect, mx, my)) { target = leaf; break; }

        d->drop_node = target;
        int kind = -1;
        // 1. The leaf's own cross, first: it is drawn on top, it is the most
        //    specific thing under the pointer, and it is what you were aiming
        //    at if you were aiming at anything.
        if (target) {
            Slot cs[5];
            cross_slots(target->rect, cs);
            for (const Slot &sl : cs) if (hit(sl.r, mx, my)) { kind = sl.kind; break; }
        }
        // 2. The outer buttons - the whole layout's edges.
        if (kind < 0 && !over_float && hit(d->area, mx, my)) {
            Slot es[4];
            edge_slots(d->area, es);
            for (const Slot &sl : es) if (hit(sl.r, mx, my)) { kind = sl.kind; break; }
        }
        if (target) {
            Rect r = target->rect;
            // 3. The tab bar strip still means "another tab of this one",
            //    which is the gesture everybody tries before they find a
            //    button, and it must keep working while the cross is up.
            if (kind < 0 && my < r.y + TAB_H + 4.0f) {
                kind = 0;
                d->drag_kind = 0;
                d->drag_preview = Rect{ r.x, r.y, r.w, TAB_H };
            } else {
                // 4. Otherwise: the middle square is a tab, and everything
                //    around it belongs to the nearest edge measured as a
                //    FRACTION of the panel, so a wide short panel has a
                //    usable right edge and a tall thin one a usable top.
                if (kind < 0) {
                    float u = r.w > 1.0f ? (mx - r.x) / r.w : 0.5f;
                    float v = r.h > 1.0f ? (my - r.y) / r.h : 0.5f;
                    if (std::fabs(u - 0.5f) < DROP_CORE && std::fabs(v - 0.5f) < DROP_CORE) {
                        kind = 0;
                    } else {
                        float dl = u, dr = 1.0f - u, dt = v, db = 1.0f - v;
                        float best = dl; kind = 1;
                        if (dr < best) { best = dr; kind = 2; }
                        if (dt < best) { best = dt; kind = 3; }
                        if (db < best) { best = db; kind = 4; }
                    }
                }
                d->drag_kind = kind;
                d->drag_preview = drop_preview_rect(r, d->area, kind);
            }
        } else if (kind >= 0) {
            d->drag_kind = kind;
            d->drag_preview = drop_preview_rect(d->area, d->area, kind);
        } else {
            d->drag_kind = 5;                            // nowhere: a new window
            d->drag_preview = Rect{ mx - 90.0f, my - 10.0f, 180.0f, 120.0f };
        }""",
    'drop target')

# ==================================================== 3. the drop itself
s = sub1(s,
"""        bool degenerate = target && target->leaf() && target->tabs.size() == 1 &&
                          target->tabs[0] == title;
        if (!degenerate) {
            if (kind == 5) {""",
"""        // ...but only for the four LEAF splits and the tab drop. Dropping
        // that same lone tab onto an outer button is not a no-op: it asks for
        // a full height column, which is a different tree.
        bool degenerate = kind >= 0 && kind <= 4 && target && target->leaf() &&
                          target->tabs.size() == 1 && target->tabs[0] == title;
        if (!degenerate) {
            if (kind >= 6) {
                // Against the whole layout. The tab comes out first, which
                // can promote a sibling and REPLACE d->root - so the root is
                // read again afterwards, never cached across remove_tab.
                bool only = d->root && d->root->leaf() && d->root->tabs.size() == 1 &&
                            d->root->tabs[0] == title;
                if (!only) {
                    remove_tab(d, title);
                    if (!d->root) d->root = new Node();
                    if (d->root->leaf() && d->root->tabs.empty())
                        add_tab_to(d->root, title, true);
                    else
                        split_into(d, d->root, title, kind - 5, 0.25f);
                }
            } else if (kind == 5) {""",
    'root drop')

# ==================================================== 4. the cross, drawn
s = sub1(s,
"""        const char *what = d->drag_kind == 0 ? "as a tab"
                         : d->drag_kind == 1 ? "left of"
                         : d->drag_kind == 2 ? "right of"
                         : d->drag_kind == 3 ? "above"
                         : d->drag_kind == 4 ? "below"
                         : "as a window";""",
"""        const char *what = d->drag_kind == 0 ? "as a tab"
                         : d->drag_kind == 1 ? "left of"
                         : d->drag_kind == 2 ? "right of"
                         : d->drag_kind == 3 ? "above"
                         : d->drag_kind == 4 ? "below"
                         : d->drag_kind == 6 ? "down the left edge"
                         : d->drag_kind == 7 ? "down the right edge"
                         : d->drag_kind == 8 ? "along the top"
                         : d->drag_kind == 9 ? "along the bottom"
                         : "as a window";""",
    'drag note text')

s = sub1(s,
"""    if (d->drop_node) {
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
    }""",
"""    {
        // One button drawer for both crosses: the four outer ones against the
        // area's edges, then the five over the leaf on top of them. Same
        // geometry the drop reads, so what lights up is what happens.
        auto draw_slot = [&](const Slot &sl, bool outer) {
            bool on = d->drag_kind == sl.kind;
            const Rect &b = sl.r;
            uint32_t bg = on ? st->accent : ((st->chrome & 0x00FFFFFFu) | 0xD8000000u);
            dai_ui_rrect(ui, b.x, b.y, b.w, b.h, 4.0f, bg);
            dai_ui_rect_outline(ui, b.x, b.y, b.w, b.h, 1.0f,
                                on ? 0xFFFFFFFFu : st->panel_border);
            uint32_t fg = on ? 0xFF101010u : st->text_dim;
            float ix = b.x + 5.0f, iy = b.y + 5.0f, iw = b.w - 10.0f, ih = b.h - 10.0f;
            // The outer buttons draw the strip INSIDE a frame of the whole
            // window, so "a column down the side of everything" is visibly a
            // different promise from "the left half of this panel".
            if (outer) dai_ui_rect_outline(ui, ix, iy, iw, ih, 1.0f, fg);
            switch (sl.kind) {
            case 1: dai_ui_rect(ui, ix, iy, iw * 0.5f, ih, fg); break;
            case 2: dai_ui_rect(ui, ix + iw * 0.5f, iy, iw * 0.5f, ih, fg); break;
            case 3: dai_ui_rect(ui, ix, iy, iw, ih * 0.5f, fg); break;
            case 4: dai_ui_rect(ui, ix, iy + ih * 0.5f, iw, ih * 0.5f, fg); break;
            case 6: dai_ui_rect(ui, ix, iy, iw * 0.34f, ih, fg); break;
            case 7: dai_ui_rect(ui, ix + iw * 0.66f, iy, iw * 0.34f, ih, fg); break;
            case 8: dai_ui_rect(ui, ix, iy, iw, ih * 0.34f, fg); break;
            case 9: dai_ui_rect(ui, ix, iy + ih * 0.66f, iw, ih * 0.34f, fg); break;
            default: dai_ui_rect(ui, ix, iy, iw, 3.0f, fg);
                     dai_ui_rect_outline(ui, ix, iy, iw, ih, 1.0f, fg); break;
            }
        };
        Slot es[4];
        edge_slots(d->area, es);
        for (const Slot &sl : es) draw_slot(sl, true);
        if (d->drop_node) {
            Slot cs[5];
            cross_slots(d->drop_node->rect, cs);
            for (const Slot &sl : cs) draw_slot(sl, false);
        }
    }""",
    'cross drawing')
wr('src/dai_dock.cpp', s)
print('patch40 ok')
