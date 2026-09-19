// QuickJS embedding. See include/dai_script.h.
//
// The binding surface is small on purpose: the UI, a few host values, and
// nothing that can touch the simulation. A script that throws must never take
// the frame down, so every entry point catches, counts and carries on.

#include "dai_script.h"

extern "C" {
#include "quickjs.h"
}

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct dai_script {
    JSRuntime *rt = nullptr;
    JSContext *ctx = nullptr;
    dai_ui *ui = nullptr;
    dai_script_node_host nodes{};
    dai_script_play_host play{};
    int has_play = 0;
    int has_nodes = 0;
    dai_script_gui_host gui{};
    int has_gui = 0;
    dai_script_anim_host anim{};
    int has_anim = 0;
    dai_script_editor_host editor{};
    int has_editor = 0;
    dai_script_audio_host audio{};
    int has_audio = 0;
    std::string last_path;
    uint32_t errors = 0;
};

namespace {

dai_script *self_of(JSContext *ctx) { return (dai_script *)JS_GetContextOpaque(ctx); }

double num(JSContext *ctx, JSValueConst v, double def = 0.0) {
    double d = def;
    if (JS_ToFloat64(ctx, &d, v) < 0) return def;
    return d;
}

std::string str(JSContext *ctx, JSValueConst v) {
    const char *c = JS_ToCString(ctx, v);
    std::string out = c ? c : "";
    if (c) JS_FreeCString(ctx, c);
    return out;
}

uint32_t colour(JSContext *ctx, JSValueConst v, uint32_t def) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) return def;
    uint32_t c = (uint32_t)num(ctx, v, def);
    return c;
}

// ---- ui bindings

JSValue ui_panel(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = self_of(ctx);
    if (!s->ui || argc < 4) return JS_UNDEFINED;
    std::string title = argc > 4 ? str(ctx, argv[4]) : "";
    dai_ui_panel_begin(s->ui, (float)num(ctx, argv[0]), (float)num(ctx, argv[1]),
                       (float)num(ctx, argv[2]), (float)num(ctx, argv[3]),
                       title.empty() ? nullptr : title.c_str());
    return JS_UNDEFINED;
}
JSValue ui_panel_end(JSContext *ctx, JSValueConst, int, JSValueConst *) {
    dai_script *s = self_of(ctx);
    if (s->ui) dai_ui_panel_end(s->ui);
    return JS_UNDEFINED;
}
JSValue ui_label(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = self_of(ctx);
    if (!s->ui || argc < 1) return JS_UNDEFINED;
    dai_ui_label(s->ui, str(ctx, argv[0]).c_str());
    return JS_UNDEFINED;
}
JSValue ui_button(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = self_of(ctx);
    if (!s->ui || argc < 1) return JS_FALSE;
    return dai_ui_button(s->ui, str(ctx, argv[0]).c_str()) ? JS_TRUE : JS_FALSE;
}
JSValue ui_slider(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = self_of(ctx);
    if (!s->ui || argc < 4) return JS_UNDEFINED;
    float v = (float)num(ctx, argv[1]);
    dai_ui_slider(s->ui, str(ctx, argv[0]).c_str(), &v,
                  (float)num(ctx, argv[2]), (float)num(ctx, argv[3]));
    return JS_NewFloat64(ctx, v);      // scripts have no pointers: return the value
}
JSValue ui_progress(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = self_of(ctx);
    if (!s->ui || argc < 1) return JS_UNDEFINED;
    std::string label = argc > 1 ? str(ctx, argv[1]) : "";
    dai_ui_progress(s->ui, (float)num(ctx, argv[0]), label.empty() ? nullptr : label.c_str());
    return JS_UNDEFINED;
}
JSValue ui_separator(JSContext *ctx, JSValueConst, int, JSValueConst *) {
    dai_script *s = self_of(ctx);
    if (s->ui) dai_ui_separator(s->ui);
    return JS_UNDEFINED;
}
JSValue ui_text(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = self_of(ctx);
    if (!s->ui || argc < 3) return JS_UNDEFINED;
    uint32_t col = argc > 3 ? colour(ctx, argv[3], 0xFFFFFFFFu) : 0xFFFFFFFFu;
    dai_ui_text(s->ui, (float)num(ctx, argv[0]), (float)num(ctx, argv[1]),
                str(ctx, argv[2]).c_str(), col);
    return JS_UNDEFINED;
}
JSValue ui_rect(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = self_of(ctx);
    if (!s->ui || argc < 5) return JS_UNDEFINED;
    dai_ui_rect(s->ui, (float)num(ctx, argv[0]), (float)num(ctx, argv[1]),
                (float)num(ctx, argv[2]), (float)num(ctx, argv[3]),
                colour(ctx, argv[4], 0xFFFFFFFFu));
    return JS_UNDEFINED;
}
JSValue ui_image(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = self_of(ctx);
    if (!s->ui || argc < 3) return JS_UNDEFINED;
    float u0 = argc > 3 ? (float)num(ctx, argv[3]) : 0.0f;
    float v0 = argc > 4 ? (float)num(ctx, argv[4]) : 0.0f;
    float u1 = argc > 5 ? (float)num(ctx, argv[5]) : 1.0f;
    float v1 = argc > 6 ? (float)num(ctx, argv[6]) : 1.0f;
    dai_ui_image(s->ui, (uint32_t)num(ctx, argv[0]), (float)num(ctx, argv[1]),
                 (float)num(ctx, argv[2]), u0, v0, u1, v1, 0xFFFFFFFFu);
    return JS_UNDEFINED;
}
JSValue js_print(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    for (int i = 0; i < argc; ++i) std::printf("%s%s", i ? " " : "", str(ctx, argv[i]).c_str());
    std::printf("\n");
    return JS_UNDEFINED;
}

