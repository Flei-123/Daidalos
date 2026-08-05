import io, sys

def rd(p):
    return io.open(p, encoding='utf-8').read()

def wr(p, s):
    io.open(p, 'w', encoding='utf-8').write(s)

def rep(s, old, new, n=1, tag=''):
    c = s.count(old)
    assert c == n, 'count %d != %d for %s :: %s' % (c, n, tag, old[:70])
    return s.replace(old, new)

# ===================================================================== dai_ui.h
H = 'include/dai_ui.h'
h = rd(H)

h = rep(h, 'DAI_API int  dai_ui_button(dai_ui *ui, const char *utf8);',
"""DAI_API int  dai_ui_button(dai_ui *ui, const char *utf8);
/* The same button, only as wide as its own label. A column of full width
 * buttons reads as a stack of banners; "Edit" is a word, not a banner. */
DAI_API int  dai_ui_button_fit(dai_ui *ui, const char *utf8);
/* Unity's object field: a label, a plate showing what is referenced, and a
 * target button on the right that opens a picker.
 *   0 = nothing, 1 = the plate was clicked (ping it), 2 = the target button.
 * The plate is also a drop target: while the pointer is over it the field's
 * label is reported by dai_ui_hot_label, which is how a dragged hierarchy
 * node knows which field it would land in. */
DAI_API int  dai_ui_object_field(dai_ui *ui, const char *label, const char *value,
                                 const char *icon);""", tag='button_fit decl')

h = rep(h, """    char  query[64];
    float w, h;            /* read by the host: where the panel ended up     */""",
"""    char  query[64];
    float w, h;            /* read by the host: where the panel ended up     */
    char  hint[40];        /* placeholder in the search box; empty = generic */""", tag='searchlist hint')

wr(H, h)

# =================================================================== dai_ui.cpp
C = 'src/dai_ui.cpp'
c = rd(C)

# ---- 1. a text field that stopped being drawn must not keep the keyboard
c = rep(c, """        float    scroll = 0;      // horizontal scroll when the text is too long
    } edit;""",
"""        float    scroll = 0;      // horizontal scroll when the text is too long
    } edit;
    // The id of the field that actually DREW itself this frame. A panel that
    // stops being drawn while one of its fields has the keyboard used to leave
    // edit.editing true for the rest of the session - and "does a text field
    // have the keyboard" is the question F2 asks before it renames anything,
    // and the one a search box asks before it takes focus. A flag only ever
    // set by the thing that is gone is not a flag, it is a fuse.
    uint64_t edit_seen = 0;""", tag='edit_seen member')

c = rep(c, """    bool over = inside_chk(ui, x, y, w, h);
    bool editing = ui->edit.editing && ui->edit.id == id;
    if (over) {
        ui->hot = id;
        ui->mouse_over_ui = true;
        ui->cursor_want = DAI_CURSOR_TEXT;
    }""",
"""    bool over = inside_chk(ui, x, y, w, h);
    bool editing = ui->edit.editing && ui->edit.id == id;
    if (editing) ui->edit_seen = id;      // it is on screen, so it may keep it
    if (over) {
        ui->hot = id;
        ui->mouse_over_ui = true;
        ui->cursor_want = DAI_CURSOR_TEXT;
    }""", tag='edit_seen set')

c = rep(c, """    if (!ui->input.mouse_down) {
        ui->active = 0; ui->drag_win = 0; ui->size_win = 0; ui->size_edge = 0;
        ui->edit.dragging = false;
    }""",
"""    // Nobody drew the field that had the keyboard: its panel was closed, its
    // tab switched away or its row scrolled out of the world. It is over.
    if (ui->edit.editing && ui->edit_seen != ui->edit.id) {
        ui->edit.editing = false;
        ui->edit.dragging = false;
        ui->edit.id = 0;
    }
    ui->edit_seen = 0;
    if (!ui->input.mouse_down) {
        ui->active = 0; ui->drag_win = 0; ui->size_win = 0; ui->size_edge = 0;
        ui->edit.dragging = false;
    }""", tag='edit_seen sweep')

# ---- 2. a focus request wins over a field that is already editing
c = rep(c, """    if (ui->focus_next_field) {
        ui->focus_next_field = 0;
        if (!ui->edit.editing) { edit_open(ui, id, buf, false, true); ui->edit.opened_now = false; }
    }""",
"""    if (ui->focus_next_field) {
        ui->focus_next_field = 0;
        // Whatever had the keyboard gives it up: a picker that opens with a
        // search box and does not GET the keys is a picker where typing does
        // nothing, which is exactly how "the search does not work" looks.
        if (ui->edit.editing && ui->edit.id != id) edit_close(ui);
        if (!ui->edit.editing) { edit_open(ui, id, buf, false, true); ui->edit.opened_now = false; }
    }""", tag='focus steals')

# ---- 3. the searchlist says what it searches
c = rep(c, """                    "Search components...", ui->style.text_dim);""",
"""                    s->hint[0] ? s->hint : "Search...", ui->style.text_dim);""", tag='hint use')

