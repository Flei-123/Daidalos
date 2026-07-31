// Scripted UI through QuickJS: bindings, hot reload, and error containment.
//
//   ./build/test_script [/tmp]

#include "dai_script.h"
#include "dai_ui.h"
#include "dai_font.h"
#include <cstdio>
#include <cstring>
#include <string>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

static uint32_t verts(dai_ui *ui) {
    const dai_ui_draw *d = nullptr;
    uint32_t n = dai_ui_draws(ui, &d), total = 0;
    for (uint32_t i = 0; i < n; ++i) total += d[i].count;
    return total;
}

static void write_file(const std::string &path, const char *text) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (f) { std::fwrite(text, 1, std::strlen(text), f); std::fclose(f); }
}

int main(int argc, char **argv) {
    std::string dir = argc > 1 ? argv[1] : "/tmp";
    char err[512] = {0};

    dai_font *font = dai_font_load("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 18.0f,
                                   nullptr, 0, err, sizeof(err));
    CHECK(font != nullptr, "font load failed: %s", err);
    if (!font) return 1;
    dai_ui *ui = dai_ui_create(font, 0);

    dai_script *s = dai_script_create(err, sizeof(err));
    CHECK(s != nullptr, "script runtime creation failed: %s", err);
    if (!s) return 1;
    dai_script_bind_ui(s, ui);
    std::printf("scripted ui\n");

    // ---- 1. plain evaluation and host values
    CHECK(dai_script_eval(s, "var answer = 6 * 7;", "test", err, sizeof(err)) == DAI_OK,
          "evaluating trivial code failed: %s", err);
    dai_script_set_number(s, "fps", 59.94);
    CHECK(dai_script_eval(s, "state.doubled = state.fps * 2;", "test", err, sizeof(err)) == DAI_OK,
          "reading a host value failed: %s", err);
    double back = dai_script_get_number(s, "doubled", -1.0);
    CHECK(back > 119.8 && back < 120.0, "host <-> script number round trip gave %.3f", back);

    // ---- 2. a script draws UI
    const char *draw_script = R"JS(
        function draw() {
            ui.panel(10, 10, 260, 200, "Scripted");
            ui.label("fps: " + state.fps.toFixed(1));
            ui.separator();
            if (ui.button("Reload")) state.clicked = 1;
            state.volume = ui.slider("Volume", state.volume, 0, 1);
            ui.progress(0.5, "half");
            ui.panelEnd();
        }
    )JS";
    dai_script_set_number(s, "volume", 0.5);
    CHECK(dai_script_eval(s, draw_script, "draw.js", err, sizeof(err)) == DAI_OK,
          "loading the draw script failed: %s", err);

    dai_ui_input in{};
    dai_ui_begin(ui, 800, 600, &in);
    CHECK(dai_script_call(s, "draw", err, sizeof(err)) == DAI_OK, "calling draw() failed: %s", err);
    dai_ui_end(ui);
    uint32_t v = verts(ui);
    std::printf("  script produced %u vertices\n", v);
    CHECK(v > 200, "the scripted UI only made %u vertices", v);

    // ---- 3. hot reload: change the file, reload, see the change
    std::string path = dir + "/ui_script.js";
    write_file(path, "function draw() { ui.panel(0,0,100,60,null); ui.label('one'); ui.panelEnd(); }");
    CHECK(dai_script_load(s, path.c_str(), err, sizeof(err)) == DAI_OK, "loading from file failed: %s", err);
    dai_ui_begin(ui, 800, 600, &in);
    dai_script_call(s, "draw", err, sizeof(err));
    dai_ui_end(ui);
    uint32_t before = verts(ui);

    write_file(path, "function draw() { ui.panel(0,0,300,300,'more');"
                     " for (var i = 0; i < 8; i++) ui.label('line ' + i); ui.panelEnd(); }");
    CHECK(dai_script_reload(s, err, sizeof(err)) == DAI_OK, "hot reload failed: %s", err);
    dai_ui_begin(ui, 800, 600, &in);
    dai_script_call(s, "draw", err, sizeof(err));
    dai_ui_end(ui);
    uint32_t after = verts(ui);
    std::printf("  before reload %u vertices, after %u\n", before, after);
    CHECK(after > before * 2, "hot reload did not change the UI (%u -> %u)", before, after);

    // ---- 4. a broken script must not take the frame down
    uint32_t errors_before = dai_script_error_count(s);
    CHECK(dai_script_eval(s, "this is not javascript", "bad", err, sizeof(err)) != DAI_OK,
          "a syntax error was accepted");
    CHECK(dai_script_error_count(s) == errors_before + 1, "the error was not counted");
    CHECK(err[0] != 0, "no error message was produced");

    dai_script_eval(s, "function boom() { null.x = 1; }", "boom", err, sizeof(err));
    CHECK(dai_script_call(s, "boom", err, sizeof(err)) != DAI_OK, "a throwing function reported success");
    CHECK(dai_script_error_count(s) == errors_before + 2, "the runtime error was not counted");

    // and the runtime still works afterwards
    dai_ui_begin(ui, 800, 600, &in);
    CHECK(dai_script_call(s, "draw", err, sizeof(err)) == DAI_OK,
          "the runtime broke after an error: %s", err);
    dai_ui_end(ui);
    CHECK(verts(ui) > 100, "drawing stopped working after a script error");

    // ---- 5. calling something that is not a function is reported, not fatal
    CHECK(dai_script_call(s, "nope", err, sizeof(err)) == DAI_ERR_NOT_FOUND,
          "calling a missing function did not report NOT_FOUND");

    dai_script_destroy(s);
    dai_ui_destroy(ui);
    dai_font_free(font);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
