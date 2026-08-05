# -*- coding: utf-8 -*-
import sys, io

def rw(p): return io.open(p, encoding='utf-8').read()
def wr(p, s): io.open(p, 'w', encoding='utf-8').write(s)
def sub1(s, old, new, tag):
    if old not in s: print("MISS", tag); sys.exit(1)
    if s.count(old) != 1: print("AMBIG", tag, s.count(old)); sys.exit(1)
    print("ok", tag); return s.replace(old, new)

P = 'src/dai_editor_ui.cpp'
s = rw(P)

# ---- the mesh picker: a real object field, not two arrows ------------------
s = sub1(s,
"""            dai_ui_label_fmt(p->ui, "Mesh: %s", cur ? cur : "?");
            dai_ui_row(p->ui, 20.0f);
            if (dai_ui_button(p->ui, "<") && p->mesh_count > 0) {
                r.mesh = r.mesh >= 0xFFFFFFFEu ? p->mesh_count - 1
                       : (r.mesh + p->mesh_count - 1) % p->mesh_count;
            }
            if (dai_ui_button(p->ui, ">") && p->mesh_count > 0) {
                r.mesh = r.mesh >= 0xFFFFFFFEu ? 0 : (r.mesh + 1) % p->mesh_count;
            }
            if (dai_ui_button(p->ui, "auto")) r.mesh = 0xFFFFFFFFu;
            dai_ui_row_end(p->ui);""",
"""            // Unity's object field: the current value, and a target button
            // that opens the list of everything you could put there. The old
            // "< >" pair made picking the fifth mesh a five click guessing
            // game with no way to see what the other four were.
            if (dai_ui_object_field(p->ui, "Mesh", cur ? cur : "None", DAI_ICON_CUBE)) {
                float mx2 = 0, my2 = 0;
                dai_ui_mouse(p->ui, &mx2, &my2, nullptr, nullptr);
                dai_ui_popup_open(&p->menu_mesh, mx2 - 150.0f, my2);
                p->mesh_menu_node = n;
            }""", "mesh object field")

# the popup itself, next to the other menus at frame level
s = sub1(s,
"""    // The Add Component list. Entries flip between add and remove so one menu""",
"""    // The mesh picker's list: every builtin plus the two "derive it" entries.
    // Opened by the object field in the inspector, applied to the node it was
    // opened for - not to the selection, which can change while it is open.
    if (p->menu_mesh.open) {
        std::vector<std::string> names;
        std::vector<uint32_t>    ids;
        names.push_back("From shape (auto)"); ids.push_back(0xFFFFFFFFu);
        names.push_back("From asset file");   ids.push_back(0xFFFFFFFEu);
        for (uint32_t i = 0; i < p->mesh_count && i < 64; ++i) {
            const char *nm = p->mesh_name ? p->mesh_name(i, p->mesh_user) : nullptr;
            char buf[64];
            if (!nm) { std::snprintf(buf, sizeof(buf), "mesh %u", i); nm = buf; }
            names.push_back(nm); ids.push_back(i);
        }
        std::vector<dai_ui_menu_item> items(names.size());
        for (size_t i = 0; i < names.size(); ++i) {
            items[i].icon = i < 2 ? DAI_ICON_RESET : DAI_ICON_CUBE;
            items[i].label = names[i].c_str();
            items[i].shortcut = nullptr;
        }
        int pick = dai_ui_popup_menu(p->ui, &p->menu_mesh, items.data(), (uint32_t)items.size());
        if (pick >= 0 && pick < (int)ids.size() && p->mesh_menu_node != DAI_INVALID_NODE) {
            dai_doc *md = dai_editor_doc(p->ed);
            dai_node_desc mr{};
            if (dai_doc_get(md, p->mesh_menu_node, &mr) == DAI_OK) {
                dai_doc_begin(md, "Mesh");
                mr.mesh = ids[(size_t)pick];
                dai_doc_set(md, p->mesh_menu_node, &mr);
                dai_doc_commit(md);
                dai_editor_resync(p->ed);
            }
            p->mesh_menu_node = DAI_INVALID_NODE;
        }
    }

    // The Add Component list. Entries flip between add and remove so one menu""",
"mesh popup")

s = sub1(s,
"""    dai_ui_popup menu_addcomp{};      // the Add Component button's list""",
"""    dai_ui_popup menu_addcomp{};      // the Add Component button's list
    dai_ui_popup menu_mesh{};         // the mesh object field's picker
    dai_node    mesh_menu_node = DAI_INVALID_NODE;""", "mesh popup state")

wr(P, s)

