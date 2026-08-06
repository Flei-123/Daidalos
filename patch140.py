#!/usr/bin/env python3
# Runde 28a - dai_ui: "##" im Label, Unterklapper wie in Unity, mehrzeiliges
# Textfeld, Autocomplete ab EINEM Zeichen und auch in C++.
import io, os, sys, shutil

ROOT = os.path.dirname(os.path.abspath(__file__))

def sub(s, old, new, what):
    if old not in s:
        sys.exit("!! nicht gefunden: " + what)
    return s.replace(old, new, 1)

# ------------------------------------------------------------------ dai_ui.cpp
p = os.path.join(ROOT, "src", "dai_ui.cpp")
t = io.open(p, encoding="utf-8").read()

# ---- 1. "X##fpx" ist ein Label UND eine Id -----------------------------
OLD = """void rgb_to_hsv(const float *rgb, float *h, float *sv, float *v) {"""
NEW = """// "X##fpx" is one letter of label and one widget id. Everything after ## is
// the id half: three checkboxes all called "X" need three ids, and nobody
// should be shown the plumbing. It WAS shown - the Freeze Position row read
// "X##fpx Y##fpy Z##fpz" on screen, which is not what anyone typed it for.
const char *vis_label(const char *s, char *tmp, size_t n) {
    if (!s) return "";
    const char *hh = std::strstr(s, "##");
    if (!hh) return s;
    size_t len = (size_t)(hh - s);
    if (len >= n) len = n - 1;
    std::memcpy(tmp, s, len);
    tmp[len] = 0;
    return tmp;
}

void rgb_to_hsv(const float *rgb, float *h, float *sv, float *v) {"""
t = sub(t, OLD, NEW, "rgb_to_hsv")

OLD = """    float box = h - 8.0f;
    uint64_t id = hash_id(utf8, x, y);
    bool over = inside_chk(ui, x, y, box + 8.0f + dai_ui_text_width(ui, utf8), h);"""
NEW = """    float box = h - 8.0f;
    char lbuf[96];
    const char *shown = vis_label(utf8, lbuf, sizeof(lbuf));
    uint64_t id = hash_id(utf8, x, y);
    bool over = inside_chk(ui, x, y, box + 8.0f + dai_ui_text_width(ui, shown), h);"""
t = sub(t, OLD, NEW, "checkbox-Kopf")
t = sub(t,
    """    dai_ui_text(ui, x + box + 8.0f, y + ui->style.row_pad * 0.5f, utf8, ui->style.text);
    return changed;
}""",
    """    dai_ui_text(ui, x + box + 8.0f, y + ui->style.row_pad * 0.5f, shown, ui->style.text);
    return changed;
}""", "checkbox-Label")

# ---- 2. das Farbrad genauer --------------------------------------------
OLD = """    const int SEG = 48, RINGS = 6;
    for (int i = 0; i < SEG; ++i) {
        float a0 = (float)i / SEG * 6.2831853f, a1 = (float)(i + 1) / SEG * 6.2831853f;
        for (int rr = 0; rr < RINGS; ++rr) {
            float r0 = rad * (float)rr / RINGS, r1 = rad * (float)(rr + 1) / RINGS;
            float c0[3], c1[3];
            hsv_to_rgb((float)i / SEG, (float)rr / RINGS, v, c0);
            hsv_to_rgb((float)i / SEG, (float)(rr + 1) / RINGS, v, c1);
            uint32_t col = pack_rgb(c1, 1.0f);"""
NEW = """    // Each cell is filled with the colour at its OWN CENTRE. It used to be
    // filled with the colour at its outer edge, so every pixel showed a hue
    // up to 7.5 degrees and a saturation up to a sixth off what clicking it
    // would give you - the wheel and the pick disagreed everywhere.
    const int SEG = 96, RINGS = 16;
    for (int i = 0; i < SEG; ++i) {
        float a0 = (float)i / SEG * 6.2831853f, a1 = (float)(i + 1) / SEG * 6.2831853f;
        float hc = ((float)i + 0.5f) / SEG;
        for (int rr = 0; rr < RINGS; ++rr) {
            float r0 = rad * (float)rr / RINGS, r1 = rad * (float)(rr + 1) / RINGS;
            float cc[3];
            hsv_to_rgb(hc, ((float)rr + 0.5f) / RINGS, v, cc);
            uint32_t col = pack_rgb(cc, 1.0f);"""
