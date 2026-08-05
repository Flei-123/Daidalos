import io

# ===========================================================================
# Das HUD. Eine Funktion, die aus dem Dokument und einem Rechteck Zeichnungen
# macht - damit Editor-Game-View UND die exportierte Runtime dasselbe zeigen.
# ===========================================================================
p = 'include/dai_editor_ui.h'
s = io.open(p, encoding='utf-8').read()
old = """/* Starts renaming a node in the hierarchy: the row turns into a text field,"""
new = """/* Draws every Text component of `doc` into the rectangle given, through `ui`.
 *
 * It lives here rather than in the host because BOTH hosts need it and they
 * must agree: what the Game view shows and what the exported game shows have
 * to be the same picture, or the editor is a rehearsal for a different play.
 * The exported runtime calls exactly this function with its whole window.
 *
 * `resolve` turns what the component holds into what the player reads - see
 * dai_strings_resolve. Null means "the text is the text", which is right for
 * a project with no string tables.
 *
 * `scale` is the UI scale, so a HUD authored at 24 px is 24 px on a 4K
 * display too rather than a sixth of the size. */
typedef const char *(*dai_hud_resolve_fn)(const char *text, void *user);
DAI_API void dai_hud_draw(struct dai_ui *ui, struct dai_doc *doc,
                          float x, float y, float w, float h, float scale,
                          dai_hud_resolve_fn resolve, void *user);

/* Starts renaming a node in the hierarchy: the row turns into a text field,"""
assert s.count(old) == 1, 'header anchor not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)

p = 'src/dai_editor_ui.cpp'
s = io.open(p, encoding='utf-8').read()
old = """void dai_editor_ui_toast(dai_editor_ui *p, const char *text, float seconds) {"""
new = """// ---------------------------------------------------------------- the HUD
//
// The game's own text, drawn on the picture. Two decisions worth writing down:
//
// ANCHORS, NOT POSITIONS. A score pinned to x=1720 is off screen the moment
// the window is 1280 wide, and a Game view is never the size of the finished
// window. So a label says "top right, 12 px in" and survives every size.
//
// IT IS CLIPPED TO THE VIEW. In the editor the Game view is a panel among
// panels; a HUD that spilled past its edge would draw over the Hierarchy.
void dai_hud_draw(dai_ui *ui, dai_doc *doc, float x, float y, float w, float h,
                  float scale, dai_hud_resolve_fn resolve, void *user) {
    if (!ui || !doc || w <= 0.0f || h <= 0.0f) return;
    if (!(scale > 0.0f)) scale = 1.0f;

    std::vector<dai_node> ids(dai_doc_count(doc));
    uint32_t n = ids.empty() ? 0 : dai_doc_nodes(doc, ids.data(), (uint32_t)ids.size());
    if (!n) return;

    dai_ui_clip_begin(ui, x, y, w, h);
    for (uint32_t i = 0; i < n; ++i) {
        dai_node_desc r{};
        if (dai_doc_get(doc, ids[i], &r) != DAI_OK) continue;
        if (!r.text_on || r.disabled) continue;
        // `hidden` is the RENDERER's checkbox and a label is something the
        // renderer draws, so it turns the label off too.
        if (r.hidden) continue;

        char shown[256];
        const char *txt = r.text;
        if (resolve) {
            const char *out = resolve(r.text, user);
            if (out) { std::snprintf(shown, sizeof(shown), "%s", out); txt = shown; }
        }
        if (!txt || !txt[0]) continue;

        float px = r.text_size > 0.0f ? r.text_size : 24.0f;
        // The atlas is one size; a label asks for another. dai_ui_text_scaled
        // draws at the ratio, which is what makes a 48 px title possible
        // without a second font.
        float k = px / (dai_ui_text_height(ui) > 0.0f ? dai_ui_text_height(ui) : 13.0f);

        // Multi-line: "\\n" in a string table entry is a line break, and a
        // two line label is the normal case for a subtitle or a hint.
        std::vector<std::string> lines;
        {
            std::string cur;
            for (const char *c = txt; ; ++c) {
                if (*c == '\\n' || !*c) { lines.push_back(cur); cur.clear(); if (!*c) break; }
                else cur += *c;
            }
        }
        float lh = dai_ui_text_height(ui) * k * 1.25f;
        float block_h = lh * (float)lines.size();
        float widest = 0.0f;
        for (const std::string &l : lines) {
            float lw = dai_ui_text_width(ui, l.c_str()) * k;
            if (lw > widest) widest = lw;
        }

        // 0..8 in reading order: column is anchor%3, row is anchor/3.
        int a = r.text_anchor < 0 ? 0 : (r.text_anchor > 8 ? 8 : r.text_anchor);
        const float PAD = 8.0f * scale;
        float col = (float)(a % 3), row = (float)(a / 3);
        float bx = x + PAD + (w - 2.0f * PAD - widest) * (col * 0.5f);
        float by = y + PAD + (h - 2.0f * PAD - block_h) * (row * 0.5f);
        bx += r.text_x * scale;
        by += r.text_y * scale;

        // 0,0,0 means "no colour chosen" here as everywhere else in the
        // document, and for text the useful default is white.
        uint32_t col32;
        if (r.text_color.x == 0.0f && r.text_color.y == 0.0f && r.text_color.z == 0.0f)
            col32 = 0xFFFFFFFFu;
        else {
            auto ch = [](float v) -> uint32_t {
                float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                return (uint32_t)(c * 255.0f + 0.5f);
            };
            col32 = 0xFF000000u | (ch(r.text_color.z) << 16) | (ch(r.text_color.y) << 8) | ch(r.text_color.x);
        }

        for (size_t li = 0; li < lines.size(); ++li) {
            // Centre and right anchors centre EACH line inside the block, so
            // a two line centred title looks centred rather than ragged.
            float lw = dai_ui_text_width(ui, lines[li].c_str()) * k;
            float lx = bx + (widest - lw) * (col * 0.5f);
            float ly = by + lh * (float)li;
            // A one pixel shadow. Not decoration: white text on a bright sky
            // is unreadable, and every game HUD in existence does this.
            dai_ui_text_scaled(ui, lx + 1.0f * scale, ly + 1.0f * scale, lines[li].c_str(),
                               0xB0000000u, k);
            dai_ui_text_scaled(ui, lx, ly, lines[li].c_str(), col32, k);
        }
    }
    dai_ui_clip_end(ui);
}

void dai_editor_ui_toast(dai_editor_ui *p, const char *text, float seconds) {"""
assert s.count(old) == 1, 'toast anchor not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)
print('dai_hud_draw: one function, both hosts')
