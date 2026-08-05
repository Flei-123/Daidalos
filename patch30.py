#!/usr/bin/env python3
# patch30 - Unity's array widget, a proper inspector header, prefabs that
# actually expand, and the last of the Jolt selection.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))

def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p30'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# ============================================================== dai_ui.h
s = rd('include/dai_ui.h')
s = sub1(s,
"DAI_API void dai_ui_scroll_begin(dai_ui *ui, const char *id, float height);",
"""/* ---- arrays -------------------------------------------------------------
 *
 * Unity's array, which is a shape and not a widget: a foldout carrying the
 * element count on the right, one indented row per element with a drag
 * handle, and the +/- pair under the list. Written once here because the
 * inspector has more than one array in it and drawing that layout by hand
 * twice is how two lists start looking different.
 *
 *     if (dai_ui_array_begin(ui, "Materials", &n, &open, 1, 8)) {
 *         for (int i = 0; i < n; ++i)
 *             if (dai_ui_array_object_row(ui, i, names[i], DAI_ICON_MATERIAL))
 *                 open_picker(i);
 *         n += dai_ui_array_end(ui, &n, 1, 8);
 *     }
 */
DAI_API int  dai_ui_array_begin(dai_ui *ui, const char *label, int *count,
                                int *open, int min_n, int max_n);
/* One element: handle, "Element n", and an object field. 1 when clicked. */
DAI_API int  dai_ui_array_object_row(dai_ui *ui, int index, const char *value,
                                     const char *icon);
/* The +/- pair. Returns +1, -1 or 0; the caller owns the list. */
DAI_API int  dai_ui_array_end(dai_ui *ui, int count, int min_n, int max_n);

DAI_API void dai_ui_scroll_begin(dai_ui *ui, const char *id, float height);""",
'ui.h array')
wr('include/dai_ui.h', s)

