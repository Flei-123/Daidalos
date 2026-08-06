import io

# ===========================================================================
# UI aus Code. Die Komponenten sind der Weg fuer feste Anzeigen; ein Menue mit
# zehn Zeilen, die je nach Spielstand da sind oder nicht, ist keine feste
# Anzeige - dafuer braucht es eine Zeichenschnittstelle.
#
# Immediate mode, wie dai_ui selbst: pro Frame sagen, was da sein soll. Kein
# Baum, kein Behalten, kein Aufraeumen - und damit auch keine Klasse von
# Fehlern, bei der ein Element aus dem letzten Spielstand haengen bleibt.
# ===========================================================================
p = 'include/dai_script.h'
s = io.open(p, encoding='utf-8').read()
old = """/* Any host value scripts can read through `state.<name>`. */"""
new = """/* Immediate mode UI for the GAME, as the global `gui`.
 *
 *   function frame() {
 *       gui.text(20, 20, "Score: " + score, 28, 0xFFFFFFFF);
 *       if (gui.button(20, 60, 160, 34, "Restart")) restart();
 *   }
 *
 * Coordinates are pixels from the top left of the view. This is the same
 * model dai_ui uses and the same one the editor is written in: a frame says
 * what should be on the screen, and nothing has to be deleted afterwards. A
 * retained tree would need creation, destruction and an owner for every
 * label - and the first bug it produces is a menu from the last game still
 * hanging there after a restart.
 *
 * The Text and Image COMPONENTS are the other half: they are for what is
 * always there, they are placed with a mouse, and they survive without a
 * script running. Use those for a HUD, this for anything conditional. */
typedef struct dai_script_gui_host {
    void (*text)(double x, double y, const char *utf8, double size, double rgba, void *user);
    void (*rect)(double x, double y, double w, double h, double rgba, void *user);
    void (*image)(double x, double y, double w, double h, const char *path, double rgba, void *user);
    /* Returns 1 on the frame the button is released over itself. */
    int  (*button)(double x, double y, double w, double h, const char *label, void *user);
    /* The view's size in pixels, so a script can lay out against the middle
     * or the right edge without being told how big the window is. */
    void (*size)(double *w, double *h, void *user);
    void *user;
} dai_script_gui_host;
DAI_API void dai_script_bind_gui(dai_script *s, const dai_script_gui_host *host);

/* Any host value scripts can read through `state.<name>`. */"""
assert s.count(old) == 1, 'state anchor not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)

p = 'src/dai_script.cpp'
s = io.open(p, encoding='utf-8').read()
old = """void dai_script_bind_play(dai_script *s, const dai_script_play_host *host) {"""
new = """// ---------------------------------------------------------------- gui
namespace {

JSValue js_gui_text(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (s->has_gui && s->gui.text && argc >= 3)
        s->gui.text(arg_num(ctx, argv[0]), arg_num(ctx, argv[1]),
                    str(ctx, argv[2]).c_str(),
                    argc >= 4 ? arg_num(ctx, argv[3]) : 24.0,
                    argc >= 5 ? arg_num(ctx, argv[4]) : (double)0xFFFFFFFFu,
                    s->gui.user);
    return JS_UNDEFINED;
}
JSValue js_gui_rect(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (s->has_gui && s->gui.rect && argc >= 4)
        s->gui.rect(arg_num(ctx, argv[0]), arg_num(ctx, argv[1]),
                    arg_num(ctx, argv[2]), arg_num(ctx, argv[3]),
                    argc >= 5 ? arg_num(ctx, argv[4]) : (double)0x80000000u,
                    s->gui.user);
    return JS_UNDEFINED;
}
JSValue js_gui_image(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (s->has_gui && s->gui.image && argc >= 5)
        s->gui.image(arg_num(ctx, argv[0]), arg_num(ctx, argv[1]),
                     arg_num(ctx, argv[2]), arg_num(ctx, argv[3]),
                     str(ctx, argv[4]).c_str(),
                     argc >= 6 ? arg_num(ctx, argv[5]) : (double)0xFFFFFFFFu,
                     s->gui.user);
    return JS_UNDEFINED;
}
JSValue js_gui_button(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (!s->has_gui || !s->gui.button || argc < 5) return JS_FALSE;
    return s->gui.button(arg_num(ctx, argv[0]), arg_num(ctx, argv[1]),
                         arg_num(ctx, argv[2]), arg_num(ctx, argv[3]),
                         str(ctx, argv[4]).c_str(), s->gui.user) ? JS_TRUE : JS_FALSE;
}
JSValue js_gui_size(JSContext *ctx, JSValueConst, int, JSValueConst *) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    double w = 0, h = 0;
    if (s->has_gui && s->gui.size) s->gui.size(&w, &h, s->gui.user);
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, w));
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, h));
    return arr;
}

} // namespace

void dai_script_bind_gui(dai_script *s, const dai_script_gui_host *host) {
    if (!s || !host) return;
    s->gui = *host;
    s->has_gui = 1;
    JSValue global = JS_GetGlobalObject(s->ctx);
    JSValue gui = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, gui, "text", JS_NewCFunction(s->ctx, js_gui_text, "text", 5));
    JS_SetPropertyStr(s->ctx, gui, "rect", JS_NewCFunction(s->ctx, js_gui_rect, "rect", 5));
    JS_SetPropertyStr(s->ctx, gui, "image", JS_NewCFunction(s->ctx, js_gui_image, "image", 6));
    JS_SetPropertyStr(s->ctx, gui, "button", JS_NewCFunction(s->ctx, js_gui_button, "button", 5));
    JS_SetPropertyStr(s->ctx, gui, "size", JS_NewCFunction(s->ctx, js_gui_size, "size", 0));
    JS_SetPropertyStr(s->ctx, global, "gui", gui);
    JS_FreeValue(s->ctx, global);
}

void dai_script_bind_play(dai_script *s, const dai_script_play_host *host) {"""
assert s.count(old) == 1, 'bind_play not found'
s = s.replace(old, new)

old = """    dai_script_play_host play{};
    int has_play = 0;
    int has_nodes = 0;"""
if s.count(old) != 1:
    import re
    m = re.search(r'\n(\s*)dai_script_play_host\s+play[^\n]*\n(\s*)int\s+has_play[^\n]*\n', s)
    assert m, 'play members not found'
    old = m.group(0)
    new = old.rstrip('\n') + '\n' + m.group(1) + 'dai_script_gui_host gui{};\n' + m.group(1) + 'int has_gui = 0;\n'
else:
    new = """    dai_script_play_host play{};
    int has_play = 0;
    int has_nodes = 0;
    dai_script_gui_host gui{};
    int has_gui = 0;"""
s = s.replace(old, new, 1)
io.open(p, 'w', encoding='utf-8').write(s)
print('dai_script: the gui binding')
