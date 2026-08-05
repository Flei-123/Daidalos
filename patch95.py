import io

# ---------------------------------------------------------------------------
# 1) The camera gesture ends when the button that STARTED it is released,
#    even if another button is still down.
# ---------------------------------------------------------------------------
p = 'src/dai_editor.cpp'
s = io.open(p, encoding='utf-8').read()

old = """    int   cam_mode = 0;                // 0 none, 1 look, 2 pan, 3 orbit, 4 dolly
    int   cam_frozen = 0;              // the mode is locked while any button is held"""
new = """    int   cam_mode = 0;                // 0 none, 1 look, 2 pan, 3 orbit, 4 dolly
    int   cam_frozen = 0;              // the mode is locked while its button is held
    int   cam_btn = 0;                 // 1 left, 2 right, 4 middle: what started it"""
assert s.count(old) == 1, 'cam state fields not found'
s = s.replace(old, new)

old = """    int holding = in->mouse_left || in->mouse_right || in->mouse_middle;
    if (!holding) e->cam_frozen = 0;
    else if (e->cam_mode == 0 || !e->cam_frozen) {
        e->cam_frozen = 1;
        e->cam_mode = mode;
        e->cam_last_x = in->mouse_x;
        e->cam_last_y = in->mouse_y;
    }"""
new = """    //
    // But "held" has to mean THE BUTTON THIS GESTURE STARTED WITH, not "any
    // button at all". Otherwise a pan that is never fully released hands its
    // mode to the next gesture: hold middle, press alt+left, let middle go -
    // and the camera keeps panning with an anchor from a mouse position two
    // gestures ago. Which button drives which mode is fixed, so it can simply
    // be recorded when the mode is chosen.
    int held = (in->mouse_left ? 1 : 0) | (in->mouse_right ? 2 : 0) | (in->mouse_middle ? 4 : 0);
    int holding = held != 0;
    if (!holding) { e->cam_frozen = 0; e->cam_btn = 0; }
    else if (e->cam_mode == 0 || !e->cam_frozen || !(held & e->cam_btn)) {
        e->cam_frozen = 1;
        e->cam_mode = mode;
        e->cam_btn = mode == 3 ? 1 :          // alt + left  -> orbit
                     mode == 4 ? 2 :          // alt + right -> dolly
                     mode == 1 ? 2 :          // right       -> look
                     mode == 2 ? 4 : 0;       // middle      -> pan
        e->cam_last_x = in->mouse_x;
        e->cam_last_y = in->mouse_y;
    }"""
assert s.count(old) == 1, 'cam freeze block not found'
s = s.replace(old, new)

io.open(p, 'w', encoding='utf-8').write(s)
print('dai_editor.cpp: camera gesture bound to its own button')
