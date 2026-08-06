#!/usr/bin/env python3
# Runde 27e - "kann man dem Text-Component HTML geben, also fett/durchgestrichen"
#
# Ja - in der Schreibweise, die Leute ohnehin schon tippen (Unity's rich text,
# das ist HTML-artig): <b>, <u>, <s>, <color=#RRGGBB> und <br>.
#
# KEIN Kursiv: eine schraege Glyphe braucht eine schraege Glyphe, und der Atlas
# hat pro Zeichen genau eine Form. Dafuer gibt es das Font-Feld.
# Alles, was kein bekanntes Tag ist, bleibt Buchstabe fuer Buchstabe stehen -
# "<3" ist ein Herz und kein kaputtes Tag.
import io, os, sys, shutil

ROOT = os.path.dirname(os.path.abspath(__file__))
p = os.path.join(ROOT, "src", "dai_editor_ui.cpp")

def sub(s, old, new, what):
    if old not in s:
        sys.exit("!! nicht gefunden: " + what)
    return s.replace(old, new, 1)

t = io.open(p, encoding="utf-8").read()

# ---- Parser ------------------------------------------------------------
OLD = """// Breaks `text` into lines that fit `maxw` at scale `k`. maxw <= 0 means "do
// not wrap" - a score is one line however long the number gets."""
NEW = """// ---- rich text -------------------------------------------------------
// The spelling people already type, Unity's: <b>, <u>, <s>, <color=#RRGGBB>
// and <br>. Anything that is not one of those is left EXACTLY as it was
// typed - "<3" is a heart, "a < b" is a comparison, and a parser that eats
// them is a parser people switch off.
//
// There is no <i>. Slanting a glyph needs a slanted glyph and the atlas holds
// one shape per character; the honest answer is an italic .ttf in the Font
// field. <i> is accepted and does nothing rather than silently printing
// "<i>" in the middle of a sentence.
enum { HUD_B = 1, HUD_U = 2, HUD_S = 4 };
struct HudStyle { uint8_t f; uint32_t col; };   // col 0 = the label's own

// #RGB, #RRGGBB, #RRGGBBAA or one of the names everybody tries first.
// 0 means "not a colour" - the label's own is kept.
static uint32_t hud_parse_col(const char *v) {
    while (*v == ' ' || *v == '"' || *v == '\\'') ++v;
    if (*v == '#') {
        ++v;
        uint32_t val = 0;
        int n = 0;
        for (; n < 8; ++n) {
            char c = v[n];
            int d;
            if (c >= '0' && c <= '9')      d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            val = (val << 4) | (uint32_t)d;
        }
        uint32_t r8 = 0, g8 = 0, b8 = 0, a8 = 255;
        if (n == 3) {
            r8 = ((val >> 8) & 0xF) * 17; g8 = ((val >> 4) & 0xF) * 17; b8 = (val & 0xF) * 17;
        } else if (n == 6) {
            r8 = (val >> 16) & 0xFF; g8 = (val >> 8) & 0xFF; b8 = val & 0xFF;
        } else if (n == 8) {
            r8 = (val >> 24) & 0xFF; g8 = (val >> 16) & 0xFF; b8 = (val >> 8) & 0xFF; a8 = val & 0xFF;
        } else {
            return 0;
        }
        return (a8 << 24) | (b8 << 16) | (g8 << 8) | r8;
    }
    struct Named { const char *name; uint32_t argb; };
    static const Named NAMES[] = {
        { "red", 0xFF0000FFu }, { "green", 0xFF00FF00u }, { "blue", 0xFFFF0000u },
        { "white", 0xFFFFFFFFu }, { "black", 0xFF000000u }, { "yellow", 0xFF00FFFFu },
        { "cyan", 0xFFFFFF00u }, { "magenta", 0xFFFF00FFu }, { "orange", 0xFF00A5FFu },
        { "grey", 0xFF808080u }, { "gray", 0xFF808080u },
    };
    for (const Named &nm : NAMES) {
        size_t l = std::strlen(nm.name);
        if (std::strncmp(v, nm.name, l) == 0 && (v[l] == 0 || v[l] == ' ' || v[l] == '"'))
            return nm.argb;
    }
    return 0;
}

// The words with the tags taken out, plus one style per remaining character.
// Per CHARACTER and not per run, because the wrap happens afterwards and a
// run that a line break falls inside has to survive being cut in two.
static void hud_rich(const char *src, std::string &plain, std::vector<HudStyle> &out) {
    plain.clear();
    out.clear();
    uint8_t f = 0;
    uint32_t col = 0;
    std::vector<uint32_t> stack;
    for (size_t i = 0; src[i]; ) {
        if (src[i] == '<') {
            size_t j = i + 1;
            while (src[j] && src[j] != '>' && j - i < 48) ++j;
            if (src[j] == '>') {
                std::string low;
                for (size_t q = i + 1; q < j; ++q) {
                    char c = src[q];
                    low += (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
                }
                bool known = true;
                if      (low == "b")  f |= HUD_B;
                else if (low == "/b") f = (uint8_t)(f & ~HUD_B);
                else if (low == "u")  f |= HUD_U;
                else if (low == "/u") f = (uint8_t)(f & ~HUD_U);
                else if (low == "s" || low == "strike")   f |= HUD_S;
                else if (low == "/s" || low == "/strike") f = (uint8_t)(f & ~HUD_S);
                else if (low == "i" || low == "/i" || low == "em" || low == "/em") { }
                else if (low == "br" || low == "br/" || low == "/br") {
                    plain += '\\n';
                    out.push_back(HudStyle{ f, col });
                } else if (low.compare(0, 6, "color=") == 0) {
                    uint32_t c = hud_parse_col(low.c_str() + 6);
                    if (c) { stack.push_back(col); col = c; }
                    else known = false;
                } else if (low == "/color") {
                    if (!stack.empty()) { col = stack.back(); stack.pop_back(); }
                    else col = 0;
                } else {
                    known = false;
                }
                if (known) { i = j + 1; continue; }
            }
        }
        plain += src[i];
        out.push_back(HudStyle{ f, col });
        ++i;
    }
}

// Breaks `text` into lines that fit `maxw` at scale `k`. maxw <= 0 means "do
// not wrap" - a score is one line however long the number gets."""
t = sub(t, OLD, NEW, "hud_wrap-Kommentar")

