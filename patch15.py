# -*- coding: utf-8 -*-
import sys, io

def rw(p): return io.open(p, encoding='utf-8').read()
def wr(p, s): io.open(p, 'w', encoding='utf-8').write(s)
def sub1(s, old, new, tag):
    if old not in s: print("MISS", tag); sys.exit(1)
    if s.count(old) != 1: print("AMBIG", tag, s.count(old)); sys.exit(1)
    print("ok", tag); return s.replace(old, new)

# ------------------------------------------------------------- dai_ui.h
P = 'include/dai_ui.h'
s = rw(P)
if 'dai_ui_searchlist_draw' not in s:
    s = sub1(s,
"""DAI_API void dai_ui_scroll_end(dai_ui *ui);

/* ---- direct drawing, for HUDs that are not widgets --------------------- */""",
"""DAI_API void dai_ui_scroll_end(dai_ui *ui);

/* ---- the searchable list -------------------------------------------------
 *
 * Unity's "Add Component" menu: a search field at the top, what you can pick
 * below it, typing filters by name. A flat list of seven buttons is fine for
 * seven things and unusable for forty - and scripts are things, so the list
 * grows whether the UI is ready for it or not.
 *
 * State lives in the caller like every other popup, because typing has to
 * survive the frames between opening the list and picking a row:
 *
 *   static dai_ui_searchlist sl = { 0 };
 *   if (open_now) { dai_ui_searchlist_open(&sl, x, y); sl.wants_focus = 1; }
 *   int pick = dai_ui_searchlist(ui, &sl, items, count);   // >= 0 = chosen
 *
 * The list stays open while the result is DAI_SEARCHLIST_OPEN (-2) and closes
 * with DAI_SEARCHLIST_CLOSED (-1): a click outside, Escape, or a pick. */
enum {
    DAI_SEARCHLIST_CLOSED = -1,
    DAI_SEARCHLIST_OPEN   = -2
};

typedef struct dai_ui_searchlist {
    float x, y;            /* where the popup opens                          */
    int   open;
    int   wants_focus;     /* set when opening: the search field takes keys  */
    int   highlight;       /* the row Enter would pick                       */
    float scroll;
    char  query[64];
    float w, h;            /* read by the host: where the panel ended up     */
} dai_ui_searchlist;

DAI_API void dai_ui_searchlist_open(dai_ui_searchlist *s, float x, float y);
DAI_API int  dai_ui_searchlist(dai_ui *ui, dai_ui_searchlist *s,
                               const dai_ui_menu_item *items, uint32_t count);

/* ---- direct drawing, for HUDs that are not widgets --------------------- */""",
"searchlist decl")
wr(P, s)

