#!/usr/bin/env python3
# patch37 - reveal the selection, group the toolbar, calm the palette down.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p37'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# ------------------------------------------------------------- scroll reveal
s = rd('include/dai_ui.h')
s = sub1(s, "DAI_API void dai_ui_scroll_end(dai_ui *ui);",
"""DAI_API void dai_ui_scroll_end(dai_ui *ui);
/* Bring a rectangle inside the current scroll region into view. What "Focus
 * selection" needs: an object selected in the viewport is no use if its row
 * in the hierarchy is forty rows below the fold. */
DAI_API void dai_ui_scroll_reveal(dai_ui *ui, float y, float h);""",
'reveal decl')
wr('include/dai_ui.h', s)

s = rd('src/dai_ui.cpp')
s = sub1(s, "void dai_ui_scroll_end(dai_ui *ui) {",
"""void dai_ui_scroll_reveal(dai_ui *ui, float y, float h) {
    if (!ui || ui->scroll_stack.empty()) return;
    const dai_ui::ScrollFrame &f = ui->scroll_stack.back();
    float &off = ui->scroll_of(f.id);
    float top = f.y + 2.0f, bot = f.y + f.h - 2.0f;
    if (y < top)              off -= (top - y);
    else if (y + h > bot)     off += (y + h - bot);
    if (off < 0.0f) off = 0.0f;
}

void dai_ui_scroll_end(dai_ui *ui) {""",
'reveal impl')

# ------------------------------------------------------------- the palette
# Warmer neutrals and a brighter, less muddy selection blue. The old accent
# (#2C5D87) is Unity's, and next to a blue-grey sky in the viewport it reads
# as "slightly different grey" rather than "this one is selected".
s = sub1(s, """    s.button_active= rgba(0x2C, 0x5D, 0x87, 255);   // Unity's selection blue
    s.accent       = rgba(0x2C, 0x5D, 0x87, 255);""",
"""    s.button_active= rgba(0x2F, 0x6C, 0xB5, 255);   // selection: readable, not neon
    s.accent       = rgba(0x3D, 0x84, 0xD8, 255);""",
'palette accent')
s = sub1(s, """    s.button       = rgba(0x50, 0x50, 0x50, 255);
    s.button_hover = rgba(0x5D, 0x5D, 0x5D, 255);""",
"""    s.button       = rgba(0x48, 0x48, 0x4A, 255);
    s.button_hover = rgba(0x58, 0x58, 0x5B, 255);""",
'palette buttons')
wr('src/dai_ui.cpp', s)

# --------------------------------------------------- the hierarchy reveals
s = rd('src/dai_editor_ui.cpp')
s = sub1(s,
"""    dai_ui_scroll_begin(p->ui, "hierarchy", h);""",
"""    dai_ui_scroll_begin(p->ui, "hierarchy", h);
    p->reveal_row_wanted = p->reveal_selection;""",
'reveal arm')
s = sub1(s, "    dai_ui_scroll_end(p->ui);\n}\n\nvoid dai_editor_ui_hierarchy",
            "    p->reveal_selection = 0;\n    dai_ui_scroll_end(p->ui);\n}\n\nvoid dai_editor_ui_hierarchy",
         'reveal disarm')
s = sub1(s, "    int   reveal_selection = 0;    // scroll the hierarchy to the selection",
            "    int   reveal_selection = 0;    // scroll the hierarchy to the selection\n"
            "    int   reveal_row_wanted = 0;",
         'reveal field2')
wr('src/dai_editor_ui.cpp', s)

# The row itself: whoever draws the selected node asks to be shown.
s = rd('src/dai_editor_ui.cpp')
import re
m = re.search(r"void draw_subtree\(dai_editor_ui \*p[^\n]*\n", s)
if not m:
    print('MISS: draw_subtree'); sys.exit(1)
# find the first tree item call inside draw_subtree
i = s.index('int rc = dai_ui_tree_item_icon(', m.end())
line_start = s.rindex('\n', 0, i) + 1
indent = s[line_start:i]
s = s[:line_start] + indent + """// "Focus selection" has to work when the row is below the fold.
""" + indent + """if (p->reveal_row_wanted && dai_editor_is_selected(p->ed, n)) {
""" + indent + """    float rvx = 0, rvy = 0;
""" + indent + """    dai_ui_cursor_pos(p->ui, &rvx, &rvy);
""" + indent + """    dai_ui_scroll_reveal(p->ui, rvy, 20.0f);
""" + indent + """    p->reveal_row_wanted = 0;
""" + indent + """}
""" + s[line_start:]
wr('src/dai_editor_ui.cpp', s)
print('patch37 ok')
