import io

p = 'tests/test_ui_field.cpp'
s = io.open(p, encoding='utf-8').read()

anchor = """    std::printf("cursor shapes\\n");"""
assert s.count(anchor) == 1, 'anchor not found'

block = """    // ---- the keyboard is given back when the field stops being drawn -----
    // Twice now F2 stopped renaming things because "a text field has the
    // keyboard" stayed true forever: once because a code editor set the flag
    // and never cleared it, once because a panel closed while one of its
    // fields was in edit. Both look identical from the outside - a shortcut
    // that worked at startup and was gone after some clicking.
    std::printf("a field that is no longer drawn lets go\\n");
    value = 3.0f;
    begin(BOX_X, BOX_Y, 0); field(); endf();
    begin(BOX_X, BOX_Y, 1); field(); endf();          // click: it has the keyboard
    CHECK(dai_ui_text_active(ui) == 1, "clicking the field did not take the keyboard");
    begin(BOX_X, BOX_Y, 0); field(); endf();          // still drawn, still editing
    CHECK(dai_ui_text_active(ui) == 1, "the field lost the keyboard while still on screen");
    begin(BOX_X, BOX_Y, 0); /* panel closed: no field */ endf();
    CHECK(dai_ui_text_active(ui) == 0,
          "a field nobody drew still holds the keyboard - this is the F2 bug");

"""

s = s.replace(anchor, block + anchor)
io.open(p, 'w', encoding='utf-8').write(s)
print('test_ui_field.cpp patched')
