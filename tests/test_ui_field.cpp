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
#include <string>
#include <vector>

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
    // ---- the search box keeps the keyboard while the list filters --------
    // Opened near the bottom edge, the panel used to be clamped against its
    // CURRENT height: every character removed rows, the panel got shorter,
    // the clamp let it slide back down - and a text field is identified by
    // where it is, so the search box became a DIFFERENT field between two
    // keystrokes. The first letters arrived, the rest went nowhere. That is
    // what "the material search does not work" looked like from outside.
    std::printf("a search box near the bottom edge keeps its caret\n");
    {
        static char labels[24][16];
        dai_ui_menu_item items[24];
        for (int i = 0; i < 24; ++i) {
            std::snprintf(labels[i], sizeof(labels[i]), "%s%d",
                          (i % 3 == 0) ? "carbon" : "steel", i);
            items[i] = { nullptr, labels[i], nullptr, 0 };
        }
        dai_ui_searchlist sl{};
        dai_ui_searchlist_open(&sl, 100.0f, 500.0f);   // 100px above the bottom
        sl.wants_focus = 1;
        auto sframe = [&]() {
            in.mouse_x = 400; in.mouse_y = 300; in.mouse_down = 0;
            dai_ui_begin(ui, 800, 600, &in);
            dai_ui_searchlist_draw(ui, &sl, items, 24);
            dai_ui_end(ui);
            std::memset(in.text, 0, sizeof(in.text));
            in.key_backspace = in.key_enter = in.key_tab = 0;
            in.key_left = in.key_right = in.key_home = in.key_end = 0;
            in.key_delete = in.key_escape = in.key_select_all = 0;
            in.double_click = 0;
        };
        sframe();                                   // it takes the keyboard
        CHECK(dai_ui_text_active(ui) == 1, "the search box never got the keyboard");
        // Long enough that the list narrows to a single row on the way -
        // which is the moment the panel used to change height and move.
        const char *word = "carbon21";
        for (const char *c = word; *c; ++c) {
            char one[2] = { *c, 0 };
            type(one);
            sframe();
        }
        CHECK(std::strcmp(sl.query, word) == 0,
              "typing \"%s\" into the search box gave \"%s\" - it lost focus mid word",
              word, sl.query);
    }

    // ---- the keyboard is given back when the field stops being drawn -----
    // Twice now F2 stopped renaming things because "a text field has the
    // keyboard" stayed true forever: once because a code editor set the flag
    // and never cleared it, once because a panel closed while one of its
    // fields was in edit. Both look identical from the outside - a shortcut
    // that worked at startup and was gone after some clicking.
    std::printf("a field that is no longer drawn lets go\n");
    value = 3.0f;
    begin(BOX_X, BOX_Y, 0); field(); endf();
    begin(BOX_X, BOX_Y, 1); field(); endf();          // click: it has the keyboard
    CHECK(dai_ui_text_active(ui) == 1, "clicking the field did not take the keyboard");
    begin(BOX_X, BOX_Y, 0); field(); endf();          // still drawn, still editing
    CHECK(dai_ui_text_active(ui) == 1, "the field lost the keyboard while still on screen");
    begin(BOX_X, BOX_Y, 0); /* panel closed: no field */ endf();
    CHECK(dai_ui_text_active(ui) == 0,
          "a field nobody drew still holds the keyboard - this is the F2 bug");

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


    // ---- array rows: the grip on the left reorders the list ----------------
    // The handle used to be three lines of decoration - it said "this is a
    // list" and did nothing. Dragging it has to MOVE the element, and the
    // element has to land where the insertion line was, not one off.
    std::printf("array rows: drag the grip, the element moves\n");
    {
        std::vector<std::string> list = { "Red", "Green", "Blue", "Yellow" };
        int arr_open = 1;
        int moves = 0;
        int last_from = -1, last_to = -1;

        const float LH   = dai_font_line_height(font);
        const float ROWH = LH + st->row_pad;              // widget_height
        const float HEAD = 20.0f + 1.0f + st->spacing;    // the array header row
        const float ROW0 = st->padding + HEAD;            // top of Element 0
        const float STEP = ROWH + 1.0f + st->spacing;     // one row to the next
        const float GRIP = 14.0f;                         // inside the handle column

        // One frame of the whole array, driven at (mx,my) with the button in
        // the given state. Returns nothing - the checks read `list`.
        auto arr_frame = [&](float mx, float my, int down) {
            in.mouse_x = mx; in.mouse_y = my; in.mouse_down = down;
            dai_ui_begin(ui, 800, 600, &in);
            dai_ui_panel_begin(ui, 0, 0, 300, 400, nullptr);
            int n = (int)list.size();
            if (dai_ui_array_begin(ui, "Materials", &n, &arr_open, 1, 8)) {
                for (size_t i = 0; i < list.size(); ++i)
                    dai_ui_array_object_row(ui, (int)i, list[i].c_str(), nullptr);
                dai_ui_array_end(ui, (int)list.size(), 1, 8);
                int f = -1, t = -1;
                if (dai_ui_array_reorder(ui, &f, &t) &&
                    f >= 0 && f < (int)list.size() && t >= 0 && t < (int)list.size()) {
                    std::string moved = list[(size_t)f];
                    list.erase(list.begin() + f);
                    list.insert(list.begin() + t, moved);
                    ++moves; last_from = f; last_to = t;
                }
            }
            dai_ui_panel_end(ui);
            dai_ui_end(ui);
            in.double_click = 0;
        };
        auto row_mid = [&](int i) { return ROW0 + STEP * (float)i + ROWH * 0.5f; };

        // 1. grabbing the grip starts a drag - and says which gap it is over
        arr_frame(GRIP, row_mid(2), 0);
        CHECK(dai_ui_array_drag_slot(ui) == -1, "a drag started without anyone pressing");
        arr_frame(GRIP, row_mid(2), 1);
        CHECK(dai_ui_array_drag_slot(ui) == 2,
              "pressing the grip of Element 2 gave slot %d, expected 2 - the handle "
              "is not where the test thinks it is", dai_ui_array_drag_slot(ui));

        // 2. drag it to the very top: the gap above Element 0
        arr_frame(GRIP, ROW0 + 1.0f, 1);
        CHECK(dai_ui_array_drag_slot(ui) == 0,
              "dragging to the top gave slot %d, expected 0", dai_ui_array_drag_slot(ui));
        CHECK(moves == 0, "the list moved while the button was still held");

        // 3. release: Blue is now first, and nothing else changed order
        arr_frame(GRIP, ROW0 + 1.0f, 0);
        CHECK(moves == 1, "releasing the grip reported %d moves, expected 1", moves);
        CHECK(last_from == 2 && last_to == 0,
              "the move was %d -> %d, expected 2 -> 0", last_from, last_to);
        CHECK(list.size() == 4, "the list changed length: %d", (int)list.size());
        CHECK(list[0] == "Blue" && list[1] == "Red" && list[2] == "Green" &&
              list[3] == "Yellow",
              "after dragging Element 2 to the top the list reads %s,%s,%s,%s",
              list[0].c_str(), list[1].c_str(), list[2].c_str(), list[3].c_str());
        CHECK(dai_ui_array_drag_slot(ui) == -1, "the grip stayed stuck to the mouse");

        // 4. downwards, and the OFF BY ONE: the gap below the last row is
        //    slot == count, and the element must land at count-1, not past it.
        arr_frame(GRIP, row_mid(0), 1);
        CHECK(dai_ui_array_drag_slot(ui) == 0, "grabbing Element 0 gave slot %d",
              dai_ui_array_drag_slot(ui));
        arr_frame(GRIP, ROW0 + STEP * 4.0f, 1);           // below every row
        CHECK(dai_ui_array_drag_slot(ui) == 4,
              "dragging past the last row gave slot %d, expected 4",
              dai_ui_array_drag_slot(ui));
        arr_frame(GRIP, ROW0 + STEP * 4.0f, 0);
        CHECK(moves == 2, "the second drag reported nothing");
        CHECK(last_from == 0 && last_to == 3,
              "dropping below the last row was %d -> %d, expected 0 -> 3",
              last_from, last_to);
        CHECK(list[0] == "Red" && list[1] == "Green" && list[2] == "Yellow" &&
              list[3] == "Blue",
              "after moving the top element to the bottom the list reads %s,%s,%s,%s",
              list[0].c_str(), list[1].c_str(), list[2].c_str(), list[3].c_str());

        // 5. a drag that ends where it started is not a move
        arr_frame(GRIP, row_mid(1), 1);
        arr_frame(GRIP, row_mid(1), 0);
        CHECK(moves == 2, "dropping a row on itself counted as a move");

        // 6. pressing the FIELD (not the grip) still picks - it must not drag
        const float FIELD_X = st->label_w > 90.0f ? st->label_w + 20.0f : 110.0f;
        arr_frame(FIELD_X, row_mid(1), 1);
        CHECK(dai_ui_array_drag_slot(ui) == -1,
              "clicking the value field started a reorder drag");
        arr_frame(FIELD_X, row_mid(1), 0);
        CHECK(moves == 2, "clicking the value field moved an element");
    }

    dai_ui_destroy(ui);
    dai_font_free(font);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
