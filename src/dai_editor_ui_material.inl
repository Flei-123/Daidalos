// MODULE 2 (triplanar) OWNS THIS FILE. Statement seam, see include/dai_ext.h.
//
// Included INSIDE asset_inspector_body() in src/dai_editor_ui.cpp, in the
// material branch, under the four rows the panel already draws. In scope:
//
//   dai_editor_ui *p     the panel      (p->inspect_mat is the parsed view)
//   dai_ui        *ui    the widgets    (dai_ui_num_field, dai_ui_check, ...)
//   const dai_ui_style *st
//   const std::string &path   the .daimat, relative to the project's assets
//   int changed          OR your rows into this; it is what lights up Save
//
// What belongs here: the three map slots (base colour, ORM, normal) as asset
// fields, the "Triplanar" tick box, the tiling size IN METRES and the blend
// sharpness. Two things to get right, both of them things a reviewer will
// look for:
//
//   * The tiling is metres, not repeats. A world projected texture has no UV
//     set to repeat against - "2 m" is a number an artist can hold in their
//     head and a number that stays true when the wall gets longer.
//   * The panel must say what triplanar costs (three samples per map) where
//     the user can read it, so nobody switches it on for every material in the
//     project and then wonders where the frame rate went.
//
// State that has to survive between frames goes in a function-local `static`
// keyed by `path`, or in the parse seam's own storage - do NOT add fields to
// struct dai_editor_ui, which is not this module's file.
//
// This seam keeps NO state at all, and that is deliberate: the file's own text
// (p->inspect_text, already read once per selection) is the state. A row is
// parsed out of it when it is drawn and written straight back into it when it
// changes, so there is no second copy to keep in step with the four numbers
// above, nothing to invalidate when the file is edited in the script tab, and
// the Save seam below reads the same text rather than a mirror of it.

// The value of one `key value` line of the material text, empty when the file
// does not have that line. The rest of the line, not one token: a path may
// have a space in it.
auto mat_line_value = [&](const char *key) -> std::string {
    const std::string all(p->inspect_text.data());
    const size_t klen = std::strlen(key);
    size_t pos = 0;
    while (pos < all.size()) {
        size_t nl = all.find('\n', pos);
        std::string line = all.substr(pos, nl == std::string::npos ? nl : nl - pos);
        pos = nl == std::string::npos ? all.size() : nl + 1;
        if (line.size() > klen && line.compare(0, klen, key) == 0 &&
            (line[klen] == ' ' || line[klen] == '\t')) {
            std::string v = line.substr(klen + 1);
            while (!v.empty() && (v.back() == '\r' || v.back() == ' ' || v.back() == '\t'))
                v.pop_back();
            size_t b = v.find_first_not_of(" \t");
            return b == std::string::npos ? std::string() : v.substr(b);
        }
    }
    return std::string();
};

// Write one line back. An empty value REMOVES the line, which is the same rule
// the format itself follows: a file holds what is not default, so switching
// triplanar off again leaves no trace of it behind.
auto mat_set_line = [&](const char *key, const std::string &value) {
    const std::string all(p->inspect_text.data());
    const size_t klen = std::strlen(key);
    std::string out;
    out.reserve(all.size() + 64);
    bool replaced = false;
    size_t pos = 0;
    while (pos < all.size()) {
        size_t nl = all.find('\n', pos);
        std::string line = all.substr(pos, nl == std::string::npos ? nl : nl - pos);
        pos = nl == std::string::npos ? all.size() : nl + 1;
        bool mine = line.size() > klen && line.compare(0, klen, key) == 0 &&
                    (line[klen] == ' ' || line[klen] == '\t');
        if (mine) {
            replaced = true;
            if (value.empty()) continue;
            out += key; out += ' '; out += value; out += '\n';
            continue;
        }
        out += line; out += '\n';
    }
    if (!replaced && !value.empty()) { out += key; out += ' '; out += value; out += '\n'; }
    if (out.size() + 1 >= p->inspect_text.size()) return;   // 64 KB of material: not ours to grow
    std::memcpy(p->inspect_text.data(), out.data(), out.size());
    p->inspect_text[out.size()] = 0;
};

