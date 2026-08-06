#!/usr/bin/env python3
# Runde 28b - Inspector: Constraints wie in Unity, Luft unter Add Component,
# Bild sichtbar machen (auch wenn es NICHT laedt), mehrzeiliges Textfeld,
# {variablen} im Text.
import io, os, sys, shutil

ROOT = os.path.dirname(os.path.abspath(__file__))

def sub(s, old, new, what):
    if old not in s:
        sys.exit("!! nicht gefunden: " + what)
    return s.replace(old, new, 1)

# =================================================== dai_doc_text.cpp: Umbrueche
p = os.path.join(ROOT, "src", "dai_doc_text.cpp")
t = io.open(p, encoding="utf-8").read()
t = sub(t, '        if (r.text[0])                    put(s, "  textstr %s\\n", r.text);',
'''        if (r.text[0]) {
            // A label may hold line breaks now, and this file is one record
            // per LINE - so they go in escaped. A raw newline would end the
            // record and the rest of the label would be read as a key.
            std::string esc;
            for (const char *q = r.text; *q; ++q) {
                if (*q == '\\\\')      esc += "\\\\\\\\";
                else if (*q == '\\n') esc += "\\\\n";
                else if (*q == '\\r') { }
                else                 esc += *q;
            }
            put(s, "  textstr %s\\n", esc.c_str());
        }''', "textstr speichern")

t = sub(t, """            std::string v = b;
            while (!v.empty() && (v.back() == ' ' || v.back() == '\\t' || v.back() == '\\r')) v.pop_back();
            if (v.size() >= sizeof(rec.text)) ok = false;""",
"""            std::string raw = b;
            while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\\t' || raw.back() == '\\r')) raw.pop_back();
            // "\\n" back into a newline. Written by the escape above; a file
            // from before it simply has none, so old scenes are unchanged.
            std::string v;
            for (size_t q = 0; q < raw.size(); ++q) {
                if (raw[q] == '\\\\' && q + 1 < raw.size()) {
                    char nx = raw[q + 1];
                    if (nx == 'n')      { v += '\\n'; ++q; continue; }
                    if (nx == '\\\\')     { v += '\\\\'; ++q; continue; }
                }
                v += raw[q];
            }
            if (v.size() >= sizeof(rec.text)) ok = false;""", "textstr laden")
shutil.copyfile(p, p + ".bak_p141")
io.open(p, "w", encoding="utf-8").write(t)
print("-- dai_doc_text.cpp: Zeilenumbrueche im Label")

# =================================================== dai_editor_ui.cpp
p = os.path.join(ROOT, "src", "dai_editor_ui.cpp")
t = io.open(p, encoding="utf-8").read()

# ---- {variablen} im Text ------------------------------------------------
t = sub(t, "static dai_hud_image_fn g_hud_image = nullptr;",
'''// ---- {variables} in a label -----------------------------------------
// "Points: {score}" is the thing every HUD wants and the thing every engine
// makes you write a script for. The host resolves the name against the
// script running on THAT node; anything it does not know is left standing,
// so in the editor you see the placeholder where the number will be.
static dai_hud_var_fn g_hud_var = nullptr;
static void          *g_hud_var_user = nullptr;
void dai_hud_vars(dai_hud_var_fn fn, void *user) { g_hud_var = fn; g_hud_var_user = user; }

static void hud_expand_vars(dai_node n, const char *src, std::string &out) {
    out.clear();
    for (size_t i = 0; src[i]; ) {
        if (src[i] != '{') { out += src[i++]; continue; }
        size_t j = i + 1;
        while (src[j] && src[j] != '}' && src[j] != '{' && j - i < 64) ++j;
        if (src[j] != '}' || j == i + 1) { out += src[i++]; continue; }
        std::string name(src + i + 1, src + j);
        char val[128] = { 0 };
        if (g_hud_var && g_hud_var(n, name.c_str(), val, sizeof(val), g_hud_var_user)) {
            out += val;
        } else {
            out.append(src + i, src + j + 1);   // unknown: leave it visible
        }
        i = j + 1;
    }
}

static dai_hud_image_fn g_hud_image = nullptr;''', "g_hud_image")

