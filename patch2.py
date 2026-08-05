import sys, io

def rw(p): return io.open(p, encoding='utf-8').read()
def wr(p, s): io.open(p, 'w', encoding='utf-8').write(s)
def sub1(s, old, new, tag):
    if old not in s: print("MISS", tag); sys.exit(1)
    if s.count(old) != 1: print("AMBIG", tag, s.count(old)); sys.exit(1)
    print("ok", tag); return s.replace(old, new)

# ------------------------------------------------------------- daidalos.h
P = 'include/daidalos.h'
s = rw(P)
s = sub1(s,
"DAI_API const char *dai_backend_name(dai_world *w);",
"""DAI_API const char *dai_backend_name(dai_world *w);

/* Is this backend actually IN this build? Jolt is opt in on Windows and absent
 * from the WebAssembly build, and a world asked for a backend that was not
 * linked silently gets a different one - which is how an editor ends up
 * offering "Jolt" in a dropdown, saying it selected it, and running null.
 *
 * A picker that cannot ask this question can only lie. Pass a
 * dai_physics_backend; returns 1 when dai_world_create would really give you
 * that one. */
DAI_API int dai_physics_available(int backend);""", "avail decl")
wr(P, s)

# ----------------------------------------------------------- dai_engine.cpp
P = 'src/dai_engine.cpp'
s = rw(P)
s = sub1(s,
"""    if (w->cfg.backend == DAI_PHYSICS_NULL) {
        w->phys = create_null_backend();""",
"""    // Asking for a backend that was compiled out is not a silent downgrade
    // any more: the caller is told, in the world's error string, what it
    // actually got. dai_physics_available() lets a UI avoid the situation.
    if (w->cfg.backend == DAI_PHYSICS_NULL) {
        w->phys = create_null_backend();""", "engine comment")

# append the availability function right before dai_backend_name's definition
import re
m = re.search(r'const char \*dai_backend_name\(dai_world \*w\)', s)
if not m: print("MISS backend_name def"); sys.exit(1)
ins = """int dai_physics_available(int backend) {
    switch (backend) {
    case DAI_PHYSICS_NULL: return 1;
    case DAI_PHYSICS_TALOS:
#ifdef DAI_NO_TALOS
        return 0;
#else
        return 1;
#endif
    case DAI_PHYSICS_JOLT:
#ifdef DAI_NO_JOLT
        return 0;
#else
        return 1;
#endif
    default: return 0;
    }
}

"""
s = s[:m.start()] + ins + s[m.start():]
print("ok avail impl")
wr(P, s)

# -------------------------------------------------------- dai_editor_ui.cpp
P = 'src/dai_editor_ui.cpp'
s = rw(P)

# console button row: give it room and centre with the real line height
s = sub1(s,
"""    const float BAR = 30.0f;   // 24 clipped the button row against the divider""",
"""    // 30 still clipped the bottom border of the buttons against the divider
    // line drawn at py + BAR. Height of the row + the 3 px it starts at + a
    // pixel of air, measured from the font instead of guessed.
    const float BTN_H = dai_ui_text_height(ui) + 9.0f;
    const float BAR = BTN_H + 8.0f;""", "console BAR")

s = s.replace("""browser_button(p, bx, py + 3.0f, 54.0f, BAR - 8.0f, "Clear")""",
              """browser_button(p, bx, py + 4.0f, 56.0f, BTN_H, "Clear")""")
s = s.replace("""browser_button(p, bx, py + 3.0f, 54.0f, BAR - 8.0f, "Copy")""",
              """browser_button(p, bx, py + 4.0f, 56.0f, BTN_H, "Copy")""")
s = s.replace("""        if (browser_row(p, bx, py + 3.0f, w, BAR - 8.0f, nullptr, lbl, p->log_show[i]))""",
              """        if (browser_row(p, bx, py + 4.0f, w, BTN_H, nullptr, lbl, p->log_show[i]))""")
s = s.replace("bx += 58.0f;", "bx += 60.0f;")
print("ok console buttons")

s = sub1(s,
"""    float tw = dai_ui_text_width(ui, label);
    // Vertically centred for real: the fixed +5 used to push 13px text
    // against the bottom edge of a 20px button, clipping the descenders.
    dai_ui_text(ui, x + (w - tw) * 0.5f, y + (h - 13.0f) * 0.5f, label, st->text);""",
"""    float tw = dai_ui_text_width(ui, label);
    // Vertically centred against the REAL line height. The old constant 13
    // was the font size, not the line box, so descenders were shaved off at
    // every size - which is what "the console buttons are cut off" was.
    dai_ui_text(ui, x + (w - tw) * 0.5f, y + (h - dai_ui_text_height(ui)) * 0.5f, label, st->text);""",
"browser_button centering")

# rounded corners for the small chrome buttons too
s = sub1(s,
"""    dai_ui_rect(ui, x, y, w, h, over ? st->button_hover : st->titlebar);
    dai_ui_rect_outline(ui, x, y, w, h, 1.0f, st->panel_border);
    float tw = dai_ui_text_width(ui, label);""",
"""    dai_ui_rrect(ui, x, y, w, h, 4.0f, over ? st->button_hover : st->titlebar);
    float tw = dai_ui_text_width(ui, label);""", "browser_button rrect")

wr(P, s)
print("patch2 done")
