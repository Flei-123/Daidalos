import io

p = 'examples/editor_demo.cpp'
s = io.open(p, encoding='utf-8').read()

# ---------------------------------------------------------------------------
# 1) WASD tat nichts, weil das Fenster nie an die Scripts gereicht wurde.
# ---------------------------------------------------------------------------
old = """    dai_window *win = dai_window_open(r, "Daidalos Editor", W, H, err, sizeof(err));
    if (!win) { std::printf("window failed: %s\\n", err); return 1; }"""
new = """    dai_window *win = dai_window_open(r, "Daidalos Editor", W, H, err, sizeof(err));
    if (!win) { std::printf("window failed: %s\\n", err); return 1; }
    // THE line that made input.key() work. sp_key() and nv_key() ask this
    // window whether a key is down, and it was declared and never assigned -
    // so every script saw every key as "up" and WASD moved nothing, in every
    // build that has ever shipped. A pointer that is only ever read is not a
    // cache, it is a missing line.
    g_win_for_scripts = win;"""
assert s.count(old) == 1, 'window open not found'
s = s.replace(old, new)

# ---------------------------------------------------------------------------
# 2) Ctrl+S ausserhalb des Script-Tabs speicherte trotzdem das Script.
# ---------------------------------------------------------------------------
old = """        if (ctrl && pressed(8) && dai_editor_ui_script_save(panels)) {
            // handled by the script panel
        } else if (ctrl && pressed(8) && !typing) {"""
new = """        // ...and it only means the SCRIPT when the code editor actually has the
        // keyboard. It used to try the script first, unconditionally: with any
        // file open in the Script tab, Ctrl+S anywhere in the editor wrote that
        // file and reported "saved playercontroller.js" while the scene - the
        // thing you had just changed - stayed on disk as it was. A save that
        // saves something else is worse than no save.
        int code_has_kb = dai_ui_code_focused(ui);
        if (ctrl && pressed(8) && code_has_kb && dai_editor_ui_script_save(panels)) {
            // handled by the script panel
        } else if (ctrl && pressed(8) && !typing) {"""
assert s.count(old) == 1, 'ctrl+s block not found'
s = s.replace(old, new)

# ---------------------------------------------------------------------------
# 3) Typisierte Objektfelder: der Typ eines "@param" darf eine Komponente sein.
# ---------------------------------------------------------------------------
old = """            if (word(second, sizeof(second))) {
                static const char *TYPES[] = { "float", "number", "int", "bool", "string", "text", "node" };"""
new = """            if (word(second, sizeof(second))) {
                // Unity's rule: the DECLARED TYPE of a reference decides what
                // the picker offers and what a drag is allowed to drop. There
                // the type is a class (Transform, Rigidbody, Camera); here it
                // is the component a node carries, which is the same question
                // asked of a different object model.
                static const char *TYPES[] = { "float", "number", "int", "bool", "string", "text",
                                               "node", "object", "transform", "camera", "light",
                                               "rigidbody", "body", "collider", "sprite", "audio",
                                               "mesh" };"""
assert s.count(old) == 1, 'TYPES list not found'
s = s.replace(old, new)

io.open(p, 'w', encoding='utf-8').write(s)
print('editor_demo.cpp: script keyboard, honest Ctrl+S, typed @param')
