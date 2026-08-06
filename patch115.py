import io

# ===========================================================================
# Der Weg vom Dokument bis in den Solver: Feld, Vergleich, Weitergabe, Datei.
# ===========================================================================
p = 'include/dai_doc.h'
s = io.open(p, encoding='utf-8').read()
old = """    int      no_sleeping;"""
new = """    int      no_sleeping;
    /* Unity's Constraints, as a dai_freeze mask. A frozen axis is one the
     * SOLVER may not change - a script that sets the transform still moves
     * the object. 0 = nothing frozen, which is what every scene written
     * before this field existed means. */
    uint32_t freeze;"""
assert s.count(old) == 1, 'no_sleeping field not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)

p = 'src/dai_doc_sync.cpp'
s = io.open(p, encoding='utf-8').read()
old = """           a.no_sleeping != b.no_sleeping ||"""
new = """           a.no_sleeping != b.no_sleeping ||
           a.freeze != b.freeze ||"""
assert s.count(old) == 1, 'no_sleeping compare not found'
s = s.replace(old, new)
old = """    d.body.no_sleeping = r.no_sleeping;"""
new = """    d.body.no_sleeping = r.no_sleeping;
    d.body.frozen = r.freeze;"""
assert s.count(old) == 1, 'no_sleeping assign not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)

p = 'src/dai_doc_text.cpp'
s = io.open(p, encoding='utf-8').read()
old = """        if (r.sprite != def.sprite)   put(s, "  sprite %d\\n", r.sprite);"""
new = """        if (r.freeze != def.freeze)   put(s, "  freeze %u\\n", (unsigned)r.freeze);
        if (r.sprite != def.sprite)   put(s, "  sprite %d\\n", r.sprite);"""
assert s.count(old) == 1, 'sprite writer not found'
s = s.replace(old, new)
old = """        else if (key == "sprite")  { ok = parse_i32(after, &rec.sprite); }"""
new = """        else if (key == "freeze")  { int fv = 0; ok = parse_i32(after, &fv); rec.freeze = (uint32_t)fv; }
        else if (key == "sprite")  { ok = parse_i32(after, &rec.sprite); }"""
assert s.count(old) == 1, 'sprite reader not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)
print('freeze: document, sync, file')

# ===========================================================================
# Inspector: Unitys Constraints-Klappe unter dem Rigidbody.
# ===========================================================================
p = 'src/dai_editor_ui.cpp'
s = io.open(p, encoding='utf-8').read()
old = """    // ---- Text ---------------------------------------------------------------"""
new = """    // ---- Constraints ---------------------------------------------------------
    // Unity's, and for the same reason: a capsule that must not tip over is
    // ONE bit here, and the alternative is a script fighting the solver with
    // corrective torque every frame. Frozen means the SOLVER may not move it;
    // a script setting the transform still can.
    if (!r.no_rigidbody && !r.no_body && r.motion == DAI_DYNAMIC) {
        dai_ui_spacing(p->ui, 2.0f);
        dai_ui_header(p->ui, "Constraints", &p->fold_freeze, nullptr);
        if (!p->fold_freeze) {
            struct Bit { const char *label; uint32_t bit; };
            static const Bit POS[3] = { { "X##fpx", DAI_FREEZE_POS_X },
                                        { "Y##fpy", DAI_FREEZE_POS_Y },
                                        { "Z##fpz", DAI_FREEZE_POS_Z } };
            static const Bit ROT[3] = { { "X##frx", DAI_FREEZE_ROT_X },
                                        { "Y##fry", DAI_FREEZE_ROT_Y },
                                        { "Z##frz", DAI_FREEZE_ROT_Z } };
            dai_ui_row(p->ui, 0.0f);
            dai_ui_label(p->ui, "Freeze Position");
            for (const Bit &b : POS) {
                int on = (r.freeze & b.bit) != 0;
                if (dai_ui_checkbox(p->ui, b.label, &on))
                    r.freeze = on ? (r.freeze | b.bit) : (r.freeze & ~b.bit);
            }
            dai_ui_row_end(p->ui);
            dai_ui_row(p->ui, 0.0f);
            dai_ui_label(p->ui, "Freeze Rotation");
            for (const Bit &b : ROT) {
                int on = (r.freeze & b.bit) != 0;
                if (dai_ui_checkbox(p->ui, b.label, &on))
                    r.freeze = on ? (r.freeze | b.bit) : (r.freeze & ~b.bit);
            }
            dai_ui_row_end(p->ui);
            // The two combinations anyone actually types out by hand.
            dai_ui_row(p->ui, 0.0f);
            if (dai_ui_button_fit(p->ui, "Upright")) r.freeze |= DAI_FREEZE_UPRIGHT;
            if (dai_ui_button_fit(p->ui, "2D plane")) r.freeze |= DAI_FREEZE_2D;
            dai_ui_row_end(p->ui);
            if (r.freeze && dai_ui_button_fit(p->ui, "Clear constraints")) r.freeze = 0;
        }
    }

    // ---- Text ---------------------------------------------------------------"""
assert s.count(old) == 1, 'text section anchor not found'
s = s.replace(old, new)

old = """    int  fold_text = 1;"""
new = """    int  fold_text = 1;
    int  fold_freeze = 1;"""
assert s.count(old) == 1, 'fold_text not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)
print('inspector: Constraints')
