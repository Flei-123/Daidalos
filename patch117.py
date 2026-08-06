import io

p = 'src/dai_editor_ui.cpp'
s = io.open(p, encoding='utf-8').read()

# ===========================================================================
# Ueberall, wo eine FARBE steht, steht jetzt auch eine.
# ===========================================================================
old = """        changed |= dai_ui_num_vec3(p->ui, "Color", p->inspect_mat.color, 0.01f);"""
new = """        changed |= dai_ui_color(p->ui, "Color", p->inspect_mat.color, "matcol");"""
assert s.count(old) == 1, 'material colour not found'
s = s.replace(old, new)

old = """            dai_ui_num_vec3(p->ui, "Colour", &r.light_color.x, 0.004f);"""
new = """            // A light with no colour chosen is white, and the picker has to
            // show white rather than black - 0,0,0 means "unset" everywhere
            // in this document, and a swatch that says black about a white
            // light is a swatch that lies.
            {
                float lc[3] = { r.light_color.x, r.light_color.y, r.light_color.z };
                if (lc[0] == 0.0f && lc[1] == 0.0f && lc[2] == 0.0f) { lc[0] = lc[1] = lc[2] = 1.0f; }
                if (dai_ui_color(p->ui, "Colour", lc, "lightcol"))
                    r.light_color = dai_vec3{ lc[0], lc[1], lc[2] };
            }"""
assert s.count(old) == 1, 'light colour not found'
s = s.replace(old, new)

old = """            dai_ui_num_vec3(p->ui, "Color", &r.text_color.x, 0.01f);"""
new = """            {
                float tc[3] = { r.text_color.x, r.text_color.y, r.text_color.z };
                if (tc[0] == 0.0f && tc[1] == 0.0f && tc[2] == 0.0f) { tc[0] = tc[1] = tc[2] = 1.0f; }
                if (dai_ui_color(p->ui, "Color", tc, "textcol"))
                    r.text_color = dai_vec3{ tc[0], tc[1], tc[2] };
            }"""
assert s.count(old) == 1, 'text colour not found'
s = s.replace(old, new)

# ===========================================================================
# Die Schrift der Text-Komponente: eine .ttf aus dem Projekt.
# ===========================================================================
old = """            if (r.text_size <= 0.0f) r.text_size = 24.0f;
            dai_ui_num_field(p->ui, "Size", &r.text_size, 0.5f, 4.0f, 400.0f, "textsize");"""
new = """            if (r.text_size <= 0.0f) r.text_size = 24.0f;
            dai_ui_num_field(p->ui, "Size", &r.text_size, 0.5f, 4.0f, 400.0f, "textsize");
            // The typeface. A .ttf in the project, or empty for the one the
            // editor itself uses - a label has to draw before anyone has
            // gone looking for a font, or the component looks broken on the
            // day it is added.
            {
                std::string fdisp = r.text_font[0] ? base_of(r.text_font)
                                                   : std::string("Default (system UI)");
                int frc = dai_ui_object_field(p->ui, "Font", fdisp.c_str(), DAI_ICON_C_TEXT);
                if (frc == 2) {
                    float fmx = 0, fmy = 0;
                    dai_ui_mouse(p->ui, &fmx, &fmy, nullptr, nullptr);
                    dai_ui_searchlist_open(&p->font_list, fmx - 210.0f, fmy);
                    p->font_list.wants_focus = 1;
                    std::snprintf(p->font_list.hint, sizeof(p->font_list.hint), "Search fonts...");
                    p->font_pick_node = n;
                }
            }"""
assert s.count(old) == 1, 'text size field not found'
s = s.replace(old, new)

old = """    dai_ui_searchlist obj_list{};"""
new = """    dai_ui_searchlist obj_list{};
    dai_ui_searchlist font_list{};
    dai_node          font_pick_node = DAI_INVALID_NODE;"""
assert s.count(old) == 1, 'obj_list member not found'
s = s.replace(old, new)

# Der Picker selbst, neben dem Objekt-Picker.
old = """    // The Add Component list. Entries flip between add and remove so one menu"""
new = """    // The font picker: every .ttf/.otf in the project, plus "Default".
    if (p->font_list.open && p->font_pick_node != DAI_INVALID_NODE) {
        std::vector<std::string> fonts;
        fonts.push_back("Default (system UI)");
        for (const char *a : p->assets) {
            if (!a) continue;
            std::string f = a;
            size_t dot = f.find_last_of('.');
            if (dot == std::string::npos) continue;
            std::string e = f.substr(dot + 1);
            for (char &c : e) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (e == "ttf" || e == "otf" || e == "ttc") fonts.push_back(f);
        }
        std::vector<dai_ui_menu_item> items(fonts.size());
        for (size_t i = 0; i < fonts.size(); ++i)
            items[i] = { i == 0 ? nullptr : DAI_ICON_C_TEXT, fonts[i].c_str(), nullptr, 0 };
        int pick = dai_ui_searchlist_draw(p->ui, &p->font_list, items.data(), (uint32_t)items.size());
        if (pick >= 0 && pick < (int)fonts.size()) {
            dai_node_desc fr{};
            if (dai_doc_get(d, p->font_pick_node, &fr) == DAI_OK) {
                dai_doc_begin(d, "Font");
                if (pick == 0) fr.text_font[0] = 0;
                else std::snprintf(fr.text_font, sizeof(fr.text_font), "%s", fonts[(size_t)pick].c_str());
                dai_doc_set(d, p->font_pick_node, &fr);
                dai_doc_commit(d);
            }
            p->font_pick_node = DAI_INVALID_NODE;
        }
    }

    // The Add Component list. Entries flip between add and remove so one menu"""
assert s.count(old) == 1, 'addcomp anchor not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)
print('colour picker in use, font field added')

# ===========================================================================
# Das Feld im Dokument.
# ===========================================================================
p = 'include/dai_doc.h'
s = io.open(p, encoding='utf-8').read()
old = """    float    text_x, text_y;    /* pixels from the anchor, + is right and down  */"""
new = """    float    text_x, text_y;    /* pixels from the anchor, + is right and down  */
    /* The typeface, as a project path to a .ttf. Empty means the editor's own
     * UI font - a label has to draw on the day it is added, before anyone has
     * gone looking for a font file. */
    char     text_font[96];"""
assert s.count(old) == 1, 'text_x field not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)

p = 'src/dai_doc_text.cpp'
s = io.open(p, encoding='utf-8').read()
old = """        if (r.text_anchor != def.text_anchor) put(s, "  textanchor %d\\n", r.text_anchor);"""
new = """        if (r.text_anchor != def.text_anchor) put(s, "  textanchor %d\\n", r.text_anchor);
        if (r.text_font[0])                   put(s, "  textfont %s\\n", r.text_font);"""
assert s.count(old) == 1, 'textanchor writer not found'
s = s.replace(old, new)
old = """        else if (key == "textanchor") { ok = parse_i32(after, &rec.text_anchor); }"""
new = """        else if (key == "textanchor") { ok = parse_i32(after, &rec.text_anchor); }
        else if (key == "textfont") {
            std::string v = after;
            while (!v.empty() && (v.front() == ' ' || v.front() == '\\t')) v.erase(0, 1);
            while (!v.empty() && (v.back() == ' ' || v.back() == '\\t' || v.back() == '\\r')) v.pop_back();
            if (v.size() >= sizeof(rec.text_font)) ok = false;
            else std::snprintf(rec.text_font, sizeof(rec.text_font), "%s", v.c_str()); }"""
assert s.count(old) == 1, 'textanchor reader not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)
print('text_font in the document format')
