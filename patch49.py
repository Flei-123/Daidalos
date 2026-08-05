#!/usr/bin/env python3
# patch49 - a JS script can finally BE a player controller: it knows which node
# it is on, what the keyboard is doing, how long the frame was, and it can push
# a rigid body around.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p49'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# ==================================================================== header
s = rd('include/dai_script.h')
s = sub1(s,
"""/* Any host value scripts can read through `state.<name>`. */""",
"""/* What a behaviour needs that a UI script does not: the keyboard, and a body
 * to push. Bound separately from the node host so a game that runs scripts
 * for its menus never has to answer these.
 *
 *   input.key("w") / ("space") / ("left") / ("shift")   -> true while held
 *   body.getVel(id)              -> [x, y, z]
 *   body.setVel(id, x, y, z)
 *   body.impulse(id, x, y, z)
 *   body.grounded(id)            -> true when something is under it
 *
 * Key names are lower case, one per physical key: the letters and digits by
 * their character, and "space", "shift", "ctrl", "alt", "enter", "escape",
 * "tab", "left", "right", "up", "down". A name the host does not know is
 * false, never an error - a typo in a script must not stop the game.
 *
 * The frame length arrives as `state.dt`, which the host sets each frame with
 * dai_script_set_number; there is nothing to bind for that. */
typedef struct dai_script_play_host {
    int    (*key)(const char *name, void *user);
    int    (*get_vel)(double id, double *xyz, void *user);
    void   (*set_vel)(double id, const double *xyz, void *user);
    void   (*impulse)(double id, const double *xyz, void *user);
    int    (*grounded)(double id, void *user);
    void  *user;
} dai_script_play_host;
DAI_API void dai_script_bind_play(dai_script *s, const dai_script_play_host *host);

/* Any host value scripts can read through `state.<name>`. */""",
    'play host decl')
wr('include/dai_script.h', s)

# ============================================================= the bindings
s = rd('src/dai_script.cpp')
s = sub1(s,
"""    dai_script_node_host nodes{};""",
"""    dai_script_node_host nodes{};
    dai_script_play_host play{};
    int has_play = 0;""",
    'play host field')

s = sub1(s,
"""} // namespace

void dai_script_bind_nodes(dai_script *s, const dai_script_node_host *host) {""",
"""JSValue js_input_key(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (!s->has_play || !s->play.key || argc < 1) return JS_FALSE;
    return s->play.key(str(ctx, argv[0]).c_str(), s->play.user) ? JS_TRUE : JS_FALSE;
}

JSValue js_body_get_vel(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    double v[3] = { 0, 0, 0 };
    if (s->has_play && s->play.get_vel && argc >= 1)
        s->play.get_vel(arg_num(ctx, argv[0]), v, s->play.user);
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < 3; ++i) JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewFloat64(ctx, v[i]));
    return arr;
}

JSValue js_body_set_vel(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (s->has_play && s->play.set_vel && argc >= 4) {
        double v[3] = { arg_num(ctx, argv[1]), arg_num(ctx, argv[2]), arg_num(ctx, argv[3]) };
        s->play.set_vel(arg_num(ctx, argv[0]), v, s->play.user);
    }
    return JS_UNDEFINED;
}

JSValue js_body_impulse(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (s->has_play && s->play.impulse && argc >= 4) {
        double v[3] = { arg_num(ctx, argv[1]), arg_num(ctx, argv[2]), arg_num(ctx, argv[3]) };
        s->play.impulse(arg_num(ctx, argv[0]), v, s->play.user);
    }
    return JS_UNDEFINED;
}

JSValue js_body_grounded(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (!s->has_play || !s->play.grounded || argc < 1) return JS_FALSE;
    return s->play.grounded(arg_num(ctx, argv[0]), s->play.user) ? JS_TRUE : JS_FALSE;
}

} // namespace

void dai_script_bind_play(dai_script *s, const dai_script_play_host *host) {
    if (!s || !host) return;
    s->play = *host;
    s->has_play = 1;
    JSValue global = JS_GetGlobalObject(s->ctx);
    JSValue input = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, input, "key", JS_NewCFunction(s->ctx, js_input_key, "key", 1));
    JS_SetPropertyStr(s->ctx, global, "input", input);
    JSValue body = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, body, "getVel", JS_NewCFunction(s->ctx, js_body_get_vel, "getVel", 1));
    JS_SetPropertyStr(s->ctx, body, "setVel", JS_NewCFunction(s->ctx, js_body_set_vel, "setVel", 4));
    JS_SetPropertyStr(s->ctx, body, "impulse", JS_NewCFunction(s->ctx, js_body_impulse, "impulse", 4));
    JS_SetPropertyStr(s->ctx, body, "grounded", JS_NewCFunction(s->ctx, js_body_grounded, "grounded", 1));
    JS_SetPropertyStr(s->ctx, global, "body", body);
    JS_FreeValue(s->ctx, global);
}

void dai_script_bind_nodes(dai_script *s, const dai_script_node_host *host) {""",
    'play bindings')
wr('src/dai_script.cpp', s)

