// MODULE 3 (texture baker) OWNS THIS FILE. Statement seam, see include/dai_ext.h.
//
// Included INSIDE asset_inspector_body() in src/dai_editor_ui.cpp, before the
// branches that handle a material, a behaviour and a scene. In scope:
//
//   dai_editor_ui     *p       p->inspect_text is the whole file, already read
//   dai_ui            *ui
//   const dai_ui_style *st
//   const std::string &path    the asset, relative to the project's assets
//   const std::string &ext     lower case, no dot - guard on ext == "daitex"
//
// What belongs here: the .daitex inspector. The node list of the graph, every
// node's parameters as sliders, the seed, the output resolution, a Re-bake
// button - and the PREVIEW: the baked base colour as a thumbnail, drawn with
// dai_ui_image_at through the host's thumbnail callback. A generator panel
// with no picture in it is a form, and nobody tunes noise by typing numbers.
//
// Anything this seam recognises should `return;` when it is done, the way the
// folder branch above it does: the byte count and "Open externally" at the
// bottom of the panel are what an asset the editor does NOT understand gets,
// and a graph the editor understands should not end with them.
//
// Two decisions worth the paragraph they cost:
//
// THE PANEL EDITS THE FILE'S OWN TEXT, not a parsed copy of it. Every slider
// knows the offset and the length of the literal it came from and writes the
// new number back into exactly that place. The alternative - parse, edit the
// tree, re-serialise - loses the author's formatting, their comments-by-key
// ordering and every node this build happens not to know about, and it loses
// them silently, on the first drag of the first slider.
//
// IT CANNOT INCLUDE A HEADER. This file is a block of statements inside a
// function, so `#include "dai_daitex.h"` here would put a namespace inside a
// function body. The scan below is therefore its own, small and local - and
// the one thing that would really hurt to duplicate, the slider range of
// every parameter, is not duplicated: dai_daitex_ranges.inl is a block of
// statements for exactly this reason, and the baker's own clamp includes the
// same file.

