import io, sys

p = 'src/dai_ui.cpp'
s = io.open(p, encoding='utf-8').read()

# 1) The "is this field still on screen" mark was taken at the TOP of the
#    function, where a field that is about to be OPENED by this very click is
#    not editing yet. dai_ui_end then found a fresh edit nobody had drawn and
#    closed it in the same frame: clicking a field did nothing at all.
old_top = """    bool editing = ui->edit.editing && ui->edit.id == id;
    if (editing) ui->edit_seen = id;      // it is on screen, so it may keep it
"""
new_top = """    bool editing = ui->edit.editing && ui->edit.id == id;
"""
assert s.count(old_top) == 1, 'top mark not found'
s = s.replace(old_top, new_top)

# 2) Mark it at the END instead, when the frame's verdict is in. A field that
#    was opened by this frame's click has drawn itself by now; one that
#    committed or cancelled is closed and must NOT be marked, or the fuse in
#    dai_ui_end never blows.
old_end = """    if (editing) {
        edit_draw(ui, x, y, w, h, text_col ? text_col : ui->style.text, pad);
    }
    if (editing) ui->edit.opened_now = false;
    return changed;
}"""
new_end = """    if (editing) {
        edit_draw(ui, x, y, w, h, text_col ? text_col : ui->style.text, pad);
    }
    if (editing) ui->edit.opened_now = false;
    // Taken here, not on the way in: a click OPENS the edit in the middle of
    // this function, and a field that is opened and closed again in the same
    // frame (Enter, Escape, a click on the way out) must not be marked at all.
    if (ui->edit.editing && ui->edit.id == id) ui->edit_seen = id;
    return changed;
}"""
assert s.count(old_end) == 1, 'end of text_field_impl not found'
s = s.replace(old_end, new_end)

io.open(p, 'w', encoding='utf-8').write(s)
print('dai_ui.cpp patched')
