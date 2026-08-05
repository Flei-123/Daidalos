#!/usr/bin/env python3
# patch82 - Ctrl+C/X/V never reached the UI at all, and the default material is
# a per-object colour again (which is also why everything went slow).
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p82'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# ============================ 1. the three keys were never delivered
# The Win32 backend drops every code point below 0x20 from the text stream, on
# purpose and with a comment saying why: backspace, tab and enter are KEYS, and
# a text field that also received them as characters would type them.
#
# Ctrl+C, Ctrl+X and Ctrl+V are in that range - 0x03, 0x18, 0x16 - so they were
# filtered out before the UI ever saw them, and reading them out of the text
# stream could not have worked on Windows however it was decoded. Ctrl+A worked
# throughout because it is not read there: the host sets a flag from the actual
# key. These three get the same treatment, for the same reason.
s = rd('include/dai_ui.h')
s = sub1(s,
"""    int      key_select_all;         /* Ctrl+A, edge triggered */""",
"""    int      key_select_all;         /* Ctrl+A, edge triggered */
    /* Ctrl+C / X / V, edge triggered. Flags rather than characters because a
     * control code is not a character: the window backend filters those out
     * of the text stream before anything can read them, and it is right to. */
    int      key_copy, key_cut, key_paste;""",
    'clipboard key flags')
wr('include/dai_ui.h', s)

s = rd('src/dai_ui.cpp')
s = sub1(s,
"""        if (in.key_ctrl) {
            int lo = sel_lo(), hi = sel_hi();
            bool has_sel = hi > lo;""",
"""        {
            int lo = sel_lo(), hi = sel_hi();
            bool has_sel = hi > lo;
            if ((in.key_copy || in.key_cut) && has_sel) {
                ui->clip_out.assign(buf + lo, (size_t)(hi - lo));
                ui->clip_out_set = true;
                if (in.key_cut) erase(lo, hi);
            }
            if (in.key_paste && !ui->clip_in.empty()) {
                // Line endings normalised on the way in: a paste from a
                // Windows editor otherwise carries a carriage return into
                // every line, and every one of them draws as a glyph.
                std::string t;
                t.reserve(ui->clip_in.size());
                for (char ch : ui->clip_in) if (ch != '\\r') t += ch;
                insert(t.c_str(), (int)t.size());
            }
        }
        if (false) {
            int lo = sel_lo(), hi = sel_hi();
            bool has_sel = hi > lo;""",
    'code editor uses the flags')
wr('src/dai_ui.cpp', s)

s = rd('examples/editor_demo.cpp')
s = sub1(s,
"""        {
            int a_now = ctrl && dai_window_key_down(win, DAI_KEY_A);
            in.key_select_all = a_now && !prev_ctrl_a;
            prev_ctrl_a = a_now;
        }""",
"""        {
            int a_now = ctrl && dai_window_key_down(win, DAI_KEY_A);
            in.key_select_all = a_now && !prev_ctrl_a;
            prev_ctrl_a = a_now;
        }
        {
            // The clipboard three, the same way: read off the KEY, edge
            // triggered. The text stream never carries them - see dai_ui.h.
            static int prev_c = 0, prev_x = 0, prev_v = 0;
            int c_now = ctrl && dai_window_key_down(win, (uint32_t)'c');
            int x_now = ctrl && dai_window_key_down(win, (uint32_t)'x');
            int v_now = ctrl && dai_window_key_down(win, (uint32_t)'v');
            in.key_copy  = c_now && !prev_c;
            in.key_cut   = x_now && !prev_x;
            in.key_paste = v_now && !prev_v;
            prev_c = c_now; prev_x = x_now; prev_v = v_now;
        }""",
    'host feeds the clipboard keys')
wr('examples/editor_demo.cpp', s)

# ==================== 2. the default material is a colour of its own again
# Reverted, because it was right: an object with no material uses THE default
# material, and the default material may look different per object - that is
# what makes a scene of untouched boxes readable.
#
# What stays from the attempt is the part that mattered: a material file
# holding pure black is nudged one 255th off zero when it is applied, so it is
# a real colour and not the sentinel. Both things are now true at once.
#
# It is also why everything got slower: dropping the guard meant
# dai_scene_set_color ran for every object on every sync, and a colour write
# is a change - so the renderer re-uploaded the lot, every frame.
s = rd('src/dai_doc_sync.cpp')
s = sub1(s,
"""            // The colour is the colour, including black. It used to be that
            // 0,0,0 meant "nobody chose one" and the scene kept whatever it
            // had picked from a palette - which made a black MATERIAL
            // impossible to express, because asking for black and asking for
            // "surprise me" were the same request.
            dai_scene_set_color(s->scene, l.entity, r.color);""",
"""            // A zero colour means "no colour was chosen": the object wears
            // the DEFAULT material, and the scene already picked a shade for
            // it from the palette when the entity was spawned. Pushing the
            // zero back would paint it black - and would also write a change
            // for every object on every sync, which is a full re-upload per
            // frame and reads as the whole editor going slow.
            //
            // Black as a deliberate colour is still expressible: a material
            // file holding 0,0,0 is applied as one 255th above it.
            if (r.color.x != 0.0f || r.color.y != 0.0f || r.color.z != 0.0f)
                dai_scene_set_color(s->scene, l.entity, r.color);""",
    'restore the default material')
wr('src/dai_doc_sync.cpp', s)

s = rd('src/dai_doc.cpp')
s = sub1(s,
"""    // A new object is light grey, and 0,0,0 is black.
    //
    // It used to be the other way round: zero meant "nobody chose a colour"
    // and the scene invented one from the node's id, which made a scene of
    // twenty untouched boxes readable. The cost only showed up once materials
    // became files - a black material asks for exactly the value that means
    // "surprise me", so black was the one colour that could not be set.
    //
    // Old scenes are unaffected in the way that matters: the text format only
    // writes a field when it DIFFERS from this default, so an object that
    // never had a colour chosen has no colour line at all and now loads as
    // this grey instead of a palette pick. It is a different picture, but a
    // predictable one, and nothing in it is black by accident.
    d.color = { 0.82f, 0.82f, 0.82f };
    d.shape = DAI_SHAPE_BOX;""",
"""    // Zero means "no colour chosen": the object wears the default material,
    // and what that looks like is the scene's business - a shade derived from
    // the id, so twenty untouched boxes are twenty distinguishable boxes.
    // A material file that wants black gets one 255th above zero when it is
    // applied, which is how both meanings fit in one field.
    d.shape = DAI_SHAPE_BOX;""",
    'default colour back to the sentinel')
wr('src/dai_doc.cpp', s)
print('patch82 ok')