if (ext == "daitex") {
    // ---- the file, as text and as rows -----------------------------------
    struct Row {
        std::string node;      // "" for the three the graph itself has
        std::string type, kind, in, key;
        float  value = 0.0f;
        size_t at = 0, len = 0;
    };
    std::string text(p->inspect_text.data());
    std::vector<Row> rows;

    auto literal = [](const std::string &s, size_t colon, size_t *at, size_t *len) {
        if (colon == std::string::npos) return false;
        size_t i = colon + 1;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
        size_t j = i;
        while (j < s.size() && (s[j] == '-' || s[j] == '+' || s[j] == '.' || s[j] == 'e' ||
                                s[j] == 'E' || (s[j] >= '0' && s[j] <= '9'))) ++j;
        if (j == i) return false;
        *at = i; *len = j - i;
        return true;
    };
    auto quoted = [](const std::string &s, size_t key_at, size_t stop, std::string *out) {
        if (key_at == std::string::npos || key_at >= stop) return false;
        size_t c = s.find(':', key_at);
        if (c == std::string::npos || c >= stop) return false;
        size_t q1 = s.find('"', c);
        if (q1 == std::string::npos || q1 >= stop) return false;
        size_t q2 = s.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 >= stop) return false;
        *out = s.substr(q1 + 1, q2 - q1 - 1);
        return true;
    };
    // The slider range of a parameter: the baker's table, not a second one.
    auto range = [](const char *type, const char *key, float *lo, float *hi, float *step) {
        *lo = 0.0f; *hi = 1.0f; *step = 0.01f;
        int hit = 0;
        #define DAI_DAITEX_RANGE(t, k, a, b, s)                                \
            if (!hit && !std::strcmp(t, type) && !std::strcmp(k, key)) {       \
                *lo = (a); *hi = (b); *step = (s); hit = 1;                    \
            }
        #include "dai_daitex_ranges.inl"
        #undef DAI_DAITEX_RANGE
        (void)hit;
    };

    size_t nodes_at = text.find("\"nodes\"");
    if (nodes_at == std::string::npos) nodes_at = text.size();
    const char *TOP[3] = { "resolution", "seed", "tiling_m" };
    for (int i = 0; i < 3; ++i) {
        std::string k = std::string("\"") + TOP[i] + "\"";
        size_t at = text.find(k);
        if (at == std::string::npos || at > nodes_at) continue;
        Row r;
        r.key = TOP[i];
        if (!literal(text, text.find(':', at), &r.at, &r.len)) continue;
        r.value = (float)std::atof(text.c_str() + r.at);
        rows.push_back(r);
    }

    std::vector<size_t> ids;
    for (size_t k = text.find("\"id\"", nodes_at); k != std::string::npos;
         k = text.find("\"id\"", k + 4))
        ids.push_back(k);
    std::string graph_name;
    {
        size_t nm = text.find("\"name\"");
        if (nm != std::string::npos && nm < nodes_at) quoted(text, nm, nodes_at, &graph_name);
    }
    for (size_t i = 0; i < ids.size(); ++i) {
        size_t start = ids[i];
        size_t stop = (i + 1 < ids.size()) ? ids[i + 1] : text.size();
        Row head;
        if (!quoted(text, start, stop, &head.node)) continue;
        quoted(text, text.find("\"type\"", start), stop, &head.type);
        quoted(text, text.find("\"kind\"", start), stop, &head.kind);
        quoted(text, text.find("\"in\"", start), stop, &head.in);
        size_t pb = text.find("\"params\"", start);
        int any = 0;
        if (pb != std::string::npos && pb < stop) {
            size_t open = text.find('{', pb);
            size_t close = open == std::string::npos ? std::string::npos : text.find('}', open);
            size_t q = open;
            while (open != std::string::npos && close != std::string::npos) {
                q = text.find('"', q + 1);
                if (q == std::string::npos || q > close) break;
                size_t q2 = text.find('"', q + 1);
                if (q2 == std::string::npos || q2 > close) break;
                Row r = head;
                r.key = text.substr(q + 1, q2 - q - 1);
                size_t colon = text.find(':', q2);
                if (colon == std::string::npos || colon > close) break;
                if (literal(text, colon, &r.at, &r.len)) {
                    r.value = (float)std::atof(text.c_str() + r.at);
                    rows.push_back(r);
                    any = 1;
                    q = r.at + r.len;
                } else {
                    q = q2;
                }
            }
        }
        if (!any) rows.push_back(head);      // a node with no numbers still shows
    }

    // ---- the picture -----------------------------------------------------
    dai_ui_label(ui, graph_name.empty() ? "Texture graph" : graph_name.c_str());
    {
        dai_texture prev = p->thumb_fn ? p->thumb_fn(path.c_str(), p->thumb_user) : 0;
        float px, py;
        dai_ui_cursor_pos(ui, &px, &py);
        const float SZ = 96.0f;
        dai_ui_advance(ui, 0, SZ + 6.0f);
        dai_ui_rrect(ui, px, py, SZ, SZ, 4.0f, st->track);
        if (prev) dai_ui_image_at(ui, prev, px + 2.0f, py + 2.0f, SZ - 4.0f, SZ - 4.0f,
                                  0, 0, 1, 1, 0xFFFFFFFFu);
        else      dai_ui_text(ui, px + 8.0f, py + SZ * 0.5f - 6.0f, "baking...", st->text_dim);
        dai_ui_rect_outline(ui, px, py, SZ, SZ, 1.0f, st->panel_border);
        // What the maps are called, so the .daimat next to it can be filled in
        // without guessing. Base colour, ORM and normal - docs/MATERIALS.md.
        std::string stem = base;
        size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos) stem.resize(dot);
        dai_ui_text(ui, px + SZ + 10.0f, py + 4.0f,  (stem + "_basecolor.png").c_str(), st->text_dim);
        dai_ui_text(ui, px + SZ + 10.0f, py + 20.0f, (stem + "_orm.png").c_str(), st->text_dim);
        dai_ui_text(ui, px + SZ + 10.0f, py + 36.0f, (stem + "_normal.png").c_str(), st->text_dim);
        dai_ui_text(ui, px + SZ + 10.0f, py + 58.0f, "baked on load and on change", st->text_dim);
    }
    dai_ui_separator(ui);

    // ---- the fields ------------------------------------------------------
    int edited = -1;
    float edited_value = 0.0f;
    std::string drawn_node = "\x01";          // nothing, and not "" either
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row &r = rows[i];
        if (r.node != drawn_node) {
            drawn_node = r.node;
            if (r.node.empty()) {
                dai_ui_label(ui, "Graph");
            } else {
                char head[192];
                if (!r.in.empty())
                    std::snprintf(head, sizeof(head), "%s  -  %s %s  < %s", r.node.c_str(),
                                  r.type.c_str(), r.kind.c_str(), r.in.c_str());
                else
                    std::snprintf(head, sizeof(head), "%s  -  %s %s", r.node.c_str(),
                                  r.type.c_str(), r.kind.c_str());
                dai_ui_label(ui, head);
            }
        }
        if (r.key.empty() || r.len == 0) continue;
        float lo, hi, step;
        range(r.type.c_str(), r.key.c_str(), &lo, &hi, &step);
        float v = r.value;
        // dai_ui_num_field, not dai_ui_slider: the same row Roughness and
        // Metallic are edited with two panels up, dragged like a slider,
        // clamped to the range above and typed into when a number is meant
        // exactly. The slider draws its label ACROSS its own track, which at
        // inspector width turns "body.lacunarity: 2.00" into a struck out
        // line - and a graph has thirty of those rows, not three.
        char label[192], id[192];
        std::snprintf(label, sizeof(label), "%s", r.key.c_str());
        std::snprintf(id, sizeof(id), "dtx%s%s", r.node.c_str(), r.key.c_str());
        if (dai_ui_num_field(ui, label, &v, step, lo, hi, id)) {
            if (step >= 1.0f) v = (float)(int)(v + 0.5f);
            edited = (int)i;
            edited_value = v;
        }
    }

    // One edit per frame, written back into the literal it came from. Doing it
    // after the loop rather than inside keeps every other row's offset valid
    // for the frame it was drawn in.
    if (edited >= 0) {
        char num[64];
        std::snprintf(num, sizeof(num), "%g", (double)edited_value);
        text.replace(rows[(size_t)edited].at, rows[(size_t)edited].len, num);
        p->inspect_text.assign(text.begin(), text.end());
        p->inspect_text.push_back(0);
        p->inspect_dirty = 1;
    }

    dai_ui_separator(ui);
    // Save writes the graph; the host's poll sees the newer mtime and re-bakes
    // it, which is the same path a file changed outside the editor takes.
    // "Bake now" writes it unchanged for exactly that reason.
    if (dai_ui_button_fit(ui, p->inspect_dirty ? "Save and bake *" : "Bake now")) {
        if (p->file_write && p->file_write(path.c_str(), text.c_str(), p->file_user)) {
            p->inspect_dirty = 0;
            p->inspect_loaded.clear();          // re-read it next frame
            dai_editor_ui_toast(p, "texture graph saved - baking", 1.5f);
        } else {
            dai_editor_ui_toast(p, "could not write that file", 2.5f);
        }
    }
    // %u and a cast, not %zu: this string is drawn on Windows too, and mingw's
    // printf is not the one the size_t length modifier is guaranteed against.
    dai_ui_label_fmt(ui, "%u nodes, %u bytes", (unsigned)ids.size(), (unsigned)bytes);
    return;
}
