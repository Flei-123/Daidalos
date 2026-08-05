import io

# ===========================================================================
# 1) Die Maus, fuer Scripts. Ohne sie kann keine Kamera einem Blick folgen.
# ===========================================================================
p = 'include/dai_script.h'
s = io.open(p, encoding='utf-8').read()
old = """    int    (*grounded)(double id, void *user);
    void  *user;
} dai_script_play_host;"""
new = """    int    (*grounded)(double id, void *user);
    /* How far the pointer moved THIS frame, in pixels, and which buttons are
     * down as a mask: 1 left, 2 right, 4 middle. A delta rather than a
     * position because that is what a look control wants, and because a
     * position would make every script do the same subtraction. */
    void   (*mouse)(double *dx, double *dy, int *buttons, void *user);
    void  *user;
} dai_script_play_host;"""
assert s.count(old) == 1, 'play host struct not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)

p = 'src/dai_script.cpp'
s = io.open(p, encoding='utf-8').read()
old = """JSValue js_body_get_vel(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {"""
new = """// input.mouseDX() / input.mouseDY() / input.mouseButton(n)
JSValue js_input_mouse_axis(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv, int axis) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    double dx = 0, dy = 0; int b = 0;
    if (s->has_play && s->play.mouse) s->play.mouse(&dx, &dy, &b, s->play.user);
    (void)argc; (void)argv;
    return JS_NewFloat64(ctx, axis ? dy : dx);
}
JSValue js_input_mouse_dx(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    return js_input_mouse_axis(ctx, t, argc, argv, 0);
}
JSValue js_input_mouse_dy(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    return js_input_mouse_axis(ctx, t, argc, argv, 1);
}
JSValue js_input_mouse_button(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    double dx = 0, dy = 0; int b = 0;
    if (s->has_play && s->play.mouse) s->play.mouse(&dx, &dy, &b, s->play.user);
    int which = argc >= 1 ? (int)arg_num(ctx, argv[0]) : 0;
    int bit = which == 1 ? 2 : (which == 2 ? 4 : 1);   // 0 left, 1 right, 2 middle
    return (b & bit) ? JS_TRUE : JS_FALSE;
}

JSValue js_body_get_vel(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {"""
assert s.count(old) == 1, 'js_body_get_vel anchor not found'
s = s.replace(old, new)

old = """    JS_SetPropertyStr(s->ctx, input, "key", JS_NewCFunction(s->ctx, js_input_key, "key", 1));"""
new = """    JS_SetPropertyStr(s->ctx, input, "key", JS_NewCFunction(s->ctx, js_input_key, "key", 1));
    JS_SetPropertyStr(s->ctx, input, "mouseDX", JS_NewCFunction(s->ctx, js_input_mouse_dx, "mouseDX", 0));
    JS_SetPropertyStr(s->ctx, input, "mouseDY", JS_NewCFunction(s->ctx, js_input_mouse_dy, "mouseDY", 0));
    JS_SetPropertyStr(s->ctx, input, "mouseButton", JS_NewCFunction(s->ctx, js_input_mouse_button, "mouseButton", 1));"""
assert s.count(old) == 1, 'input binding not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)
print('dai_script: input.mouseDX/mouseDY/mouseButton')

# ===========================================================================
# 2) Host: Delta pro Frame, und Space gehoert dem Spiel, sobald es laeuft.
# ===========================================================================
p = 'examples/editor_demo.cpp'
s = io.open(p, encoding='utf-8').read()

old = """static dai_script_play_host g_play_host = {
    sp_key, sp_get_vel, sp_set_vel, sp_impulse, sp_grounded, nullptr
};"""
new = """// Mouse movement since the last frame, in pixels, plus the button mask. The
// editor loop fills these in; a script asks for them through input.mouseDX().
static double g_mouse_dx = 0.0, g_mouse_dy = 0.0;
static int    g_mouse_buttons = 0;
static void sp_mouse(double *dx, double *dy, int *buttons, void *) {
    if (dx) *dx = g_mouse_dx;
    if (dy) *dy = g_mouse_dy;
    if (buttons) *buttons = g_mouse_buttons;
}

static dai_script_play_host g_play_host = {
    sp_key, sp_get_vel, sp_set_vel, sp_impulse, sp_grounded, sp_mouse, nullptr
};"""
assert s.count(old) == 1, 'g_play_host not found'
s = s.replace(old, new)

old = """        if (pressed(7) && !typing) {
            if (dai_editor_state_get(ed) == DAI_EDITOR_PLAY) dai_editor_pause(ed);
            else dai_editor_play(ed);
        }"""
new = """        // Play/pause. Ctrl+P always, Space only while EDITING.
        //
        // Space used to toggle play in both states, so the first jump in any
        // game paused the editor: the same key was the game's and the
        // editor's at the same moment, and the editor won. Unity's binding is
        // Ctrl+P for exactly this reason, and while the game is running the
        // keyboard belongs to the game.
        int p_key = dai_window_key_down(win, 'p');
        int ctrl_p = ctrl && p_key && !prev_ctrl_p;
        prev_ctrl_p = p_key;
        int playing_now = dai_editor_state_get(ed) == DAI_EDITOR_PLAY;
        if ((ctrl_p || (pressed(7) && !playing_now)) && !typing) {
            if (playing_now) dai_editor_pause(ed);
            else dai_editor_play(ed);
        }"""
assert s.count(old) == 1, 'play toggle not found'
s = s.replace(old, new)

old = """    int prev_ctrl_a = 0;"""
new = """    int prev_ctrl_a = 0;
    int prev_ctrl_p = 0;
    int prev_mouse_x = 0, prev_mouse_y = 0, mouse_seen = 0;"""
assert s.count(old) == 1, 'prev_ctrl_a not found'
s = s.replace(old, new)

# Delta jeden Frame ausrechnen, direkt bevor die Shortcuts laufen.
old = """        int ctrl = dai_window_key_down(win, DAI_KEY_CTRL_L) || dai_window_key_down(win, DAI_KEY_CTRL_R);
        int keys[10] = {"""
new = """        // Pointer delta for scripts. Taken here, once, so every script in the
        // frame sees the SAME movement - asking the window per call would give
        // the second caller a delta of zero.
        {
            int mxr = 0, myr = 0; uint32_t mb = 0;
            if (dai_window_mouse(win, &mxr, &myr, &mb)) {
                g_mouse_dx = mouse_seen ? (double)(mxr - prev_mouse_x) : 0.0;
                g_mouse_dy = mouse_seen ? (double)(myr - prev_mouse_y) : 0.0;
                prev_mouse_x = mxr; prev_mouse_y = myr; mouse_seen = 1;
                g_mouse_buttons = (int)mb;
            } else {
                g_mouse_dx = g_mouse_dy = 0.0;
            }
        }
        int ctrl = dai_window_key_down(win, DAI_KEY_CTRL_L) || dai_window_key_down(win, DAI_KEY_CTRL_R);
        int keys[10] = {"""
assert s.count(old) == 1, 'keys block not found'
s = s.replace(old, new)

io.open(p, 'w', encoding='utf-8').write(s)
print('editor_demo: mouse delta for scripts, Ctrl+P plays, Space belongs to the game')
