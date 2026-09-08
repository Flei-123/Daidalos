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

    // ---- 6. the `editor` binding: what a TOOL may do and a behaviour may not
    //
    // The Jarvis bridge drives modelling through this and nothing else (see
    // include/dai_bridge_host.inl), so the surface is checked here rather than
    // only through the socket: a fake host records what the script asked for,
    // and the checks are that the right call arrived with the right arguments.
    // Every value below is one a script wrote - none of them is set by the
    // fixture, or the test would be agreeing with itself.
    {
        static struct FakeEditor {
            int added = 0, removed = 0, begun = 0, committed = 0, undone = 0, redone = 0;
            int saved = 0, selected = 0, shot = 0, material_set = 0;
            std::string last_name, last_label, last_path, last_material;
            double last_parent = -1, last_removed = -1, last_selected = -1;
            double eye[3] = { 0, 0, 0 }, target[3] = { 0, 0, 0 }, fov = 0;
            double next_id = 100;
        } fake;

        dai_script_editor_host eh{};
        eh.add = [](const char *name, double parent, void *) -> double {
            ++fake.added; fake.last_name = name ? name : ""; fake.last_parent = parent;
            return fake.next_id++;
        };
        eh.remove = [](double id, void *) -> int { ++fake.removed; fake.last_removed = id; return 1; };
        eh.begin = [](const char *label, void *) { ++fake.begun; fake.last_label = label ? label : ""; };
        eh.commit = [](void *) { ++fake.committed; };
        eh.undo = [](void *) -> int { ++fake.undone; return 1; };
        eh.redo = [](void *) -> int { ++fake.redone; return 0; };
        eh.count = [](void *) -> double { return 7.0; };
        eh.at = [](double index, void *) -> double { return 200.0 + index; };
        eh.save = [](const char *path, void *) -> int {
            ++fake.saved; fake.last_path = path ? path : ""; return 1;
        };
        eh.select = [](double id, void *) { ++fake.selected; fake.last_selected = id; };
        eh.camera = [](const double *eye, const double *target, double fov, void *) {
            for (int i = 0; i < 3; ++i) { fake.eye[i] = eye[i]; fake.target[i] = target[i]; }
            fake.fov = fov;
        };
        eh.shot = [](const char *path, void *) -> int { ++fake.shot; return 1; };
        eh.set_material = [](double, const char *path, void *) -> int {
            ++fake.material_set; fake.last_material = path ? path : ""; return 1;
        };
        eh.get_material = [](double, void *) -> const char * { return "materials/rost.daimat"; };

        // Before the binding exists, `editor` must not: a game that never
        // called bind must not be able to delete its own scene from a script.
        CHECK(dai_script_eval(s, "state.hasEditor = (typeof editor === 'undefined') ? 0 : 1;",
                              "editor", err, sizeof(err)) == DAI_OK, "probing editor failed: %s", err);
        CHECK(dai_script_get_number(s, "hasEditor", -1.0) == 0.0,
              "the `editor` global existed before anything bound it");

        dai_script_bind_editor(s, &eh);

        CHECK(dai_script_eval(s,
              "editor.begin('build a room');"
              "state.made = editor.add('Wall', 3);"
              "editor.setMaterial(state.made, 'materials/raufaser.daimat');"
              "state.mat = editor.getMaterial(state.made);"
              "editor.select(state.made);"
              "editor.camera([1,2,3],[4,5,6],52);"
              "editor.commit();"
              "state.count = editor.count();"
              "state.third = editor.at(2);"
              "state.undone = editor.undo() ? 1 : 0;"
              "state.redone = editor.redo() ? 1 : 0;"
              "state.savedOk = editor.save('scenes/room.daiscene') ? 1 : 0;"
              "state.shotOk = editor.shot('/tmp/x.png') ? 1 : 0;"
              "editor.remove(state.made);",
              "editor", err, sizeof(err)) == DAI_OK, "the editor script failed: %s", err);

        CHECK(fake.added == 1, "add was called %d times, not once", fake.added);
        CHECK(fake.last_name == "Wall", "the node was called '%s'", fake.last_name.c_str());
        CHECK(fake.last_parent == 3.0, "the parent arrived as %.1f, not 3", fake.last_parent);
        CHECK(dai_script_get_number(s, "made", -1.0) == 100.0,
              "add did not answer the host's id");
        CHECK(fake.begun == 1 && fake.committed == 1,
              "the undo bracket was begun %d and committed %d times", fake.begun, fake.committed);
        CHECK(fake.last_label == "build a room", "the undo step is called '%s'", fake.last_label.c_str());
        CHECK(fake.material_set == 1 && fake.last_material == "materials/raufaser.daimat",
              "setMaterial got '%s'", fake.last_material.c_str());
        char mat[64] = { 0 };
        dai_script_get_string(s, "mat", mat, sizeof(mat));
        CHECK(std::strcmp(mat, "materials/rost.daimat") == 0,
              "getMaterial answered '%s'", mat);
        CHECK(fake.selected == 1 && fake.last_selected == 100.0,
              "select got node %.0f", fake.last_selected);
        CHECK(fake.eye[0] == 1.0 && fake.eye[1] == 2.0 && fake.eye[2] == 3.0,
              "the camera's eye arrived as %.1f %.1f %.1f", fake.eye[0], fake.eye[1], fake.eye[2]);
        CHECK(fake.target[0] == 4.0 && fake.target[1] == 5.0 && fake.target[2] == 6.0,
              "the camera's target arrived as %.1f %.1f %.1f",
              fake.target[0], fake.target[1], fake.target[2]);
        CHECK(fake.fov == 52.0, "the fov arrived as %.1f", fake.fov);
        CHECK(dai_script_get_number(s, "count", -1.0) == 7.0, "count did not come through");
        CHECK(dai_script_get_number(s, "third", -1.0) == 202.0, "at(2) did not come through");
        CHECK(dai_script_get_number(s, "undone", -1.0) == 1.0, "undo answered false");
        CHECK(dai_script_get_number(s, "redone", -1.0) == 0.0,
              "redo answered true although the host said there was nothing to redo");
        CHECK(fake.undone == 1 && fake.redone == 1, "undo/redo were not called once each");
        CHECK(fake.saved == 1 && fake.last_path == "scenes/room.daiscene",
              "save got '%s'", fake.last_path.c_str());
        CHECK(fake.shot == 1, "shot was not taken");
        CHECK(fake.removed == 1 && fake.last_removed == 100.0,
              "remove got node %.0f", fake.last_removed);

        // A tool script that throws must not take the editor down either.
        CHECK(dai_script_eval(s, "editor.add()", "editor", err, sizeof(err)) == DAI_OK,
              "editor.add() with no name threw: %s", err);
        CHECK(fake.added == 1, "add ran with no name at all");
    }

    dai_script_destroy(s);
    dai_ui_destroy(ui);
    dai_font_free(font);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