# ---------------------------------------------------------------- dai_ui.cpp
P = 'src/dai_ui.cpp'
s = rw(P)
s = sub1(s,
"""int dai_ui_option(dai_ui *ui, const char *label, int *value,""",
"""int dai_ui_object_field(dai_ui *ui, const char *label, const char *value, const char *icon) {
    if (!ui) return 0;
    float h = widget_height(ui), x, y, w;
    field_rect(ui, label, &x, &y, &w, h);
    // The target button on the right is the whole point: it says "there is a
    // list of these" without a manual, and it is where Unity puts it.
    float bw = h;
    float fw = w - bw - 2.0f;
    if (fw < 20.0f) fw = w;
    bool over_f = inside_chk(ui, x, y, fw, h);
    bool over_b = inside_chk(ui, x + fw + 2.0f, y, bw, h);
    bool pressed = ui->input.mouse_down && !ui->prev.mouse_down;
    if (over_f || over_b) { ui->mouse_over_ui = true; ui->cursor_want = DAI_CURSOR_HAND; }

    dai_ui_rrect(ui, x, y + 1.0f, fw, h - 2.0f, ui->style.rounding,
                 over_f ? ui->style.button_hover : ui->style.track);
    dai_ui_rect_outline(ui, x, y + 1.0f, fw, h - 2.0f, 1.0f, ui->style.panel_border);
    float tx = x + 5.0f;
    float isz = dai_icons_size(ui->icons);
    if (isz <= 0.0f || isz > h - 4.0f) isz = h - 6.0f;
    if (icon && dai_ui_has_icon(ui, icon)) {
        dai_ui_icon_at(ui, icon, tx, y + (h - isz) * 0.5f, isz, ui->style.text_dim);
        tx += isz + 4.0f;
    }
    dai_ui_text(ui, tx, y + (h - dai_font_line_height(ui->font)) * 0.5f,
                value ? value : "None", ui->style.text);

    if (fw < w) {
        float bx = x + fw + 2.0f;
        dai_ui_rrect(ui, bx, y + 1.0f, bw, h - 2.0f, ui->style.rounding,
                     over_b ? ui->style.button_hover : ui->style.button);
        if (dai_ui_has_icon(ui, "target"))
            dai_ui_icon_at(ui, "target", bx + (bw - isz) * 0.5f, y + (h - isz) * 0.5f,
                           isz, ui->style.text);
        else
            dai_ui_text(ui, bx + 4.0f, y + 2.0f, "...", ui->style.text);
    }
    return (over_f || over_b) && pressed ? 1 : 0;
}

int dai_ui_option(dai_ui *ui, const char *label, int *value,""", "object_field")
wr(P, s)

P = 'include/dai_ui.h'
s = rw(P)
s = sub1(s,
"""DAI_API int  dai_ui_option(dai_ui *ui, const char *label, int *value,
                           const char *const *items, int count);""",
"""/* An asset reference field: what is assigned now, plus the button that opens
 * the list of what could be. Returns 1 on click - the CALLER opens the picker,
 * because only it knows what kind of thing goes in there.
 *
 * Two arrows that cycle a value are not a picker: they hide the list, and
 * choosing the fifth of six is five clicks and a memory test. */
DAI_API int  dai_ui_object_field(dai_ui *ui, const char *label, const char *value,
                                 const char *icon);

DAI_API int  dai_ui_option(dai_ui *ui, const char *label, int *value,
                           const char *const *items, int count);""", "object_field decl")
wr(P, s)

# ---------------------------------------------------------- editor_demo.cpp
P = 'examples/editor_demo.cpp'
s = rw(P)
s = sub1(s,
"""static int prefab_save_cb(const char *node_id, const char *rel_path, void *) {
    if (!g_prefab_doc || !node_id || !rel_path || !g_assets_dir[0]) return 0;
    dai_node n = (dai_node)strtoul(node_id, nullptr, 10);
    char dir[640];
    std::snprintf(dir, sizeof(dir), "%s/prefabs", g_assets_dir);
#ifdef _WIN32
    CreateDirectoryA(dir, nullptr);
#else
    mkdir(dir, 0755);
#endif
    char path[700];
    std::snprintf(path, sizeof(path), "%s/%s", g_assets_dir, rel_path);
    return dai_doc_prefab_save(g_prefab_doc, n, path) == DAI_OK ? 1 : 0;
}""",
"""static int prefab_save_cb(const char *node_id, const char *rel_path, void *) {
    if (!g_prefab_doc || !node_id || !rel_path || !g_assets_dir[0]) return 0;
    dai_node n = (dai_node)strtoul(node_id, nullptr, 10);
    char path[700];
    std::snprintf(path, sizeof(path), "%s/%s", g_assets_dir, rel_path);
    // Create the folder the prefab is going INTO, not a fixed "prefabs" one.
    // Dropping an object on a subfolder of the project window wrote to a
    // directory that did not exist, the save failed, and the editor said
    // nothing - which is what "I still cannot make prefabs" was.
    {
        char dir[700];
        std::snprintf(dir, sizeof(dir), "%s", path);
        char *slash = std::strrchr(dir, '/');
        if (slash) {
            *slash = 0;
            // every missing level, not just the last
            for (char *c = dir + 1; *c; ++c) {
                if (*c != '/') continue;
                *c = 0;
#ifdef _WIN32
                CreateDirectoryA(dir, nullptr);
#else
                mkdir(dir, 0755);
#endif
                *c = '/';
            }
#ifdef _WIN32
            CreateDirectoryA(dir, nullptr);
#else
            mkdir(dir, 0755);
#endif
        }
    }
    dai_result rc = dai_doc_prefab_save(g_prefab_doc, n, path);
    if (rc != DAI_OK) {
        char msg[800];
        std::snprintf(msg, sizeof(msg), "prefab save failed: %s", path);
        if (g_panels_for_log) dai_editor_ui_log(g_panels_for_log, 2, msg);
        std::printf("%s\\n", msg);
        return 0;
    }
    return 1;
}""", "prefab dirs")
wr(P, s)
print("patch9 done")
