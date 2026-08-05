# -*- coding: utf-8 -*-
import sys, io

def rw(p): return io.open(p, encoding='utf-8').read()
def wr(p, s): io.open(p, 'w', encoding='utf-8').write(s)
def sub1(s, old, new, tag):
    if old not in s: print("MISS", tag); sys.exit(1)
    if s.count(old) != 1: print("AMBIG", tag, s.count(old)); sys.exit(1)
    print("ok", tag); return s.replace(old, new)

# --------------------------------------------------------------- dai_ui.cpp
P = 'src/dai_ui.cpp'
s = rw(P)

# #39: the scroll region reserved its own height in the layout, so the content
# below it moved one region-height down every frame and the clamp snapped the
# offset back. Regions clip themselves; they are windows, not spacers.
s = sub1(s,
"""void dai_ui_scroll_begin(dai_ui *ui, const char *id_str, float height) {
    if (!ui) return;
    float x, y;
    next_rect(ui, 0, height, &x, &y);
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);""",
"""void dai_ui_scroll_begin(dai_ui *ui, const char *id_str, float height) {
    if (!ui) return;
    float x, y;
    // Place, do not advance: the region clips what is inside it, so reserving
    // its height in the layout moved whatever follows by the region's height
    // EVERY frame - which is exactly the snap-back when you scrolled down.
    x = ui->cursor_x; y = ui->cursor_y;
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);""",
"scroll begin")

s = sub1(s,
"""    if (max_off > 0.0f) {""",
"""    // No dead travel: while the content is shorter than the region, any
    // scroll offset is invalid - so the wheel does nothing instead of
    // scrolling into empty space and being corrected next frame.
    if (content < f.h) off = 0.0f;

    if (max_off > 0.0f) {""", "scroll no-dead-travel")
wr(P, s)

# ------------------------------------------------------------ dai_dock.cpp
P = 'src/dai_dock.cpp'
s = rw(P)

# #41 + #40: clicks on a tab must never reach the viewport pick, and a tab
# drag on its own bar reorders instead of starting a dock drag.
s = sub1(s,
"""    float tx = r.x;
    for (size_t i = 0; i < leaf->tabs.size(); ++i) {""",
"""    // Whatever happens up here, it stays up here: the Scene/Game viewport
    // behind the bar asks "was the mouse over UI?" and a click it cannot see
    // as handled is a click that DESELECTS what you were working on.
    if (over_bar) dai_ui_claim_mouse(ui);

    float tx = r.x;
    for (size_t i = 0; i < leaf->tabs.size(); ++i) {""", "tab claim")

wr(P, s)

# -------------------------------------------------------- dai_editor_ui.cpp
P = 'src/dai_editor_ui.cpp'
s = rw(P)

# #38: the prefab drop never fired, three reasons, all here:
#  - hover_node is RESET at the top of the hierarchy body every frame and only
#    rebuilt while the pointer is over the hierarchy - so a node dropped on
#    the Project panel read "no target".
#  - dai_ui_root_hovered("Project") is only meaningful when the pointer is
#    over the project root, which the drag handler asked the frame AFTER the
#    hierarchy reset ran... and the hierarchy was last drawn.
# Fix: keep the press target, and ask the mouse directly which panel it is
# over at drop time - no state to go stale.
s = sub1(s,
"""    dai_doc *d = dai_editor_doc(p->ed);
    p->visible_rows = 0;
    p->hover_node = DAI_INVALID_NODE;   // rebuilt per frame, for script drops""",
"""    dai_doc *d = dai_editor_doc(p->ed);
    p->visible_rows = 0;""", "no hover reset")

s = sub1(s,
"""        if (!ddown && p->drag_node_pending != DAI_INVALID_NODE) {
            // Dropped on the PROJECT window: the object becomes a prefab
            // file, exactly the Unity gesture.
            if (p->drag_node != DAI_INVALID_NODE && dai_ui_root_hovered(ui, "Project")) {""",
"""        if (!ddown && p->drag_node_pending != DAI_INVALID_NODE) {
            // Dropped on the PROJECT window: the object becomes a prefab
            // file, exactly the Unity gesture. Asked by position, not by a
            // hover flag - the flag is rebuilt per panel per frame, and the
            // drop can land between rebuilds.
            float dmx2 = 0, dmy2 = 0;
            dai_ui_mouse(ui, &dmx2, &dmy2, nullptr, nullptr);
            bool over_project = false;
            {
                float qx, qy, qw, qh;
                for (int inst = 0; inst < 8 &&
                     dai_dock_panel_rect(p->dock, "Project", inst, &qx, &qy, &qw, &qh); ++inst) {
                    if (dmx2 >= qx && dmx2 < qx + qw && dmy2 >= qy && dmy2 < qy + qh)
                        { over_project = true; break; }
                }
            }
            if (p->drag_node != DAI_INVALID_NODE && over_project) {""", "prefab drop by rect")
wr(P, s)
print("patch19 done")
