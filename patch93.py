import io

p = 'tests/test_ui_field.cpp'
s = io.open(p, encoding='utf-8').read()

anchor = """    // ---- the keyboard is given back when the field stops being drawn -----"""
assert s.count(anchor) == 1, 'anchor not found'

block = """    // ---- the search box keeps the keyboard while the list filters --------
    // Opened near the bottom edge, the panel used to be clamped against its
    // CURRENT height: every character removed rows, the panel got shorter,
    // the clamp let it slide back down - and a text field is identified by
    // where it is, so the search box became a DIFFERENT field between two
    // keystrokes. The first letters arrived, the rest went nowhere. That is
    // what "the material search does not work" looked like from outside.
    std::printf("a search box near the bottom edge keeps its caret\\n");
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
              "typing \\"%s\\" into the search box gave \\"%s\\" - it lost focus mid word",
              word, sl.query);
    }

"""

s = s.replace(anchor, block + anchor)
io.open(p, 'w', encoding='utf-8').write(s)
print('searchlist test added')