t = sub(t, OLD, NEW, "Farbrad")
t = sub(t, """                      cx + std::cos(a0) * r1, cy + std::sin(a0) * r1, col);
            (void)c0;
        }""", """                      cx + std::cos(a0) * r1, cy + std::sin(a0) * r1, col);
        }""", "Farbrad-Ende")

OLD = """    for (int i = 0; i < 32; ++i) {
        float t0 = (float)i / 32.0f, t1 = (float)(i + 1) / 32.0f;
        float c[3];
        hsv_to_rgb(h, sv, 1.0f - t1, c);"""
NEW = """    for (int i = 0; i < 64; ++i) {
        float t0 = (float)i / 64.0f, t1 = (float)(i + 1) / 64.0f;
        float c[3];
        hsv_to_rgb(h, sv, 1.0f - (t0 + t1) * 0.5f, c);"""
t = sub(t, OLD, NEW, "Value-Bar")

# ---- 3. Autocomplete ----------------------------------------------------
OLD = """const AcEntry AC_NODE[] = {"""
NEW = """// The same idea for the C++ behaviours: `Node me(api, self); me.` is how
// every one of them starts, and none of those members were ever offered -
// which is exactly what "autocomplete only works in JavaScript" was.
const AcEntry AC_NODE_CPP[] = {
    { "position()",   "-> Vec3" },
    { "position(",    "Vec3 - move it" },
    { "velocity()",   "-> Vec3" },
    { "velocity(",    "Vec3 - drive it" },
    { "scale()",      "-> Vec3" },
    { "scale(",       "Vec3" },
    { "impulse(",     "Vec3 - one push" },
    { "key(",         "'w' or DAI_KEY_* - held?" },
    { "find(",        "\\"name\\" -> Node" },
    { "param(",       "\\"name\\", fallback - the inspector's value" },
    { "node(",        "\\"name\\" -> the Node field's target" },
    { "grounded()",   "standing on something?" },
    { "log(",         "one line into the Console" },
};

const AcEntry AC_NODE[] = {"""
t = sub(t, OLD, NEW, "AC_NODE")

OLD = """        if (st->focused && prefix.size() >= 2) {"""
NEW = """        // ONE character is enough. Two meant the list never appeared for the
        // thing you were most likely to want it for - `a`, `s`, `me.` - and a
        // completion you have to earn is one people stop waiting for.
        if (st->focused && prefix.size() >= 1 && lang != DAI_CODE_LANG_NONE) {"""
t = sub(t, OLD, NEW, "Autocomplete-Schwelle")

OLD = """            size_t dot = prefix.find('.');
            if (lang != DAI_CODE_LANG_CPP && dot != std::string::npos) {
                std::string root = prefix.substr(0, dot);
                std::string rest = prefix.substr(dot + 1);
                if (!root.empty() && !ac_root_is_api(root)) {
                    for (const AcEntry &e : AC_NODE) {
                        if (std::strncmp(e.text, rest.c_str(), rest.size()) != 0) continue;
                        std::string full = root + "." + e.text;
                        bool dup = false;
                        for (const AcHit &h : hits) if (h.text == full) { dup = true; break; }
                        if (!dup) hits.push_back(AcHit{ full, e.hint });
                    }
                }
            }"""
