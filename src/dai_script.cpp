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
