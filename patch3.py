import sys, io

def rw(p): return io.open(p, encoding='utf-8').read()
def wr(p, s): io.open(p, 'w', encoding='utf-8').write(s)
def sub1(s, old, new, tag):
    if old not in s: print("MISS", tag); sys.exit(1)
    if s.count(old) != 1: print("AMBIG", tag, s.count(old)); sys.exit(1)
    print("ok", tag); return s.replace(old, new)

# ---------------------------------------------------------------- dai_ui.cpp
P = 'src/dai_ui.cpp'
s = rw(P)

s = sub1(s,
"""float g_label_x = 0.0f, g_label_y = 0.0f, g_label_w = 0.0f;""",
"""float g_label_x = 0.0f, g_label_y = 0.0f, g_label_w = 0.0f;
// The whole row of the last field, label column included - what a help
// tooltip has to hover over. A field whose units are only in the manual is a
// field whose units nobody knows: "Friction 10" means nothing until you learn
// it is a coefficient and not a percentage.
float g_row_x = 0.0f, g_row_y = 0.0f, g_row_w = 0.0f, g_row_h = 0.0f;""", "row rect decl")

s = sub1(s,
"""    g_label_x = rx; g_label_y = ry; g_label_w = 0.0f;""",
"""    g_label_x = rx; g_label_y = ry; g_label_w = 0.0f;
    g_row_x = rx; g_row_y = ry; g_row_w = full; g_row_h = h;""", "row rect set")

# public: attach a help tooltip to the field just drawn
s = sub1(s,
"""float dai_ui_text_height(dai_ui *ui) {""",
"""void dai_ui_help(dai_ui *ui, const char *text) {
    if (!ui || !text || !*text) return;
    // Uses the rect of the field drawn immediately before, which is why this
    // is a separate call and not a parameter: half the widgets in this file
    // would need one, and a host that does not want tooltips pays nothing.
    if (ui->in_popup || ui->blocked) return;
    float x = dai::ui_detail_row_x(), y = dai::ui_detail_row_y();
    float w = dai::ui_detail_row_w(), h = dai::ui_detail_row_h();
    if (w <= 0.0f || h <= 0.0f) return;
    if (ui->input.mouse_x < x || ui->input.mouse_x >= x + w) return;
    if (ui->input.mouse_y < y || ui->input.mouse_y >= y + h) return;
    if (ui->input.mouse_down) return;
    std::snprintf(ui->tooltip, sizeof(ui->tooltip), "%s", text);
    ui->tooltip_x = x;
    ui->tooltip_y = y + h + 4.0f;
    ui->tooltip_on = true;
}

float dai_ui_text_height(dai_ui *ui) {""", "dai_ui_help")

# accessors so the extern "C" function can read the anonymous-namespace values
s = sub1(s,
"""} // namespace

int dai_ui_drag_float(dai_ui *ui, const char *label, float *value, float step) {""",
"""} // namespace

namespace dai {
float ui_detail_row_x() { return g_row_x; }
float ui_detail_row_y() { return g_row_y; }
float ui_detail_row_w() { return g_row_w; }
float ui_detail_row_h() { return g_row_h; }
}

int dai_ui_drag_float(dai_ui *ui, const char *label, float *value, float step) {""", "row accessors")

# forward declaration near the top of the file so dai_ui_help compiles wherever it sits
s = sub1(s,
"""float dai_ui_text_width(dai_ui *ui, const char *utf8) {""",
"""namespace dai {
float ui_detail_row_x();
float ui_detail_row_y();
float ui_detail_row_w();
float ui_detail_row_h();
}

float dai_ui_text_width(dai_ui *ui, const char *utf8) {""", "row accessor decl")

wr(P, s)

P = 'include/dai_ui.h'
s = rw(P)
s = sub1(s,
"DAI_API float dai_ui_text_height(dai_ui *ui);",
"""DAI_API float dai_ui_text_height(dai_ui *ui);
/* Attaches an explanation to the field drawn immediately before this call; it
 * is shown while the pointer rests on that row. Units belong here: "Friction"
 * with no tooltip is a number whose meaning you have to already know. */
DAI_API void dai_ui_help(dai_ui *ui, const char *text);""", "help decl")
wr(P, s)
print("patch3 done")