const JSCFunctionListEntry kUiFuncs[] = {
    JS_CFUNC_DEF("panel", 5, ui_panel),
    JS_CFUNC_DEF("panelEnd", 0, ui_panel_end),
    JS_CFUNC_DEF("label", 1, ui_label),
    JS_CFUNC_DEF("button", 1, ui_button),
    JS_CFUNC_DEF("slider", 4, ui_slider),
    JS_CFUNC_DEF("progress", 2, ui_progress),
    JS_CFUNC_DEF("separator", 0, ui_separator),
    JS_CFUNC_DEF("text", 4, ui_text),
    JS_CFUNC_DEF("rect", 5, ui_rect),
    JS_CFUNC_DEF("image", 7, ui_image),
};

void record_error(dai_script *s, char *err, size_t err_len) {
    s->errors++;
    JSValue e = JS_GetException(s->ctx);
    std::string msg = str(s->ctx, e);
    JSValue stack = JS_GetPropertyStr(s->ctx, e, "stack");
    if (!JS_IsUndefined(stack)) msg += "\n" + str(s->ctx, stack);
    JS_FreeValue(s->ctx, stack);
    JS_FreeValue(s->ctx, e);
    if (err && err_len) std::snprintf(err, err_len, "%s", msg.c_str());
}

} // namespace

extern "C" {

dai_script *dai_script_create(char *err, size_t err_len) {
    dai_script *s = new dai_script();
    s->rt = JS_NewRuntime();
    if (!s->rt) { delete s; if (err && err_len) std::snprintf(err, err_len, "JS_NewRuntime failed"); return nullptr; }
    // a runaway script must not wedge the frame: cap the stack, not the time
    JS_SetMaxStackSize(s->rt, 512 * 1024);
    s->ctx = JS_NewContext(s->rt);
    if (!s->ctx) { JS_FreeRuntime(s->rt); delete s; if (err && err_len) std::snprintf(err, err_len, "JS_NewContext failed"); return nullptr; }
    JS_SetContextOpaque(s->ctx, s);

    JSValue global = JS_GetGlobalObject(s->ctx);
    JSValue ui = JS_NewObject(s->ctx);
    JS_SetPropertyFunctionList(s->ctx, ui, kUiFuncs, (int)(sizeof(kUiFuncs) / sizeof(kUiFuncs[0])));
    JS_SetPropertyStr(s->ctx, global, "ui", ui);
    JS_SetPropertyStr(s->ctx, global, "state", JS_NewObject(s->ctx));
    JS_SetPropertyStr(s->ctx, global, "print", JS_NewCFunction(s->ctx, js_print, "print", 1));
    JS_FreeValue(s->ctx, global);
    return s;
}

void dai_script_destroy(dai_script *s) {
    if (!s) return;
    if (s->ctx) JS_FreeContext(s->ctx);
    if (s->rt) JS_FreeRuntime(s->rt);
    delete s;
}

void dai_script_bind_ui(dai_script *s, dai_ui *ui) { if (s) s->ui = ui; }

void dai_script_set_number(dai_script *s, const char *name, double v) {
    if (!s || !name) return;
    JSValue g = JS_GetGlobalObject(s->ctx);
    JSValue st = JS_GetPropertyStr(s->ctx, g, "state");
    JS_SetPropertyStr(s->ctx, st, name, JS_NewFloat64(s->ctx, v));
    JS_FreeValue(s->ctx, st);
    JS_FreeValue(s->ctx, g);
}

void dai_script_set_string(dai_script *s, const char *name, const char *v) {
    if (!s || !name) return;
    JSValue g = JS_GetGlobalObject(s->ctx);
    JSValue st = JS_GetPropertyStr(s->ctx, g, "state");
    JS_SetPropertyStr(s->ctx, st, name, JS_NewString(s->ctx, v ? v : ""));
    JS_FreeValue(s->ctx, st);
    JS_FreeValue(s->ctx, g);
}

/* The string twin of dai_script_get_number. A HUD label wants "3 lives" as
 * readily as it wants 3, and formatting a number the script already turned
 * into words is a round trip that loses. */
int dai_script_get_string(dai_script *s, const char *name, char *out, size_t out_size) {
    if (!s || !name || !out || out_size < 2) return 0;
    JSValue g = JS_GetGlobalObject(s->ctx);
    JSValue st = JS_GetPropertyStr(s->ctx, g, "state");
    JSValue v = JS_GetPropertyStr(s->ctx, st, name);
    int ok = 0;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        std::string sv = str(s->ctx, v);
        std::snprintf(out, out_size, "%s", sv.c_str());
        ok = 1;
    }
    JS_FreeValue(s->ctx, v);
    JS_FreeValue(s->ctx, st);
    JS_FreeValue(s->ctx, g);
    return ok;
}

size_t dai_script_get_string_size(dai_script *s, const char *name) {
    if (!s || !name) return 0;
    JSValue g = JS_GetGlobalObject(s->ctx);
    JSValue st = JS_GetPropertyStr(s->ctx, g, "state");
    JSValue v = JS_GetPropertyStr(s->ctx, st, name);
    size_t n = 0;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) n = str(s->ctx, v).size() + 1;
    JS_FreeValue(s->ctx, v);
    JS_FreeValue(s->ctx, st);
    JS_FreeValue(s->ctx, g);
    return n;
}

double dai_script_get_number(dai_script *s, const char *name, double fallback) {
    if (!s || !name) return fallback;
    JSValue g = JS_GetGlobalObject(s->ctx);
    JSValue st = JS_GetPropertyStr(s->ctx, g, "state");
    JSValue v = JS_GetPropertyStr(s->ctx, st, name);
    double out = JS_IsUndefined(v) ? fallback : num(s->ctx, v, fallback);
    JS_FreeValue(s->ctx, v);
    JS_FreeValue(s->ctx, st);
    JS_FreeValue(s->ctx, g);
    return out;
}

