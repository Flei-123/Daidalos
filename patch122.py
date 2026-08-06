import io

p = 'src/dai_editor_ui.cpp'
s = io.open(p, encoding='utf-8').read()

# ===========================================================================
# 1) Textbox + Autosize im Inspector.
# ===========================================================================
old = """            float off[2] = { r.text_x, r.text_y };"""
new = """            // The box. 0 wide means "as wide as the words", which is what a
            // score wants; a real width is what a subtitle wants.
            float bw2 = r.text_w, bh2 = r.text_h;
            if (dai_ui_num_field(p->ui, "Box W", &bw2, 1.0f, 0.0f, 8000.0f, "textbw")) r.text_w = bw2;
            if (dai_ui_num_field(p->ui, "Box H", &bh2, 1.0f, 0.0f, 8000.0f, "textbh")) r.text_h = bh2;
            r.text_w = bw2; r.text_h = bh2;
            dai_ui_help(p->ui, "0 = no box: one line, as wide as the words. With a width "
                               "the text wraps; with a height, autosize can shrink it.");
            int fit = r.text_autosize;
            if (dai_ui_checkbox(p->ui, "Autosize", &fit)) r.text_autosize = fit;
            dai_ui_help(p->ui, "Shrink the text until it fits the box. Needs a box height. "
                               "This is what saves a layout when a translation is a third "
                               "longer than the language it was drawn for.");
            float off[2] = { r.text_x, r.text_y };"""
assert s.count(old) == 1, 'text offset fields not found'
s = s.replace(old, new)

# ===========================================================================
# 2) Die Image-Komponente im Inspector - und die alte Sprite-Attrappe raus.
# ===========================================================================
old = """    // ---- Text ---------------------------------------------------------------"""
new = """    // ---- Image (UI) ----------------------------------------------------------
    // The screen space picture: a panel, a heart, a crosshair. This is what
    // the old Sprite checkbox was meant to be - that one set a flag NOTHING
    // read, so it drew nothing, ever.
    if (r.image_on) {
        int on = 1;
        {
            int hrc = dai_ui_header_icon_col(p->ui, DAI_ICON_C_SPRITE, rgba(0x6F, 0xC7, 0xEA, 255),
                                             "Image (UI)", &p->fold_image, &on);
            if (hrc == 2 && !on) r.image_on = 0;
            else if (hrc == 3) {
                p->comp_menu_target = 8;
                float cmx = 0, cmy = 0;
                dai_ui_mouse(p->ui, &cmx, &cmy, nullptr, nullptr);
                dai_ui_popup_open(&p->menu_comp, cmx, cmy);
            }
        }
        if (p->fold_image && r.image_on) {
            std::string idisp = r.image[0] ? base_of(r.image) : std::string("None (Texture)");
            int irc = dai_ui_object_field(p->ui, "Image", idisp.c_str(), DAI_ICON_IMAGE);
            if (irc == 2) {
                float imx = 0, imy = 0;
                dai_ui_mouse(p->ui, &imx, &imy, nullptr, nullptr);
                dai_ui_searchlist_open(&p->image_list, imx - 210.0f, imy);
                p->image_list.wants_focus = 1;
                std::snprintf(p->image_list.hint, sizeof(p->image_list.hint), "Search textures...");
                p->image_pick_node = n;
            }
            float iw = r.image_w, ih = r.image_h;
            if (dai_ui_num_field(p->ui, "Width", &iw, 1.0f, 0.0f, 8000.0f, "imgw")) r.image_w = iw;
            if (dai_ui_num_field(p->ui, "Height", &ih, 1.0f, 0.0f, 8000.0f, "imgh")) r.image_h = ih;
            r.image_w = iw; r.image_h = ih;
            dai_ui_help(p->ui, "0 = the file's own size in pixels.");
            {
                float ic[3] = { r.image_color.x, r.image_color.y, r.image_color.z };
                if (ic[0] == 0.0f && ic[1] == 0.0f && ic[2] == 0.0f) { ic[0] = ic[1] = ic[2] = 1.0f; }
                if (dai_ui_color(p->ui, "Tint", ic, "imgcol"))
                    r.image_color = dai_vec3{ ic[0], ic[1], ic[2] };
            }
            static const char *const ANCHOR2[] = {
                "Top left", "Top", "Top right", "Left", "Centre", "Right",
                "Bottom left", "Bottom", "Bottom right",
            };
            dai_ui_option(p->ui, "Anchor", &r.image_anchor, ANCHOR2, 9);
            float ioff[2] = { r.image_x, r.image_y };
            if (dai_ui_num_field(p->ui, "Offset X", &ioff[0], 1.0f, -8000.0f, 8000.0f, "imgox"))
                r.image_x = ioff[0];
            if (dai_ui_num_field(p->ui, "Offset Y", &ioff[1], 1.0f, -8000.0f, 8000.0f, "imgoy"))
                r.image_y = ioff[1];
            r.image_x = ioff[0]; r.image_y = ioff[1];
        }
    }

    // ---- Text ---------------------------------------------------------------"""
