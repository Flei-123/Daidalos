#!/usr/bin/env python3
# patch52b - what the crash handler needs: the signal headers, and a way to ask
# the console what it said last.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p52b'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# ------------------------------------------------------- the console's tail
s = rd('include/dai_editor_ui.h')
s = sub1(s,
"""DAI_API void dai_editor_ui_log_clear(dai_editor_ui *p);""",
"""DAI_API void dai_editor_ui_log_clear(dai_editor_ui *p);
/* The last lines the console holds, newest LAST, as plain text. What a crash
 * report wants: the console is the only running narrative the editor keeps,
 * and after the window is gone it is the only one that survives. Returns the
 * number of bytes written (never more than buf_size - 1, always NUL
 * terminated). Safe to call from a crash handler: it only reads and copies. */
DAI_API uint32_t dai_editor_ui_log_tail(const dai_editor_ui *p, char *buf, uint32_t buf_size);""",
    'log_tail decl')
wr('include/dai_editor_ui.h', s)

s = rd('src/dai_editor_ui.cpp')
s = sub1(s,
"""void dai_editor_ui_log_clear(dai_editor_ui *p) {""",
"""uint32_t dai_editor_ui_log_tail(const dai_editor_ui *p, char *buf, uint32_t buf_size) {
    if (!buf || buf_size < 2) return 0;
    buf[0] = 0;
    if (!p) return 0;
    // Walk BACKWARDS to find how many of the newest lines fit, then write them
    // in order. Nothing is allocated: this runs inside a crash handler, where
    // the heap is exactly the thing that might be broken.
    uint32_t need = 0;
    size_t first = p->log.size();
    while (first > 0) {
        const auto &l = p->log[first - 1];
        uint32_t line = (uint32_t)l.text.size() + 1;
        if (need + line >= buf_size) break;
        need += line;
        --first;
    }
    uint32_t used = 0;
    for (size_t i = first; i < p->log.size(); ++i) {
        const std::string &t = p->log[i].text;
        uint32_t n = (uint32_t)t.size();
        if (used + n + 2 >= buf_size) break;
        std::memcpy(buf + used, t.data(), n);
        used += n;
        buf[used++] = '\\n';
    }
    buf[used] = 0;
    return used;
}

void dai_editor_ui_log_clear(dai_editor_ui *p) {""",
    'log_tail impl')
wr('src/dai_editor_ui.cpp', s)

# ---------------------------------------------------------- the two headers
s = rd('examples/editor_demo.cpp')
s = sub1(s,
"""#include <dirent.h>   // mingw has it too - one directory API for both""",
"""#include <csignal>    // the crash handler's POSIX half
#include <dirent.h>   // mingw has it too - one directory API for both
#if !defined(_WIN32) && defined(__GLIBC__)
#include <execinfo.h>  // backtrace(); glibc only, and only used there
#endif""",
    'signal headers')
wr('examples/editor_demo.cpp', s)
print('patch52b ok')