# ============================================================ dai_ui.cpp
s = rd('src/dai_ui.cpp')
MARKER = '} // extern "C"'
idx = s.rfind(MARKER)
assert idx > 0
ARRAY_CODE = """// ------------------------------------------------------------------ arrays

int dai_ui_array_begin(dai_ui *ui, const char *label, int *count,
                       int *open, int min_n, int max_n) {
    if (!ui || !count) return 0;
    float h = 20.0f;
    float x = ui->in_panel ? ui->panel_x + ui->style.padding : 0.0f;
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    float y = ui->cursor_y;
    dai_ui_advance(ui, 0, h + 1.0f);

    bool over_head = inside_chk(ui, x, y, w - 56.0f, h);
    bool pressed = ui->input.mouse_down && !ui->prev.mouse_down;
    if (over_head) { ui->mouse_over_ui = true; ui->cursor_want = DAI_CURSOR_HAND; }
    int is_open = open ? (*open ? 1 : 0) : 1;
    if (over_head && pressed && open) { *open = !*open; is_open = *open ? 1 : 0; }

    // The foldout, then the name. No background: Unity's array header is part
    // of the component it lives in, not a box inside it.
    const char *chev = is_open ? DAI_ICON_CHEVRON_D : DAI_ICON_CHEVRON_R;
    float isz = 12.0f;
    if (dai_ui_has_icon(ui, chev))
        dai_ui_icon_at(ui, chev, x + 2.0f, y + (h - isz) * 0.5f, isz,
                       over_head ? ui->style.text : ui->style.text_dim);
    float lh = dai_font_line_height(ui->font);
    dai_ui_text(ui, x + 18.0f, y + (h - lh) * 0.5f, label ? label : "Array",
                ui->style.text);

    // The size, on the right, typeable - that is where Unity puts it and
    // typing 4 into it is how you make four of something.
    float bw = 48.0f;
    float fv = (float)*count;
    if (num_field_at(ui, x + w - bw, y + 1.0f, bw, h - 2.0f, &fv, 1.0f,
                     (float)min_n, (float)max_n, "arrsize", false, 0, nullptr)) {
        int nv = (int)(fv + 0.5f);
        if (nv < min_n) nv = min_n;
        if (nv > max_n) nv = max_n;
        *count = nv;
    }
    return is_open;
}

int dai_ui_array_object_row(dai_ui *ui, int index, const char *value,
                            const char *icon) {
    if (!ui) return 0;
    float h = widget_height(ui);
    float x = ui->in_panel ? ui->panel_x + ui->style.padding : 0.0f;
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    float y = ui->cursor_y;
    dai_ui_advance(ui, 0, h + 1.0f);

    // The handle. It does not drag yet; it is what tells you the row belongs
    // to a list rather than being another field that happens to be numbered.
    float hx = x + 12.0f, hy = y + h * 0.5f - 3.0f;
    for (int k = 0; k < 3; ++k)
        dai_ui_rect(ui, hx, hy + (float)k * 3.0f, 9.0f, 1.0f, ui->style.text_dim);

    char lbl[32];
    std::snprintf(lbl, sizeof(lbl), "Element %d", index);
    float lh = dai_font_line_height(ui->font);
    dai_ui_text(ui, x + 28.0f, y + (h - lh) * 0.5f, lbl, ui->style.text_dim);

    float fx = x + (ui->style.label_w > 90.0f ? ui->style.label_w : 90.0f);
    float fw = x + w - fx;
    if (fw < 60.0f) { fx = x + w * 0.45f; fw = w * 0.55f; }
    float bw = h;
    float vw = fw - bw - 2.0f;
    if (vw < 20.0f) { vw = fw; bw = 0.0f; }

    bool over_f = inside_chk(ui, fx, y, vw, h);
    bool over_b = bw > 0.0f && inside_chk(ui, fx + vw + 2.0f, y, bw, h);
    bool pressed = ui->input.mouse_down && !ui->prev.mouse_down;
    if (over_f || over_b) { ui->mouse_over_ui = true; ui->cursor_want = DAI_CURSOR_HAND; }

    dai_ui_rrect(ui, fx, y + 1.0f, vw, h - 2.0f, ui->style.rounding,
                 over_f ? ui->style.button_hover : ui->style.track);
    dai_ui_rect_outline(ui, fx, y + 1.0f, vw, h - 2.0f, 1.0f, ui->style.panel_border);
    float isz = dai_icons_size(ui->icons);
    if (isz <= 0.0f || isz > h - 4.0f) isz = h - 6.0f;
    float tx = fx + 5.0f;
    if (icon && dai_ui_has_icon(ui, icon)) {
        dai_ui_icon_at(ui, icon, tx, y + (h - isz) * 0.5f, isz, ui->style.accent);
        tx += isz + 4.0f;
    }
    dai_ui_text(ui, tx, y + (h - lh) * 0.5f, value ? value : "None", ui->style.text);
    if (bw > 0.0f) {
        float bx = fx + vw + 2.0f;
        dai_ui_rrect(ui, bx, y + 1.0f, bw, h - 2.0f, ui->style.rounding,
                     over_b ? ui->style.button_hover : ui->style.button);
        if (dai_ui_has_icon(ui, "target"))
            dai_ui_icon_at(ui, "target", bx + (bw - isz) * 0.5f, y + (h - isz) * 0.5f,
                           isz, ui->style.text);
    }
    return (over_f || over_b) && pressed ? 1 : 0;
}

int dai_ui_array_end(dai_ui *ui, int count, int min_n, int max_n) {
    if (!ui) return 0;
    float h = 18.0f;
    float x = ui->in_panel ? ui->panel_x + ui->style.padding : 0.0f;
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    float y = ui->cursor_y;
    dai_ui_advance(ui, 0, h + 3.0f);

    float bw = 22.0f;
    float px = x + w - bw * 2.0f - 1.0f, mx2 = x + w - bw;
    int add = dai_ui_icon_button_at(ui, DAI_ICON_PLUS, px, y, bw, h, 0);
    // A minus glyph the set does not have: two pixels of line, exactly where
    // the plus has its horizontal bar, so the pair reads as a pair.
    bool over_m = inside_chk(ui, mx2, y, bw, h);
    bool pressed = ui->input.mouse_down && !ui->prev.mouse_down;
    if (over_m) { ui->mouse_over_ui = true; ui->cursor_want = DAI_CURSOR_HAND; }
    dai_ui_rrect(ui, mx2, y, bw, h, ui->style.rounding,
                 over_m ? ui->style.button_hover : ui->style.button);
    dai_ui_rect(ui, mx2 + bw * 0.5f - 4.0f, y + h * 0.5f - 1.0f, 8.0f, 2.0f,
                count > min_n ? ui->style.text : ui->style.text_dim);
    int sub = (over_m && pressed) ? 1 : 0;

    if (add && count < max_n) return +1;
    if (sub && count > min_n) return -1;
    return 0;
}

"""
s = s[:idx] + ARRAY_CODE + s[idx:]
wr('src/dai_ui.cpp', s)

# ======================================================= dai_editor_ui.cpp
s = rd('src/dai_editor_ui.cpp')

