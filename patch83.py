#!/usr/bin/env python3
# patch83 - the code editor fought the wheel: every frame it dragged the view
# back to the caret, so scrolling past it was impossible.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p83'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

s = rd('include/dai_ui.h')
s = sub1(s,
"""    float prefer_x;     /* remembered column, so up/down do not drift left   */
    int   dragging;""",
"""    float prefer_x;     /* remembered column, so up/down do not drift left   */
    int   dragging;
    int   last_caret;   /* what the caret was last frame - see the widget    */
    int   have_last;""",
    'last caret state')
wr('include/dai_ui.h', s)

s = rd('src/dai_ui.cpp')

# "Keep the caret in view" is a response to the caret MOVING. Running it every
# frame turns it into "keep the view on the caret", which is a different and
# much worse thing: the wheel moves the view, the next frame drags it back, and
# the file cannot be read past the line you last clicked on.
#
# Every editor there is scrolls freely and only jumps back when you type.
s = sub1(s,
"""    // ---- keep the caret in view --------------------------------------------
    {
        float cx, cy;
        caret_xy(st->caret, &cx, &cy);""",
"""    // ---- keep the caret in view --------------------------------------------
    // Only when it MOVED, or when the text under it changed. Doing this
    // unconditionally is why the wheel did nothing: scroll away, and the very
    // next frame pulled the view back onto the caret.
    bool caret_moved = !st->have_last || st->last_caret != st->caret || changed;
    st->last_caret = st->caret;
    st->have_last = 1;
    if (caret_moved) {
        float cx, cy;
        caret_xy(st->caret, &cx, &cy);""",
    'reveal only on movement')
wr('src/dai_ui.cpp', s)
print('patch83 ok')