dai_result dai_script_eval(dai_script *s, const char *code, const char *name,
                           char *err, size_t err_len) {
    if (!s || !code) return DAI_ERR_INVALID_ARG;
    JSValue v = JS_Eval(s->ctx, code, std::strlen(code), name ? name : "<eval>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) { record_error(s, err, err_len); JS_FreeValue(s->ctx, v); return DAI_ERR_STATE; }
    JS_FreeValue(s->ctx, v);
    return DAI_OK;
}

dai_result dai_script_load(dai_script *s, const char *path, char *err, size_t err_len) {
    if (!s || !path) return DAI_ERR_INVALID_ARG;
    FILE *f = std::fopen(path, "rb");
    if (!f) { if (err && err_len) std::snprintf(err, err_len, "cannot open %s", path); return DAI_ERR_FILE; }
    std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    std::string code((size_t)n, '\0');
    bool ok = n == 0 || std::fread(&code[0], 1, (size_t)n, f) == (size_t)n;
    std::fclose(f);
    if (!ok) { if (err && err_len) std::snprintf(err, err_len, "short read on %s", path); return DAI_ERR_FILE; }
    s->last_path = path;
    return dai_script_eval(s, code.c_str(), path, err, err_len);
}

dai_result dai_script_reload(dai_script *s, char *err, size_t err_len) {
    if (!s || s->last_path.empty()) return DAI_ERR_STATE;
    return dai_script_load(s, s->last_path.c_str(), err, err_len);
}

dai_result dai_script_call(dai_script *s, const char *fn, char *err, size_t err_len) {
    if (!s || !fn) return DAI_ERR_INVALID_ARG;
    JSValue g = JS_GetGlobalObject(s->ctx);
    JSValue f = JS_GetPropertyStr(s->ctx, g, fn);
    if (!JS_IsFunction(s->ctx, f)) {
        JS_FreeValue(s->ctx, f); JS_FreeValue(s->ctx, g);
        if (err && err_len) std::snprintf(err, err_len, "%s is not a function", fn);
        return DAI_ERR_NOT_FOUND;
    }
    JSValue r = JS_Call(s->ctx, f, g, 0, nullptr);
    dai_result res = DAI_OK;
    if (JS_IsException(r)) { record_error(s, err, err_len); res = DAI_ERR_STATE; }
    JS_FreeValue(s->ctx, r);
    JS_FreeValue(s->ctx, f);
    JS_FreeValue(s->ctx, g);
    return res;
}

uint32_t dai_script_error_count(const dai_script *s) { return s ? s->errors : 0; }

} // extern "C"


// ---------------------------------------------------------------- node access
// The `scene`/`node` globals, bound by the host through dai_script_bind_nodes.
// Self-contained on purpose: this block only talks to the host callbacks, so
// it sits at the end of the file and touches nothing else.
namespace {

double arg_num(JSContext *ctx, JSValueConst v) {
    double d = 0.0;
    JS_ToFloat64(ctx, &d, v);
    return d;
}

JSValue js_scene_find(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (!s->has_nodes || argc < 1) return JS_NewFloat64(ctx, -1.0);
    const char *c = JS_ToCString(ctx, argv[0]);
    double r = c ? s->nodes.find(c, s->nodes.user) : -1.0;
    if (c) JS_FreeCString(ctx, c);
    return JS_NewFloat64(ctx, r);
}

// ---- spawn: the only way a behaviour may bring something into the world ---
// A copy of something that already exists, never a node out of nothing. See
// the comment on dai_script_node_host::spawn in include/dai_script.h.
JSValue js_scene_spawn(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (!s->has_nodes || !s->nodes.spawn || argc < 1) return JS_NewFloat64(ctx, -1.0);
    double src = arg_num(ctx, argv[0]);
    double parent = (argc >= 2) ? arg_num(ctx, argv[1]) : 0.0;
    const char *nm = (argc >= 3 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2]))
                     ? JS_ToCString(ctx, argv[2]) : nullptr;
    double r = s->nodes.spawn(src, parent, nm, s->nodes.user);
    if (nm) JS_FreeCString(ctx, nm);
    return JS_NewFloat64(ctx, r);
}

JSValue js_scene_destroy(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (!s->has_nodes || !s->nodes.destroy || argc < 1) return JS_FALSE;
    return s->nodes.destroy(arg_num(ctx, argv[0]), s->nodes.user) ? JS_TRUE : JS_FALSE;
}

JSValue js_scene_child_count(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (!s->has_nodes || !s->nodes.child_count || argc < 1) return JS_NewFloat64(ctx, 0.0);
    return JS_NewFloat64(ctx, s->nodes.child_count(arg_num(ctx, argv[0]), s->nodes.user));
}

JSValue js_scene_child_at(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (!s->has_nodes || !s->nodes.child_at || argc < 2) return JS_NewFloat64(ctx, -1.0);
    return JS_NewFloat64(ctx, s->nodes.child_at(arg_num(ctx, argv[0]),
                                                arg_num(ctx, argv[1]), s->nodes.user));
}

JSValue js_scene_parent(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (!s->has_nodes || !s->nodes.parent_of || argc < 1) return JS_NewFloat64(ctx, -1.0);
    return JS_NewFloat64(ctx, s->nodes.parent_of(arg_num(ctx, argv[0]), s->nodes.user));
}

JSValue js_node_get_pos(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    double xyz[3] = { 0, 0, 0 };
    if (s->has_nodes && argc >= 1) s->nodes.get_pos(arg_num(ctx, argv[0]), xyz, s->nodes.user);
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < 3; ++i) JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewFloat64(ctx, xyz[i]));
    return arr;
}

JSValue js_node_set_pos(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (s->has_nodes && argc >= 4) {
        double xyz[3] = { arg_num(ctx, argv[1]), arg_num(ctx, argv[2]), arg_num(ctx, argv[3]) };
        s->nodes.set_pos(arg_num(ctx, argv[0]), xyz, s->nodes.user);
    }
    return JS_UNDEFINED;
}