# ---- vor dem Umbruch parsen -------------------------------------------
OLD = """        if (!txt || !txt[0]) continue;

        float px = r.text_size > 0.0f ? r.text_size : 24.0f;"""
NEW = """        if (!txt || !txt[0]) continue;

        // Tags out, styles kept. Everything below works on the WORDS - the
        // wrap, the widths, the autosize - because "<b>" is not two
        // characters wide on screen and a layout that measures it is wrong by
        // exactly the length of the markup.
        std::string plain;
        std::vector<HudStyle> rich;
        hud_rich(txt, plain, rich);
        if (plain.empty()) continue;

        float px = r.text_size > 0.0f ? r.text_size : 24.0f;"""
t = sub(t, OLD, NEW, "Textquelle")

t = sub(t, "            hud_wrap(ui, txt, boxw > 0.0f ? boxw : 0.0f, k, lines);",
           "            hud_wrap(ui, plain, boxw > 0.0f ? boxw : 0.0f, k, lines);",
        "hud_wrap-Aufruf")

# ---- Zeichnen in Laeufen ----------------------------------------------
OLD = """        for (size_t li = 0; li < lines.size(); ++li) {
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
        }"""
NEW = """        // Where each wrapped line sits in the unwrapped words, so a line
        // knows which styles are its own. Searched forward rather than
        // counted: the wrap drops the space it broke at, and counting would
        // drift by one character per line for the rest of the paragraph.
        size_t cur = 0;
        for (size_t li = 0; li < lines.size(); ++li) {
            const std::string &ln = lines[li];
            size_t at = plain.find(ln, cur);
            if (at == std::string::npos) at = cur;
            cur = at + ln.size();

            // Centre and right anchors centre EACH line inside the block, so
            // a two line centred title looks centred rather than ragged.
            float lw = dai_ui_text_width(ui, ln.c_str()) * k;
            float lx = bx + (widest - lw) * (col * 0.5f);
            float ly = by + lh * (float)li;
            float th = dai_ui_text_height(ui) * k;
            float rule = th * 0.075f < 1.0f ? 1.0f : th * 0.075f;

            // Runs of one style. Plain text is ONE run, so the common case is
            // the same two draws it always was.
            size_t s0 = 0;
            float rx = lx;
            while (s0 < ln.size()) {
                HudStyle sy = at + s0 < rich.size() ? rich[at + s0] : HudStyle{ 0, 0 };
                size_t s1 = s0 + 1;
                while (s1 < ln.size()) {
                    HudStyle s2 = at + s1 < rich.size() ? rich[at + s1] : HudStyle{ 0, 0 };
                    if (s2.f != sy.f || s2.col != sy.col) break;
                    ++s1;
                }
                std::string run = ln.substr(s0, s1 - s0);
                float rw = dai_ui_text_width(ui, run.c_str()) * k;
                uint32_t rc = sy.col ? sy.col : col32;
                // A one pixel shadow. Not decoration: white text on a bright
                // sky is unreadable, and every game HUD in existence does this.
                dai_ui_text_scaled(ui, rx + 1.0f * scale, ly + 1.0f * scale, run.c_str(),
                                   0xB0000000u, k);
                dai_ui_text_scaled(ui, rx, ly, run.c_str(), rc, k);
                // Bold out of one face: the same run again, a hair to the
                // right. Not a real bold cut and it does not pretend to be
                // one - it is what a single atlas can honestly do.
                if (sy.f & HUD_B) dai_ui_text_scaled(ui, rx + 0.9f, ly, run.c_str(), rc, k);
                if (sy.f & HUD_U) dai_ui_rect(ui, rx, ly + th * 0.98f, rw, rule, rc);
                if (sy.f & HUD_S) dai_ui_rect(ui, rx, ly + th * 0.55f, rw, rule, rc);
                rx += rw;
                s0 = s1;
            }
        }"""
t = sub(t, OLD, NEW, "Textzeichnung")

# ---- Hilfetext im Inspector -------------------------------------------
OLD = """            dai_ui_help(p->ui, "The words, or \\"@key\\" to look them up in the "
                               "project's string table");"""
NEW = """            dai_ui_help(p->ui, "The words, or \\"@key\\" to look them up in the "
                               "project's string table");
            dai_ui_label(p->ui, "markup: <b> <u> <s> <color=#ff0> <br>");
            dai_ui_help(p->ui, "HTML-style tags, Unity's spelling. No <i>: an italic "
                               "glyph needs an italic font - put one in Font below. "
                               "Anything that is not a known tag stays as typed.");"""
t = sub(t, OLD, NEW, "Text-Hilfe")

shutil.copyfile(p, p + ".bak_p139")
io.open(p, "w", encoding="utf-8").write(t)
print("-- dai_editor_ui.cpp: Rich Text (<b> <u> <s> <color=..> <br>)")
print("OK")
