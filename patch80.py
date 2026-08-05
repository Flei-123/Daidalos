#!/usr/bin/env python3
# patch80 - the code editor gets a clipboard. Ctrl+C, Ctrl+X, Ctrl+V.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p80'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# The UI has no clipboard of its own and should not grow one that guesses: the
# OS clipboard belongs to the window, and the window belongs to the host. So
# the host hands the text in each frame and takes back whatever a widget cut or
# copied. Two functions, no ownership questions, and a widget that works
# identically on a platform whose clipboard bridge returns nothing.
s = rd('include/dai_ui.h')
s = sub1(s,
"""DAI_API int  dai_ui_typing(const dai_ui *ui);""",
"""DAI_API int  dai_ui_typing(const dai_ui *ui);

/* The clipboard, as a hand-off rather than an ownership.
 *   _feed  - the host pushes the OS clipboard in, once a frame. Copied.
 *   _taken - what a widget cut or copied this frame, or NULL. Read once; the
 *            host puts it on the OS clipboard and the UI forgets it. */
DAI_API void        dai_ui_clipboard_feed(dai_ui *ui, const char *utf8);
DAI_API const char *dai_ui_clipboard_taken(dai_ui *ui);""",
    'clipboard decl')
wr('include/dai_ui.h', s)

s = rd('src/dai_ui.cpp')
s = sub1(s,
"""    int code_focus = 0;""",
"""    int code_focus = 0;
    // The clipboard, handed in and handed back. See dai_ui.h.
    std::string clip_in;
    std::string clip_out;
    bool        clip_out_set = false;""",
    'clipboard fields')

s = sub1(s,
"""int  dai_ui_typing(const dai_ui *ui) {""",
"""void dai_ui_clipboard_feed(dai_ui *ui, const char *utf8) {
    if (!ui) return;
    ui->clip_in = utf8 ? utf8 : "";
}

const char *dai_ui_clipboard_taken(dai_ui *ui) {
    if (!ui || !ui->clip_out_set) return nullptr;
    ui->clip_out_set = false;
    return ui->clip_out.c_str();
}

int  dai_ui_typing(const dai_ui *ui) {""",
    'clipboard impl')

# ---- the three keys, in the code editor --------------------------------
s = sub1(s,
"""        if (in.key_select_all) { st->anchor = 0; st->caret = len; }""",
"""        // Ctrl+C / Ctrl+X / Ctrl+V. The characters arrive as text events too
        // when Ctrl is held on some layouts, which is why the insert loop
        // above skips anything below 0x20 and these are checked on the KEY.
        if (in.key_ctrl) {
            int lo = sel_lo(), hi = sel_hi();
            bool has_sel = hi > lo;
            // 'c' == 0x63, 'x' == 0x78, 'v' == 0x76 as text events; the input
            // struct has no per-letter key flags, so the text stream is where
            // they are read from - Ctrl+C sends no printable character, but
            // the host forwards the code point.
            for (int i = 0; i < 8 && in.text[i]; ++i) {
                uint32_t cp = in.text[i] | 0x20u;      // fold case
                if (cp == 'c' && has_sel) {
                    ui->clip_out.assign(buf + lo, (size_t)(hi - lo));
                    ui->clip_out_set = true;
                } else if (cp == 'x' && has_sel) {
                    ui->clip_out.assign(buf + lo, (size_t)(hi - lo));
                    ui->clip_out_set = true;
                    erase(lo, hi);
                } else if (cp == 'v' && !ui->clip_in.empty()) {
                    // Line endings normalised on the way in: a paste from a
                    // Windows editor otherwise carries a carriage return into
                    // every line, and every one of them draws as a glyph.
                    std::string t;
                    t.reserve(ui->clip_in.size());
                    for (char ch : ui->clip_in) if (ch != '\\r') t += ch;
                    insert(t.c_str(), (int)t.size());
                }
            }
        }
        if (in.key_select_all) { st->anchor = 0; st->caret = len; }""",
    'code editor clipboard keys')
wr('src/dai_ui.cpp', s)

# ---------------------------------------------------------------- the host
s = rd('examples/editor_demo.cpp')
s = sub1(s,
"""        dai_ui_begin(ui, lw, lh, &in);""",
"""        // The OS clipboard, in - and whatever a widget copied, out. One
        // frame's round trip, so Ctrl+C in the script editor lands in the
        // same clipboard everything else on the machine uses.
        {
            char cb[64 * 1024];
            uint32_t cn = dai_window_clipboard_get(win, cb, sizeof(cb));
            dai_ui_clipboard_feed(ui, cn ? cb : "");
        }
        dai_ui_begin(ui, lw, lh, &in);""",
    'feed clipboard')

s = sub1(s,
"""        // Unsaved? The asterisk in the hierarchy comes from here.""",
"""        if (const char *taken = dai_ui_clipboard_taken(ui))
            dai_window_clipboard_set(win, taken);

        // Unsaved? The asterisk in the hierarchy comes from here.""",
    'take clipboard')
wr('examples/editor_demo.cpp', s)
print('patch80 ok')