i0 = s.index("            std::vector<std::string> mats = script_list(r.materials);")
end_marker = "\n        }\n    }\n\n    // ---- Collider ---"
i1 = s.index(end_marker)
new_mat = """            std::vector<std::string> mats = script_list(r.materials);
            if (mats.empty()) mats.push_back("Default");
            int mcount = (int)mats.size();
            if (dai_ui_array_begin(p->ui, "Materials", &mcount, &p->fold_materials, 1, 8)) {
                // The size field can be typed into: match the list to it
                // before drawing the rows, or the last row shows a slot that
                // does not exist yet.
                while ((int)mats.size() < mcount) mats.push_back("Default");
                while ((int)mats.size() > mcount) mats.pop_back();
                for (size_t mi = 0; mi < mats.size(); ++mi) {
                    if (dai_ui_array_object_row(p->ui, (int)mi, mats[mi].c_str(),
                                                DAI_ICON_MATERIAL)) {
                        float mx2 = 0, my2 = 0;
                        dai_ui_mouse(p->ui, &mx2, &my2, nullptr, nullptr);
                        dai_ui_searchlist_open(&p->mat_list, mx2 - 150.0f, my2);
                        p->mat_list.wants_focus = 1;
                        p->mat_menu_node = n;
                        p->mat_menu_slot = (int)mi;
                    }
                }
                int delta = dai_ui_array_end(p->ui, (int)mats.size(), 1, 8);
                if (delta > 0) mats.push_back("Default");
                else if (delta < 0 && mats.size() > 1) mats.pop_back();
            } else {
                while ((int)mats.size() < mcount) mats.push_back("Default");
                while ((int)mats.size() > mcount) mats.pop_back();
            }
            script_join(r.materials, sizeof(r.materials), mats);"""
s = s[:i0] + new_mat + s[i1:]

# the foldout state
s = sub1(s, "    float settings_font_px = 13.0f;",
            "    float settings_font_px = 13.0f;\n    int   fold_materials = 1;      // the Materials array, open like Unity's",
         'fold_materials field')

# ---- a header worth looking at
s = sub1(s,
"""        dai_ui_cursor_pos(ui2, &hx, &hy);
        dai_ui_advance(ui2, 0, 26.0f);
        float hw = dai_ui_panel_width(ui2) - st2->padding * 2;
        float mx2 = 0, my2 = 0;
        int d2 = 0, p2 = 0;
        dai_ui_mouse(ui2, &mx2, &my2, &d2, &p2);
        dai_ui_rrect(ui2, hx, hy, hw, 26.0f, 4.0f, rgba(0x34, 0x34, 0x34, 255));

        // the active checkbox
        float bx = hx + 6.0f, by = hy + 6.0f, bsz = 14.0f;""",
"""        dai_ui_cursor_pos(ui2, &hx, &hy);
        dai_ui_advance(ui2, 0, 34.0f);
        float hw = dai_ui_panel_width(ui2) - st2->padding * 2;
        float mx2 = 0, my2 = 0;
        int d2 = 0, p2 = 0;
        dai_ui_mouse(ui2, &mx2, &my2, &d2, &p2);
        // A card, not a strip: the object header is the one thing in the
        // inspector that says WHAT you are editing, and it was the same
        // height as a numeric field.
        dai_ui_rrect(ui2, hx, hy, hw, 34.0f, 5.0f, rgba(0x3A, 0x3A, 0x3A, 255));
        dai_ui_rect_outline(ui2, hx, hy, hw, 34.0f, 1.0f, st2->panel_border);
        dai_ui_rect(ui2, hx, hy + 33.0f, hw, 1.0f, st2->accent);

        // the active checkbox
        float bx = hx + 8.0f, by = hy + 10.0f, bsz = 14.0f;""",
'header card')