t = sub(t, """        if (!txt || !txt[0]) continue;

        // Tags out, styles kept.""",
"""        if (!txt || !txt[0]) continue;

        // {score} -> what the script says. Before the markup, so a value can
        // carry its own colour tag, and before the wrap, because a number is
        // not the same width as its name.
        std::string expanded;
        if (std::strchr(txt, '{')) {
            hud_expand_vars(ids[i], txt, expanded);
            txt = expanded.c_str();
        }

        // Tags out, styles kept.""", "Variablen-Ersetzung")

# ---- Bild: auch sichtbar, wenn es nicht laedt --------------------------
OLD = """        if (r.image_on && !r.disabled && r.image[0] && g_hud_image) {
            float iw = 0.0f, ih = 0.0f;
            uint32_t tex = g_hud_image(r.image, &iw, &ih, g_hud_image_user);
            if (tex) {
                float dw"""
NEW = """        if (r.image_on && !r.disabled && r.image[0] && g_hud_image) {
            float iw = 0.0f, ih = 0.0f;
            uint32_t tex = g_hud_image(r.image, &iw, &ih, g_hud_image_user);
            {
                float dw"""
t = sub(t, OLD, NEW, "Image-Block-Kopf")

OLD = """                if (r.button_on && !r.disabled) {
                    btn = hud_button_state(ui, ids[i], ix, iy, dw, dh);
                    tint = hud_button_tint(r, btn, tint);
                }
                dai_ui_image_at(ui, tex, ix, iy, dw, dh, 0, 0, 1, 1, tint);
                g_hud_rects.push_back(HudRect{ ids[i], ix, iy, dw, dh });"""
NEW = """                if (r.button_on && !r.disabled) {
                    btn = hud_button_state(ui, ids[i], ix, iy, dw, dh);
                    tint = hud_button_tint(r, btn, tint);
                }
                if (tex) {
                    dai_ui_image_at(ui, tex, ix, iy, dw, dh, 0, 0, 1, 1, tint);
                } else {
                    // The file is named and it did not load. Before this the
                    // component drew NOTHING - no picture, no rectangle, no
                    // message - so "I see no image in the UI" was the only
                    // symptom there was, and it could not even be selected to
                    // ask what was wrong. Now it shows where it would be and
                    // says what it wanted.
                    dai_ui_rect(ui, ix, iy, dw, dh, 0x30FFFFFFu);
                    dai_ui_rect_outline(ui, ix, iy, dw, dh, 1.0f, 0xFF3D84D8u);
                    dai_ui_line(ui, ix, iy, ix + dw, iy + dh, 1.0f, 0x803D84D8u);
                    dai_ui_line(ui, ix + dw, iy, ix, iy + dh, 1.0f, 0x803D84D8u);
                    const char *bn = r.image;
                    for (const char *q = r.image; *q; ++q)
                        if (*q == '/' || *q == '\\\\') bn = q + 1;
                    dai_ui_text(ui, ix + 4.0f, iy + 3.0f, "image not loaded", 0xFFFFFFFFu);
                    dai_ui_text(ui, ix + 4.0f, iy + 3.0f + dai_ui_text_height(ui) + 2.0f,
                                bn, 0xFFC8C8C8u);
                }
                // Registered either way: an element you cannot select is an
                // element you cannot fix.
                g_hud_rects.push_back(HudRect{ ids[i], ix, iy, dw, dh });"""
t = sub(t, OLD, NEW, "Image-Zeichnung")

