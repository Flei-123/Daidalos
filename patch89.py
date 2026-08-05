import io

def rd(p): return io.open(p, encoding='utf-8').read()
def wr(p, s): io.open(p, 'w', encoding='utf-8').write(s)
def rep(s, old, new, n=1, tag=''):
    c = s.count(old)
    assert c == n, 'count %d != %d for %s :: %r' % (c, n, tag, old[:70])
    return s.replace(old, new)

# ---- the header already declared one: drop the duplicate ------------------
H = 'include/dai_ui.h'
h = rd(H)
h = rep(h, """/* Unity's object field: a label, a plate showing what is referenced, and a
 * target button on the right that opens a picker.
 *   0 = nothing, 1 = the plate was clicked (ping it), 2 = the target button.
 * The plate is also a drop target: while the pointer is over it the field's
 * label is reported by dai_ui_hot_label, which is how a dragged hierarchy
 * node knows which field it would land in. */
DAI_API int  dai_ui_object_field(dai_ui *ui, const char *label, const char *value,
                                 const char *icon);
""", "", tag='drop dup decl')
h = rep(h, """DAI_API int  dai_ui_object_field(dai_ui *ui, const char *label, const char *value,""",
"""/* Unity's object field. Returns 0 for nothing, 1 when the plate was clicked
 * (show what it points at) and 2 for the target button on the right, which is
 * where a picker belongs. While the pointer is over it the field's label is
 * reported by dai_ui_hot_label - that is how a dragged hierarchy node knows
 * which field it would land in. */
DAI_API int  dai_ui_object_field(dai_ui *ui, const char *label, const char *value,""",
       tag='doc existing decl')
wr(H, h)

# ---- and one implementation, upgraded rather than a second one ------------
C = 'src/dai_ui.cpp'
c = rd(C)
start = c.index("""int dai_ui_object_field(dai_ui *ui, const char *label, const char *value,
                        const char *icon) {""")
end = c.index("int dai_ui_array_end(dai_ui *ui, int count, int min_n, int max_n) {")
c = c[:start] + c[end:]

c = rep(c, """    bool over_f = inside_chk(ui, x, y, fw, h);
    bool over_b = inside_chk(ui, x + fw + 2.0f, y, bw, h);
    uint64_t oid = hash_id(label ? label : "objfield", x, y);
    if (over_f || over_b) { ui->mouse_over_ui = true; ui->cursor_want = DAI_CURSOR_HAND; }
    if ((over_f || over_b) && ui->input.mouse_down && !ui->prev.mouse_down) ui->active = oid;
    bool pressed = false;
    if (ui->active == oid && !ui->input.mouse_down) {
        pressed = over_f || over_b;
        ui->active = 0;
    }""",
"""    bool over_f = inside_chk(ui, x, y, fw, h);
    bool over_b = fw < w && inside_chk(ui, x + fw + 2.0f, y, bw, h);
    uint64_t oid = hash_id(label ? label : "objfield", x, y);
    if (over_f || over_b) {
        ui->mouse_over_ui = true;
        ui->cursor_want = DAI_CURSOR_HAND;
        // What a dragged hierarchy node asks: which field am I over?
        if (label) ui->hot_label = label;
    }
    if ((over_f || over_b) && ui->input.mouse_down && !ui->prev.mouse_down) ui->active = oid;
    int result = 0;
    if (ui->active == oid && !ui->input.mouse_down) {
        // The target button and the plate are two different questions: one
        // opens the list of everything, the other means "show me this one".
        if (over_b)      result = 2;
        else if (over_f) result = 1;
        ui->active = 0;
    }""", tag='object_field split')

c = rep(c, """    dai_ui_rrect(ui, x, y + 1.0f, fw, h - 2.0f, ui->style.rounding,
                 over_f ? ui->style.button_hover : ui->style.track);
    dai_ui_rect_outline(ui, x, y + 1.0f, fw, h - 2.0f, 1.0f, ui->style.panel_border);""",
"""    dai_ui_rrect(ui, x, y + 1.0f, fw, h - 2.0f, ui->style.rounding,
                 over_f ? ui->style.button_hover : ui->style.track);
    dai_ui_rect_outline(ui, x, y + 1.0f, fw, h - 2.0f, 1.0f,
                        over_f ? ui->style.accent : ui->style.panel_border);""",
       tag='object_field outline')

c = rep(c, """        else
            dai_ui_text(ui, bx + 4.0f, y + 2.0f, "...", ui->style.text);
    }
    return pressed ? 1 : 0;
}""",
"""        else
            dai_ui_text(ui, bx + 4.0f, y + 2.0f, "...", ui->style.text);
    }
    return result;
}""", tag='object_field return')

wr(C, c)
print('ok')
