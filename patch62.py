#!/usr/bin/env python3
# patch62 - put the net under the frame, and say so in the console.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p62'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

s = rd('examples/editor_demo.cpp')

s = sub1(s,
"""        dai_editor_ui_frame(panels, lw, lh);""",
"""        // The interface, under the net. A fault here abandons ONE frame and
        // says what it was; the scene, the undo history and everything you
        // have not saved are still in memory afterwards.
        {
            struct FrameArgs { dai_editor_ui *p; float w, h; } fa{ panels, lw, lh };
            auto draw = [](void *u) {
                FrameArgs *a = (FrameArgs *)u;
                dai_editor_ui_frame(a->p, a->w, a->h);
            };
            if (guard_run(draw, &fa)) {
                g_guard_faults = 0;          // a clean frame clears the count
            } else {
                char line[256];
                std::snprintf(line, sizeof(line),
                              "interface fault 0x%lx at %p - frame skipped (%d in a row)",
                              g_guard_last_code, g_guard_last_addr, g_guard_faults);
                dai_editor_ui_log(panels, 2, line);
                std::printf("%s\\n", line);
                if (g_guard_faults >= 8) {
                    dai_editor_ui_log(panels, 2,
                        "too many faults in a row - saving a report and stopping");
                    std::printf("giving up after %d consecutive faults\\n", g_guard_faults);
                }
            }
        }""",
    'guard the frame')
wr('examples/editor_demo.cpp', s)
print('patch62 ok')
