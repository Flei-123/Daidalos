// Text fields, the way a text field has to behave.
//
//   ./build/test_ui_field
//
// This file exists because "you can type into it" is not the same thing as a
// text field. The old numeric field had a caret that could only ever sit at
// the end, no selection at all, and no way to replace a value except by
// holding backspace - which is how an inspector ends up feeling like a form
// from 1994. Every check below is a thing a user does without thinking:
// click and type over the value, click again to put the caret somewhere,
// drag across three characters, press Home, press Escape.
//
// No renderer and no window: the UI turns input into vertices, and both ends
// of that are testable in a plain process.

#include "dai_ui.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

int main() {
    char err[256] = { 0 };
    dai_font *font = dai_font_load("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 16.0f,
                                   nullptr, 0, err, sizeof(err));
    CHECK(font != nullptr, "font load failed: %s", err);
    if (!font) return 1;
    dai_ui *ui = dai_ui_create(font, 0);

    // One numeric field, always at the same place, driven frame by frame.
    float value = 12.5f;
    dai_ui_input in{};
    auto begin = [&](float mx, float my, int down) {
        in.mouse_x = mx; in.mouse_y = my; in.mouse_down = down;
        dai_ui_begin(ui, 800, 600, &in);
        dai_ui_panel_begin(ui, 0, 0, 300, 200, nullptr);
    };
    auto endf = [&]() {
        dai_ui_panel_end(ui);
        dai_ui_end(ui);
        // Text and key events are edge triggered - the host clears them every
        // frame, and so does this.
        std::memset(in.text, 0, sizeof(in.text));
        in.key_backspace = in.key_enter = in.key_tab = 0;
        in.key_left = in.key_right = in.key_home = in.key_end = 0;
        in.key_delete = in.key_escape = in.key_select_all = 0;
        in.double_click = 0;
    };
    auto field = [&]() { dai_ui_num_field(ui, "X", &value, 0.1f, 0.0f, 0.0f, "x"); };
    auto type = [&](const char *s) {
        int i = 0;
        for (const char *c = s; *c && i < 7; ++c) in.text[i++] = (uint32_t)(unsigned char)*c;
        in.text[i] = 0;
    };

    // Where the box is: past the label column, inside the first row.
    const dai_ui_style *st = dai_ui_style_of(ui);
    const float BOX_X = st->padding + st->label_w + 20.0f;
    const float BOX_Y = st->padding + 8.0f;

    // ---- 1. one click selects the whole value, and typing replaces it ------
    std::printf("click, type, enter\n");
    begin(BOX_X, BOX_Y, 0); field(); endf();
    begin(BOX_X, BOX_Y, 1); field(); endf();          // press: focus + select all
    CHECK(dai_ui_num_editing(ui) == 1, "clicking a numeric field did not start editing");
    begin(BOX_X, BOX_Y, 0); field(); endf();
    type("7");
    begin(BOX_X, BOX_Y, 0); field(); endf();
    in.key_enter = 1;
    begin(BOX_X, BOX_Y, 0); field(); endf();
    CHECK(std::fabs(value - 7.0f) < 1e-5f,
          "typing 7 over a selected 12.5 gave %.3f - the click did not select the value", value);
    CHECK(dai_ui_num_editing(ui) == 0, "Enter did not end the edit");

    // ---- 2. typing appends when the caret was placed by a second click -----
    std::printf("second click places the caret\n");
    value = 12.0f;
    begin(BOX_X, BOX_Y, 0); field(); endf();
    begin(BOX_X, BOX_Y, 1); field(); endf();          // click 1: select all
    begin(BOX_X, BOX_Y, 0); field(); endf();
    in.key_end = 1;                                    // caret to the end, nothing selected
    begin(BOX_X, BOX_Y, 0); field(); endf();
    type("5");
    begin(BOX_X, BOX_Y, 0); field(); endf();
    in.key_enter = 1;
    begin(BOX_X, BOX_Y, 0); field(); endf();
    CHECK(std::fabs(value - 125.0f) < 1e-3f,
          "End then 5 on \"12\" gave %.3f, expected 125 - Home/End do not move the caret", value);

    // ---- 3. Home, arrows and Backspace ------------------------------------
    std::printf("home, arrows, backspace\n");
    value = 125.0f;
    begin(BOX_X, BOX_Y, 0); field(); endf();
    begin(BOX_X, BOX_Y, 1); field(); endf();
    begin(BOX_X, BOX_Y, 0); field(); endf();
    in.key_home = 1;                                   // caret before the 1
    begin(BOX_X, BOX_Y, 0); field(); endf();
    in.key_right = 1;                                  // after the 1
    begin(BOX_X, BOX_Y, 0); field(); endf();
    in.key_delete = 1;                                 // eat the 2
    begin(BOX_X, BOX_Y, 0); field(); endf();
    in.key_enter = 1;
    begin(BOX_X, BOX_Y, 0); field(); endf();
    CHECK(std::fabs(value - 15.0f) < 1e-3f,
          "Home, Right, Delete on \"125\" gave %.3f, expected 15", value);

    // ---- 4. Escape puts the old value back --------------------------------
    std::printf("escape cancels\n");
    value = 42.0f;
    begin(BOX_X, BOX_Y, 0); field(); endf();
    begin(BOX_X, BOX_Y, 1); field(); endf();
    begin(BOX_X, BOX_Y, 0); field(); endf();
    type("9");
    begin(BOX_X, BOX_Y, 0); field(); endf();
    in.key_escape = 1;
    begin(BOX_X, BOX_Y, 0); field(); endf();
    CHECK(std::fabs(value - 42.0f) < 1e-5f, "Escape kept the typed value (%.3f)", value);
    CHECK(dai_ui_num_editing(ui) == 0, "Escape did not end the edit");

    // ---- 5. clicking somewhere else commits -------------------------------
    std::printf("click away commits\n");
    value = 1.0f;
    begin(BOX_X, BOX_Y, 0); field(); endf();
    begin(BOX_X, BOX_Y, 1); field(); endf();
    begin(BOX_X, BOX_Y, 0); field(); endf();
    type("8");
    begin(BOX_X, BOX_Y, 0); field(); endf();
    begin(10.0f, 180.0f, 1); field(); endf();          // press far away
    CHECK(std::fabs(value - 8.0f) < 1e-5f,
          "clicking away dropped the typed value (%.3f) - half typed numbers must commit", value);

    // ---- 6. a numeric field refuses letters, a text field takes them -------
    std::printf("what each field accepts\n");
    value = 3.0f;
    begin(BOX_X, BOX_Y, 0); field(); endf();
    begin(BOX_X, BOX_Y, 1); field(); endf();
    begin(BOX_X, BOX_Y, 0); field(); endf();
    type("abc");
    begin(BOX_X, BOX_Y, 0); field(); endf();
    in.key_enter = 1;
    begin(BOX_X, BOX_Y, 0); field(); endf();
    CHECK(std::fabs(value - 3.0f) < 1e-5f, "letters got into a numeric field (%.3f)", value);

    char name[64] = "Cube";
    auto text_field = [&]() { dai_ui_input_text(ui, "Name", name, sizeof(name)); };
    begin(BOX_X, BOX_Y, 0); text_field(); endf();
    begin(BOX_X, BOX_Y, 1); text_field(); endf();       // select all
    begin(BOX_X, BOX_Y, 0); text_field(); endf();
    type("Wall");
    begin(BOX_X, BOX_Y, 0); text_field(); endf();
    in.key_enter = 1;
    begin(BOX_X, BOX_Y, 0); text_field(); endf();
    CHECK(std::strcmp(name, "Wall") == 0,
          "typing over a selected name gave \"%s\", expected \"Wall\"", name);

    // ---- 7. select all, then one keystroke replaces everything ------------
    std::printf("ctrl+a\n");
    std::snprintf(name, sizeof(name), "Something Long");
    begin(BOX_X, BOX_Y, 0); text_field(); endf();
    begin(BOX_X, BOX_Y, 1); text_field(); endf();
    begin(BOX_X, BOX_Y, 0); text_field(); endf();
    in.key_end = 1;                                     // deselect first
    begin(BOX_X, BOX_Y, 0); text_field(); endf();
    in.key_select_all = 1;
    begin(BOX_X, BOX_Y, 0); text_field(); endf();
    type("Z");
    begin(BOX_X, BOX_Y, 0); text_field(); endf();
    in.key_enter = 1;
    begin(BOX_X, BOX_Y, 0); text_field(); endf();
    CHECK(std::strcmp(name, "Z") == 0, "Ctrl+A then Z gave \"%s\"", name);

    // ---- 8. the pointer says what it is over ------------------------------
    std::printf("cursor shapes\n");
    begin(BOX_X, BOX_Y, 0); field(); endf();
    CHECK(dai_ui_cursor(ui) == DAI_CURSOR_TEXT,
          "hovering a field gave cursor %d, expected the I-beam", dai_ui_cursor(ui));
    begin(5.0f, 190.0f, 0); field(); endf();
    CHECK(dai_ui_cursor(ui) == DAI_CURSOR_ARROW,
          "empty panel space gave cursor %d, expected the arrow", dai_ui_cursor(ui));
    // The axis letter of a vec3 row is a drag handle, and says so.
    float xyz[3] = { 1, 2, 3 };
    in.mouse_x = st->padding + st->label_w + 3.0f; in.mouse_y = BOX_Y; in.mouse_down = 0;
    dai_ui_begin(ui, 800, 600, &in);
    dai_ui_panel_begin(ui, 0, 0, 300, 200, nullptr);
    dai_ui_num_vec3(ui, "Pos", xyz, 0.1f);
    dai_ui_panel_end(ui);
    dai_ui_end(ui);
    CHECK(dai_ui_cursor(ui) == DAI_CURSOR_SIZE_WE,
          "hovering the X handle gave cursor %d, expected the horizontal resize",
          dai_ui_cursor(ui));

    // ---- 9. a window can be resized by ANY edge, and the pointer shows it --
    std::printf("window edges\n");
    dai_ui_window win = dai_ui_window_make(200, 100, 300, 200);
    auto win_frame = [&](float mx, float my, int down) {
        in.mouse_x = mx; in.mouse_y = my; in.mouse_down = down;
        dai_ui_begin(ui, 800, 600, &in);
        if (dai_ui_window_begin(ui, "Inspector", &win)) dai_ui_label(ui, "body");
        dai_ui_window_end(ui);
        dai_ui_end(ui);
    };
    win_frame(200.0f, 200.0f, 0);          // on the left edge, half way down
    CHECK(dai_ui_cursor(ui) == DAI_CURSOR_SIZE_WE,
          "the left edge gave cursor %d, expected the horizontal resize", dai_ui_cursor(ui));
    win_frame(499.0f, 299.0f, 0);          // bottom right corner
    CHECK(dai_ui_cursor(ui) == DAI_CURSOR_SIZE_NWSE,
          "the bottom right corner gave cursor %d, expected the diagonal resize",
          dai_ui_cursor(ui));

    float w0 = win.w;
    win_frame(200.0f, 200.0f, 1);          // grab the left edge
    win_frame(180.0f, 200.0f, 1);          // and pull it left
    CHECK(win.w > w0 + 15.0f,
          "dragging the left edge 20 px left changed the width by %.1f - a right docked "
          "window can only be resized by its left edge", win.w - w0);
    CHECK(std::fabs(win.x - 180.0f) < 2.0f, "the left edge ended up at %.1f, expected 180", win.x);
    win_frame(180.0f, 200.0f, 0);

    float h0 = win.h;
    win_frame(300.0f, 300.0f, 1);          // bottom edge
    win_frame(300.0f, 340.0f, 1);
    CHECK(win.h > h0 + 30.0f, "dragging the bottom edge down 40 px changed the height by %.1f",
          win.h - h0);
    win_frame(300.0f, 340.0f, 0);

    dai_ui_destroy(ui);
    dai_font_free(font);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