# ---- Constraints wie in Unity ------------------------------------------
OLD = """            if (r.motion == DAI_DYNAMIC) {
                dai_ui_spacing(p->ui, 2.0f);
                dai_ui_header(p->ui, "Constraints", &p->fold_freeze, nullptr);
                if (!p->fold_freeze) {
                    struct Bit { const char *label; uint32_t bit; };
                    static const Bit POS[3] = { { "X##fpx", DAI_FREEZE_POS_X },
                                                { "Y##fpy", DAI_FREEZE_POS_Y },
                                                { "Z##fpz", DAI_FREEZE_POS_Z } };
                    static const Bit ROT[3] = { { "X##frx", DAI_FREEZE_ROT_X },
                                                { "Y##fry", DAI_FREEZE_ROT_Y },
                                                { "Z##frz", DAI_FREEZE_ROT_Z } };
                    dai_ui_row(p->ui, 0.0f);
                    dai_ui_label(p->ui, "Freeze Position");
                    for (const Bit &b : POS) {
                        int fon = (r.freeze & b.bit) != 0;
                        if (dai_ui_checkbox(p->ui, b.label, &fon))
                            r.freeze = fon ? (r.freeze | b.bit) : (r.freeze & ~b.bit);
                    }
                    dai_ui_row_end(p->ui);
                    dai_ui_row(p->ui, 0.0f);
                    dai_ui_label(p->ui, "Freeze Rotation");
                    for (const Bit &b : ROT) {
                        int fon = (r.freeze & b.bit) != 0;
                        if (dai_ui_checkbox(p->ui, b.label, &fon))
                            r.freeze = fon ? (r.freeze | b.bit) : (r.freeze & ~b.bit);
                    }
                    dai_ui_row_end(p->ui);
                    // The two combinations anyone actually types out by hand.
                    dai_ui_row(p->ui, 0.0f);
                    if (dai_ui_button_fit(p->ui, "Upright")) r.freeze |= DAI_FREEZE_UPRIGHT;
                    if (dai_ui_button_fit(p->ui, "2D plane")) r.freeze |= DAI_FREEZE_2D;
                    dai_ui_row_end(p->ui);
                    if (r.freeze && dai_ui_button_fit(p->ui, "Clear constraints")) r.freeze = 0;
                }
            }"""
NEW = """            if (r.motion == DAI_DYNAMIC) {
                // Unity's shape, down to the indent: a small triangle, then
                // two labelled rows of three ticks. It used to be a full
                // header - which read as another component - and the ticks
                // were captioned "X##fpx", because the id half of the label
                // was being drawn on screen.
                int c_open = !p->fold_freeze;
                dai_ui_subheader(p->ui, "Constraints", &c_open);
                p->fold_freeze = !c_open;
                if (c_open) {
                    struct Bit { const char *label; uint32_t bit; };
                    static const Bit POS[3] = { { "X##fpx", DAI_FREEZE_POS_X },
                                                { "Y##fpy", DAI_FREEZE_POS_Y },
                                                { "Z##fpz", DAI_FREEZE_POS_Z } };
                    static const Bit ROT[3] = { { "X##frx", DAI_FREEZE_ROT_X },
                                                { "Y##fry", DAI_FREEZE_ROT_Y },
                                                { "Z##frz", DAI_FREEZE_ROT_Z } };
                    dai_ui_row(p->ui, 0.0f);
                    dai_ui_spacing(p->ui, 14.0f);          // the indent
                    dai_ui_label(p->ui, "Freeze Position");
                    for (const Bit &b : POS) {
                        int fon = (r.freeze & b.bit) != 0;
                        if (dai_ui_checkbox(p->ui, b.label, &fon))
                            r.freeze = fon ? (r.freeze | b.bit) : (r.freeze & ~b.bit);
                        dai_ui_spacing(p->ui, 6.0f);
                    }
                    dai_ui_row_end(p->ui);
                    dai_ui_row(p->ui, 0.0f);
                    dai_ui_spacing(p->ui, 14.0f);
                    dai_ui_label(p->ui, "Freeze Rotation");
                    for (const Bit &b : ROT) {
                        int fon = (r.freeze & b.bit) != 0;
                        if (dai_ui_checkbox(p->ui, b.label, &fon))
                            r.freeze = fon ? (r.freeze | b.bit) : (r.freeze & ~b.bit);
                        dai_ui_spacing(p->ui, 6.0f);
                    }
                    dai_ui_row_end(p->ui);
                    // The two combinations anyone actually types out by hand.
                    dai_ui_row(p->ui, 0.0f);
                    dai_ui_spacing(p->ui, 14.0f);
                    if (dai_ui_button_fit(p->ui, "Upright")) r.freeze |= DAI_FREEZE_UPRIGHT;
                    if (dai_ui_button_fit(p->ui, "2D plane")) r.freeze |= DAI_FREEZE_2D;
                    if (r.freeze && dai_ui_button_fit(p->ui, "Clear")) r.freeze = 0;
                    dai_ui_row_end(p->ui);
                }
            }"""
