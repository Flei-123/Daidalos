import io

# ===========================================================================
# Das HUD kann jetzt drei Dinge mehr: umbrechen, schrumpfen, und Bilder.
# ===========================================================================
p = 'include/dai_editor_ui.h'
s = io.open(p, encoding='utf-8').read()
old = """typedef const char *(*dai_hud_resolve_fn)(const char *text, void *user);"""
new = """typedef const char *(*dai_hud_resolve_fn)(const char *text, void *user);
/* Turns a project path into a texture the renderer already holds. Null means
 * "this host has no textures", and then Image components simply do not draw -
 * which is the right behaviour for a headless test, not an error. */
typedef uint32_t (*dai_hud_image_fn)(const char *path, float *out_w, float *out_h, void *user);
DAI_API void dai_hud_images(dai_hud_image_fn fn, void *user);"""
assert s.count(old) == 1, 'resolve typedef not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)

p = 'src/dai_editor_ui.cpp'
s = io.open(p, encoding='utf-8').read()

old = """void dai_hud_draw(dai_ui *ui, dai_doc *doc, float x, float y, float w, float h,
                  float scale, dai_hud_resolve_fn resolve, void *user) {"""
new = """// Where a HUD gets its pictures. A global rather than a parameter because
// both hosts set it once at startup and neither ever changes it, and threading
// it through dai_hud_draw would put it in the signature of a function whose
// whole point is that the editor and the runtime call it identically.
static dai_hud_image_fn g_hud_image = nullptr;
static void            *g_hud_image_user = nullptr;
void dai_hud_images(dai_hud_image_fn fn, void *user) {
    g_hud_image = fn; g_hud_image_user = user;
}

// Breaks `text` into lines that fit `maxw` at scale `k`. maxw <= 0 means "do
// not wrap" - a score is one line however long the number gets.
static void hud_wrap(dai_ui *ui, const std::string &text, float maxw, float k,
                     std::vector<std::string> &out) {
    out.clear();
    std::string para;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\\n') {
            if (maxw <= 0.0f) { out.push_back(para); para.clear(); continue; }
            // Greedy wrap at spaces; a word longer than the box is cut rather
            // than allowed to stick out, because sticking out is what a box
            // exists to prevent.
            std::string line;
            size_t pos = 0;
            while (pos <= para.size()) {
                size_t sp = para.find(' ', pos);
                std::string word = para.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
                std::string cand = line.empty() ? word : line + " " + word;
                if (dai_ui_text_width(ui, cand.c_str()) * k <= maxw || line.empty()) {
                    line = cand;
                } else {
                    out.push_back(line);
                    line = word;
                }
                if (sp == std::string::npos) break;
                pos = sp + 1;
            }
            out.push_back(line);
            para.clear();
            continue;
        }
        para += text[i];
    }
}

void dai_hud_draw(dai_ui *ui, dai_doc *doc, float x, float y, float w, float h,
                  float scale, dai_hud_resolve_fn resolve, void *user) {"""
assert s.count(old) == 1, 'hud_draw def not found'
s = s.replace(old, new)

# --- Bilder zeichnen, vor dem Text (Text liegt oben drauf) -----------------
old = """        if (!r.text_on || r.disabled) continue;"""
new = """        // ---- Image ---------------------------------------------------------
        // Drawn before the text of the same frame, so a label on a panel is a
        // label on a panel and not behind it. Both live on their own node in
        // practice; when they share one, the picture is the background.
        if (r.image_on && !r.disabled && r.image[0] && g_hud_image) {
            float iw = 0.0f, ih = 0.0f;
            uint32_t tex = g_hud_image(r.image, &iw, &ih, g_hud_image_user);
            if (tex) {
                float dw = r.image_w > 0.0f ? r.image_w : (iw > 0.0f ? iw : 64.0f);
                float dh = r.image_h > 0.0f ? r.image_h : (ih > 0.0f ? ih : 64.0f);
                dw *= scale; dh *= scale;
                int ia = r.image_anchor < 0 ? 0 : (r.image_anchor > 8 ? 8 : r.image_anchor);
                const float IPAD = 8.0f * scale;
                float icol = (float)(ia % 3), irow = (float)(ia / 3);
                float ix = x + IPAD + (w - 2.0f * IPAD - dw) * (icol * 0.5f) + r.image_x * scale;
                float iy = y + IPAD + (h - 2.0f * IPAD - dh) * (irow * 0.5f) + r.image_y * scale;
                uint32_t tint = 0xFFFFFFFFu;
                if (r.image_color.x != 0.0f || r.image_color.y != 0.0f || r.image_color.z != 0.0f) {
                    auto ch = [](float v) -> uint32_t {
                        float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                        return (uint32_t)(c * 255.0f + 0.5f);
                    };
                    tint = 0xFF000000u | (ch(r.image_color.z) << 16) |
                           (ch(r.image_color.y) << 8) | ch(r.image_color.x);
                }
                dai_ui_image_at(ui, tex, ix, iy, dw, dh, 0, 0, 1, 1, tint);
            }
        }

        if (!r.text_on || r.disabled) continue;"""
assert s.count(old) == 1, 'text_on check not found'
s = s.replace(old, new)

# --- Box, Umbruch, Autosize ------------------------------------------------
old = """        // Multi-line: "\\n" in a string table entry is a line break, and a
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
        }"""
new = """        // The box, if there is one. Without it the label is as wide as its
        // words; with it the text wraps, and with autosize it also shrinks
        // until it fits - which is the only honest answer to "the German
        // translation is a third longer and the box did not grow".
        float boxw = r.text_w * scale, boxh = r.text_h * scale;
        std::vector<std::string> lines;
        float lh = 0.0f, block_h = 0.0f, widest = 0.0f;
        for (int attempt = 0; attempt < 40; ++attempt) {
            hud_wrap(ui, txt, boxw > 0.0f ? boxw : 0.0f, k, lines);
            lh = dai_ui_text_height(ui) * k * 1.25f;
            block_h = lh * (float)lines.size();
            widest = 0.0f;
            for (const std::string &l : lines) {
                float lw = dai_ui_text_width(ui, l.c_str()) * k;
                if (lw > widest) widest = lw;
            }
            if (!r.text_autosize || boxh <= 0.0f) break;
            if (block_h <= boxh && (boxw <= 0.0f || widest <= boxw)) break;
            // 4% a step: enough to converge in a few dozen tries, small enough
            // that the result is not visibly quantised.
            k *= 0.96f;
            if (k * dai_ui_text_height(ui) < 6.0f) break;   // a floor: unreadable is not a fit
        }
        // Inside a box the block is laid out against the BOX, not the words -
        // otherwise a centred paragraph re-centres itself every time a word
        // changes length.
        if (boxw > 0.0f) widest = boxw;
        if (boxh > 0.0f && block_h < boxh) block_h = boxh;"""
assert s.count(old) == 1, 'line splitting not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)
print('hud: wrap, autosize, images')
