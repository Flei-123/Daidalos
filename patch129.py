import io

p = 'src/dai_ui.cpp'
s = io.open(p, encoding='utf-8').read()

# Die Liste wird NACH der Eingabe gebaut (sie haengt am Wort unter dem Cursor)
# und vor dem Ende gezeichnet.
old = """    // ---- the plate ---------------------------------------------------------
    dai_ui_rect(ui, x, y, w, h, sty->track);"""
new = """    // ---- the plate ---------------------------------------------------------
    dai_ui_rect(ui, x, y, w, h, sty->track);"""
assert s.count(old) == 1

# Am Ende der Funktion: Liste bauen, ggf. uebernehmen, zeichnen.
import re
m = re.search(r'\n(\s*)return changed;\n\}\n', s[s.index('int dai_ui_code_edit('):])
assert m, 'end of code_edit not found'
tail = m.group(0)
idx = s.index('int dai_ui_code_edit(') + m.start()
old_tail = s[idx:idx + len(tail)]

new_tail = """

    // ---- autocomplete ------------------------------------------------------
    // Built here, at the end, because it depends on the word under the caret
    // AFTER this frame's typing. The list is what the engine offers plus what
    // the file already contains, and nothing else: no parse, no types, no
    // guessing what an expression evaluates to. A half parser is wrong on
    // exactly the lines you are in the middle of writing.
    {
        // The word being typed: letters back from the caret. A completion
        // that triggers on one character would pop up on every `i`.
        int a = st->caret;
        while (a > 0 && code_is_word(buf[a - 1])) --a;
        std::string prefix(buf + a, buf + st->caret);
        // A dotted call is one word for this purpose: typing "input.k" should
        // offer input.key, and the dot is not a word character.
        int dotted = a;
        if (dotted > 0 && buf[dotted - 1] == '.') {
            int b = dotted - 1;
            while (b > 0 && code_is_word(buf[b - 1])) --b;
            prefix = std::string(buf + b, buf + st->caret);
            a = b;
        } else if (dotted > 1 && buf[dotted - 1] == '>' && buf[dotted - 2] == '-') {
            int b = dotted - 2;
            while (b > 0 && code_is_word(buf[b - 1])) --b;
            prefix = std::string(buf + b, buf + st->caret);
            a = b;
        }

        std::vector<const AcEntry *> hits;
        std::vector<std::string> words;
        if (st->focused && prefix.size() >= 2) {
            const AcEntry *table = lang == DAI_CODE_LANG_CPP ? AC_CPP : AC_JS;
            size_t count = lang == DAI_CODE_LANG_CPP
                         ? sizeof(AC_CPP) / sizeof(AC_CPP[0])
                         : sizeof(AC_JS) / sizeof(AC_JS[0]);
            for (size_t i = 0; i < count; ++i)
                if (std::strncmp(table[i].text, prefix.c_str(), prefix.size()) == 0)
                    hits.push_back(&table[i]);
            ac_identifiers(buf, prefix, a, words);
        }
        int total = (int)hits.size() + (int)words.size();
        if (total > 8) total = 8;
        st->ac_open = total;
        st->ac_start = a;
        if (st->ac_sel >= total) st->ac_sel = 0;
        if (st->ac_sel < 0) st->ac_sel = 0;

        if (total > 0 && ac_take) {
            const char *pick = st->ac_sel < (int)hits.size()
                             ? hits[(size_t)st->ac_sel]->text
                             : words[(size_t)(st->ac_sel - (int)hits.size())].c_str();
            // Replace the typed prefix, do not append to it.
            int plen = st->caret - a;
            if (plen > 0) {
                std::memmove(buf + a, buf + st->caret, (size_t)(len - st->caret + 1));
                len -= plen;
                st->caret = a;
                st->anchor = a;
            }
            int n = (int)std::strlen(pick);
            if ((size_t)(len + n + 1) <= buf_size) {
                std::memmove(buf + st->caret + n, buf + st->caret, (size_t)(len - st->caret + 1));
                std::memcpy(buf + st->caret, pick, (size_t)n);
                len += n;
                st->caret += n;
                st->anchor = st->caret;
                changed = 1;
            }
            st->ac_open = 0;
            st->follow_caret = 1;
        } else if (total > 0) {
            // Under the caret, or above it when there is no room below - a
            // list that falls off the bottom of the panel is a list you
            // cannot read the last entry of.
            float cx2, cy2;
            {
                int ls = code_line_start(buf, a);
                cx2 = TEXT_X + code_run_w(ui, buf + ls, a - ls) - st->scroll_x;
                cy2 = y + 4.0f + (float)code_line_of(buf, a) * LH - st->scroll_y;
            }
            const float RH = dai_font_line_height(ui->font) + 4.0f;
            float lw = 180.0f;
            for (const AcEntry *e : hits) {
                float tw2 = dai_ui_text_width(ui, e->text) +
                            (e->hint ? dai_ui_text_width(ui, e->hint) + 24.0f : 0.0f) + 24.0f;
                if (tw2 > lw) lw = tw2;
            }
            if (lw > w - 20.0f) lw = w - 20.0f;
            float lh2 = RH * (float)total + 4.0f;
            float ly = cy2 + LH + 2.0f;
            if (ly + lh2 > y + h) ly = cy2 - lh2 - 2.0f;
            if (ly < y) ly = y;
            float lx = cx2;
            if (lx + lw > x + w - 4.0f) lx = x + w - 4.0f - lw;
            if (lx < x + 2.0f) lx = x + 2.0f;

            dai_ui_layer_push(ui, DAI_LAYER_POPUP);
            dai_ui_rrect(ui, lx, ly, lw, lh2, 4.0f, sty->panel);
            dai_ui_rect_outline(ui, lx, ly, lw, lh2, 1.0f, sty->accent);
            for (int i = 0; i < total; ++i) {
                float ry = ly + 2.0f + RH * (float)i;
                bool on = i == st->ac_sel;
                if (on) dai_ui_rect(ui, lx + 1.0f, ry, lw - 2.0f, RH, sty->button_active);
                const char *label = i < (int)hits.size() ? hits[(size_t)i]->text
                                                         : words[(size_t)(i - (int)hits.size())].c_str();
                const char *hint = i < (int)hits.size() ? hits[(size_t)i]->hint : "in this file";
                dai_ui_text(ui, lx + 8.0f, ry + 2.0f, label, on ? 0xFFFFFFFFu : sty->text);
                if (hint) {
                    float hw2 = dai_ui_text_width(ui, hint);
                    if (lx + 8.0f + dai_ui_text_width(ui, label) + 12.0f + hw2 < lx + lw - 6.0f)
                        dai_ui_text(ui, lx + lw - 6.0f - hw2, ry + 2.0f, hint, sty->text_dim);
                }
            }
            dai_ui_layer_pop(ui);
        }
    }

    return changed;
}
"""
s = s[:idx] + new_tail + s[idx + len(old_tail):]
io.open(p, 'w', encoding='utf-8').write(s)
print('autocomplete list built, applied and drawn')
