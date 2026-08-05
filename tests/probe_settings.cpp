// Why is the Settings panel black? Run real frames with it focused and count
// what came out of the UI inside its rectangle.
#include "dai_editor_ui.h"
#include <cstdio>
#include <cstring>

static uint32_t verts_in(dai_ui *ui, float x0, float y0, float x1, float y1) {
    const dai_ui_draw *d = nullptr;
    uint32_t n = dai_ui_draws(ui, &d), c = 0;
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t v = 0; v < d[i].count; ++v) {
            const dai_ui_vertex &p = d[i].vertices[v];
            if (p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1) ++c;
        }
    return c;
}

int main() {
    char err[256] = { 0 };
    dai_font *font = dai_font_load("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 13.0f,
                                   nullptr, 0, err, sizeof(err));
    if (!font) { std::printf("no font: %s\n", err); return 1; }
    dai_ui *ui = dai_ui_create(font, 0);
    dai_config cfg{};
    cfg.tick_hz = 60; cfg.max_bodies = 64; cfg.physics_threads = 1; cfg.seed = 1;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) { std::printf("no world\n"); return 1; }
    dai_scene *sc = dai_scene_create(w);
    dai_doc *doc = dai_doc_create();
    dai_doc_sync *sync = dai_doc_sync_create(doc, sc);
    dai_editor *ed = dai_editor_create(doc, sync);
    dai_editor_ui *p = dai_editor_ui_create(ed, ui);

    dai_ui_input in{};
    for (int f = 0; f < 3; ++f) {
        dai_ui_begin(ui, 1600.0f, 900.0f, &in);
        dai_editor_ui_frame(p, 1600.0f, 900.0f);
        dai_ui_end(ui);
    }
    std::printf("settings open before: visible=%d is_open=%d\n",
                dai_editor_ui_panel_visible(p, "Settings"),
                dai_editor_ui_panel_is_open(p, "Settings"));

    dai_editor_ui_open_panel(p, "Settings");
    for (int f = 0; f < 3; ++f) {
        dai_ui_begin(ui, 1600.0f, 900.0f, &in);
        dai_editor_ui_frame(p, 1600.0f, 900.0f);
        dai_ui_end(ui);
    }
    float x, y, ww, hh;
    int got = dai_editor_ui_panel_rect(p, "Settings", &x, &y, &ww, &hh);
    std::printf("rect got=%d  %.0f %.0f %.0f %.0f\n", got, x, y, ww, hh);
    if (got)
        std::printf("vertices inside the Settings rect: %u\n",
                    verts_in(ui, x, y, x + ww, y + hh));
    return 0;
}