assert s.count(old) == 1, 'text section anchor not found'
s = s.replace(old, new)

old = """    int  fold_freeze = 1;"""
new = """    int  fold_freeze = 1;
    int  fold_image = 1;
    dai_ui_searchlist image_list{};
    dai_node          image_pick_node = DAI_INVALID_NODE;"""
assert s.count(old) == 1, 'fold_freeze not found'
s = s.replace(old, new)

# Der Textur-Picker.
old = """    // The font picker: every .ttf/.otf in the project, plus "Default"."""
new = """    // The texture picker for an Image component.
    if (p->image_list.open && p->image_pick_node != DAI_INVALID_NODE) {
        std::vector<std::string> imgs;
        imgs.push_back("None");
        for (const char *a : p->assets) {
            if (!a) continue;
            std::string f = a;
            size_t dot = f.find_last_of('.');
            if (dot == std::string::npos) continue;
            std::string e = f.substr(dot + 1);
            for (char &c : e) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (e == "png" || e == "jpg" || e == "jpeg" || e == "tga") imgs.push_back(f);
        }
        std::vector<dai_ui_menu_item> items(imgs.size());
        for (size_t i = 0; i < imgs.size(); ++i)
            items[i] = { i == 0 ? nullptr : DAI_ICON_IMAGE, imgs[i].c_str(), nullptr, 0 };
        int pick = dai_ui_searchlist_draw(p->ui, &p->image_list, items.data(), (uint32_t)items.size());
        if (pick >= 0 && pick < (int)imgs.size()) {
            dai_node_desc ir{};
            if (dai_doc_get(d, p->image_pick_node, &ir) == DAI_OK) {
                dai_doc_begin(d, "Image");
                if (pick == 0) ir.image[0] = 0;
                else std::snprintf(ir.image, sizeof(ir.image), "%s", imgs[(size_t)pick].c_str());
                dai_doc_set(d, p->image_pick_node, &ir);
                dai_doc_commit(d);
            }
            p->image_pick_node = DAI_INVALID_NODE;
        }
    }

    // The font picker: every .ttf/.otf in the project, plus "Default"."""
assert s.count(old) == 1, 'font picker anchor not found'
s = s.replace(old, new)

# Add Component: Image rein, Sprite raus.
old = """            if (!ar2.sprite)      entries.push_back({ "Sprite (2D)", "Rendering", 4, "" });
            if (!ar2.text_on)     entries.push_back({ "Text (UI)", "Rendering", 7, "" });"""
new = """            // No "Sprite" entry any more. It set a flag nothing read - a
            // component that cannot be seen after it is added is worse than
            // one that is missing, because the second one you go and look
            // for. Image (UI) is what it was trying to be.
            if (!ar2.text_on)     entries.push_back({ "Text (UI)", "Rendering", 7, "" });
            if (!ar2.image_on)    entries.push_back({ "Image (UI)", "Rendering", 8, "" });"""
assert s.count(old) == 1, 'addcomp sprite entry not found'
s = s.replace(old, new)

old = """                case 4: ar2.sprite = 1; break;"""
new = """                case 8: ar2.image_on = 1;
                        if (ar2.image_w <= 0.0f) { ar2.image_w = 128.0f; ar2.image_h = 128.0f; }
                        break;"""
assert s.count(old) == 1, 'addcomp sprite case not found'
s = s.replace(old, new)

old = """                               : e.kind == 7 ? DAI_ICON_C_TEXT : DAI_ICON_SPRITE;"""
new = """                               : e.kind == 7 ? DAI_ICON_C_TEXT
                               : e.kind == 8 ? DAI_ICON_IMAGE : DAI_ICON_SPRITE;"""
assert s.count(old) == 1, 'addcomp icon not found'
s = s.replace(old, new)

old = """                    else if (target == 7) ar.text_on = 0;"""
new = """                    else if (target == 7) ar.text_on = 0;
                    else if (target == 8) ar.image_on = 0;"""
assert s.count(old) == 1, 'comp menu remove not found'
s = s.replace(old, new)

io.open(p, 'w', encoding='utf-8').write(s)
print('inspector: text box, Image (UI), sprite retired')
