import io

p = 'src/dai_editor_ui.cpp'
s = io.open(p, encoding='utf-8').read()

# --- Faltzustand -----------------------------------------------------------
old = """    int  fold_camera = 1, fold_light = 1, fold_sprite = 1, fold_audio = 1;"""
new = """    int  fold_camera = 1, fold_light = 1, fold_sprite = 1, fold_audio = 1;
    int  fold_text = 1;
    // The project's active language, so the inspector previews a "@key" as
    // the words a player would see. Owned by the host - it knows which files
    // exist - and pushed in with dai_editor_ui_strings().
    const char *(*tr_fn)(const char *text, void *user) = nullptr;
    void *tr_user = nullptr;"""
assert s.count(old) == 1, 'fold fields not found'
s = s.replace(old, new)

# --- Inspector-Abschnitt, direkt vor Audio Source --------------------------
old = """    // ---- Audio Source -------------------------------------------------------
    if (r.audio_event[0] || r.audio_autoplay || r.audio_bus) {"""
new = """    // ---- Text ---------------------------------------------------------------
    // The game's own UI. Drawn on the picture, not in the world: a score
    // belongs to a corner of the screen, and a corner is where it has to stay
    // when the window is a different size.
    if (r.text_on) {
        int on = 1;
        {
            int hrc = dai_ui_header_icon_col(p->ui, DAI_ICON_C_TEXT, rgba(0x9C, 0xD6, 0x8C, 255),
                                             "Text", &p->fold_text, &on);
            if (hrc == 2 && !on) r.text_on = 0;
            else if (hrc == 3) {
                p->comp_menu_target = 7;
                float cmx = 0, cmy = 0;
                dai_ui_mouse(p->ui, &cmx, &cmy, nullptr, nullptr);
                dai_ui_popup_open(&p->menu_comp, cmx, cmy);
            }
        }
        if (p->fold_text && r.text_on) {
            char tbuf[192];
            std::snprintf(tbuf, sizeof(tbuf), "%s", r.text);
            if (dai_ui_input_text(p->ui, "Text", tbuf, sizeof(tbuf)))
                std::snprintf(r.text, sizeof(r.text), "%s", tbuf);
            dai_ui_help(p->ui, "The words, or \\"@key\\" to look them up in the "
                               "project's string table");
            // What a PLAYER would see. A key on its own tells you the label
            // is localised; it does not tell you whether the table has it.
            if (r.text[0] == '@') {
                const char *shown = p->tr_fn ? p->tr_fn(r.text, p->tr_user) : nullptr;
                char line[240];
                if (shown && shown[0] && std::strcmp(shown, r.text + 1) != 0)
                    std::snprintf(line, sizeof(line), "shows: %s", shown);
                else
                    std::snprintf(line, sizeof(line), "no entry for %s - it will show the key",
                                  r.text + 1);
                dai_ui_label(p->ui, line);
            }
            if (r.text_size <= 0.0f) r.text_size = 24.0f;
            dai_ui_num_field(p->ui, "Size", &r.text_size, 0.5f, 4.0f, 400.0f, "textsize");
            dai_ui_num_vec3(p->ui, "Color", &r.text_color.x, 0.01f);
            static const char *const ANCHOR[] = {
                "Top left", "Top", "Top right",
                "Left", "Centre", "Right",
                "Bottom left", "Bottom", "Bottom right",
            };
            dai_ui_option(p->ui, "Anchor", &r.text_anchor, ANCHOR, 9);
            float off[2] = { r.text_x, r.text_y };
            if (dai_ui_num_field(p->ui, "Offset X", &off[0], 1.0f, -8000.0f, 8000.0f, "textox"))
                r.text_x = off[0];
            if (dai_ui_num_field(p->ui, "Offset Y", &off[1], 1.0f, -8000.0f, 8000.0f, "textoy"))
                r.text_y = off[1];
            r.text_x = off[0]; r.text_y = off[1];
        }
    }

    // ---- Audio Source -------------------------------------------------------
    if (r.audio_event[0] || r.audio_autoplay || r.audio_bus) {"""
assert s.count(old) == 1, 'audio source section not found'
s = s.replace(old, new)

# --- Add Component ---------------------------------------------------------
old = """            if (!ar2.sprite)      entries.push_back({ "Sprite (2D)", "Rendering", 4, "" });"""
new = """            if (!ar2.sprite)      entries.push_back({ "Sprite (2D)", "Rendering", 4, "" });
            if (!ar2.text_on)     entries.push_back({ "Text (UI)", "Rendering", 7, "" });"""
assert s.count(old) == 1, 'sprite entry not found'
s = s.replace(old, new)

old = """                case 4: ar2.sprite = 1; break;"""
new = """                case 4: ar2.sprite = 1; break;
                case 7: ar2.text_on = 1;
                        // A label with nothing in it is invisible, and an
                        // invisible component reads as one that did not get
                        // added. It says its own name until told otherwise.
                        if (!ar2.text[0]) std::snprintf(ar2.text, sizeof(ar2.text), "Text");
                        if (ar2.text_size <= 0.0f) ar2.text_size = 24.0f;
                        break;"""
assert s.count(old) == 1, 'sprite add case not found'
s = s.replace(old, new)

old = """                            ic = e.kind == 2 ? DAI_ICON_CAMERA
                               : e.kind == 3 ? DAI_ICON_LIGHT : DAI_ICON_SPRITE;"""
new = """                            ic = e.kind == 2 ? DAI_ICON_CAMERA
                               : e.kind == 3 ? DAI_ICON_LIGHT
                               : e.kind == 7 ? DAI_ICON_C_TEXT : DAI_ICON_SPRITE;"""
assert s.count(old) == 1, 'add comp icon not found'
s = s.replace(old, new)

# --- Kopieren/Einfuegen einer Komponente -----------------------------------
old = """    case 6: std::snprintf(b, sizeof(b), "comp:6 event=%s bus=%d vol=%g loop=%d autoplay=%d",
                          r.audio_event, r.audio_bus, (double)r.audio_volume, r.audio_loop, r.audio_autoplay); break;"""
new = """    case 6: std::snprintf(b, sizeof(b), "comp:6 event=%s bus=%d vol=%g loop=%d autoplay=%d",
                          r.audio_event, r.audio_bus, (double)r.audio_volume, r.audio_loop, r.audio_autoplay); break;
    case 7: std::snprintf(b, sizeof(b), "comp:7 size=%g tr=%g tg=%g tb=%g anchor=%d tx=%g ty=%g str=%s",
                          (double)r.text_size, (double)r.text_color.x, (double)r.text_color.y,
                          (double)r.text_color.z, r.text_anchor, (double)r.text_x, (double)r.text_y,
                          r.text); break;"""
assert s.count(old) == 1, 'comp copy case 6 not found'
s = s.replace(old, new)

io.open(p, 'w', encoding='utf-8').write(s)
print('dai_editor_ui.cpp: Text component in the inspector and in Add Component')
