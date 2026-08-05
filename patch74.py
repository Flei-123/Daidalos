#!/usr/bin/env python3
# patch74 - the camera preview was drawing a black rectangle ON TOP of itself.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p74'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

s = rd('src/dai_editor_ui.cpp')

# The renderer draws the world first and the interface over it - that is the
# whole arrangement, and it is why panels can sit on top of the scene. So a
# plate painted "behind" the preview is not behind anything: it is the last
# thing drawn in that rectangle, and the picture underneath never had a chance.
#
# The preview needs no background at all. The world lands there; the frame and
# the caption go around it.
s = sub1(s,
"""            dai_ui_layer_push(ui, DAI_LAYER_WINDOW + 4);
            dai_ui_rect(ui, cx2 - 1.0f, cy2 - 1.0f, cw2 + 2.0f, ch2 + 2.0f, cs2->panel_border);
            dai_ui_rect(ui, cx2, cy2, cw2, ch2, rgba(0x0A, 0x0A, 0x0C, 255));
            float lh2 = dai_ui_text_height(ui) + 6.0f;""",
"""            dai_ui_layer_push(ui, DAI_LAYER_WINDOW + 4);
            // A BORDER, drawn as four thin bars - not a filled rectangle with
            // a smaller one inside it. The interface is composited over the
            // world, so anything filled here is simply opaque.
            const float B2 = 1.0f;
            dai_ui_rect(ui, cx2 - B2, cy2 - B2, cw2 + B2 * 2, B2, cs2->panel_border);
            dai_ui_rect(ui, cx2 - B2, cy2 + ch2, cw2 + B2 * 2, B2, cs2->panel_border);
            dai_ui_rect(ui, cx2 - B2, cy2, B2, ch2, cs2->panel_border);
            dai_ui_rect(ui, cx2 + cw2, cy2, B2, ch2, cs2->panel_border);
            float lh2 = dai_ui_text_height(ui) + 6.0f;""",
    'preview border not plate')
wr('src/dai_editor_ui.cpp', s)

# ...and the second view has to be switched OFF when nobody wants it, or the
# last rectangle keeps being rendered into for ever.
s = rd('examples/editor_demo.cpp')
s = sub1(s,
"""        // The camera preview, in the corner of the scene view. It uses the""",
"""        // Nobody has asked for the second view yet this frame. Without this
        // the last rectangle any of them set keeps rendering, so closing the
        // Game panel or deselecting the camera leaves a picture behind.
        dai_render_world_clip2(r, 0.0f, 0.0f, 0.0f, 0.0f);

        // The camera preview, in the corner of the scene view. It uses the""",
    'reset second view')
wr('examples/editor_demo.cpp', s)
print('patch74 ok')