JSValue js_node_get_rot(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    double q[4] = { 0, 0, 0, 1 };
    if (s->has_nodes && argc >= 1) s->nodes.get_rot(arg_num(ctx, argv[0]), q, s->nodes.user);
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < 4; ++i) JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewFloat64(ctx, q[i]));
    return arr;
}

JSValue js_node_set_rot(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (s->has_nodes && argc >= 5) {
        double q[4] = { arg_num(ctx, argv[1]), arg_num(ctx, argv[2]),
                        arg_num(ctx, argv[3]), arg_num(ctx, argv[4]) };
        s->nodes.set_rot(arg_num(ctx, argv[0]), q, s->nodes.user);
    }
    return JS_UNDEFINED;
}

JSValue js_node_set_text(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (s->has_nodes && s->nodes.set_text && argc >= 2)
        s->nodes.set_text(arg_num(ctx, argv[0]), str(ctx, argv[1]).c_str(), s->nodes.user);
    return JS_UNDEFINED;
}

// ---- the component bridge -----------------------------------------------
// node.getNum(id, "light.intensity"), node.setVec(id, "light.color", r, g, b),
// node.getStr(id, "text.value"). One pair of functions for every component
// property there is or ever will be - see dai_script.h for why by name.
//
// A host that did not install them is not an error either: the prelude asks
// whether they exist before it uses them, and these answer harmlessly if it
// somehow does not.
JSValue js_node_get_num(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (!s->has_nodes || !s->nodes.get_num || argc < 2) return JS_NewFloat64(ctx, 0);
    return JS_NewFloat64(ctx, s->nodes.get_num(arg_num(ctx, argv[0]),
                                               str(ctx, argv[1]).c_str(), s->nodes.user));
}

JSValue js_node_set_num(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (s->has_nodes && s->nodes.set_num && argc >= 3)
        s->nodes.set_num(arg_num(ctx, argv[0]), str(ctx, argv[1]).c_str(),
                         arg_num(ctx, argv[2]), s->nodes.user);
    return JS_UNDEFINED;
}

JSValue js_node_get_vec(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    double xyz[3] = { 0, 0, 0 };
    if (s->has_nodes && s->nodes.get_vec && argc >= 2)
        s->nodes.get_vec(arg_num(ctx, argv[0]), str(ctx, argv[1]).c_str(), xyz, s->nodes.user);
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < 3; ++i) JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewFloat64(ctx, xyz[i]));
    return arr;
}

JSValue js_node_set_vec(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (s->has_nodes && s->nodes.set_vec && argc >= 5) {
        double xyz[3] = { arg_num(ctx, argv[2]), arg_num(ctx, argv[3]), arg_num(ctx, argv[4]) };
        s->nodes.set_vec(arg_num(ctx, argv[0]), str(ctx, argv[1]).c_str(), xyz, s->nodes.user);
    }
    return JS_UNDEFINED;
}

JSValue js_node_get_str(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    const char *r = nullptr;
    if (s->has_nodes && s->nodes.get_str && argc >= 2)
        r = s->nodes.get_str(arg_num(ctx, argv[0]), str(ctx, argv[1]).c_str(), s->nodes.user);
    return JS_NewString(ctx, r ? r : "");
}

JSValue js_node_set_str(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (s->has_nodes && s->nodes.set_str && argc >= 3)
        s->nodes.set_str(arg_num(ctx, argv[0]), str(ctx, argv[1]).c_str(),
                         str(ctx, argv[2]).c_str(), s->nodes.user);
    return JS_UNDEFINED;
}

JSValue js_input_key(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (!s->has_play || !s->play.key || argc < 1) return JS_FALSE;
    return s->play.key(str(ctx, argv[0]).c_str(), s->play.user) ? JS_TRUE : JS_FALSE;
}

// input.mouseDX() / input.mouseDY() / input.mouseButton(n)
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
    // The mask comes straight from dai_window_mouse, and that one follows X11's
    // button NUMBERS: bit 1 is left, bit 2 middle, bit 3 right (Win32 sets the
    // same bits on purpose). This used to test bit 0 for left - a bit no
    // backend ever sets - so input.mouseButton(0) was false for ever and no
    // game could shoot, throw or click anything.
    int bit = which == 1 ? (1 << 3) : (which == 2 ? (1 << 2) : (1 << 1));
    return (b & bit) ? JS_TRUE : JS_FALSE;
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

// ---------------------------------------------------------------- gui
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

/* ------------------------------------------------------------------ audio
 * audio.play(name [, volume, pitch]) and audio.play3d(name, x, y, z [, ...]).
 * A name the bank does not know is not an error here: the host returns 0 and
 * the game carries on without that sound. */
JSValue js_audio_play_common(JSContext *ctx, int argc, JSValueConst *argv, int spatial) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (!s->has_audio || !s->audio.play || argc < 1) return JS_NewFloat64(ctx, 0);
    std::string name = str(ctx, argv[0]);
    double pos[3] = { 0, 0, 0 };
    int extra = 1;
    if (spatial) {
        for (int i = 0; i < 3; ++i) pos[i] = argc > i + 1 ? num(ctx, argv[i + 1]) : 0.0;
        extra = 4;
    }
    double vol = argc > extra ? num(ctx, argv[extra], 1.0) : 1.0;
    double pitch = argc > extra + 1 ? num(ctx, argv[extra + 1], 1.0) : 1.0;
    double h = s->audio.play(name.c_str(), spatial ? pos : nullptr, vol, pitch, s->audio.user);
    return JS_NewFloat64(ctx, h);
}
JSValue js_audio_play(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    return js_audio_play_common(ctx, argc, argv, 0);
}
JSValue js_audio_play3d(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    return js_audio_play_common(ctx, argc, argv, 1);
}
JSValue js_audio_stop(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (s->has_audio && s->audio.stop && argc >= 1) s->audio.stop(num(ctx, argv[0]), s->audio.user);
    return JS_UNDEFINED;
}
JSValue js_audio_listener(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (!s->has_audio || !s->audio.listener) return JS_UNDEFINED;
    double pos[3] = { 0, 0, 0 }, fwd[3] = { 0, 0, -1 };
    for (int i = 0; i < 3 && i + 0 < argc; ++i) pos[i] = num(ctx, argv[i]);
    for (int i = 0; i < 3 && i + 3 < argc; ++i) fwd[i] = num(ctx, argv[i + 3]);
    s->audio.listener(pos, fwd, s->audio.user);
    return JS_UNDEFINED;
}

} // namespace

