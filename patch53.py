#!/usr/bin/env python3
# patch53 - a category is not a component, a folder shows whether it has
# anything in it, and a prefab instance is blue.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p53'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# ====================================== 1. a header is not a thing you pick
s = rd('include/dai_ui.h')
s = sub1(s,
"""typedef struct dai_ui_menu_item {
    const char *icon;        /* dai_icons name, or NULL  */
    const char *label;
    const char *shortcut;    /* shown right aligned, or NULL */
} dai_ui_menu_item;""",
"""typedef struct dai_ui_menu_item {
    const char *icon;        /* dai_icons name, or NULL  */
    const char *label;
    const char *shortcut;    /* shown right aligned, or NULL */
    /* A caption row: drawn dim and small, never highlighted, never returned as
     * a pick, skipped by the arrow keys. Zero for a normal item, so every
     * existing three-element initialiser keeps meaning exactly what it did.
     *
     * It exists because a searchlist that groups its rows has to draw the
     * group name SOMEWHERE, and drawing it as an ordinary row makes a
     * category look like a component you can add - which is a thing you then
     * click, and nothing happens, and you conclude the editor is broken. */
    int         header;
} dai_ui_menu_item;""",
    'menu item header flag')
wr('include/dai_ui.h', s)

s = rd('src/dai_ui.cpp')
# Keyboard navigation has to step OVER headers, in the direction it was going.
s = sub1(s,
"""    // ---- keyboard navigation ----------------------------------------------
    if (ui->input.key_down_arrow) ++s->highlight;
    if (ui->input.key_up_arrow)   --s->highlight;
    if (s->highlight < 0) s->highlight = (int)nshown - 1;
    if (s->highlight >= (int)nshown) s->highlight = 0;
    if (commit && nshown) {
        int pick = shown[s->highlight];
        s->open = 0;
        dai_ui_layer_pop(ui);
        return pick;
    }""",
"""    // ---- keyboard navigation ----------------------------------------------
    auto is_header = [&](int row) {
        return row >= 0 && row < (int)nshown && items[shown[row]].header != 0;
    };
    int step = 0;
    if (ui->input.key_down_arrow) { ++s->highlight; step = 1; }
    if (ui->input.key_up_arrow)   { --s->highlight; step = -1; }
    if (s->highlight < 0) s->highlight = (int)nshown - 1;
    if (s->highlight >= (int)nshown) s->highlight = 0;
    // Walk past captions rather than landing on them. Bounded by the row
    // count so a list that is nothing but headers cannot spin here.
    if (step && nshown) {
        for (uint32_t guard = 0; guard < nshown && is_header(s->highlight); ++guard) {
            s->highlight += step;
            if (s->highlight < 0) s->highlight = (int)nshown - 1;
            if (s->highlight >= (int)nshown) s->highlight = 0;
        }
    }
    if (commit && nshown && !is_header(s->highlight)) {
        int pick = shown[s->highlight];
        s->open = 0;
        dai_ui_layer_pop(ui);
        return pick;
    }""",
    'searchlist header nav')

s = sub1(s,
"""    for (uint32_t r = 0; r < nshown; ++r) {
        int idx = shown[r];
        bool over = mx >= x && mx < x + W && my >= ry && my < ry + ROW_H;
        if (over) s->highlight = (int)r;
        bool hl = (int)r == s->highlight;
        if (hl) dai_ui_rect(ui, x + 2.0f, ry, W - 4.0f, ROW_H, ui->style.button_hover);
        float tx = x + 10.0f;
        if (items[idx].icon && dai_ui_has_icon(ui, items[idx].icon)) {
            dai_ui_icon_at(ui, items[idx].icon, tx, ry + (ROW_H - isz) * 0.5f, isz,
                           hl ? ui->style.text : ui->style.text_dim);
            tx += isz + 6.0f;
        }
        dai_ui_text(ui, tx, ry + (ROW_H - lh) * 0.5f, items[idx].label,
                    hl ? ui->style.text : ui->style.text_dim);
        if (over && pressed) { result = idx; s->open = 0; }
        ry += ROW_H;
    }""",
"""    for (uint32_t r = 0; r < nshown; ++r) {
        int idx = shown[r];
        bool head = items[idx].header != 0;
        bool over = !head && mx >= x && mx < x + W && my >= ry && my < ry + ROW_H;
        if (over) s->highlight = (int)r;
        bool hl = !head && (int)r == s->highlight;
        if (head) {
            // A caption: a rule and a quiet label, nothing that invites a
            // click. No icon either - the icon is what made it look like a row.
            dai_ui_text(ui, x + 8.0f, ry + (ROW_H - lh) * 0.5f, items[idx].label,
                        ui->style.text_dim);
            float lw = dai_ui_text_width(ui, items[idx].label);
            float rule_x = x + 14.0f + lw;
            if (rule_x < x + W - 10.0f)
                dai_ui_rect(ui, rule_x, ry + ROW_H * 0.5f, x + W - 10.0f - rule_x, 1.0f,
                            ui->style.panel_border);
            ry += ROW_H;
            continue;
        }
        if (hl) dai_ui_rect(ui, x + 2.0f, ry, W - 4.0f, ROW_H, ui->style.button_hover);
        float tx = x + 10.0f;
        if (items[idx].icon && dai_ui_has_icon(ui, items[idx].icon)) {
            dai_ui_icon_at(ui, items[idx].icon, tx, ry + (ROW_H - isz) * 0.5f, isz,
                           hl ? ui->style.text : ui->style.text_dim);
            tx += isz + 6.0f;
        }
        dai_ui_text(ui, tx, ry + (ROW_H - lh) * 0.5f, items[idx].label,
                    hl ? ui->style.text : ui->style.text_dim);
        if (over && pressed) { result = idx; s->open = 0; }
        ry += ROW_H;
    }""",
    'searchlist header row')

