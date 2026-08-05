#!/usr/bin/env python3
# patch56 - a code editor widget. Multi-line, line numbers, selection, syntax
# colour. No dependency, no second process, no browser.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p56'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# =================================================================== header
s = rd('include/dai_ui.h')
s = sub1(s,
"""DAI_API int  dai_ui_text_active(const dai_ui *ui);""",
"""DAI_API int  dai_ui_text_active(const dai_ui *ui);

/* ---- the code editor ----------------------------------------------------
 *
 * A multi-line text widget that edits the caller's buffer in place. It is not
 * a text field with newlines: a code editor has to keep a caret across lines,
 * a selection that spans them, a viewport that follows the caret, and colour
 * that comes from what the text MEANS - and a field that does none of those
 * is the reason people alt-tab to a real editor and never come back.
 *
 * The state is the caller's so several editors can be open at once (one per
 * file, which is what a tab bar is).
 *
 *   static dai_ui_code_state st = {0};
 *   dai_ui_code_edit(ui, "script", x, y, w, h, buf, sizeof(buf), &st,
 *                    DAI_CODE_LANG_JS);
 *
 * Returns 1 on any frame the buffer changed. */
typedef struct dai_ui_code_state {
    int   caret;        /* byte offset of the caret                          */
    int   anchor;       /* the other end of the selection; == caret when none */
    float scroll_x, scroll_y;
    int   focused;
    int   want_focus;   /* set to 1 to take the keyboard on the next draw    */
    float blink;
    float prefer_x;     /* remembered column, so up/down do not drift left   */
    int   dragging;
} dai_ui_code_state;

enum {
    DAI_CODE_LANG_NONE = 0,
    DAI_CODE_LANG_JS   = 1,
    DAI_CODE_LANG_CPP  = 2
};

DAI_API int dai_ui_code_edit(dai_ui *ui, const char *id, float x, float y,
                             float w, float h, char *buf, size_t buf_size,
                             dai_ui_code_state *st, int lang);
/* Which line the caret is on (1 based) and its column, for a status bar. */
DAI_API void dai_ui_code_caret_pos(const char *buf, int caret, int *line, int *col);""",
    'code editor decl')
wr('include/dai_ui.h', s)

# =============================================================== the widget
s = rd('src/dai_ui.cpp')
s = sub1(s,
"""int  dai_ui_text_active(const dai_ui *ui) { return ui && ui->edit.editing ? 1 : 0; }""",
"""int  dai_ui_text_active(const dai_ui *ui) {
    return ui && (ui->edit.editing || ui->code_focus) ? 1 : 0;
}""",
    'text_active includes code')

s = sub1(s,
"""    // layout cursor
    float cursor_x = 0, cursor_y = 0;""",
"""    // The code editor has the keyboard. Kept next to edit.editing because the
    // host asks one question - "is the user typing" - and must get one answer.
    int code_focus = 0;

    // layout cursor
    float cursor_x = 0, cursor_y = 0;""",
    'code_focus field')