NEW = """            size_t dot = prefix.find('.');
            if (dot != std::string::npos) {
                std::string root = prefix.substr(0, dot);
                std::string rest = prefix.substr(dot + 1);
                if (!root.empty() && !ac_root_is_api(root)) {
                    const AcEntry *mt = lang == DAI_CODE_LANG_CPP ? AC_NODE_CPP : AC_NODE;
                    size_t mn = lang == DAI_CODE_LANG_CPP
                              ? sizeof(AC_NODE_CPP) / sizeof(AC_NODE_CPP[0])
                              : sizeof(AC_NODE) / sizeof(AC_NODE[0]);
                    for (size_t mi = 0; mi < mn; ++mi) {
                        if (std::strncmp(mt[mi].text, rest.c_str(), rest.size()) != 0) continue;
                        std::string full = root + "." + mt[mi].text;
                        bool dup = false;
                        for (const AcHit &hh2 : hits) if (hh2.text == full) { dup = true; break; }
                        if (!dup) hits.push_back(AcHit{ full, mt[mi].hint });
                    }
                }
            }"""
t = sub(t, OLD, NEW, "Member-Vervollstaendigung")

# ---- 4. Code-Editor ohne Zeilennummern (fuer das Textfeld) --------------
OLD = """    const float GUT = dai_ui_text_width(ui, gut) + 14.0f;"""
NEW = """    // A plain multi-line field is the same editor with the machinery off:
    // line numbers in a Text component would be nonsense.
    const float GUT = st->plain ? 0.0f : dai_ui_text_width(ui, gut) + 14.0f;"""
t = sub(t, OLD, NEW, "GUT")

OLD = """    dai_ui_rect(ui, x, y, GUT, h, (sty->chrome & 0x00FFFFFFu) | 0xFF000000u);
    dai_ui_rect(ui, x + GUT, y, 1.0f, h, sty->panel_border);"""
NEW = """    if (!st->plain) {
        dai_ui_rect(ui, x, y, GUT, h, (sty->chrome & 0x00FFFFFFu) | 0xFF000000u);
        dai_ui_rect(ui, x + GUT, y, 1.0f, h, sty->panel_border);
    }"""
t = sub(t, OLD, NEW, "Gutter-Hintergrund")

OLD = """        dai_ui_text(ui, x + GUT - 8.0f - nw, ry, nb,"""
NEW = """        if (!st->plain) dai_ui_text(ui, x + GUT - 8.0f - nw, ry, nb,"""
t = sub(t, OLD, NEW, "Zeilennummern")

