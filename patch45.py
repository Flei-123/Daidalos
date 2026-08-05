#!/usr/bin/env python3
# patch45 - a drag you can see: the node being carried, the row it would land
# in, and what letting go would do.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p45'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# ===================================================== 1. where did that go?
# A drop target has to be drawn OVER the row it is about, and the caller is
# the only one who knows a row is a target - but the layout is the only one
# who knows where the row ended up. Without this the caller re-derives the
# rectangle from the layout cursor and a guessed row height, which is wrong
# the moment the font changes.
s = rd('include/dai_ui.h')
s = sub1(s,
"""DAI_API void dai_ui_cursor_pos(const dai_ui *ui, float *x, float *y);""",
"""DAI_API void dai_ui_cursor_pos(const dai_ui *ui, float *x, float *y);
/* The rectangle the widget that was just drawn occupies. What drag & drop
 * needs to mark a row as the drop target: the caller knows it IS the target,
 * the layout knows where it is. Valid until the next widget. */
DAI_API void dai_ui_last_rect(const dai_ui *ui, float *x, float *y,
                              float *w, float *h);""",
    'last_rect decl')
wr('include/dai_ui.h', s)

s = rd('src/dai_ui.cpp')
s = sub1(s,
"""    // layout cursor
    float cursor_x = 0, cursor_y = 0;""",
"""    // layout cursor
    float cursor_x = 0, cursor_y = 0;
    // The rectangle the last row widget claimed, for callers that have to
    // draw ON it (a drop target marker) rather than after it.
    float last_x = 0, last_y = 0, last_w = 0, last_h = 0;""",
    'last rect fields')

s = sub1(s,
"""void dai_ui_cursor_pos(const dai_ui *ui, float *x, float *y) {
    if (!ui) return;
    if (x) *x = ui->cursor_x;
    if (y) *y = ui->cursor_y;
}""",
"""void dai_ui_cursor_pos(const dai_ui *ui, float *x, float *y) {
    if (!ui) return;
    if (x) *x = ui->cursor_x;
    if (y) *y = ui->cursor_y;
}

void dai_ui_last_rect(const dai_ui *ui, float *x, float *y, float *w, float *h) {
    if (!ui) return;
    if (x) *x = ui->last_x;
    if (y) *y = ui->last_y;
    if (w) *w = ui->last_w;
    if (h) *h = ui->last_h;
}""",
    'last_rect impl')

s = sub1(s,
"""int dai_ui_tree_item_icon(dai_ui *ui, const char *icon, const char *label, int depth,
                          int has_children, int *open, int selected) {
    if (!ui || !label) return 0;
    float h = dai_font_line_height(ui->font) + 2.0f;
    float x, y;
    next_rect(ui, 0, h, &x, &y);
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    float indent = 12.0f * (float)(depth < 0 ? 0 : depth);

    uint64_t id = hash_id(label, x, y);""",
"""int dai_ui_tree_item_icon(dai_ui *ui, const char *icon, const char *label, int depth,
                          int has_children, int *open, int selected) {
    if (!ui || !label) return 0;
    float h = dai_font_line_height(ui->font) + 2.0f;
    float x, y;
    next_rect(ui, 0, h, &x, &y);
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    float indent = 12.0f * (float)(depth < 0 ? 0 : depth);

    ui->last_x = x; ui->last_y = y; ui->last_w = w; ui->last_h = h;
    uint64_t id = hash_id(label, x, y);""",
    'tree_item_icon records rect')
wr('src/dai_ui.cpp', s)

# =========================================== 2. the row that would take it
s = rd('src/dai_editor_ui.cpp')
s = sub1(s,
"""        if (rc & 4) {
            p->hover_node = n;   // a dragged script aims at this row
            int lpe = 0;""",
"""        if (rc & 4) {
            p->hover_node = n;   // a dragged script aims at this row
            // ...and while a node IS being dragged, say so ON the row. A
            // re-parent whose target only becomes visible after the button
            // comes up is a re-parent you undo half the time - and the
            // hierarchy is where people actually build the scene graph.
            if (p->drag_node != DAI_INVALID_NODE && p->drag_node != n) {
                const dai_ui_style *hs = dai_ui_style_of(p->ui);
                float lx = 0, ly = 0, lw = 0, lh = 0;
                dai_ui_last_rect(p->ui, &lx, &ly, &lw, &lh);
                dai_ui_rect(p->ui, lx, ly, lw, lh, (hs->accent & 0x00FFFFFFu) | 0x55000000u);
                dai_ui_rect_outline(p->ui, lx, ly, lw, lh, 1.0f, hs->accent);
                dai_ui_cursor_set(p->ui, DAI_CURSOR_HAND);
            }
            int lpe = 0;""",
    'hierarchy drop marker')

s = sub1(s,
"""        int rc = dai_ui_tree_item_ex(p->ui, label, 0, 1, &root_open, 0);
        p->scene_root_folded = !root_open;
        if (rc & 4) p->hover_node = DAI_SCENE_ROOT_NODE;   // drop here = unparent""",
"""        int rc = dai_ui_tree_item_ex(p->ui, label, 0, 1, &root_open, 0);
        p->scene_root_folded = !root_open;
        if (rc & 4) {
            p->hover_node = DAI_SCENE_ROOT_NODE;   // drop here = unparent
            if (p->drag_node != DAI_INVALID_NODE) {
                const dai_ui_style *hs = dai_ui_style_of(p->ui);
                float lx = 0, ly = 0, lw = 0, lh = 0;
                dai_ui_last_rect(p->ui, &lx, &ly, &lw, &lh);
                dai_ui_rect(p->ui, lx, ly, lw, lh, (hs->accent & 0x00FFFFFFu) | 0x55000000u);
                dai_ui_rect_outline(p->ui, lx, ly, lw, lh, 1.0f, hs->accent);
                dai_ui_cursor_set(p->ui, DAI_CURSOR_HAND);
            }
        }""",
    'scene root drop marker')

