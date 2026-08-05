#!/usr/bin/env python3
# patch78 - selecting a file shows the file. The inspector inspects assets.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p78'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

s = rd('src/dai_editor_ui.cpp')

# ---------------------------------------------------------------- the state
s = sub1(s,
"""    std::string proj_sel_folder;""",
"""    std::string proj_sel_folder;
    // What the inspector is inspecting when it is not inspecting an object.
    // Unity's rule: the inspector shows THE selection, and a file in the
    // project window is a selection like any other. Set by a click in the
    // browser, cleared by a click in the hierarchy or the viewport.
    std::string inspect_asset;
    std::string inspect_loaded;        // the path the buffer below belongs to
    std::vector<char> inspect_text;    // the file, when it is one we read
    dai_matfile_view inspect_mat{};    // parsed, when it is a material
    int inspect_mat_ok = 0;
    int inspect_dirty = 0;""",
    'inspect state')

# A tiny mirror of the material record. The editor UI does not include
# dai_material.h on purpose - it has no business knowing how a material is
# stored, only how to show one - so the host hands it over field by field.
s = sub1(s,
"""// A .daimat. Asked here rather than through dai_material_is_file so this file""",
"""// What the inspector shows for a material. Deliberately a copy of the fields
// rather than the material struct itself: this file does not include the
// material header, because the day it does is the day the editor UI cannot be
// built without the material format.
struct dai_matfile_view {
    float color[3];
    float roughness;
    float metallic;
    float emissive;
};

// A .daimat. Asked here rather than through dai_material_is_file so this file""",
    'matfile view')

# --------------------------------------------- selecting a file selects it
s = sub1(s,
"""                        p->asset_sel = fi;
                        p->last_pick = full;
                        p->proj_sel_folder.clear();""",
"""                        p->asset_sel = fi;
                        p->last_pick = full;
                        p->proj_sel_folder.clear();
                        p->inspect_asset = full;      // the inspector follows
                        dai_editor_deselect_all(p->ed);""",
    'file click inspects')

s = sub1(s,
"""                        p->last_pick = ffull;      // F2 renames THIS folder
                        p->proj_sel_folder = ffull;
                        p->asset_sel = -1;         // a folder and a file cannot both be it""",
"""                        p->last_pick = ffull;      // F2 renames THIS folder
                        p->proj_sel_folder = ffull;
                        p->asset_sel = -1;         // a folder and a file cannot both be it
                        p->inspect_asset = ffull;
                        dai_editor_deselect_all(p->ed);""",
    'folder click inspects')

# ...and selecting an object un-selects the file.
s = sub1(s,
"""        if (rc & 1) {
            // Ctrl-click ADDS to the selection, the way Unity multi-selects in
            // the hierarchy; a plain click replaces.
            dai_editor_select(p->ed, n, p->last_ctrl_held);
        }""",
"""        if (rc & 1) {
            // Ctrl-click ADDS to the selection, the way Unity multi-selects in
            // the hierarchy; a plain click replaces.
            dai_editor_select(p->ed, n, p->last_ctrl_held);
            p->inspect_asset.clear();       // an object is the selection now
        }""",
    'node click clears asset inspect')

