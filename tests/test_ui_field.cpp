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

#include <algorithm>
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

    // ---- a segmented strip in a 180 px dock column -------------------------
    //
    // The show panels dock at 180 px, and "Sampling  Surface|Volume|Silhouette"
    // used to be drawn wider than that: the strip claimed a 40 px minimum per
    // row whatever was left over, so the last segment ended outside the panel
    // and its label outside the segment. Nothing crashed, nothing asserted, and
    // the screenshot was unreadable - which is why the geometry is measured
    // here rather than looked at.
    {
        std::printf("segmented strip in a narrow panel\n");
        const float PX = 40.0f, PY = 10.0f, PW = 180.0f;
        static const char *const MODES[3] = { "Surface", "Volume", "Silhouette" };
        int mode = 0;
        in.mouse_x = -100.0f; in.mouse_y = -100.0f; in.mouse_down = 0;
        dai_ui_begin(ui, 800, 600, &in);
        dai_ui_panel_begin(ui, PX, PY, PW, 200.0f, nullptr);
        dai_ui_seg_buttons(ui, "Sampling", &mode, MODES, 3);
        dai_ui_segmented(ui, MODES, 3, &mode);
        dai_ui_panel_end(ui);
        dai_ui_end(ui);

        // The strip's own pixels, told apart from the panel behind them by the
        // colours only a segment uses: the two button fills, the accent of the
        // selected one, and the white the selected label is drawn in.
        const dai_ui_style *ss = dai_ui_style_of(ui);
        const uint32_t MINE[5] = { ss->button, ss->button_hover, ss->button_active,
                                   ss->accent, 0xFFFFFFFFu };
        const dai_ui_draw *draws = nullptr;
        uint32_t nb = dai_ui_draws(ui, &draws);
        float maxx = 0.0f;
        int seen = 0;
        for (uint32_t b = 0; b < nb; ++b) {
            for (uint32_t v = 0; v < draws[b].count; ++v) {
                const dai_ui_vertex &vx = draws[b].vertices[v];
                for (int m = 0; m < 5; ++m) {
                    if (vx.color != MINE[m]) continue;
                    if (vx.x > maxx) maxx = vx.x;
                    ++seen;
                    break;
                }
            }
        }
        const float limit = PX + PW - ss->padding;
        CHECK(seen > 0, "the strip drew nothing the test can recognise");
        CHECK(maxx <= limit + 0.5f,
              "the segmented strip reaches x=%.1f, the panel ends at %.1f - the "
              "last segment hangs over the panel edge", (double)maxx, (double)limit);

        // And the same, squeezed: a column narrow enough that the label and
        // three segments cannot both have their preferred width. This is the
        // width at which the old minimum bit, so it is the width the check has
        // to survive - the label column gives way, the strip does not grow.
        const float NW = 90.0f;
        mode = 1;
        dai_ui_begin(ui, 800, 600, &in);
        dai_ui_panel_begin(ui, PX, PY, NW, 200.0f, nullptr);
        dai_ui_seg_buttons(ui, "Sampling", &mode, MODES, 3);
        dai_ui_panel_end(ui);
        dai_ui_end(ui);
        nb = dai_ui_draws(ui, &draws);
        float narrow_max = 0.0f;
        for (uint32_t b = 0; b < nb; ++b) {
            for (uint32_t v = 0; v < draws[b].count; ++v) {
                const dai_ui_vertex &vx = draws[b].vertices[v];
                for (int m = 0; m < 5; ++m) {
                    if (vx.color != MINE[m]) continue;
                    if (vx.x > narrow_max) narrow_max = vx.x;
                    break;
                }
            }
        }
        const float narrow_limit = PX + NW - ss->padding;
        CHECK(narrow_max <= narrow_limit + 0.5f,
              "in a %.0f px panel the strip reaches x=%.1f, the panel ends at %.1f",
              (double)NW, (double)narrow_max, (double)narrow_limit);
    }

    // ---- "Element 0" in a narrow inspector column --------------------------
    //
    // The array row used to start its value box at a fixed 90 px column. At
    // 18 px "Element 0" is wider than that, so in the 200 px inspector of a
    // 1100x700 window the box was drawn over the last letters of the label -
    // the row read "Element" next to a material name. The field now begins
    // behind the MEASURED label, and where even that does not fit the label is
    // clipped rather than overdrawn. Both are geometry, so both are measured.
    {
        std::printf("array row: the element label and its field do not overlap\n");
        const dai_ui_style *ss = dai_ui_style_of(ui);
        const float PX = 40.0f, PY = 10.0f;

        // Every vertex of the label (the only text_dim pixels right of the
        // grip) and of the field fill (the only track coloured ones), plus the
        // clip the label was drawn under.
        struct RowGeom { float lbl_x0, lbl_x1, field_x0, clip_x1; int glyphs, fills; };
        auto row_at = [&](float pw) {
            in.mouse_x = -100.0f; in.mouse_y = -100.0f; in.mouse_down = 0;
            dai_ui_begin(ui, 800, 600, &in);
            dai_ui_panel_begin(ui, PX, PY, pw, 200.0f, nullptr);
            dai_ui_array_object_row(ui, 0, "Bricks", nullptr);
            dai_ui_panel_end(ui);
            dai_ui_end(ui);

            const float label_x = PX + ss->padding + 28.0f;
            RowGeom g{ 1e9f, -1e9f, 1e9f, -1e9f, 0, 0 };
            const dai_ui_draw *draws = nullptr;
            uint32_t nb = dai_ui_draws(ui, &draws);
            for (uint32_t b = 0; b < nb; ++b)
                for (uint32_t v = 0; v < draws[b].count; ++v) {
                    const dai_ui_vertex &vx = draws[b].vertices[v];
                    if (vx.color == ss->text_dim && vx.x >= label_x - 1.0f) {
                        if (vx.x < g.lbl_x0) g.lbl_x0 = vx.x;
                        if (vx.x > g.lbl_x1) g.lbl_x1 = vx.x;
                        if (draws[b].clip[2] > g.clip_x1) g.clip_x1 = draws[b].clip[2];
                        ++g.glyphs;
                    } else if (vx.color == ss->track) {
                        if (vx.x < g.field_x0) g.field_x0 = vx.x;
                        ++g.fills;
                    }
                }
            return g;
        };

        const float TW = dai_ui_text_width(ui, "Element 0");
        CHECK(TW > 40.0f, "the label measures %.1f px - the font did not load", (double)TW);

        // The inspector at 1100x700: 200 px of column, which is the width the
        // narrow screenshots are taken at.
        RowGeom g = row_at(200.0f);
        CHECK(g.glyphs > 0 && g.fills > 0,
              "the row drew no label (%d) or no field (%d)", g.glyphs, g.fills);
        CHECK(g.lbl_x1 <= g.field_x0 + 0.5f,
              "in a 200 px column the label runs to x=%.1f and the field starts at "
              "x=%.1f - the value box is drawn over 'Element 0'",
              (double)g.lbl_x1, (double)g.field_x0);
        CHECK(g.clip_x1 >= PX + ss->padding + 28.0f + TW - 0.5f,
              "the label is clipped at x=%.1f, 'Element 0' needs %.1f px and ends at "
              "x=%.1f", (double)g.clip_x1, (double)TW,
              (double)(PX + ss->padding + 28.0f + TW));

        // Wider than the label needs: the field must not creep left of the old
        // 90 px column just because the label is short.
        RowGeom wide = row_at(320.0f);
        CHECK(wide.field_x0 >= PX + ss->padding + 90.0f - 0.5f,
              "in a 320 px column the field starts at x=%.1f, before the %.1f px "
              "label column", (double)wide.field_x0,
              (double)(PX + ss->padding + 90.0f));
        CHECK(wide.lbl_x1 <= wide.field_x0 + 0.5f,
              "the label overlaps the field in a wide column too (%.1f > %.1f)",
              (double)wide.lbl_x1, (double)wide.field_x0);

        // Narrower than label plus field: the label gives way. What may NOT
        // happen is the two drawing on top of each other.
        RowGeom tight = row_at(130.0f);
        CHECK(tight.glyphs > 0 && tight.fills > 0,
              "the squeezed row drew no label (%d) or no field (%d)",
              tight.glyphs, tight.fills);
        CHECK(tight.lbl_x1 <= tight.field_x0 + 0.5f,
              "squeezed to 130 px the label reaches x=%.1f and the field starts at "
              "x=%.1f", (double)tight.lbl_x1, (double)tight.field_x0);
    }

    // ---- a vector row in the 200 px inspector of a 1100x700 window ---------
    //
    // "Position 1.025 / 0.05 / -12.5" is the row the narrow screenshots show,
    // and it showed it as one smear: the three boxes were divided out of the
    // full row width with the gaps taken off afterwards, and each number was
    // drawn at its full length from the left edge of its box - so "1.025" ran
    // over the frame of the Y field and into its green "Y". Three things are
    // measured here, all of them geometry: the boxes are pairwise disjoint,
    // every digit stays inside the box that owns it, and the name column does
    // not reach into the first box.
    {
        std::printf("vector row: three boxes, pairwise disjoint, digits inside\n");
        const dai_ui_style *ss = dai_ui_style_of(ui);
        const float PX = 40.0f, PY = 10.0f;

        struct Box { float x0, x1; int digits; };
        auto vec_row_at = [&](float pw, std::vector<Box> *boxes, float *label_x1,
                              float *digits_out, float *digits_in) {
            float v[3] = { 1.025f, 0.05f, -12.5f };
            in.mouse_x = -100.0f; in.mouse_y = -100.0f; in.mouse_down = 0;
            dai_ui_begin(ui, 1100, 700, &in);
            dai_ui_panel_begin(ui, PX, PY, pw, 300.0f, nullptr);
            dai_ui_num_vec3(ui, "Position", v, 0.01f);
            dai_ui_panel_end(ui);
            dai_ui_end(ui);

            // The boxes: the only track coloured fills in the panel. One fill
            // is one RUN of vertices in the buffer (two triangles of the same
            // colour), so a run is a rectangle - grouping by position instead
            // would happily merge two boxes that overlap, which is the one
            // thing this test is here to catch.
            *label_x1 = -1e9f;
            boxes->clear();
            const dai_ui_draw *draws = nullptr;
            uint32_t nb = dai_ui_draws(ui, &draws);
            bool in_run = false;
            for (uint32_t b = 0; b < nb; ++b)
                for (uint32_t vv = 0; vv < draws[b].count; ++vv) {
                    const dai_ui_vertex &vx = draws[b].vertices[vv];
                    if (vx.color == ss->track) {
                        if (!in_run) { boxes->push_back(Box{ vx.x, vx.x, 0 }); in_run = true; }
                        if (vx.x < boxes->back().x0) boxes->back().x0 = vx.x;
                        if (vx.x > boxes->back().x1) boxes->back().x1 = vx.x;
                        continue;
                    }
                    in_run = false;
                    if (vx.color == ss->text_dim && vx.x > *label_x1) *label_x1 = vx.x;
                }
            std::sort(boxes->begin(), boxes->end(),
                      [](const Box &a, const Box &b2) { return a.x0 < b2.x0; });
            // The digits: every glyph drawn in the text colour. Each one has
            // to sit in one of the boxes.
            *digits_out = 0.0f; *digits_in = 0.0f;
            for (uint32_t b = 0; b < nb; ++b)
                for (uint32_t vv = 0; vv < draws[b].count; ++vv) {
                    const dai_ui_vertex &vx = draws[b].vertices[vv];
                    if (vx.color != ss->text) continue;
                    int home = -1;
                    for (size_t k = 0; k < boxes->size(); ++k)
                        if (vx.x >= (*boxes)[k].x0 - 0.5f && vx.x <= (*boxes)[k].x1 + 0.5f)
                            home = (int)k;
                    if (home < 0) *digits_out += 1.0f;
                    else { *digits_in += 1.0f; (*boxes)[(size_t)home].digits++; }
                }
        };

        std::vector<Box> boxes;
        float label_x1 = 0.0f, out = 0.0f, insid = 0.0f;
        vec_row_at(200.0f, &boxes, &label_x1, &out, &insid);
        CHECK(boxes.size() == 3,
              "a vector row in a 200 px panel drew %d boxes, not 3 - two of them "
              "touch or overlap", (int)boxes.size());
        if (boxes.size() == 3) {
            for (int i = 0; i + 1 < 3; ++i)
                CHECK(boxes[(size_t)i].x1 + 1.0f <= boxes[(size_t)i + 1].x0,
                      "field %d ends at x=%.1f and field %d starts at x=%.1f - the "
                      "two rectangles are not disjoint", i, (double)boxes[(size_t)i].x1,
                      i + 1, (double)boxes[(size_t)i + 1].x0);
            for (int i = 0; i < 3; ++i) {
                CHECK(boxes[(size_t)i].x1 - boxes[(size_t)i].x0 >= 18.0f,
                      "field %d is %.1f px wide - no number fits in that", i,
                      (double)(boxes[(size_t)i].x1 - boxes[(size_t)i].x0));
                CHECK(boxes[(size_t)i].digits > 0,
                      "field %d drew no digits at all", i);
            }
            CHECK(label_x1 <= boxes[0].x0 + 0.5f,
                  "the name column reaches x=%.1f, the X field starts at x=%.1f - "
                  "'Position' is drawn into the first value box",
                  (double)label_x1, (double)boxes[0].x0);
        }
        CHECK(insid > 0.0f && out == 0.0f,
              "%d of %d digit quads are drawn outside every field rectangle - "
              "1.025 runs into its neighbour", (int)out, (int)(out + insid));

        // The same row with room to spare: the boxes must not stop being
        // disjoint just because they got bigger, and the name is written out
        // in full rather than shortened out of habit.
        vec_row_at(360.0f, &boxes, &label_x1, &out, &insid);
        CHECK(boxes.size() == 3, "a 360 px panel drew %d boxes, not 3", (int)boxes.size());
        CHECK(out == 0.0f, "%d digit quads outside their field in a 360 px panel", (int)out);
        if (boxes.size() == 3)
            CHECK(label_x1 <= boxes[0].x0 + 0.5f,
                  "wide panel: the name reaches x=%.1f, the X field starts at x=%.1f",
                  (double)label_x1, (double)boxes[0].x0);
    }

    // ---- the label column gives way, and says what it gave up --------------
    //
    // The column used to be a fixed fraction of the panel whatever stood in it:
    // at 200 px that is 64 px, so "Restitution" was a stump next to a value box
    // with room to spare. It measures its name now - and where the row cannot
    // afford the whole name the rest is one hover away rather than gone.
    {
        std::printf("label column: measured, shrinking, and never silent\n");
        const dai_ui_style *ss = dai_ui_style_of(ui);
        const char *LONG = "Restitution";
        float v = 0.35f;
        auto label_run = [&](float pw, float mx, float my, float *x0, float *x1,
                             float *field_x0) {
            in.mouse_x = mx; in.mouse_y = my; in.mouse_down = 0;
            dai_ui_begin(ui, 1100, 700, &in);
            dai_ui_panel_begin(ui, 0.0f, 0.0f, pw, 200.0f, nullptr);
            dai_ui_num_field(ui, LONG, &v, 0.01f, 0.0f, 0.0f, "rest");
            dai_ui_panel_end(ui);
            dai_ui_end(ui);
            *x0 = 1e9f; *x1 = -1e9f; *field_x0 = 1e9f;
            const dai_ui_draw *draws = nullptr;
            uint32_t nb = dai_ui_draws(ui, &draws);
            for (uint32_t b = 0; b < nb; ++b)
                for (uint32_t vv = 0; vv < draws[b].count; ++vv) {
                    const dai_ui_vertex &vx = draws[b].vertices[vv];
                    if (vx.color == ss->text_dim) {
                        if (vx.x < *x0) *x0 = vx.x;
                        if (vx.x > *x1) *x1 = vx.x;
                    } else if (vx.color == ss->track && vx.x < *field_x0) *field_x0 = vx.x;
                }
        };

        float x0 = 0, x1 = 0, fx = 0;
        label_run(320.0f, -100.0f, -100.0f, &x0, &x1, &fx);
        CHECK(x1 <= fx + 0.5f, "in a 320 px panel '%s' runs to x=%.1f and the box "
              "starts at x=%.1f", LONG, (double)x1, (double)fx);
        CHECK(x1 - x0 >= dai_ui_text_width(ui, LONG) - 4.0f,
              "'%s' needs %.1f px and got %.1f in a panel with room to spare - the "
              "column did not measure its name", LONG,
              (double)dai_ui_text_width(ui, LONG), (double)(x1 - x0));

        // 150 px: the name no longer fits next to a usable box, so it is
        // shortened - and the row hands the whole of it back on hover.
        label_run(150.0f, -100.0f, -100.0f, &x0, &x1, &fx);
        CHECK(x1 <= fx + 0.5f, "in a 150 px panel the name reaches x=%.1f and the box "
              "starts at x=%.1f", (double)x1, (double)fx);
        CHECK(fx <= 150.0f - 40.0f,
              "the value box starts at x=%.1f in a 150 px panel - less than 40 px of "
              "field is left for the number", (double)fx);
        label_run(150.0f, 10.0f, 10.0f, &x0, &x1, &fx);
        CHECK(std::strcmp(dai_ui_tooltip_text(ui), LONG) == 0,
              "hovering the shortened '%s' says '%s'", LONG, dai_ui_tooltip_text(ui));
        label_run(320.0f, 10.0f, 10.0f, &x0, &x1, &fx);
        CHECK(std::strcmp(dai_ui_tooltip_text(ui), LONG) != 0,
              "a name that fits still raises a tooltip repeating it");
    }

    // ---- a name too long for its column is ENDED, not chopped --------------
    // dai_ui_fit_text is the one rule every panel shortens with, so it is
    // measured once here rather than in each of them.
    {
        std::printf("fit_text: whole characters, ellipsis, never wider than asked\n");
        char buf[64];
        const char *full = "DoorSocket.Front";
        const float wide = dai_ui_text_width(ui, full) + 10.0f;
        CHECK(std::strcmp(dai_ui_fit_text(ui, full, wide, buf, sizeof(buf)), full) == 0,
              "a name that fits was shortened anyway: '%s'", buf);
        const float narrow = dai_ui_text_width(ui, "DoorSock") + 2.0f;
        const char *cut = dai_ui_fit_text(ui, full, narrow, buf, sizeof(buf));
        size_t cl = std::strlen(cut);
        CHECK(cl >= 4 && std::strcmp(cut + cl - 3, "...") == 0,
              "'%s' does not end in an ellipsis", cut);
        CHECK(dai_ui_text_width(ui, cut) <= narrow + 0.5f,
              "'%s' is %.1f px wide, %.1f px were free", cut,
              (double)dai_ui_text_width(ui, cut), (double)narrow);
        CHECK(std::strncmp(cut, full, cl - 3) == 0,
              "'%s' is not a prefix of '%s'", cut, full);
    }

    dai_ui_destroy(ui);
    dai_font_free(font);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