# ---- 5. Unterklapper + mehrzeiliges Feld -------------------------------
OLD = """void dai_ui_separator(dai_ui *ui) {"""
NEW = """// A fold INSIDE a component, Unity's shape: a small triangle, a dim label,
// no bar and no accent stripe. A second full-width header made "Constraints"
// read as a second component - which is exactly what it looked like.
int dai_ui_subheader(dai_ui *ui, const char *title, int *open) {
    if (!ui || !title) return 0;
    float h = dai_font_line_height(ui->font) + 4.0f;
    float x, y;
    next_rect(ui, 0, h, &x, &y);
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    uint64_t id = hash_id(title, x, y);
    bool over = inside_chk(ui, x, y, w, h);
    if (over) { ui->hot = id; ui->mouse_over_ui = true; }
    int clicked = 0;
    if (over && ui->input.mouse_down && !ui->prev.mouse_down) {
        if (open) *open = !*open;
        clicked = 1;
    }
    float tx = x + 4.0f;
    bool folded = (open && !*open);
    float isz = dai_icons_size(ui->icons);
    if (isz <= 0.0f || isz > h - 2.0f) isz = h - 4.0f;
    const char *chev = folded ? "chevron-right" : "chevron-down";
    if (dai_ui_has_icon(ui, chev)) {
        dai_ui_icon_at(ui, chev, tx, y + (h - isz) * 0.5f, isz, ui->style.text_dim);
        tx += isz + 3.0f;
    } else {
        float ax = tx + 5.0f, ay = y + h * 0.5f;
        if (folded) {
            dai_ui_line(ui, ax - 1.5f, ay - 3.5f, ax + 2.5f, ay, 1.6f, ui->style.text_dim);
            dai_ui_line(ui, ax + 2.5f, ay, ax - 1.5f, ay + 3.5f, 1.6f, ui->style.text_dim);
        } else {
            dai_ui_line(ui, ax - 3.5f, ay - 1.5f, ax, ay + 2.5f, 1.6f, ui->style.text_dim);
            dai_ui_line(ui, ax, ay + 2.5f, ax + 3.5f, ay - 1.5f, 1.6f, ui->style.text_dim);
        }
        tx += 14.0f;
    }
    dai_ui_text(ui, tx, y + 1.0f, title, over ? ui->style.text : ui->style.text_dim);
    return clicked;
}

// A multi-line text field. The code editor with the code turned off: it
// already knows about carets, selections, Enter and scrolling, and a second
// implementation of all four would be a second set of the same bugs.
//
// The per-field state lives here, keyed by the widget id, so callers keep
// passing a plain char buffer like every other field in this file.
int dai_ui_input_multiline(dai_ui *ui, const char *label, char *buf, size_t buf_size,
                           int rows) {
    if (!ui || !buf || buf_size < 2) return 0;
    if (rows < 2) rows = 3;
    float lh = dai_font_line_height(ui->font);
    float h = lh * (float)rows + 10.0f;
    float x, y, w;
    field_rect(ui, label, &x, &y, &w, h);
    uint64_t id = hash_id(label ? label : "multi", x, y);

    static std::vector<std::pair<uint64_t, dai_ui_code_state> > states;
    dai_ui_code_state *st = nullptr;
    for (size_t i = 0; i < states.size(); ++i)
        if (states[i].first == id) { st = &states[i].second; break; }
    if (!st) {
        dai_ui_code_state fresh{};
        fresh.plain = 1;
        states.push_back(std::make_pair(id, fresh));
        st = &states.back().second;
    }
    st->plain = 1;

    char sid[48];
    std::snprintf(sid, sizeof(sid), "ml%llu", (unsigned long long)id);
    dai_ui_rect(ui, x, y, w, h, ui->style.track);
    dai_ui_rect_outline(ui, x, y, w, h, 1.0f,
                        st->focused ? ui->style.accent : ui->style.panel_border);
    return dai_ui_code_edit(ui, sid, x + 1.0f, y + 1.0f, w - 2.0f, h - 2.0f,
                            buf, buf_size, st, DAI_CODE_LANG_NONE);
}

void dai_ui_separator(dai_ui *ui) {"""
t = sub(t, OLD, NEW, "dai_ui_separator")

shutil.copyfile(p, p + ".bak_p140")
io.open(p, "w", encoding="utf-8").write(t)
print("-- dai_ui.cpp")

# ------------------------------------------------------------------ dai_ui.h
p = os.path.join(ROOT, "include", "dai_ui.h")
t = io.open(p, encoding="utf-8").read()
t = sub(t, """    int   ac_open;
    int   ac_sel;
    int   ac_start;
} dai_ui_code_state;""",
"""    int   ac_open;
    int   ac_sel;
    int   ac_start;
    /* 1 = a plain multi-line field, not code: no gutter, no line numbers and
     * no completion. Set by dai_ui_input_multiline; leave it 0 for scripts. */
    int   plain;
} dai_ui_code_state;""", "code_state")

t = sub(t, "DAI_API void dai_ui_separator(dai_ui *ui);",
"""DAI_API void dai_ui_separator(dai_ui *ui);

/* A fold INSIDE a component: small triangle, dim label, no header bar. Unity
 * spells "Constraints" and "Info" this way, and for the reason it matters -
 * a full header makes a sub-section read as another component. */
DAI_API int  dai_ui_subheader(dai_ui *ui, const char *title, int *open);

/* A multi-line text field, `rows` lines tall. Enter puts in a line break
 * instead of committing. Returns 1 on any frame the buffer changed. */
DAI_API int  dai_ui_input_multiline(dai_ui *ui, const char *label, char *buf,
                                    size_t buf_size, int rows);""", "separator-Deklaration")
shutil.copyfile(p, p + ".bak_p140")
io.open(p, "w", encoding="utf-8").write(t)
print("-- dai_ui.h")
print("OK")