s = sub1(s,
"""int dai_ui_option(dai_ui *ui, const char *label, int *value,
                  const char *const *items, int count) {""",
"""// ---- the code editor ------------------------------------------------------

namespace {

// The keywords worth colouring. Deliberately short: a list that tries to be
// complete is a list that is wrong for the next language, and the value of
// syntax colour is almost entirely in "string", "comment", "everything else".
const char *const CODE_KW[] = {
    "var", "let", "const", "function", "return", "if", "else", "for", "while",
    "do", "break", "continue", "new", "delete", "typeof", "this", "null",
    "true", "false", "undefined", "switch", "case", "default", "try", "catch",
    "throw", "class", "extends", "in", "of",
    /* the C++ half, for .cpp behaviours */
    "int", "float", "double", "bool", "void", "char", "struct", "auto",
    "static", "public", "private", "namespace", "include", "define", "using",
    "nullptr", "template", "unsigned", "size_t", "const_cast"
};

bool code_is_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '#';
}

bool code_is_keyword(const char *s, int n) {
    for (const char *k : CODE_KW) {
        int i = 0;
        while (i < n && k[i] && k[i] == s[i]) ++i;
        if (i == n && !k[i]) return true;
    }
    return false;
}

int code_line_start(const char *b, int off) {
    while (off > 0 && b[off - 1] != '\\n') --off;
    return off;
}
int code_line_end(const char *b, int off) {
    while (b[off] && b[off] != '\\n') ++off;
    return off;
}
int code_count_lines(const char *b) {
    int n = 1;
    for (const char *c = b; *c; ++c) if (*c == '\\n') ++n;
    return n;
}
int code_line_of(const char *b, int off) {
    int n = 0;
    for (int i = 0; i < off && b[i]; ++i) if (b[i] == '\\n') ++n;
    return n;
}
int code_offset_of_line(const char *b, int line) {
    int n = 0, i = 0;
    while (b[i] && n < line) { if (b[i] == '\\n') ++n; ++i; }
    return i;
}

// The width of a run of bytes in the current font, without building a string.
float code_run_w(dai_ui *ui, const char *s, int n) {
    if (n <= 0) return 0.0f;
    char tmp[512];
    float total = 0.0f;
    while (n > 0) {
        int chunk = n < (int)sizeof(tmp) - 1 ? n : (int)sizeof(tmp) - 1;
        std::memcpy(tmp, s, (size_t)chunk);
        tmp[chunk] = 0;
        total += dai_ui_text_width(ui, tmp);
        s += chunk; n -= chunk;
    }
    return total;
}

} // namespace

void dai_ui_code_caret_pos(const char *buf, int caret, int *line, int *col) {
    if (!buf) { if (line) *line = 1; if (col) *col = 1; return; }
    int ls = code_line_start(buf, caret);
    if (line) *line = code_line_of(buf, caret) + 1;
    if (col)  *col = caret - ls + 1;
}

int dai_ui_code_edit(dai_ui *ui, const char *id, float x, float y, float w, float h,
                     char *buf, size_t buf_size, dai_ui_code_state *st, int lang) {
    if (!ui || !buf || !st || buf_size < 2 || w < 40.0f || h < 20.0f) return 0;
    (void)lang;
    const dai_ui_style *sty = &ui->style;
    int len = (int)std::strlen(buf);
    if (st->caret > len) st->caret = len;
    if (st->anchor > len) st->anchor = len;
    if (st->caret < 0) st->caret = 0;
    if (st->anchor < 0) st->anchor = 0;

    const float LH = dai_font_line_height(ui->font) + 2.0f;
    int nlines = code_count_lines(buf);
    char gut[16];
    std::snprintf(gut, sizeof(gut), "%d", nlines < 100 ? 100 : nlines);
    const float GUT = dai_ui_text_width(ui, gut) + 14.0f;
    const float TEXT_X = x + GUT + 6.0f;
    const float VIEW_W = w - GUT - 10.0f;

    uint64_t wid = hash_id(id ? id : "code", x, y);
    float mx = ui->input.mouse_x, my = ui->input.mouse_y;
    bool over = inside_chk(ui, x, y, w, h);
    bool pressed = ui->input.mouse_down && !ui->prev.mouse_down;
    if (over) ui->mouse_over_ui = true;

    // ---- focus ------------------------------------------------------------
    if (st->want_focus) { st->focused = 1; st->want_focus = 0; }
    if (pressed) st->focused = over ? 1 : 0;
    if (st->focused) ui->code_focus = 1;

    // ---- the plate ---------------------------------------------------------
    dai_ui_rect(ui, x, y, w, h, sty->track);
    dai_ui_rect(ui, x, y, GUT, h, (sty->chrome & 0x00FFFFFFu) | 0xFF000000u);
    dai_ui_rect(ui, x + GUT, y, 1.0f, h, sty->panel_border);
    dai_ui_rect_outline(ui, x, y, w, h, 1.0f,
                        st->focused ? sty->accent : sty->panel_border);

    // ---- where is the caret, in pixels -------------------------------------
    auto caret_xy = [&](int off, float *ox, float *oy) {
        int ls = code_line_start(buf, off);
        *ox = TEXT_X + code_run_w(ui, buf + ls, off - ls);
        *oy = y + 4.0f + (float)code_line_of(buf, off) * LH;
    };
    // ...and the reverse: which offset is under this point.
    auto offset_at = [&](float px, float py) {
        int line = (int)((py - (y + 4.0f) + st->scroll_y) / LH);
        if (line < 0) line = 0;
        if (line > nlines - 1) line = nlines - 1;
        int ls = code_offset_of_line(buf, line);
        int le = code_line_end(buf, ls);
        float want = px - TEXT_X + st->scroll_x;
        if (want <= 0) return ls;
        int best = ls;
        float acc = 0.0f;
        for (int i = ls; i < le; ++i) {
            char one[2] = { buf[i], 0 };
            float cw = dai_ui_text_width(ui, one);
            if (acc + cw * 0.5f > want) return best;
            acc += cw;
            best = i + 1;
        }
        return le;
    };

    // ---- mouse: caret and selection ----------------------------------------
    if (pressed && over && mx > x + GUT) {
        st->caret = st->anchor = offset_at(mx, my);
        st->dragging = 1;
        st->focused = 1;
        dai_ui_claim_mouse(ui);
        ui->active = wid;
        // A double click takes the word under it - the one selection gesture
        // everybody uses without being taught.
        if (ui->input.double_click) {
            int a = st->caret, b = st->caret;
            while (a > 0 && code_is_word(buf[a - 1])) --a;
            while (buf[b] && code_is_word(buf[b])) ++b;
            st->anchor = a; st->caret = b;
            st->dragging = 0;
        }
    }
    if (st->dragging) {
        if (ui->input.mouse_down) {
            st->caret = offset_at(mx, my);
            dai_ui_claim_mouse(ui);
        } else st->dragging = 0;
    }
    if (over) {
        ui->cursor_want = DAI_CURSOR_TEXT;
        st->scroll_y -= ui->input.wheel * LH * 3.0f;
    }

    // ---- keyboard -----------------------------------------------------------
    int changed = 0;
    auto sel_lo = [&]() { return st->caret < st->anchor ? st->caret : st->anchor; };
    auto sel_hi = [&]() { return st->caret > st->anchor ? st->caret : st->anchor; };
    auto erase = [&](int a, int b) {
        if (b <= a) return;
        std::memmove(buf + a, buf + b, (size_t)(len - b + 1));
        len -= (b - a);
        st->caret = st->anchor = a;
        changed = 1;
    };
    auto insert = [&](const char *txt, int n) {
        if (n <= 0) return;
        if (sel_hi() > sel_lo()) erase(sel_lo(), sel_hi());
        if ((size_t)(len + n + 1) > buf_size) return;
        std::memmove(buf + st->caret + n, buf + st->caret, (size_t)(len - st->caret + 1));
        std::memcpy(buf + st->caret, txt, (size_t)n);
        len += n;
        st->caret += n;
        st->anchor = st->caret;
        changed = 1;
    };

    if (st->focused) {
        const dai_ui_input &in = ui->input;
        bool shift = in.key_shift != 0;
        int before = st->caret;

        for (int i = 0; i < 8 && in.text[i]; ++i) {
            uint32_t cp = in.text[i];
            if (cp < 0x20 || cp == 0x7F) continue;
            char utf[4];
            int n = 0;
            if (cp < 0x80) { utf[0] = (char)cp; n = 1; }
            else if (cp < 0x800) { utf[0] = (char)(0xC0 | (cp >> 6)); utf[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
            else { utf[0] = (char)(0xE0 | (cp >> 12)); utf[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); utf[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
            insert(utf, n);
        }
        if (in.key_enter) {
            // Auto-indent: a new line starts where the old one's text starts.
            // Without it every block has to be re-indented by hand, and an
            // editor that fights the shape of the code is not used twice.
            int ls = code_line_start(buf, st->caret);
            char pad[64];
            int np = 0;
            while (ls + np < len && np < 60 && (buf[ls + np] == ' ' || buf[ls + np] == '\\t'))
                { pad[np + 1] = buf[ls + np]; ++np; }
            pad[0] = '\\n';
            insert(pad, np + 1);
        }
        if (in.key_tab) insert("    ", 4);
        if (in.key_backspace) {
            if (sel_hi() > sel_lo()) erase(sel_lo(), sel_hi());
            else if (st->caret > 0) {
                int a = st->caret - 1;
                while (a > 0 && ((unsigned char)buf[a] & 0xC0) == 0x80) --a;  // whole code point
                erase(a, st->caret);
            }
        }
        if (in.key_delete) {
            if (sel_hi() > sel_lo()) erase(sel_lo(), sel_hi());
            else if (st->caret < len) {
                int b = st->caret + 1;
                while (b < len && ((unsigned char)buf[b] & 0xC0) == 0x80) ++b;
                erase(st->caret, b);
            }
        }
        if (in.key_left && st->caret > 0) {
            --st->caret;
            while (st->caret > 0 && ((unsigned char)buf[st->caret] & 0xC0) == 0x80) --st->caret;
        }
        if (in.key_right && st->caret < len) {
            ++st->caret;
            while (st->caret < len && ((unsigned char)buf[st->caret] & 0xC0) == 0x80) ++st->caret;
        }
        if (in.key_home) st->caret = code_line_start(buf, st->caret);
        if (in.key_end)  st->caret = code_line_end(buf, st->caret);
        if (in.key_up_arrow || in.key_down_arrow) {
            int line = code_line_of(buf, st->caret);
            int ls = code_line_start(buf, st->caret);
            float want = code_run_w(ui, buf + ls, st->caret - ls);
            int tgt = line + (in.key_down_arrow ? 1 : -1);
            if (tgt < 0) tgt = 0;
            if (tgt > nlines - 1) tgt = nlines - 1;
            int ts = code_offset_of_line(buf, tgt), te = code_line_end(buf, ts);
            int best = ts;
            float acc = 0.0f;
            for (int i = ts; i < te; ++i) {
                char one[2] = { buf[i], 0 };
                float cw = dai_ui_text_width(ui, one);
                if (acc + cw * 0.5f > want) break;
                acc += cw;
                best = i + 1;
            }
            st->caret = best;
        }
        if (in.key_select_all) { st->anchor = 0; st->caret = len; }
        else if (st->caret != before && !shift) st->anchor = st->caret;
        else if (st->caret != before && shift) { /* anchor stays: that IS a selection */ }
        if (changed) st->anchor = st->caret;
    }

    // ---- keep the caret in view --------------------------------------------
    {
        float cx, cy;
        caret_xy(st->caret, &cx, &cy);
        float rel_y = cy - (y + 4.0f);
        if (rel_y - st->scroll_y < 0.0f) st->scroll_y = rel_y;
        if (rel_y - st->scroll_y > h - LH - 8.0f) st->scroll_y = rel_y - (h - LH - 8.0f);
        float rel_x = cx - TEXT_X;
        if (rel_x - st->scroll_x < 0.0f) st->scroll_x = rel_x;
        if (rel_x - st->scroll_x > VIEW_W - 20.0f) st->scroll_x = rel_x - (VIEW_W - 20.0f);
    }
    float max_y = (float)nlines * LH - (h - 8.0f);
    if (max_y < 0.0f) max_y = 0.0f;
    if (st->scroll_y > max_y) st->scroll_y = max_y;
    if (st->scroll_y < 0.0f) st->scroll_y = 0.0f;
    if (st->scroll_x < 0.0f) st->scroll_x = 0.0f;

    // ---- draw ---------------------------------------------------------------
    const uint32_t C_TEXT    = sty->text;
    const uint32_t C_COMMENT = rgba(0x6A, 0x9B, 0x6A, 255);
    const uint32_t C_STRING  = rgba(0xCE, 0x9A, 0x63, 255);
    const uint32_t C_NUMBER  = rgba(0xB5, 0xCE, 0xA8, 255);
    const uint32_t C_KEYWORD = rgba(0x86, 0xB3, 0xE8, 255);
    const uint32_t C_LINENO  = sty->text_dim;

    dai_ui_clip_begin(ui, x + 1.0f, y + 1.0f, w - 2.0f, h - 2.0f);
    int first_line = (int)(st->scroll_y / LH);
    if (first_line < 0) first_line = 0;
    int last_line = first_line + (int)(h / LH) + 2;
    if (last_line > nlines) last_line = nlines;
    int caret_line = code_line_of(buf, st->caret);
    int lo = sel_lo(), hi = sel_hi();

    // A block comment can start on a line above the first one drawn, so the
    // scan begins at the top of the file. Only the state is carried, not the
    // drawing - this costs one pass over the bytes and nothing else.
    bool in_block = false;
    {
        int upto = code_offset_of_line(buf, first_line);
        for (int i = 0; i + 1 < upto; ++i) {
            if (!in_block && buf[i] == '/' && buf[i + 1] == '*') { in_block = true; ++i; }
            else if (in_block && buf[i] == '*' && buf[i + 1] == '/') { in_block = false; ++i; }
        }
    }

    for (int ln = first_line; ln < last_line; ++ln) {
        float ry = y + 4.0f + (float)ln * LH - st->scroll_y;
        int ls = code_offset_of_line(buf, ln), le = code_line_end(buf, ls);

        if (ln == caret_line && st->focused)
            dai_ui_rect(ui, x + GUT + 1.0f, ry - 1.0f, w - GUT - 2.0f, LH,
                        (sty->accent & 0x00FFFFFFu) | 0x18000000u);

        char nb[16];
        std::snprintf(nb, sizeof(nb), "%d", ln + 1);
        float nw = dai_ui_text_width(ui, nb);
        dai_ui_text(ui, x + GUT - 8.0f - nw, ry, nb,
                    ln == caret_line ? sty->text : C_LINENO);

        // selection band
        if (hi > lo && hi > ls && lo <= le) {
            int a = lo > ls ? lo : ls, b = hi < le ? hi : le;
            float ax = TEXT_X + code_run_w(ui, buf + ls, a - ls) - st->scroll_x;
            float bw = code_run_w(ui, buf + a, b - a);
            if (b == le && hi > le) bw += 6.0f;      // the newline, visibly
            dai_ui_rect(ui, ax, ry - 1.0f, bw, LH, (sty->accent & 0x00FFFFFFu) | 0x66000000u);
        }

        // ---- one line, token by token -------------------------------------
        float tx = TEXT_X - st->scroll_x;
        int i = ls;
        while (i < le) {
            int start = i;
            uint32_t col = C_TEXT;
            if (in_block) {
                while (i < le && !(buf[i] == '*' && i + 1 < le && buf[i + 1] == '/')) ++i;
                if (i < le) { i += 2; in_block = false; }
                col = C_COMMENT;
            } else if (buf[i] == '/' && i + 1 < le && buf[i + 1] == '/') {
                i = le; col = C_COMMENT;
            } else if (buf[i] == '/' && i + 1 < le && buf[i + 1] == '*') {
                in_block = true; i += 2;
                while (i < le && !(buf[i] == '*' && i + 1 < le && buf[i + 1] == '/')) ++i;
                if (i < le) { i += 2; in_block = false; }
                col = C_COMMENT;
            } else if (buf[i] == '"' || buf[i] == '\\'') {
                char q = buf[i++];
                while (i < le && buf[i] != q) { if (buf[i] == '\\\\' && i + 1 < le) ++i; ++i; }
                if (i < le) ++i;
                col = C_STRING;
            } else if (buf[i] >= '0' && buf[i] <= '9') {
                while (i < le && ((buf[i] >= '0' && buf[i] <= '9') || buf[i] == '.')) ++i;
                col = C_NUMBER;
            } else if (code_is_word(buf[i])) {
                while (i < le && code_is_word(buf[i])) ++i;
                col = code_is_keyword(buf + start, i - start) ? C_KEYWORD : C_TEXT;
            } else {
                ++i;
                col = C_TEXT;
            }
            int n = i - start;
            if (n > 0 && tx < x + w) {
                char tmp[512];
                int cn = n < (int)sizeof(tmp) - 1 ? n : (int)sizeof(tmp) - 1;
                std::memcpy(tmp, buf + start, (size_t)cn);
                tmp[cn] = 0;
                if (tx + code_run_w(ui, tmp, cn) > x + GUT)   // skip what is left of view
                    dai_ui_text(ui, tx, ry, tmp, col);
                tx += code_run_w(ui, tmp, cn);
            }
        }
    }

    // the caret itself
    if (st->focused) {
        st->blink += 1.0f / 60.0f;
        if (st->blink > 1.06f) st->blink = 0.0f;
        if (st->blink < 0.66f) {
            float cx, cy;
            caret_xy(st->caret, &cx, &cy);
            dai_ui_rect(ui, cx - st->scroll_x, cy - st->scroll_y - 1.0f, 1.5f, LH, sty->text);
        }
    }
    dai_ui_clip_end(ui);
    return changed;
}

int dai_ui_option(dai_ui *ui, const char *label, int *value,
                  const char *const *items, int count) {""",
    'code editor impl')
wr('src/dai_ui.cpp', s)
print('patch56 ok')
