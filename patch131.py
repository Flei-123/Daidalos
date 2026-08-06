import io

# ===========================================================================
# UI im Viewport anfassen.
#
# Ein Anker plus ein Offset laesst sich im Inspector eintippen, aber niemand
# platziert eine Anzeige, indem er Zahlen raet - man schiebt sie hin, wo sie
# hingehoert. Die Umrandung zeigt, WELCHES Rechteck das Element belegt, und
# das Ziehen schreibt in den Offset zurueck, nicht in eine absolute Position:
# der Anker bleibt der Anker, sonst waere die Anzeige beim naechsten
# Fenstergroessenwechsel wieder woanders.
# ===========================================================================
p = 'include/dai_editor_ui.h'
s = io.open(p, encoding='utf-8').read()
old = """DAI_API void dai_hud_images(dai_hud_image_fn fn, void *user);"""
new = """DAI_API void dai_hud_images(dai_hud_image_fn fn, void *user);

/* Where the last dai_hud_draw put a node's UI, in screen pixels. The editor
 * uses it to draw a frame around the selected element and to drag it; nothing
 * else can know, because the anchor maths lives inside the draw. Returns 0
 * when that node drew nothing this frame. */
DAI_API int dai_hud_rect_of(dai_node n, float *x, float *y, float *w, float *h);"""
assert s.count(old) == 1, 'hud_images decl not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)

p = 'src/dai_editor_ui.cpp'
s = io.open(p, encoding='utf-8').read()

old = """static dai_hud_image_fn g_hud_image = nullptr;"""
new = """// What the last draw laid out, so the editor can put a handle on it. A frame
// of latency by construction - the rectangle is from the frame before the one
// being built - and that is fine: it moves when the thing moves.
struct HudRect { dai_node n; float x, y, w, h; };
static std::vector<HudRect> g_hud_rects;
static std::vector<HudRect> g_hud_rects_prev;

int dai_hud_rect_of(dai_node n, float *x, float *y, float *w, float *h) {
    for (const HudRect &r : g_hud_rects_prev) {
        if (r.n != n) continue;
        if (x) *x = r.x;
        if (y) *y = r.y;
        if (w) *w = r.w;
        if (h) *h = r.h;
        return 1;
    }
    return 0;
}

static dai_hud_image_fn g_hud_image = nullptr;"""
assert s.count(old) == 1, 'hud_image global not found'
s = s.replace(old, new)

old = """    dai_ui_clip_begin(ui, x, y, w, h);
    for (uint32_t i = 0; i < n; ++i) {
        dai_node_desc r{};
        if (dai_doc_get(doc, ids[i], &r) != DAI_OK) continue;"""
new = """    g_hud_rects_prev.swap(g_hud_rects);
    g_hud_rects.clear();

    dai_ui_clip_begin(ui, x, y, w, h);
    for (uint32_t i = 0; i < n; ++i) {
        dai_node_desc r{};
        if (dai_doc_get(doc, ids[i], &r) != DAI_OK) continue;"""
assert s.count(old) == 1, 'hud loop start not found'
s = s.replace(old, new)

# Rechtecke mitschreiben - Bild und Text.
old = """                dai_ui_image_at(ui, tex, ix, iy, dw, dh, 0, 0, 1, 1, tint);"""
new = """                dai_ui_image_at(ui, tex, ix, iy, dw, dh, 0, 0, 1, 1, tint);
                g_hud_rects.push_back(HudRect{ ids[i], ix, iy, dw, dh });"""
assert s.count(old) == 1, 'image draw not found'
s = s.replace(old, new)

old = """        for (size_t li = 0; li < lines.size(); ++li) {"""
new = """        g_hud_rects.push_back(HudRect{ ids[i], bx, by, widest, block_h });
        for (size_t li = 0; li < lines.size(); ++li) {"""
assert s.count(old) == 1, 'text line loop not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)
print('hud remembers its rectangles')
