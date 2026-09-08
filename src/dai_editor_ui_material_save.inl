// MODULE 2 (triplanar) OWNS THIS FILE. Statement seam, see include/dai_ext.h.
//
// Included INSIDE asset_inspector_body() in src/dai_editor_ui.cpp, inside the
// Save button, where the file's text has just been built. In scope:
//
//   std::string text          the six lines the panel writes today - APPEND
//   const std::string &path   which material is being written
//   dai_editor_ui *p
//
// Append the lines this module owns - the maps, `triplanar`, tiling, blend -
// and only when they differ from the default, which is the rule the whole
// format follows: a file holds what is not default, so it stays readable and
// so adding a field later does not rewrite every material in the project.
//
// Without this seam the Save button would write a six line file over a
// material that had ten lines in it, and the maps would be gone. That is the
// one failure mode of this seam, and it is why it exists.
//
// The values come out of p->inspect_text through the same `mat_line_value`
// the rows above are drawn from - the text IS the state of those rows, so
// what is saved is exactly what the panel is showing.
{
    static const char *MAP_KEYS[] = { "base_color_map", "orm_map", "normal_map" };
    for (const char *key : MAP_KEYS) {
        std::string v = mat_line_value(key);
        if (v.empty()) continue;              // no map in that slot: no line
        text += key; text += ' '; text += v; text += '\n';
    }
    // Numbers: written only when they are not the default, and the default is
    // the one the loader falls back to - 1 for the normal strength, 1 metre
    // for the tiling, 4 for the blend.
    static const struct { const char *key; float def; } NUMS[] = {
        { "normal_strength", 1.0f },
        { "triplanar_scale", 1.0f },
        { "triplanar_blend", 4.0f },
    };
    for (const auto &num : NUMS) {
        std::string v = mat_line_value(num.key);
        if (v.empty()) continue;
        float f = num.def;
        std::sscanf(v.c_str(), "%f", &f);
        if (f == num.def) continue;
        char line[64];
        std::snprintf(line, sizeof(line), "%s %g\n", num.key, (double)f);
        text += line;
    }
    // The switch itself, only when it is ON: off is the default, and a
    // material that is not projected says nothing about projection.
    float tri = 0.0f;
    std::string tv = mat_line_value("triplanar");
    if (!tv.empty()) std::sscanf(tv.c_str(), "%f", &tri);
    if (tri != 0.0f) text += "triplanar 1\n";
}