void dai_script_bind_audio(dai_script *s, const dai_script_audio_host *host) {
    if (!s || !host) return;
    s->audio = *host;
    s->has_audio = 1;
    JSValue global = JS_GetGlobalObject(s->ctx);
    JSValue audio = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, audio, "play", JS_NewCFunction(s->ctx, js_audio_play, "play", 3));
    JS_SetPropertyStr(s->ctx, audio, "play3d", JS_NewCFunction(s->ctx, js_audio_play3d, "play3d", 6));
    JS_SetPropertyStr(s->ctx, audio, "stop", JS_NewCFunction(s->ctx, js_audio_stop, "stop", 1));
    JS_SetPropertyStr(s->ctx, audio, "listener", JS_NewCFunction(s->ctx, js_audio_listener, "listener", 6));
    JS_SetPropertyStr(s->ctx, global, "audio", audio);
    JS_FreeValue(s->ctx, global);
}

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

void dai_script_bind_play(dai_script *s, const dai_script_play_host *host) {
    if (!s || !host) return;
    s->play = *host;
    s->has_play = 1;
    JSValue global = JS_GetGlobalObject(s->ctx);
    JSValue input = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, input, "key", JS_NewCFunction(s->ctx, js_input_key, "key", 1));
    JS_SetPropertyStr(s->ctx, input, "mouseDX", JS_NewCFunction(s->ctx, js_input_mouse_dx, "mouseDX", 0));
    JS_SetPropertyStr(s->ctx, input, "mouseDY", JS_NewCFunction(s->ctx, js_input_mouse_dy, "mouseDY", 0));
    JS_SetPropertyStr(s->ctx, input, "mouseButton", JS_NewCFunction(s->ctx, js_input_mouse_button, "mouseButton", 1));
    JS_SetPropertyStr(s->ctx, global, "input", input);
    JSValue body = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, body, "getVel", JS_NewCFunction(s->ctx, js_body_get_vel, "getVel", 1));
    JS_SetPropertyStr(s->ctx, body, "setVel", JS_NewCFunction(s->ctx, js_body_set_vel, "setVel", 4));
    JS_SetPropertyStr(s->ctx, body, "impulse", JS_NewCFunction(s->ctx, js_body_impulse, "impulse", 4));
    JS_SetPropertyStr(s->ctx, body, "grounded", JS_NewCFunction(s->ctx, js_body_grounded, "grounded", 1));
    JS_SetPropertyStr(s->ctx, global, "body", body);
    JS_FreeValue(s->ctx, global);
}

void dai_script_bind_nodes(dai_script *s, const dai_script_node_host *host) {
    if (!s || !host) return;
    s->nodes = *host;
    s->has_nodes = 1;
    JSValue global = JS_GetGlobalObject(s->ctx);
    JSValue scene = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, scene, "find", JS_NewCFunction(s->ctx, js_scene_find, "find", 1));
    JS_SetPropertyStr(s->ctx, scene, "spawn", JS_NewCFunction(s->ctx, js_scene_spawn, "spawn", 3));
    JS_SetPropertyStr(s->ctx, scene, "destroy", JS_NewCFunction(s->ctx, js_scene_destroy, "destroy", 1));
    JS_SetPropertyStr(s->ctx, scene, "childCount", JS_NewCFunction(s->ctx, js_scene_child_count, "childCount", 1));
    JS_SetPropertyStr(s->ctx, scene, "childAt", JS_NewCFunction(s->ctx, js_scene_child_at, "childAt", 2));
    JS_SetPropertyStr(s->ctx, scene, "parent", JS_NewCFunction(s->ctx, js_scene_parent, "parent", 1));
    JS_SetPropertyStr(s->ctx, global, "scene", scene);
    JSValue node = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, node, "setText", JS_NewCFunction(s->ctx, js_node_set_text, "setText", 2));
    JS_SetPropertyStr(s->ctx, node, "getPos", JS_NewCFunction(s->ctx, js_node_get_pos, "getPos", 1));
    JS_SetPropertyStr(s->ctx, node, "setPos", JS_NewCFunction(s->ctx, js_node_set_pos, "setPos", 4));
    JS_SetPropertyStr(s->ctx, node, "getRot", JS_NewCFunction(s->ctx, js_node_get_rot, "getRot", 1));
    JS_SetPropertyStr(s->ctx, node, "setRot", JS_NewCFunction(s->ctx, js_node_set_rot, "setRot", 5));
    JS_SetPropertyStr(s->ctx, node, "getNum", JS_NewCFunction(s->ctx, js_node_get_num, "getNum", 2));
    JS_SetPropertyStr(s->ctx, node, "setNum", JS_NewCFunction(s->ctx, js_node_set_num, "setNum", 3));
    JS_SetPropertyStr(s->ctx, node, "getVec", JS_NewCFunction(s->ctx, js_node_get_vec, "getVec", 2));
    JS_SetPropertyStr(s->ctx, node, "setVec", JS_NewCFunction(s->ctx, js_node_set_vec, "setVec", 5));
    JS_SetPropertyStr(s->ctx, node, "getStr", JS_NewCFunction(s->ctx, js_node_get_str, "getStr", 2));
    JS_SetPropertyStr(s->ctx, node, "setStr", JS_NewCFunction(s->ctx, js_node_set_str, "setStr", 3));
    JS_SetPropertyStr(s->ctx, global, "node", node);
    JS_FreeValue(s->ctx, global);
}