t = sub(t, OLD, NEW, "Constraints")

# ---- mehrzeiliges Textfeld ---------------------------------------------
OLD = """            char tbuf[192];
            std::snprintf(tbuf, sizeof(tbuf), "%s", r.text);
            if (dai_ui_input_text(p->ui, "Text", tbuf, sizeof(tbuf)))
                std::snprintf(r.text, sizeof(r.text), "%s", tbuf);"""
NEW = """            // Multi-line: Enter puts in a line break instead of committing.
            // A HUD label is a paragraph as often as it is a word, and
            // "<br>" was the only way to get a second line.
            char tbuf[192];
            std::snprintf(tbuf, sizeof(tbuf), "%s", r.text);
            if (dai_ui_input_multiline(p->ui, "Text", tbuf, sizeof(tbuf), 3))
                std::snprintf(r.text, sizeof(r.text), "%s", tbuf);"""
t = sub(t, OLD, NEW, "Textfeld")

t = sub(t, '''            dai_ui_label(p->ui, "markup: <b> <u> <s> <color=#ff0> <br>");''',
'''            dai_ui_label(p->ui, "markup: <b> <u> <s> <color=#ff0>   Enter = new line");
            dai_ui_label(p->ui, "variables: Points: {score}");
            dai_ui_help(p->ui, "{name} is read from this object's script while it plays: "
                               "a global, a field on state, or a @param. In the editor "
                               "the placeholder stays visible so you can see where it goes.");''',
    "Markup-Hinweis")

# ---- Luft unter Add Component ------------------------------------------
OLD = """        p->addcomp_node = dai_editor_selected(p->ed, 0);
    }"""
NEW = """        p->addcomp_node = dai_editor_selected(p->ed, 0);
    }

    // Room to scroll past the end. Without it the last widget sat ON the
    // bottom edge, and anything that opens DOWNWARDS - the colour wheel is
    // the obvious one - had nowhere to go: its numbers were simply cut off
    // and there was no way to scroll to them.
    dai_ui_spacing(p->ui, 220.0f);"""
t = sub(t, OLD, NEW, "Add-Component-Ende")

shutil.copyfile(p, p + ".bak_p141")
io.open(p, "w", encoding="utf-8").write(t)
print("-- dai_editor_ui.cpp")

# =================================================== dai_editor_ui.h
p = os.path.join(ROOT, "include", "dai_editor_ui.h")
t = io.open(p, encoding="utf-8").read()
t = sub(t, "/* The buttons clicked since the last call, and forgets them. */",
'''/* Resolves {name} inside a HUD label against the script on node `n`. Write
 * the value into `out` and return 1; return 0 for a name you do not know and
 * the placeholder is left on screen, which is what the editor wants. */
typedef int (*dai_hud_var_fn)(dai_node n, const char *name, char *out,
                              size_t out_size, void *user);
DAI_API void dai_hud_vars(dai_hud_var_fn fn, void *user);

/* The buttons clicked since the last call, and forgets them. */''', "hud_vars")
shutil.copyfile(p, p + ".bak_p141")
io.open(p, "w", encoding="utf-8").write(t)
print("-- dai_editor_ui.h")
print("OK")