s = sub1(s,
"""        // the icon says the kind, the field says the name
        dai_ui_icon_at(ui2, node_icon(r), hx + 26.0f, hy + 5.0f, 16.0f, st2->accent);
        // Written back only when it actually changed, or every frame would
        // count as an edit and every rename from elsewhere would be undone.
        if (dai_ui_text_field(ui2, "objname", hx + 48.0f, hy + 4.0f, hw - 56.0f, 18.0f,
                              p->name_buf, sizeof(p->name_buf), nullptr))
            std::snprintf(r.name, sizeof(r.name), "%s", p->name_buf);
    }""",
"""        // The kind, in a tile of its own - Unity's inspector puts the icon
        // on a plate so the eye finds it before it reads anything.
        dai_ui_rrect(ui2, hx + 28.0f, hy + 5.0f, 24.0f, 24.0f, 4.0f, st2->track);
        dai_ui_icon_at(ui2, node_icon(r), hx + 32.0f, hy + 9.0f, 16.0f, st2->accent);

        // Static, on the right, where Unity has it. It is the motion type
        // here, which is the same promise: this thing does not move.
        float sw = dai_ui_text_width(ui2, "Static") + 22.0f;
        float sx = hx + hw - sw - 6.0f;
        {
            float cx = sx, cy = hy + 10.0f;
            bool over_s = mx2 >= cx && mx2 < cx + sw && my2 >= hy + 4.0f && my2 < hy + 30.0f;
            dai_ui_rect(ui2, cx, cy, 14.0f, 14.0f, over_s ? st2->button_hover : st2->track);
            dai_ui_rect_outline(ui2, cx, cy, 14.0f, 14.0f, 1.0f, st2->panel_border);
            int is_static = r.motion == DAI_STATIC;
            if (is_static) {
                dai_ui_line(ui2, cx + 3.0f, cy + 7.0f, cx + 6.0f, cy + 10.5f, 2.0f, st2->text);
                dai_ui_line(ui2, cx + 6.0f, cy + 10.5f, cx + 11.5f, cy + 3.5f, 2.0f, st2->text);
            }
            dai_ui_text(ui2, cx + 18.0f, cy + 1.0f, "Static", st2->text_dim);
            if (over_s && p2) r.motion = is_static ? DAI_DYNAMIC : DAI_STATIC;
        }

        // Written back only when it actually changed, or every frame would
        // count as an edit and every rename from elsewhere would be undone.
        float nfw = sx - (hx + 58.0f) - 8.0f;
        if (nfw < 60.0f) nfw = 60.0f;
        if (dai_ui_text_field(ui2, "objname", hx + 58.0f, hy + 8.0f, nfw, 18.0f,
                              p->name_buf, sizeof(p->name_buf), nullptr))
            std::snprintf(r.name, sizeof(r.name), "%s", p->name_buf);
    }
    dai_ui_spacing(p->ui, 2.0f);""",
'header icon tile + static')
wr('src/dai_editor_ui.cpp', s)

# ======================================================== editor_demo.cpp
s = rd('examples/editor_demo.cpp')
s = sub1(s,
"""                    dai_node made = dai_doc_prefab_instantiate(doc, full, DAI_INVALID_NODE,
                                                               perr, sizeof(perr));""",
"""                    // The path is stored AS GIVEN, so it goes in relative to
                    // the assets folder and the scene stays portable; the
                    // base dir is how it is found again.
                    (void)full;
                    dai_node made = dai_doc_prefab_instantiate(doc, pick, DAI_INVALID_NODE,
                                                               g_assets_dir, perr, sizeof(perr));""",
'prefab call')

# A scene that CONTAINS prefab instances only holds the reference; the
# subtree comes from disk and nothing was ever expanding it.
s = sub1(s,
"""            {   // the hierarchy's root row says which scene is open""",
"""            // Prefab instances hold a path, not a subtree - expanding them
            // is what turns the reference back into objects. Nothing did it,
            // so every prefab in a saved scene came back empty.
            if (g_assets_dir[0]) {
                uint32_t nrb = dai_doc_prefab_reload(doc, g_assets_dir);
                if (nrb) {
                    dai_doc_sync_reset(sync);
                    dai_doc_sync_apply(sync);
                    char pm[96];
                    std::snprintf(pm, sizeof(pm), "%u prefab instance%s expanded",
                                  nrb, nrb == 1 ? "" : "s");
                    dai_editor_ui_log(panels, 0, pm);
                }
            }
            {   // the hierarchy's root row says which scene is open""",
'prefab expand')
wr('examples/editor_demo.cpp', s)

# ============================================================ legacy Jolt
# Jolt is gone from this engine. A project file written when it was still an
# option must not select a backend that no longer exists - it opens as Talos,
# which is what every one of those scenes was tuned against anyway.
s = rd('src/dai_project.cpp')
s = sub1(s,
"""        else if (key == "physics-backend") ok = parse_int(after, &out->physics_backend);""",
"""        else if (key == "physics-backend") {
            ok = parse_int(after, &out->physics_backend);
            // 2 was Jolt. The backend is gone; the project opens on Talos
            // rather than on a number nothing answers to.
            if (out->physics_backend != DAI_PHYSICS_NULL)
                out->physics_backend = DAI_PHYSICS_TALOS;
        }""",
'jolt legacy')
wr('src/dai_project.cpp', s)
print('patch30 ok')