auto mat_line_float = [&](const char *key, float fallback) -> float {
    std::string v = mat_line_value(key);
    float out = fallback;
    if (!v.empty()) std::sscanf(v.c_str(), "%f", &out);
    return out;
};

auto mat_float_text = [](float v) -> std::string {
    char b[32];
    std::snprintf(b, sizeof(b), "%g", (double)v);
    return b;
};

// ---- the maps ---------------------------------------------------------
//
// Paths, relative to the project's Assets folder, exactly like every other
// asset reference in the document. Typed or pasted here; a .daitex bakes its
// three PNGs next to itself and the names go in the same way.
dai_ui_advance(ui, 0, 4.0f);
dai_ui_label(ui, "Maps");
{
    static const struct { const char *key; const char *label; const char *tip; } SLOTS[] = {
        { "base_color_map", "Base Color", "sRGB albedo. Multiplied by the colour above." },
        { "orm_map",        "ORM",        "linear: R occlusion, G roughness, B metallic" },
        { "normal_map",     "Normal",     "linear tangent space normal map" },
    };
    for (const auto &slot : SLOTS) {
        char buf[160];      // DAI_MATFILE_PATH, which this file cannot include
        std::snprintf(buf, sizeof(buf), "%s", mat_line_value(slot.key).c_str());
        if (dai_ui_input_text(ui, slot.label, buf, sizeof(buf))) {
            mat_set_line(slot.key, buf);
            changed = 1;
        }
        dai_ui_help(ui, slot.tip);
    }
    if (!mat_line_value("normal_map").empty()) {
        float strength = mat_line_float("normal_strength", 1.0f);
        if (dai_ui_num_field(ui, "Normal Strength", &strength, 0.02f, 0.0f, 4.0f, "matnrmstr")) {
            mat_set_line("normal_strength", strength == 1.0f ? std::string() : mat_float_text(strength));
            changed = 1;
        }
    }
}

// ---- the world projection ---------------------------------------------
dai_ui_advance(ui, 0, 4.0f);
dai_ui_label(ui, "Projection");
{
    int tri = mat_line_float("triplanar", 0.0f) != 0.0f;
    if (dai_ui_checkbox(ui, "Triplanar (project in world space)", &tri)) {
        mat_set_line("triplanar", tri ? std::string("1") : std::string());
        changed = 1;
    }
    if (tri) {
        // METRES per repeat, not a repeat count. A projected texture has no UV
        // set to repeat against, and "2 m" stays true when the wall gets
        // longer - which is the whole reason a blockout wall can wear this.
        float metres = mat_line_float("triplanar_scale", 1.0f);
        if (dai_ui_num_field(ui, "Tiling (m)", &metres, 0.05f, 0.05f, 64.0f, "mattriscale")) {
            if (metres < 0.05f) metres = 0.05f;
            mat_set_line("triplanar_scale", metres == 1.0f ? std::string() : mat_float_text(metres));
            changed = 1;
        }
        dai_ui_help(ui, "edge length of one repeat, in metres of world space");
        float blend = mat_line_float("triplanar_blend", 4.0f);
        if (dai_ui_num_field(ui, "Blend", &blend, 0.1f, 1.0f, 16.0f, "mattriblend")) {
            mat_set_line("triplanar_blend", blend == 4.0f ? std::string() : mat_float_text(blend));
            changed = 1;
        }
        dai_ui_help(ui, "1 = wide wash across a corner, 16 = a hard edge");
        // Said where it is read, not in a document nobody opens: this is the
        // one material switch in the editor that triples a cost.
        dai_ui_label(ui, "costs three samples per map");
        dai_ui_label(ui, "no UV set needed - for blockout and CSG");
    } else {
        dai_ui_label(ui, "sampling the mesh's UV set");
    }
}