// ------------------------------------------------------------------- editor
// The `editor` global, bound by the host through dai_script_bind_editor. Same
// shape as the node block above - nothing here reaches past the host's own
// callbacks - and bound separately for a reason that is not tidiness: a
// behaviour that could delete nodes and overwrite the scene file is a save
// game corrupter waiting for a typo. A tool may; a game may not.
namespace {

JSValue js_editor_add(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (!s->has_editor || !s->editor.add || argc < 1) return JS_NewFloat64(ctx, -1.0);
    double parent = argc > 1 ? arg_num(ctx, argv[1]) : 0.0;
    return JS_NewFloat64(ctx, s->editor.add(str(ctx, argv[0]).c_str(), parent, s->editor.user));
}

JSValue js_editor_remove(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (!s->has_editor || !s->editor.remove || argc < 1) return JS_FALSE;
    return s->editor.remove(arg_num(ctx, argv[0]), s->editor.user) ? JS_TRUE : JS_FALSE;
}

JSValue js_editor_begin(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (s->has_editor && s->editor.begin)
        s->editor.begin(argc >= 1 ? str(ctx, argv[0]).c_str() : "script", s->editor.user);
    return JS_UNDEFINED;
}

JSValue js_editor_commit(JSContext *ctx, JSValueConst, int, JSValueConst *) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (s->has_editor && s->editor.commit) s->editor.commit(s->editor.user);
    return JS_UNDEFINED;
}

JSValue js_editor_undo(JSContext *ctx, JSValueConst, int, JSValueConst *) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (!s->has_editor || !s->editor.undo) return JS_FALSE;
    return s->editor.undo(s->editor.user) ? JS_TRUE : JS_FALSE;
}

JSValue js_editor_redo(JSContext *ctx, JSValueConst, int, JSValueConst *) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (!s->has_editor || !s->editor.redo) return JS_FALSE;
    return s->editor.redo(s->editor.user) ? JS_TRUE : JS_FALSE;
}

JSValue js_editor_count(JSContext *ctx, JSValueConst, int, JSValueConst *) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (!s->has_editor || !s->editor.count) return JS_NewFloat64(ctx, 0.0);
    return JS_NewFloat64(ctx, s->editor.count(s->editor.user));
}

JSValue js_editor_at(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (!s->has_editor || !s->editor.at || argc < 1) return JS_NewFloat64(ctx, -1.0);
    return JS_NewFloat64(ctx, s->editor.at(arg_num(ctx, argv[0]), s->editor.user));
}

JSValue js_editor_save(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (!s->has_editor || !s->editor.save) return JS_FALSE;
    return s->editor.save(argc >= 1 ? str(ctx, argv[0]).c_str() : "", s->editor.user) ? JS_TRUE : JS_FALSE;
}

JSValue js_editor_select(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (s->has_editor && s->editor.select && argc >= 1)
        s->editor.select(arg_num(ctx, argv[0]), s->editor.user);
    return JS_UNDEFINED;
}

// editor.camera([ex,ey,ez], [tx,ty,tz], fov). Arrays rather than nine
// arguments, because a camera call with the target and the eye swapped is a
// bug you find by looking at a black picture.
JSValue js_editor_camera(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (!s->has_editor || !s->editor.camera || argc < 2) return JS_UNDEFINED;
    double eye[3] = { 0, 0, 0 }, target[3] = { 0, 0, 0 };
    for (int i = 0; i < 3; ++i) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        JSValue t = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)i);
        eye[i] = arg_num(ctx, e);
        target[i] = arg_num(ctx, t);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, t);
    }
    s->editor.camera(eye, target, argc > 2 ? arg_num(ctx, argv[2]) : 0.0, s->editor.user);
    return JS_UNDEFINED;
}

JSValue js_editor_shot(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (!s->has_editor || !s->editor.shot || argc < 1) return JS_FALSE;
    return s->editor.shot(str(ctx, argv[0]).c_str(), s->editor.user) ? JS_TRUE : JS_FALSE;
}

JSValue js_editor_set_material(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (!s->has_editor || !s->editor.set_material || argc < 2) return JS_FALSE;
    return s->editor.set_material(arg_num(ctx, argv[0]), str(ctx, argv[1]).c_str(),
                                  s->editor.user) ? JS_TRUE : JS_FALSE;
}

JSValue js_editor_get_material(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    const char *p = nullptr;
    if (s->has_editor && s->editor.get_material && argc >= 1)
        p = s->editor.get_material(arg_num(ctx, argv[0]), s->editor.user);
    return JS_NewString(ctx, p ? p : "");
}

} // namespace