# ============================================ 3. the thing under the cursor
s = sub1(s,
"""        if (p->drag_node != DAI_INVALID_NODE) {
            dai_node_desc dr{};
            const char *nm = "node";
            if (dai_doc_get(dai_editor_doc(p->ed), p->drag_node, &dr) == DAI_OK && dr.name[0])
                nm = dr.name;
            float tw = dai_ui_text_width(ui, nm) + 12.0f;
            dai_ui_rect(ui, dmx + 10.0f, dmy + 8.0f, tw, 18.0f, st->accent);
            dai_ui_text(ui, dmx + 16.0f, dmy + 11.0f, nm, st->text);
        }""",
"""        if (p->drag_node != DAI_INVALID_NODE) {
            dai_node_desc dr{};
            const char *nm = "node";
            if (dai_doc_get(dai_editor_doc(p->ed), p->drag_node, &dr) == DAI_OK && dr.name[0])
                nm = dr.name;
            // What letting go would DO, not just what is being held. Three
            // gestures share this drag - re-parent, unparent, save a prefab -
            // and they are told apart by where the pointer is, which is
            // exactly the thing the pointer is covering up.
            char note[224];
            dai_node_desc tr2{};
            if (p->hover_node == DAI_SCENE_ROOT_NODE)
                std::snprintf(note, sizeof(note), "%s  ->  scene root", nm);
            else if (p->hover_node != DAI_INVALID_NODE && p->hover_node != p->drag_node &&
                     dai_doc_get(dai_editor_doc(p->ed), p->hover_node, &tr2) == DAI_OK &&
                     tr2.name[0])
                std::snprintf(note, sizeof(note), "%s  ->  child of %s", nm, tr2.name);
            else if (dai_ui_root_hovered(ui, "Project"))
                std::snprintf(note, sizeof(note), "%s  ->  prefab", nm);
            else
                std::snprintf(note, sizeof(note), "%s", nm);
            float tw = dai_ui_text_width(ui, note) + 18.0f;
            float th = dai_ui_text_height(ui) + 8.0f;
            // On the window layer, like the script drag: a pill clipped to
            // the panel it started in disappears the moment the drag leaves
            // the hierarchy, which is every drag that matters.
            dai_ui_layer_push(ui, DAI_LAYER_WINDOW + 100);
            dai_ui_rrect(ui, dmx + 12.0f, dmy + 10.0f, tw, th, 4.0f, st->accent);
            dai_ui_rect_outline(ui, dmx + 12.0f, dmy + 10.0f, tw, th, 1.0f, 0xFFFFFFFFu);
            dai_ui_text(ui, dmx + 21.0f, dmy + 14.0f, note, st->text);
            dai_ui_layer_pop(ui);
            dai_ui_claim_mouse(ui);
        }""",
    'node drag pill')

# The script/asset pill gets the same treatment: it says what it is going to
# do, and it is a rounded plate like everything else in this editor.
s = sub1(s,
"""        if (!p->drag_script.empty()) {
            std::string lbl = base_of(p->drag_script);
            float tw = dai_ui_text_width(ui, lbl.c_str()) + 12.0f;
            // Drawn on the window layer so the pill survives leaving the
            // editor window - a drag you cannot see outside is a drag lost.
            dai_ui_layer_push(ui, DAI_LAYER_WINDOW + 100);
            dai_ui_rect(ui, dmx + 10.0f, dmy + 8.0f, tw, 18.0f, st->accent);
            dai_ui_text(ui, dmx + 16.0f, dmy + 11.0f, lbl.c_str(), st->text);
            dai_ui_layer_pop(ui);
        }""",
"""        if (!p->drag_script.empty()) {
            std::string lbl = base_of(p->drag_script);
            if (p->proj_drop_ok)
                lbl += "  ->  " + (p->proj_drop_dir.empty() ? std::string("Assets")
                                                            : p->proj_drop_dir);
            else if (is_behaviour_file(p->drag_script) && p->hover_node != DAI_INVALID_NODE)
                lbl += "  ->  attach";
            float tw = dai_ui_text_width(ui, lbl.c_str()) + 18.0f;
            float th = dai_ui_text_height(ui) + 8.0f;
            // Drawn on the window layer so the pill survives leaving the
            // editor window - a drag you cannot see outside is a drag lost.
            dai_ui_layer_push(ui, DAI_LAYER_WINDOW + 100);
            dai_ui_rrect(ui, dmx + 12.0f, dmy + 10.0f, tw, th, 4.0f, st->accent);
            dai_ui_rect_outline(ui, dmx + 12.0f, dmy + 10.0f, tw, th, 1.0f, 0xFFFFFFFFu);
            dai_ui_text(ui, dmx + 21.0f, dmy + 14.0f, lbl.c_str(), st->text);
            dai_ui_layer_pop(ui);
            dai_ui_claim_mouse(ui);
        }""",
    'asset drag pill')
wr('src/dai_editor_ui.cpp', s)
print('patch45 ok')