# ---- 4. button_fit + object_field
c = rep(c, """int dai_ui_array_end(dai_ui *ui, int count, int min_n, int max_n) {""",
"""int dai_ui_button_fit(dai_ui *ui, const char *utf8) {
    if (!ui || !utf8) return 0;
    float h = widget_height(ui);
    float w = dai_ui_text_width(ui, utf8) + ui->style.padding * 3.0f;
    float avail = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    if (w > avail) w = avail;
    float x, y;
    next_rect(ui, w, h, &x, &y);
    uint64_t id = hash_id(utf8, x, y);
    bool over = inside_chk(ui, x, y, w, h);
    if (over) { ui->hot = id; ui->hot_label = utf8; ui->mouse_over_ui = true;
                ui->cursor_want = DAI_CURSOR_HAND; }
    bool pressed = false;
    if (over && ui->input.mouse_down && !ui->prev.mouse_down) ui->active = id;
    if (ui->active == id && !ui->input.mouse_down) { pressed = over; ui->active = 0; }
    uint32_t col = ui->style.button;
    if (ui->active == id) col = ui->style.button_active;
    else if (over) col = ui->style.button_hover;
    dai_ui_rrect(ui, x, y, w, h, ui->style.rounding, col);
    float tw = dai_ui_text_width(ui, utf8);
    dai_ui_text(ui, x + (w - tw) * 0.5f, y + ui->style.row_pad * 0.5f, utf8, ui->style.text);
    return pressed ? 1 : 0;
}

int dai_ui_object_field(dai_ui *ui, const char *label, const char *value,
                        const char *icon) {
    if (!ui) return 0;
    float h = widget_height(ui), x, y, w;
    field_rect(ui, label, &x, &y, &w, h);
    float bw = h;                       // the target button: square, on the right
    float vw = w - bw - 2.0f;
    if (vw < 40.0f) { vw = w; bw = 0.0f; }
    bool over_f = inside_chk(ui, x, y, vw, h);
    bool over_b = bw > 0.0f && inside_chk(ui, x + vw + 2.0f, y, bw, h);
    if (over_f || over_b) {
        ui->mouse_over_ui = true;
        ui->cursor_want = DAI_CURSOR_HAND;
        // What a dragged node asks: which field am I over?
        if (label) ui->hot_label = label;
    }
    uint64_t id = hash_id(label ? label : "objfield", x, y);
    int res = 0;
    if ((over_f || over_b) && ui->input.mouse_down && !ui->prev.mouse_down) ui->active = id;
    if (ui->active == id && !ui->input.mouse_down) {
        if (over_b)      res = 2;
        else if (over_f) res = 1;
        ui->active = 0;
    }
    dai_ui_rrect(ui, x, y + 1.0f, vw, h - 2.0f, ui->style.rounding,
                 over_f ? ui->style.button_hover : ui->style.track);
    dai_ui_rect_outline(ui, x, y + 1.0f, vw, h - 2.0f, 1.0f,
                        over_f ? ui->style.accent : ui->style.panel_border);
    float lh = dai_font_line_height(ui->font);
    float tx = x + 5.0f;
    if (icon && dai_ui_has_icon(ui, icon)) {
        float isz = dai_icons_size(ui->icons);
        if (isz <= 0.0f || isz > h - 6.0f) isz = h - 8.0f;
        dai_ui_icon_at(ui, icon, tx, y + (h - isz) * 0.5f, isz, ui->style.accent);
        tx += isz + 4.0f;
    }
    dai_ui_text(ui, tx, y + (h - lh) * 0.5f, value ? value : "None", ui->style.text);
    if (bw > 0.0f) {
        float bx = x + vw + 2.0f;
        dai_ui_rrect(ui, bx, y + 1.0f, bw, h - 2.0f, ui->style.rounding,
                     over_b ? ui->style.button_hover : ui->style.button);
        float isz = dai_icons_size(ui->icons);
        if (isz <= 0.0f || isz > h - 6.0f) isz = h - 8.0f;
        if (dai_ui_has_icon(ui, "target"))
            dai_ui_icon_at(ui, "target", bx + (bw - isz) * 0.5f, y + (h - isz) * 0.5f,
                           isz, ui->style.text);
        else {
            // No glyph for it: a ring with a dot, which is what the icon is.
            float cx = bx + bw * 0.5f, cy = y + h * 0.5f, r = isz * 0.35f;
            dai_ui_rect_outline(ui, cx - r, cy - r, r * 2.0f, r * 2.0f, 1.0f, ui->style.text);
            dai_ui_rect(ui, cx - 1.0f, cy - 1.0f, 2.0f, 2.0f, ui->style.text);
        }
    }
    return res;
}

int dai_ui_array_end(dai_ui *ui, int count, int min_n, int max_n) {""", tag='new widgets')

wr(C, c)
print('dai_ui ok')