# ================================================================= the host
s = rd('examples/editor_demo.cpp')
s = sub1(s,
"""struct RunningScript { dai_script *s = nullptr; std::string path; };""",
"""// The JS side of what the C++ behaviours already had: a keyboard, a body, and
// the node the script is attached to. Without these a .js could read the scene
// and move a transform by hand, which is not a player controller - it is a
// cutscene.
static int sp_key(const char *name, void *) {
    if (!g_win_for_scripts || !name || !*name) return 0;
    std::string n;
    for (const char *c = name; *c; ++c) n += (char)std::tolower((unsigned char)*c);
    uint32_t code = 0;
    if (n.size() == 1) {
        char c = n[0];
        if (c >= 'a' && c <= 'z') code = (uint32_t)c;
        else if (c >= '0' && c <= '9') code = (uint32_t)c;
    }
    if (!code) {
        if (n == "space")  code = DAI_KEY_SPACE;
        else if (n == "enter" || n == "return") code = DAI_KEY_RETURN;
        else if (n == "escape") code = DAI_KEY_ESCAPE;
        else if (n == "tab")   code = DAI_KEY_TAB;
        else if (n == "left")  code = DAI_KEY_LEFT;
        else if (n == "right") code = DAI_KEY_RIGHT;
        else if (n == "up")    code = DAI_KEY_UP;
        else if (n == "down")  code = DAI_KEY_DOWN;
        else if (n == "shift")
            return dai_window_key_down(g_win_for_scripts, DAI_KEY_SHIFT_L) ||
                   dai_window_key_down(g_win_for_scripts, DAI_KEY_SHIFT_R);
        else if (n == "ctrl" || n == "control")
            return dai_window_key_down(g_win_for_scripts, DAI_KEY_CTRL_L) ||
                   dai_window_key_down(g_win_for_scripts, DAI_KEY_CTRL_R);
        else if (n == "alt")
            return dai_window_key_down(g_win_for_scripts, DAI_KEY_ALT_L) ||
                   dai_window_key_down(g_win_for_scripts, DAI_KEY_ALT_R);
    }
    if (!code) return 0;                    // an unknown name is false, not an error
    return dai_window_key_down(g_win_for_scripts, code) ? 1 : 0;
}

static int sp_get_vel(double id, double *xyz, void *) {
    dai_vec3 l{}, a{};
    if (!g_script_ed || !xyz) return 0;
    dai_editor_live_velocity(g_script_ed, (dai_node)(uint32_t)id, &l, &a);
    xyz[0] = l.x; xyz[1] = l.y; xyz[2] = l.z;
    return 1;
}

static void sp_set_vel(double id, const double *xyz, void *) {
    if (!g_script_ed || !xyz) return;
    dai_editor_live_set_velocity(g_script_ed, (dai_node)(uint32_t)id,
                                 dai_vec3{ (float)xyz[0], (float)xyz[1], (float)xyz[2] });
}

static void sp_impulse(double id, const double *xyz, void *) {
    if (!g_script_ed || !xyz) return;
    dai_editor_live_impulse(g_script_ed, (dai_node)(uint32_t)id,
                            dai_vec3{ (float)xyz[0], (float)xyz[1], (float)xyz[2] });
}

// "Is there floor under me." Not a raycast: the engine has no query API bound
// here, and a controller only needs to know whether it is falling. A body that
// is neither rising nor sinking measurably is standing on something - which is
// exactly the test a platformer wants, and it costs nothing.
static int sp_grounded(double id, void *) {
    dai_vec3 l{}, a{};
    if (!g_script_ed) return 0;
    dai_editor_live_velocity(g_script_ed, (dai_node)(uint32_t)id, &l, &a);
    return (l.y > -0.35f && l.y < 0.35f) ? 1 : 0;
}

static dai_script_play_host g_play_host = {
    sp_key, sp_get_vel, sp_set_vel, sp_impulse, sp_grounded, nullptr
};

struct RunningScript { dai_script *s = nullptr; std::string path; };""",
    'play host impl')

s = sub1(s,
"""            dai_script_bind_nodes(s, &g_node_host);""",
"""            dai_script_bind_nodes(s, &g_node_host);
            dai_script_bind_play(s, &g_play_host);""",
    'bind play')

# `self` is the node the script sits on. Every behaviour's first question.
s = sub1(s,
"""            if (!params_js.empty()) dai_script_eval(s, params_js.c_str(), "params", err, sizeof(err));
            dai_script_call(s, "init", err, sizeof(err));""",
"""            // Which object am I on. Unity calls it gameObject, Godot calls it
            // self; either way it is the first thing a behaviour needs and the
            // only one it cannot look up.
            {
                char selfjs[64];
                std::snprintf(selfjs, sizeof(selfjs), "var self = %u;", (unsigned)id);
                dai_script_eval(s, selfjs, "self", err, sizeof(err));
            }
            if (!params_js.empty()) dai_script_eval(s, params_js.c_str(), "params", err, sizeof(err));
            dai_script_call(s, "init", err, sizeof(err));""",
    'self global')

# The frame length, every frame, before frame() runs.
s = sub1(s,
"""            if (g_scripts_live)
                for (RunningScript &rs : g_running) {
                    char serr[192] = { 0 };
                    if (dai_script_call(rs.s, "frame", serr, sizeof(serr)) != DAI_OK && serr[0]) {""",
"""            if (g_scripts_live)
                for (RunningScript &rs : g_running) {
                    char serr[192] = { 0 };
                    // state.dt, so a script can be frame rate independent
                    // without asking the host for a clock it does not have.
                    dai_script_set_number(rs.s, "dt", 1.0 / 60.0);
                    if (dai_script_call(rs.s, "frame", serr, sizeof(serr)) != DAI_OK && serr[0]) {""",
    'state.dt per frame')
wr('examples/editor_demo.cpp', s)
print('patch49 ok')