# ----------------------------------------------------------- dai_ui.cpp
P = 'src/dai_ui.cpp'
s = rw(P)
# append the implementation after the object field
s = sub1(s,
"""int dai_ui_option(dai_ui *ui, const char *label, int *value,
                  const char *const *items, int count) {""",
"""void dai_ui_searchlist_open(dai_ui_searchlist *s, float x, float y) {
    if (!s) return;
    s->x = x; s->y = y;
    s->open = 1;
    s->highlight = 0;
    s->scroll = 0.0f;
    s->query[0] = 0;
    s->wants_focus = 1;
}

int dai_ui_searchlist_draw(dai_ui *ui, dai_ui_searchlist *s,
                           const dai_ui_menu_item *items, uint32_t count) {
    if (!ui || !s || !s->open) return DAI_SEARCHLIST_CLOSED;
    const float W = 260.0f;
    const float ROW_H = 22.0f;
    const float SEARCH_H = 26.0f;
    const float MAX_ROWS = 11.0f;

    // Keep it on screen: opening at the mouse near the bottom edge would put
    // half the list under it.
    float x = s->x, y = s->y;
    if (x + W > ui->width - 4.0f) x = ui->width - 4.0f - W;
    if (x < 4.0f) x = 4.0f;
    float rows_avail = (ui->height - y - 8.0f - SEARCH_H) / ROW_H;
    float max_rows = rows_avail < MAX_ROWS ? rows_avail : MAX_ROWS;
    if (max_rows < 2.0f) max_rows = 2.0f;

    // ---- filter, and remember which row maps to which item ----------------
    int shown[128];
    uint32_t nshown = 0;
    for (uint32_t i = 0; i < count && nshown < 128; ++i) {
        if (s->query[0] && items[i].label) {
            const char *q = s->query;
            const char *l = items[i].label;
            bool hit = false;
            for (const char *c = l; *c && !hit; ++c) {
                const char *cc = c, *qq = q;
                while (*qq && *cc &&
                       ((*cc >= 'A' && *cc <= 'Z') ? *cc + 32 : *cc) ==
                       ((*qq >= 'A' && *qq <= 'Z') ? *qq + 32 : *qq)) { ++cc; ++qq; }
                if (!*qq) hit = true;
            }
            if (!hit) continue;
        }
        shown[nshown++] = (int)i;
    }
    if (s->highlight >= (int)nshown) s->highlight = (int)nshown - 1;
    if (s->highlight < 0) s->highlight = 0;

    float rows_h = (nshown < (uint32_t)max_rows ? (float)nshown : max_rows) * ROW_H;
    float H = SEARCH_H + rows_h + 6.0f;
    if (y + H > ui->height - 4.0f) y = ui->height - 4.0f - H;
    if (y < 4.0f) y = 4.0f;
    s->w = W; s->h = H;

    float mx = ui->input.mouse_x, my = ui->input.mouse_y;
    bool over_panel = mx >= x && mx < x + W && my >= y && my < y + H;
    bool pressed = ui->input.mouse_down && !ui->prev.mouse_down;

    // A click outside closes without a pick - a menu that cannot be
    // dismissed trains people to press things they did not want.
    if (pressed && !over_panel) { s->open = 0; return DAI_SEARCHLIST_CLOSED; }
    if (ui->input.key_escape)   { s->open = 0; return DAI_SEARCHLIST_CLOSED; }

    // Keep the whole list above every panel it was opened from.
    dai_ui_layer_push(ui, DAI_LAYER_POPUP);
    dai_ui_rrect(ui, x, y, W, H, 6.0f, ui->style.panel);
    dai_ui_rect_outline(ui, x, y, W, H, 1.0f, ui->style.panel_border);

    // ---- the search field -------------------------------------------------
    if (s->wants_focus) { dai_ui_text_focus_next(ui); s->wants_focus = 0; }
    int commit = 0;
    dai_ui_text_field(ui, "searchlist", x + 6.0f, y + 4.0f, W - 12.0f, SEARCH_H - 4.0f,
                      s->query, sizeof(s->query), &commit);
    if (!s->query[0] && !dai_ui_text_active(ui))
        dai_ui_text(ui, x + 12.0f, y + 4.0f + (SEARCH_H - 4.0f - dai_font_line_height(ui->font)) * 0.5f,
                    "Search components...", ui->style.text_dim);

    // ---- keyboard navigation ----------------------------------------------
    if (ui->input.key_down_arrow) ++s->highlight;
    if (ui->input.key_up_arrow)   --s->highlight;
    if (s->highlight < 0) s->highlight = (int)nshown - 1;
    if (s->highlight >= (int)nshown) s->highlight = 0;
    if (commit && nshown) {
        int pick = shown[s->highlight];
        s->open = 0;
        dai_ui_layer_pop(ui);
        return pick;
    }

    // ---- the rows ----------------------------------------------------------
    float list_y = y + SEARCH_H + 2.0f;
    dai_ui_clip_begin(ui, x, list_y, W, rows_h + 2.0f);
    if (over_panel && mx >= x && mx < x + W && my >= list_y)
        s->scroll -= ui->input.wheel * ROW_H * 2.0f;
    float max_scroll = (float)nshown * ROW_H - rows_h;
    if (max_scroll < 0.0f) max_scroll = 0.0f;
    if (s->scroll < 0.0f) s->scroll = 0.0f;
    if (s->scroll > max_scroll) s->scroll = max_scroll;

    float ry = list_y - s->scroll;
    int result = DAI_SEARCHLIST_OPEN;
    float lh = dai_font_line_height(ui->font);
    float isz = dai_icons_size(ui->icons);
    if (isz <= 0.0f || isz > ROW_H - 4.0f) isz = ROW_H - 6.0f;
    for (uint32_t r = 0; r < nshown; ++r) {
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
    }
    if (!nshown)
        dai_ui_text(ui, x + 10.0f, list_y + 4.0f, "nothing matches", ui->style.text_dim);
    dai_ui_clip_end(ui);
    dai_ui_layer_pop(ui);
    return result;
}

int dai_ui_option(dai_ui *ui, const char *label, int *value,
                  const char *const *items, int count) {""", "searchlist impl")

# wheel + escape need to reach the list; the popup system usually eats them,
# so the searchlist is checked by the callers BEFORE the menus run. That is
# already how it will be used.
wr(P, s)
print("patch15 done")
