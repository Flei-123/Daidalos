#!/usr/bin/env python3
# patch79 - black was a sentinel, a material could not be dropped in the
# viewport, F2 lost its keyboard to the code editor, and the code editor had
# no clipboard.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p79'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# ======================================= 1. black meant "choose one for me"
# dai_node_desc::color documents 0,0,0 as "a stable colour derived from the
# id" - the reason a fresh cube is not white. Which means a material whose
# colour IS black asks for exactly the thing the sentinel means, and the
# object comes back pink. The material is not wrong and the sentinel is not
# wrong; they simply cannot both have that value.
#
# The material wins, because it was set on purpose. One 255th of a unit above
# black is not a colour anybody can tell from black, and it is not the
# sentinel.
s = rd('examples/editor_demo.cpp')
s = sub1(s,
"""        dai_node_desc before = r;
        r.color = m.color;
        r.roughness = m.roughness;
        r.emissive = m.emissive;""",
"""        dai_node_desc before = r;
        r.color = m.color;
        // Never exactly 0,0,0 - see above. Nudged, not clamped to grey.
        if (r.color.x == 0.0f && r.color.y == 0.0f && r.color.z == 0.0f)
            r.color = dai_vec3{ 1.0f / 255.0f, 1.0f / 255.0f, 1.0f / 255.0f };
        r.roughness = m.roughness;
        r.emissive = m.emissive;""",
    'black is not the sentinel')
wr('examples/editor_demo.cpp', s)

s = rd('src/dai_editor_ui.cpp')

# ============================== 2. a material dropped in the VIEWPORT lands
s = sub1(s,
"""                } else if (is_material_file(p->drag_script) &&
                           (p->hover_node != DAI_INVALID_NODE ||
                            dai_ui_root_hovered(ui, "Inspector"))) {""",
"""                } else if (is_material_file(p->drag_script) &&
                           (p->hover_node != DAI_INVALID_NODE ||
                            dai_ui_root_hovered(ui, "Inspector") ||
                            dai_ui_root_hovered(ui, "Scene"))) {""",
    'material drop accepts the viewport')

s = sub1(s,
"""                    dai_node target = p->hover_node != DAI_INVALID_NODE
                                    ? p->hover_node
                                    : (dai_editor_selection_count(p->ed) > 0
                                       ? dai_editor_selected(p->ed, 0) : DAI_INVALID_NODE);""",
"""                    // In the viewport it is whatever is UNDER the pointer -
                    // the same pick a click makes. Falling back to "whatever
                    // was selected" there would paint an object you cannot
                    // see because the dragged pill is over it.
                    dai_node target = DAI_INVALID_NODE;
                    if (p->hover_node != DAI_INVALID_NODE) target = p->hover_node;
                    else if (dai_ui_root_hovered(ui, "Scene"))
                        target = dai_editor_pick(p->ed, dmx, dmy);
                    else if (dai_editor_selection_count(p->ed) > 0)
                        target = dai_editor_selected(p->ed, 0);""",
    'pick under the pointer')

s = sub1(s,
"""            else if (is_material_file(p->drag_script) &&
                     (p->hover_node != DAI_INVALID_NODE || dai_ui_root_hovered(ui, "Inspector")))
                lbl += "  ->  apply material";""",
"""            else if (is_material_file(p->drag_script) &&
                     (p->hover_node != DAI_INVALID_NODE ||
                      dai_ui_root_hovered(ui, "Inspector") ||
                      dai_ui_root_hovered(ui, "Scene")))
                lbl += "  ->  apply material";""",
    'material pill in the viewport')
wr('src/dai_editor_ui.cpp', s)

# ============ 3. the code editor was answering "is a text field being typed in"
# dai_ui_text_active means "a text FIELD has the keyboard", and half the editor
# uses it to decide whether a shortcut belongs to the scene or to a caret. The
# code editor folded itself into that answer, so with a script open F2 was
# never the browser's, and an inline rename could not tell it had lost focus -
# which is what "F2 does nothing again" was.
#
# Two questions, two answers: text_active is still only about fields, and
# dai_ui_typing() is the one a host should ask before eating a key.
s = rd('src/dai_ui.cpp')
s = sub1(s,
"""int  dai_ui_text_active(const dai_ui *ui) {
    return ui && (ui->edit.editing || ui->code_focus) ? 1 : 0;
}""",
"""int  dai_ui_text_active(const dai_ui *ui) { return ui && ui->edit.editing ? 1 : 0; }

int  dai_ui_typing(const dai_ui *ui) {
    return ui && (ui->edit.editing || ui->code_focus) ? 1 : 0;
}""",
    'split text_active and typing')
wr('src/dai_ui.cpp', s)

s = rd('include/dai_ui.h')
s = sub1(s,
"""DAI_API int  dai_ui_text_active(const dai_ui *ui);""",
"""DAI_API int  dai_ui_text_active(const dai_ui *ui);
/* "Is the keyboard somebody's?" - a text field OR the code editor. What a host
 * asks before treating a key as a shortcut. dai_ui_text_active is the narrower
 * question (a FIELD is being edited) and stays that way, because widgets use
 * it to decide whether THEY lost focus - and a code editor two panels away is
 * not a reason for a rename to give up. */
DAI_API int  dai_ui_typing(const dai_ui *ui);""",
    'typing decl')
wr('include/dai_ui.h', s)

s = rd('examples/editor_demo.cpp')
s = sub1(s,
"""        if (dai_window_key_down(win, DAI_KEY_F2) && !prev_f2 && !dai_ui_text_active(ui)) {""",
"""        if (dai_window_key_down(win, DAI_KEY_F2) && !prev_f2 && !dai_ui_typing(ui)) {""",
    'F2 asks the wider question')
wr('examples/editor_demo.cpp', s)
print('patch79 ok')