# ============== 2. a tree row whose LABEL carries a colour (prefabs are blue)
s = sub1(s,
"""int dai_ui_tree_item_ex(dai_ui *ui, const char *label, int depth,
                        int has_children, int *open, int selected) {
    return dai_ui_tree_item_icon(ui, nullptr, label, depth, has_children, open, selected);
}""",
"""int dai_ui_tree_item_ex(dai_ui *ui, const char *label, int depth,
                        int has_children, int *open, int selected) {
    return dai_ui_tree_item_icon(ui, nullptr, label, depth, has_children, open, selected);
}

// The colour of the LAST row's label, consumed by dai_ui_tree_item_icon on the
// next call. A parameter would have meant touching every call site for one
// case; this is the same trick the style stack is, scoped to one row.
static uint32_t g_tree_label_col = 0;
void dai_ui_tree_label_color(dai_ui *ui, uint32_t rgba) { (void)ui; g_tree_label_col = rgba; }""",
    'tree label colour setter')

s = sub1(s,
"""    dai_ui_text(ui, tx, y + 1.0f, label, ui->style.text);
    return clicked;
}""",
"""    dai_ui_text(ui, tx, y + 1.0f, label,
                g_tree_label_col ? g_tree_label_col : ui->style.text);
    g_tree_label_col = 0;      // one row only: it is set immediately before
    return clicked;
}""",
    'tree label colour use')
wr('src/dai_ui.cpp', s)

s = rd('include/dai_ui.h')
s = sub1(s,
"""DAI_API int  dai_ui_tree_item_icon(dai_ui *ui, const char *icon, const char *label,
                                   int depth, int has_children, int *open, int selected);""",
"""DAI_API int  dai_ui_tree_item_icon(dai_ui *ui, const char *icon, const char *label,
                                   int depth, int has_children, int *open, int selected);
/* Colours the label of the NEXT tree row, then forgets. What a hierarchy needs
 * to say "this one is a prefab instance" the way Unity does - in blue, on the
 * name, without a second column. Pass 0 to go back to the normal colour. */
DAI_API void dai_ui_tree_label_color(dai_ui *ui, uint32_t rgba);""",
    'tree label colour decl')
wr('include/dai_ui.h', s)

# ======================================== 3. a folder that shows it has content
s = rd('src/dai_icons.cpp')
s = sub1(s,
"""{ "folder", STROKE_HEAD
  "<path d='M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z'/>" TAIL },""",
"""{ "folder", STROKE_HEAD
  "<path d='M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z'/>" TAIL },
// The same folder with something in it. Unity draws an empty folder hollow and
// a full one solid, and that one difference answers "is there anything in
// here" without opening it - which is most of what a file browser is for.
{ "folder-full", FILL_HEAD
  "<path d='M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z'/>" TAIL },""",
    'folder-full icon')
wr('src/dai_icons.cpp', s)

s = rd('include/dai_icons.h')
s = sub1(s,
"""#define DAI_ICON_FOLDER     "folder\"""",
"""#define DAI_ICON_FOLDER     "folder"
#define DAI_ICON_FOLDER_FULL "folder-full\"""",
    'folder-full define')
wr('include/dai_icons.h', s)
print('patch53 ok')