void dai_script_bind_editor(dai_script *s, const dai_script_editor_host *host) {
    if (!s || !host) return;
    s->editor = *host;
    s->has_editor = 1;
    JSValue global = JS_GetGlobalObject(s->ctx);
    JSValue ed = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, ed, "add", JS_NewCFunction(s->ctx, js_editor_add, "add", 2));
    JS_SetPropertyStr(s->ctx, ed, "remove", JS_NewCFunction(s->ctx, js_editor_remove, "remove", 1));
    JS_SetPropertyStr(s->ctx, ed, "begin", JS_NewCFunction(s->ctx, js_editor_begin, "begin", 1));
    JS_SetPropertyStr(s->ctx, ed, "commit", JS_NewCFunction(s->ctx, js_editor_commit, "commit", 0));
    JS_SetPropertyStr(s->ctx, ed, "undo", JS_NewCFunction(s->ctx, js_editor_undo, "undo", 0));
    JS_SetPropertyStr(s->ctx, ed, "redo", JS_NewCFunction(s->ctx, js_editor_redo, "redo", 0));
    JS_SetPropertyStr(s->ctx, ed, "count", JS_NewCFunction(s->ctx, js_editor_count, "count", 0));
    JS_SetPropertyStr(s->ctx, ed, "at", JS_NewCFunction(s->ctx, js_editor_at, "at", 1));
    JS_SetPropertyStr(s->ctx, ed, "save", JS_NewCFunction(s->ctx, js_editor_save, "save", 1));
    JS_SetPropertyStr(s->ctx, ed, "select", JS_NewCFunction(s->ctx, js_editor_select, "select", 1));
    JS_SetPropertyStr(s->ctx, ed, "camera", JS_NewCFunction(s->ctx, js_editor_camera, "camera", 3));
    JS_SetPropertyStr(s->ctx, ed, "shot", JS_NewCFunction(s->ctx, js_editor_shot, "shot", 1));
    JS_SetPropertyStr(s->ctx, ed, "setMaterial",
                      JS_NewCFunction(s->ctx, js_editor_set_material, "setMaterial", 2));
    JS_SetPropertyStr(s->ctx, ed, "getMaterial",
                      JS_NewCFunction(s->ctx, js_editor_get_material, "getMaterial", 1));
    JS_SetPropertyStr(s->ctx, global, "editor", ed);
    JS_FreeValue(s->ctx, global);
}


// ------------------------------------------------------------ animation
// The `anim` global, bound by the host through dai_script_bind_anim. Same
// shape as the node block above: nothing here talks to the engine, only to the
// host's callbacks, so it sits at the end of the file and touches nothing else.
//
// Two things are not function calls on purpose. `anim.speed = 1.5` and
// `anim.time = 0` are PROPERTIES, because that is how the API reads in the
// header and a scripting layer that renames things is a scripting layer you
// have to keep translating in your head.
namespace {

std::string arg_str(JSContext *ctx, JSValueConst v) {
    const char *c = JS_ToCString(ctx, v);
    std::string out = c ? c : "";
    if (c) JS_FreeCString(ctx, c);
    return out;
}

double arg_f(JSContext *ctx, JSValueConst v, double def) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) return def;
    double d = def;
    if (JS_ToFloat64(ctx, &d, v) < 0) return def;
    return d;
}

JSValue js_anim_play(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (!s->has_anim || !s->anim.play || argc < 1) return JS_FALSE;
    return s->anim.play(arg_str(ctx, argv[0]).c_str(), argc > 1 ? arg_f(ctx, argv[1], 0.0) : 0.0,
                        s->anim.user) ? JS_TRUE : JS_FALSE;
}

JSValue js_anim_restart(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (!s->has_anim || !s->anim.restart || argc < 1) return JS_FALSE;
    return s->anim.restart(arg_str(ctx, argv[0]).c_str(),
                           argc > 1 ? arg_f(ctx, argv[1], 0.0) : 0.0,
                           argc > 2 ? arg_f(ctx, argv[2], 0.0) : 0.0,
                           s->anim.user) ? JS_TRUE : JS_FALSE;
}

JSValue js_anim_stop(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (s->has_anim && s->anim.stop) s->anim.stop(argc > 0 ? arg_f(ctx, argv[0], 0.0) : 0.0, s->anim.user);
    return JS_UNDEFINED;
}

JSValue js_anim_is_playing(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (!s->has_anim || !s->anim.is_playing || argc < 1) return JS_FALSE;
    return s->anim.is_playing(arg_str(ctx, argv[0]).c_str(), s->anim.user) ? JS_TRUE : JS_FALSE;
}

JSValue js_anim_current(JSContext *ctx, JSValueConst, int, JSValueConst *) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    const char *n = (s->has_anim && s->anim.current) ? s->anim.current(s->anim.user) : "";
    return JS_NewString(ctx, n ? n : "");
}

JSValue js_anim_weight(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (!s->has_anim || !s->anim.weight || argc < 1) return JS_NewFloat64(ctx, 0.0);
    return JS_NewFloat64(ctx, s->anim.weight(arg_str(ctx, argv[0]).c_str(), s->anim.user));
}

JSValue js_anim_finished(JSContext *ctx, JSValueConst, int, JSValueConst *) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (!s->has_anim || !s->anim.finished) return JS_FALSE;
    return s->anim.finished(s->anim.user) ? JS_TRUE : JS_FALSE;
}

// anim.onEvent("footstep", fn) and anim.onEvent(fn) for all of them. The
// handlers live in a plain object hanging off `anim`, so the garbage collector
// owns them and this file does not have to track JSValue lifetimes by hand.
JSValue js_anim_on_event(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_UNDEFINED;
    std::string key = "*";
    JSValueConst fn = argv[0];
    if (argc >= 2) { key = arg_str(ctx, argv[0]); fn = argv[1]; }
    if (!JS_IsFunction(ctx, fn)) return JS_UNDEFINED;

    JSValue handlers = JS_GetPropertyStr(ctx, this_val, "_handlers");
    if (!JS_IsObject(handlers)) {
        JS_FreeValue(ctx, handlers);
        handlers = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, this_val, "_handlers", JS_DupValue(ctx, handlers));
    }
    JSValue list = JS_GetPropertyStr(ctx, handlers, key.c_str());
    if (!JS_IsArray(list)) {
        JS_FreeValue(ctx, list);
        list = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, handlers, key.c_str(), JS_DupValue(ctx, list));
    }
    JSValue len = JS_GetPropertyStr(ctx, list, "length");
    uint32_t n = 0; JS_ToUint32(ctx, &n, len); JS_FreeValue(ctx, len);
    JS_SetPropertyUint32(ctx, list, n, JS_DupValue(ctx, fn));
    JS_FreeValue(ctx, list);
    JS_FreeValue(ctx, handlers);
    return JS_UNDEFINED;
}