# ------------------------------------------------------------ the panel
s = sub1(s,
"""static void inspector_body(dai_editor_ui *p) {
    close_field_tx_on_release(p);
    dai_doc *d = dai_editor_doc(p->ed);

    uint32_t sel = dai_editor_selection_count(p->ed);
    if (sel == 0) { dai_ui_label(p->ui, "nothing selected"); return; }""",
"""// The inspector for a FILE. Everything an asset can say about itself without
// the editor having to understand its contents - and, for the two kinds where
// it can do better than that, the fields themselves.
static void asset_inspector_body(dai_editor_ui *p) {
    dai_ui *ui = p->ui;
    const dai_ui_style *st = dai_ui_style_of(ui);
    const std::string &path = p->inspect_asset;
    std::string base = base_of(path);
    std::string ext;
    {
        size_t dot = base.find_last_of('.');
        if (dot != std::string::npos) ext = base.substr(dot + 1);
        for (char &c : ext) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    bool is_folder = ext.empty();

    // ---- the header card, the same shape the object inspector uses --------
    {
        float hx, hy;
        dai_ui_cursor_pos(ui, &hx, &hy);
        dai_ui_advance(ui, 0, 34.0f);
        float hw = dai_ui_panel_width(ui) - st->padding * 2;
        dai_ui_rrect(ui, hx, hy, hw, 34.0f, 5.0f, rgba(0x3A, 0x3A, 0x3A, 255));
        dai_ui_rect_outline(ui, hx, hy, hw, 34.0f, 1.0f, st->panel_border);
        dai_ui_rect(ui, hx, hy + 33.0f, hw, 1.0f, st->accent);
        const char *ic = is_folder ? DAI_ICON_FOLDER : icon_for_asset(path);
        dai_ui_rrect(ui, hx + 6.0f, hy + 5.0f, 24.0f, 24.0f, 4.0f, st->track);
        if (ic && dai_ui_has_icon(ui, ic))
            dai_ui_icon_at(ui, ic, hx + 10.0f, hy + 9.0f, 16.0f, st->accent);
        dai_ui_text(ui, hx + 38.0f, hy + 5.0f, base.c_str(), st->text);
        const char *kind = is_folder ? "Folder"
                         : is_material_file(path) ? "Material"
                         : is_scene_asset(path) ? "Scene"
                         : is_scene_file(path) ? "Prefab"
                         : is_behaviour_file(path) ? "Behaviour"
                         : (ext == "png" || ext == "jpg" || ext == "jpeg") ? "Texture"
                         : (ext == "glb" || ext == "gltf") ? "Model"
                         : "File";
        dai_ui_text(ui, hx + 38.0f, hy + 19.0f, kind, st->text_dim);
    }
    dai_ui_label(p->ui, path.c_str());
    dai_ui_separator(p->ui);
    if (is_folder) {
        dai_ui_label(p->ui, "double click it in the browser to go inside");
        return;
    }

    // ---- the file itself, read once per selection -------------------------
    if (p->inspect_loaded != path) {
        p->inspect_loaded = path;
        p->inspect_text.assign(64 * 1024, 0);
        p->inspect_mat_ok = 0;
        p->inspect_dirty = 0;
        uint32_t got = 0;
        if (p->file_read)
            got = p->file_read(path.c_str(), p->inspect_text.data(),
                               (uint32_t)p->inspect_text.size() - 1, p->file_user);
        p->inspect_text[got < p->inspect_text.size() ? got : p->inspect_text.size() - 1] = 0;
        if (is_material_file(path) && got) {
            // Parsed here rather than through the material module, for the
            // same reason the struct is a copy: four numbers off a text file
            // is not worth a dependency in the other direction.
            dai_matfile_view v{ { 0.8f, 0.8f, 0.8f }, 0.5f, 0.0f, 0.0f };
            const char *c = p->inspect_text.data();
            while (*c) {
                const char *nl = std::strchr(c, '\\n');
                std::string line(c, nl ? (size_t)(nl - c) : std::strlen(c));
                c = nl ? nl + 1 : c + line.size();
                if (line.compare(0, 6, "color ") == 0)
                    std::sscanf(line.c_str() + 6, "%f %f %f", &v.color[0], &v.color[1], &v.color[2]);
                else if (line.compare(0, 10, "roughness ") == 0)
                    std::sscanf(line.c_str() + 10, "%f", &v.roughness);
                else if (line.compare(0, 9, "metallic ") == 0)
                    std::sscanf(line.c_str() + 9, "%f", &v.metallic);
                else if (line.compare(0, 9, "emissive ") == 0)
                    std::sscanf(line.c_str() + 9, "%f", &v.emissive);
            }
            p->inspect_mat = v;
            p->inspect_mat_ok = 1;
        }
    }
    size_t bytes = std::strlen(p->inspect_text.data());

    // ---- what each kind can say for itself --------------------------------
    if (p->inspect_mat_ok) {
        dai_ui_label(p->ui, "Surface");
        int changed = 0;
        changed |= dai_ui_num_vec3(p->ui, "Color", p->inspect_mat.color, 0.01f);
        changed |= dai_ui_num_field(p->ui, "Roughness", &p->inspect_mat.roughness, 0.01f, 0.02f, 1.0f, "matrough");
        changed |= dai_ui_num_field(p->ui, "Metallic", &p->inspect_mat.metallic, 0.01f, 0.0f, 1.0f, "matmetal");
        changed |= dai_ui_num_field(p->ui, "Emissive", &p->inspect_mat.emissive, 0.05f, 0.0f, 40.0f, "matemis");
        if (changed) p->inspect_dirty = 1;
        // The swatch: a number is not a colour.
        {
            float sx, sy;
            dai_ui_cursor_pos(p->ui, &sx, &sy);
            dai_ui_advance(p->ui, 0, 26.0f);
            float sw = dai_ui_panel_width(p->ui) - st->padding * 2;
            uint32_t col = rgba((int)(p->inspect_mat.color[0] * 255.0f),
                                (int)(p->inspect_mat.color[1] * 255.0f),
                                (int)(p->inspect_mat.color[2] * 255.0f), 255);
            dai_ui_rrect(p->ui, sx, sy, sw, 22.0f, 4.0f, col);
            dai_ui_rect_outline(p->ui, sx, sy, sw, 22.0f, 1.0f, st->panel_border);
        }
        if (dai_ui_button(p->ui, p->inspect_dirty ? "Save material *" : "Save material")) {
            char text[512];
            std::snprintf(text, sizeof(text),
                          "daidalos-material 1\\ncolor %g %g %g\\nroughness %g\\n"
                          "metallic %g\\nemissive %g\\n",
                          (double)p->inspect_mat.color[0], (double)p->inspect_mat.color[1],
                          (double)p->inspect_mat.color[2], (double)p->inspect_mat.roughness,
                          (double)p->inspect_mat.metallic, (double)p->inspect_mat.emissive);
            if (p->file_write && p->file_write(path.c_str(), text, p->file_user)) {
                p->inspect_dirty = 0;
                p->inspect_loaded.clear();          // re-read it next frame
                p->want_material_apply = 1;         // every object wearing it
                dai_editor_ui_toast(p, "material saved", 1.5f);
            } else {
                dai_editor_ui_toast(p, "could not write that file", 2.5f);
            }
        }
        dai_ui_separator(p->ui);
    } else if (is_behaviour_file(path)) {
        // The fields it declares - the same list the object inspector draws
        // when this script is attached to something.
        if (p->params_fn) {
            char keys[1024] = { 0 };
            p->params_fn(path.c_str(), keys, sizeof(keys), p->params_user);
            std::vector<ParamDecl> decls = parse_params(keys);
            dai_ui_label_fmt(p->ui, "Serialized fields: %u", (unsigned)decls.size());
            for (const ParamDecl &pd : decls) {
                static const char *TN[] = { "node", "float", "int", "bool", "string" };
                dai_ui_label_fmt(p->ui, "   %s %s%s%s", TN[pd.type], pd.name.c_str(),
                                 pd.def.empty() ? "" : " = ", pd.def.c_str());
            }
        }
        int lines = 1;
        for (const char *c = p->inspect_text.data(); *c; ++c) if (*c == '\\n') ++lines;
        dai_ui_label_fmt(p->ui, "%d lines", lines);
        if (dai_ui_button(p->ui, "Edit")) dai_editor_ui_script_open(p, path.c_str());
        dai_ui_separator(p->ui);
    } else if (is_scene_file(path)) {
        // A scene file and a prefab file are the same format; the count of
        // "node" lines is what either of them is made of.
        int nodes = 0;
        const char *c = p->inspect_text.data();
        while ((c = std::strstr(c, "node ")) != nullptr) { ++nodes; c += 5; }
        dai_ui_label_fmt(p->ui, "%d object(s) inside", nodes);
        if (is_scene_asset(path)) {
            if (dai_ui_button(p->ui, "Open scene")) p->prefab_open_want = path;
        } else {
            if (dai_ui_button(p->ui, "Open prefab")) p->prefab_open_want = path;
            dai_ui_label(p->ui, "drag it into the scene to place one");
        }
        dai_ui_separator(p->ui);
    }

    if (bytes) dai_ui_label_fmt(p->ui, "%u bytes", (unsigned)bytes);
    else       dai_ui_label(p->ui, "binary, or not readable as text");
    if (dai_ui_button(p->ui, "Open externally") && p->open_asset)
        p->open_asset(nullptr, path.c_str(), p->open_asset_user);
}

static void inspector_body(dai_editor_ui *p) {
    close_field_tx_on_release(p);
    dai_doc *d = dai_editor_doc(p->ed);

    uint32_t sel = dai_editor_selection_count(p->ed);
    // A file is a selection too. It loses to an object, so clicking in the
    // hierarchy always wins - and clicking in the browser clears the object.
    if (sel == 0 && !p->inspect_asset.empty()) { asset_inspector_body(p); return; }
    if (sel == 0) { dai_ui_label(p->ui, "nothing selected"); return; }""",
    'asset inspector')
wr('src/dai_editor_ui.cpp', s)
print('patch78 ok')