JSValue js_anim_get_speed(JSContext *ctx, JSValueConst) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    return JS_NewFloat64(ctx, (s->has_anim && s->anim.get_speed) ? s->anim.get_speed(s->anim.user) : 0.0);
}
JSValue js_anim_set_speed(JSContext *ctx, JSValueConst, JSValueConst v) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (s->has_anim && s->anim.set_speed) s->anim.set_speed(arg_f(ctx, v, 1.0), s->anim.user);
    return JS_UNDEFINED;
}
JSValue js_anim_get_time(JSContext *ctx, JSValueConst) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    return JS_NewFloat64(ctx, (s->has_anim && s->anim.get_time) ? s->anim.get_time(s->anim.user) : 0.0);
}
JSValue js_anim_set_time(JSContext *ctx, JSValueConst, JSValueConst v) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (s->has_anim && s->anim.set_time) s->anim.set_time(arg_f(ctx, v, 0.0), s->anim.user);
    return JS_UNDEFINED;
}
JSValue js_anim_get_norm(JSContext *ctx, JSValueConst) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    return JS_NewFloat64(ctx, (s->has_anim && s->anim.get_normalized) ? s->anim.get_normalized(s->anim.user) : 0.0);
}
JSValue js_anim_get_param(JSContext *ctx, JSValueConst) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    return JS_NewFloat64(ctx, (s->has_anim && s->anim.get_parameter) ? s->anim.get_parameter(s->anim.user) : 0.0);
}
JSValue js_anim_set_param(JSContext *ctx, JSValueConst, JSValueConst v) {
    dai_script *s = (dai_script *)JS_GetContextOpaque(ctx);
    if (s->has_anim && s->anim.set_parameter) s->anim.set_parameter(arg_f(ctx, v, 0.0), s->anim.user);
    return JS_UNDEFINED;
}

const JSCFunctionListEntry kAnimFuncs[] = {
    JS_CFUNC_DEF("play", 2, js_anim_play),
    JS_CFUNC_DEF("crossfade", 2, js_anim_play),      // the same call, the other word for it
    JS_CFUNC_DEF("restart", 3, js_anim_restart),
    JS_CFUNC_DEF("stop", 1, js_anim_stop),
    JS_CFUNC_DEF("isPlaying", 1, js_anim_is_playing),
    JS_CFUNC_DEF("current", 0, js_anim_current),
    JS_CFUNC_DEF("weight", 1, js_anim_weight),
    JS_CFUNC_DEF("finished", 0, js_anim_finished),
    JS_CFUNC_DEF("onEvent", 2, js_anim_on_event),
    JS_CGETSET_DEF("speed", js_anim_get_speed, js_anim_set_speed),
    JS_CGETSET_DEF("time", js_anim_get_time, js_anim_set_time),
    JS_CGETSET_DEF("normalizedTime", js_anim_get_norm, nullptr),
    JS_CGETSET_DEF("parameter", js_anim_get_param, js_anim_set_param),
};

// Calls every handler in one list. A handler that throws is counted and the
// rest still run: one broken footstep sound must not silence the others.
void call_list(dai_script *s, JSValue list, JSValue ev, JSValue cl, JSValue tm) {
    if (!JS_IsArray(list)) return;
    JSValue len = JS_GetPropertyStr(s->ctx, list, "length");
    uint32_t n = 0; JS_ToUint32(s->ctx, &n, len); JS_FreeValue(s->ctx, len);
    for (uint32_t i = 0; i < n; ++i) {
        JSValue fn = JS_GetPropertyUint32(s->ctx, list, i);
        if (JS_IsFunction(s->ctx, fn)) {
            JSValue args[3] = { ev, cl, tm };
            JSValue r = JS_Call(s->ctx, fn, JS_UNDEFINED, 3, args);
            if (JS_IsException(r)) record_error(s, nullptr, 0);
            JS_FreeValue(s->ctx, r);
        }
        JS_FreeValue(s->ctx, fn);
    }
}

} // namespace

void dai_script_bind_anim(dai_script *s, const dai_script_anim_host *host) {
    if (!s || !host) return;
    s->anim = *host;
    s->has_anim = 1;
    JSValue global = JS_GetGlobalObject(s->ctx);
    JSValue anim = JS_NewObject(s->ctx);
    JS_SetPropertyFunctionList(s->ctx, anim, kAnimFuncs, (int)(sizeof(kAnimFuncs) / sizeof(kAnimFuncs[0])));
    JS_SetPropertyStr(s->ctx, anim, "_handlers", JS_NewObject(s->ctx));
    JS_SetPropertyStr(s->ctx, global, "anim", anim);
    JS_FreeValue(s->ctx, global);
}

void dai_script_anim_event(dai_script *s, const char *event, const char *clip, double time) {
    if (!s || !s->ctx || !event) return;
    JSValue global = JS_GetGlobalObject(s->ctx);
    JSValue anim = JS_GetPropertyStr(s->ctx, global, "anim");
    JSValue handlers = JS_IsObject(anim) ? JS_GetPropertyStr(s->ctx, anim, "_handlers") : JS_UNDEFINED;
    if (JS_IsObject(handlers)) {
        JSValue ev = JS_NewString(s->ctx, event);
        JSValue cl = JS_NewString(s->ctx, clip ? clip : "");
        JSValue tm = JS_NewFloat64(s->ctx, time);
        JSValue by_name = JS_GetPropertyStr(s->ctx, handlers, event);
        call_list(s, by_name, ev, cl, tm);
        JS_FreeValue(s->ctx, by_name);
        JSValue any = JS_GetPropertyStr(s->ctx, handlers, "*");
        call_list(s, any, ev, cl, tm);
        JS_FreeValue(s->ctx, any);
        JS_FreeValue(s->ctx, ev); JS_FreeValue(s->ctx, cl); JS_FreeValue(s->ctx, tm);
    }
    JS_FreeValue(s->ctx, handlers);
    JS_FreeValue(s->ctx, anim);
    JS_FreeValue(s->ctx, global);
}
