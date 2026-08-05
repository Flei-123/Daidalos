#!/usr/bin/env python3
# patch72 - a floor grid that does not run out, and an FPS readout you can
# switch off.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p72'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

s = rd('src/dai_editor_ui.cpp')

# =============================================== 1. the grid follows the eye
# It was 41 lines each way from the origin, one metre apart, for ever - so it
# ended at 20 m and stayed there however far you flew. A grid is not scenery,
# it is a ruler: it has to be under you, and its spacing has to be readable at
# the height you are at. Both come from the camera:
#
#   * centred on the camera's ground position, SNAPPED to the cell size, so
#     the lines do not crawl as the camera moves;
#   * cell size stepping 1 / 10 / 100 m by height, so the line count is
#     constant and the grid never turns into a grey wash;
#   * a second, coarser set at ten times the spacing, which is what keeps the
#     far field readable when the near cells go sub-pixel.
s = sub1(s,
"""uint32_t dai_editor_ui_grid_lines(const dai_editor_ui *p, float *out, uint32_t max_points) {
    if (!p || !p->gizmo_grid || !out) return 0;
    uint32_t n = 0;
    auto put = [&](dai_vec3 a, dai_vec3 b) {
        if (n + 2 > max_points) return;
        out[n*3+0] = a.x; out[n*3+1] = a.y; out[n*3+2] = a.z; ++n;
        out[n*3+0] = b.x; out[n*3+1] = b.y; out[n*3+2] = b.z; ++n;
    };
    for (int g = -20; g <= 20; ++g) {
        if (g == 0) continue;
        put(dai_vec3{ (float)g, 0, -20 }, dai_vec3{ (float)g, 0, 20 });
        put(dai_vec3{ -20, 0, (float)g }, dai_vec3{ 20, 0, (float)g });
    }
    put(dai_vec3{ -20, 0, 0 }, dai_vec3{ 20, 0, 0 });
    put(dai_vec3{ 0, 0, -20 }, dai_vec3{ 0, 0, 20 });
    return n;
}""",
"""uint32_t dai_editor_ui_grid_lines(const dai_editor_ui *p, float *out, uint32_t max_points) {
    if (!p || !p->gizmo_grid || !out) return 0;
    uint32_t n = 0;
    auto put = [&](dai_vec3 a, dai_vec3 b) {
        if (n + 2 > max_points) return;
        out[n*3+0] = a.x; out[n*3+1] = a.y; out[n*3+2] = a.z; ++n;
        out[n*3+0] = b.x; out[n*3+1] = b.y; out[n*3+2] = b.z; ++n;
    };

    // Where the camera is. dai_editor_ray hands back the eye as the origin of
    // any ray, which is the only getter this layer needs.
    dai_vec3 eye{ 0, 6, 0 }, dir{ 0, -1, 0 };
    if (p->ed) dai_editor_ray(p->ed, 0.0f, 0.0f, &eye, &dir);

    // Spacing by height: 1 m near the floor, then ten times that per decade.
    // Height, not distance to the origin - what matters is how much floor one
    // metre covers on screen.
    float h = std::fabs(eye.y);
    if (h < 1.0f) h = 1.0f;
    float cell = 1.0f;
    while (cell * 12.0f < h) cell *= 10.0f;      // 1 -> 10 -> 100
    if (cell > 1000.0f) cell = 1000.0f;

    const int HALF = 40;                          // cells each way from the eye
    float ext = (float)HALF * cell;
    // Snapped to the grid, so the lines stay still while the camera moves and
    // only the far edge is ever added or dropped.
    float cx = std::floor(eye.x / cell + 0.5f) * cell;
    float cz = std::floor(eye.z / cell + 0.5f) * cell;

    for (int g = -HALF; g <= HALF; ++g) {
        float x = cx + (float)g * cell;
        float z = cz + (float)g * cell;
        // The world axes are drawn last, in their own colour, so skip the
        // cell line that would sit exactly on top of one.
        if (std::fabs(x) > 0.001f * cell)
            put(dai_vec3{ x, 0, cz - ext }, dai_vec3{ x, 0, cz + ext });
        if (std::fabs(z) > 0.001f * cell)
            put(dai_vec3{ cx - ext, 0, z }, dai_vec3{ cx + ext, 0, z });
    }
    // The coarse set, ten cells apart, reaching ten times as far: this is the
    // half that makes it read as endless rather than as a mat you are standing
    // on the edge of.
    float big = cell * 10.0f, bext = ext * 6.0f;
    float bx = std::floor(eye.x / big + 0.5f) * big;
    float bz = std::floor(eye.z / big + 0.5f) * big;
    for (int g = -HALF / 2; g <= HALF / 2; ++g) {
        float x = bx + (float)g * big;
        float z = bz + (float)g * big;
        put(dai_vec3{ x, 0, bz - bext }, dai_vec3{ x, 0, bz + bext });
        put(dai_vec3{ bx - bext, 0, z }, dai_vec3{ bx + bext, 0, z });
    }
    // The two world axes, through the origin, however far away it is.
    put(dai_vec3{ cx - ext * 8.0f, 0, 0 }, dai_vec3{ cx + ext * 8.0f, 0, 0 });
    put(dai_vec3{ 0, 0, cz - ext * 8.0f }, dai_vec3{ 0, 0, cz + ext * 8.0f });
    return n;
}""",
    'infinite grid')

# ==================================================== 2. the FPS readout
s = sub1(s,
"""    int  gizmo_grid = 1;            // the floor grid in the scene view""",
"""    int  gizmo_grid = 1;            // the floor grid in the scene view
    int  gizmo_fps = 1;             // the frame counter in the corner
    float fps_now = 0.0f;           // pushed by the host, already smoothed""",
    'fps state')

s = sub1(s,
"""        { int g = p->gizmo_grid;      if (dai_ui_checkbox(p->ui, "Floor grid", &g))      p->gizmo_grid = g; }""",
"""        { int g = p->gizmo_grid;      if (dai_ui_checkbox(p->ui, "Floor grid", &g))      p->gizmo_grid = g; }
        { int g = p->gizmo_fps;       if (dai_ui_checkbox(p->ui, "FPS", &g))             p->gizmo_fps = g; }""",
    'fps checkbox 1')

s = sub1(s,
"""                int g = p->gizmo_grid;      if (dai_ui_checkbox(ui, "Floor grid", &g))      p->gizmo_grid = g;""",
"""                int g = p->gizmo_grid;      if (dai_ui_checkbox(ui, "Floor grid", &g))      p->gizmo_grid = g;
                int q = p->gizmo_fps;       if (dai_ui_checkbox(ui, "FPS", &q))             p->gizmo_fps = q;""",
    'fps checkbox 2')

# Drawn over BOTH views, in their own corners: the scene view's number is the
# editor's cost, the game view's is the game's, and while both are docked they
# are genuinely different numbers.
s = sub1(s,
"""    {
        // The preview's chrome. Drawn AFTER the scene panel has reported its""",
"""    if (p->gizmo_fps) {
        // Top left of each view. A frame counter belongs where nothing else
        // is, and it belongs in BOTH views: the scene view's number is what
        // the editor costs, the game view's is what the game costs, and with
        // both docked they are not the same number.
        const dai_ui_style *fs = dai_ui_style_of(ui);
        char fb[64];
        std::snprintf(fb, sizeof(fb), "%.0f fps   %.1f ms",
                      (double)p->fps_now,
                      p->fps_now > 0.01f ? (double)(1000.0f / p->fps_now) : 0.0);
        // Green while it is comfortable, amber when it is not, red when the
        // frame is longer than a 30 Hz budget - the number you read at a
        // glance is the colour, not the digits.
        uint32_t col = p->fps_now >= 55.0f ? rgba(0x8A, 0xD6, 0x8A, 255)
                     : p->fps_now >= 28.0f ? rgba(0xE6, 0xC0, 0x6A, 255)
                                           : rgba(0xE8, 0x7A, 0x70, 255);
        float tw2 = dai_ui_text_width(ui, fb) + 14.0f;
        float th2 = dai_ui_text_height(ui) + 8.0f;
        struct Corner { float x, y, w, h; int on; };
        Corner views[2] = {
            { p->view_x, p->view_y, p->view_w, p->view_h, p->view_w > 0.0f },
            { p->game_x, p->game_y, p->game_w, p->game_h, p->has_game != 0 },
        };
        dai_ui_layer_push(ui, DAI_LAYER_WINDOW + 3);
        for (const Corner &c : views) {
            if (!c.on || c.w < tw2 + 16.0f) continue;
            float fx = c.x + 8.0f, fy = c.y + 8.0f;
            dai_ui_rrect(ui, fx, fy, tw2, th2, 4.0f, 0xB4000000u);
            dai_ui_text(ui, fx + 7.0f, fy + 4.0f, fb, col);
            if (views[1].on && c.x == views[1].x && c.y == views[1].y) {
                // Two counters on screen at once need to say which is which.
                dai_ui_text(ui, fx + tw2 + 6.0f, fy + 4.0f, "game", fs->text_dim);
            }
        }
        dai_ui_layer_pop(ui);
    }
    {
        // The preview's chrome. Drawn AFTER the scene panel has reported its""",
    'fps overlay')

s = sub1(s,
"""uint32_t dai_editor_ui_grid_lines(const dai_editor_ui *p, float *out, uint32_t max_points) {""",
"""void dai_editor_ui_fps(dai_editor_ui *p, float fps) {
    if (p) p->fps_now = fps;
}

uint32_t dai_editor_ui_grid_lines(const dai_editor_ui *p, float *out, uint32_t max_points) {""",
    'fps setter')
wr('src/dai_editor_ui.cpp', s)

s = rd('include/dai_editor_ui.h')
s = sub1(s,
"""DAI_API uint32_t dai_editor_ui_visible_rows(const dai_editor_ui *p);""",
"""DAI_API uint32_t dai_editor_ui_visible_rows(const dai_editor_ui *p);

/* The frame rate, for the readout in the corner of the views. The host owns
 * the clock - it is the only thing that knows when a frame began and ended -
 * and should hand over an already smoothed value; the editor draws digits, it
 * does not average. Switched off in the view options. */
DAI_API void dai_editor_ui_fps(dai_editor_ui *p, float fps);""",
    'fps decl')
wr('include/dai_editor_ui.h', s)

# ============================================ 3. the host: clock and buffer
s = rd('examples/editor_demo.cpp')
s = sub1(s,
"""        {   // The floor grid as world lines: depth tested, so boxes hide it.
            static float grid_xyz[84 * 2 * 3];
            uint32_t gn = dai_editor_ui_grid_lines(panels, grid_xyz, 84 * 2);""",
"""        {   // The floor grid as world lines: depth tested, so boxes hide it.
            // Bigger buffer than the old fixed 20 m mat needed: the grid now
            // follows the camera and carries a coarse set as well.
            static float grid_xyz[420 * 2 * 3];
            uint32_t gn = dai_editor_ui_grid_lines(panels, grid_xyz, 420 * 2);""",
    'bigger grid buffer')

s = sub1(s,
"""        // Unsaved? The asterisk in the hierarchy comes from here.""",
"""        // The frame rate, smoothed over about half a second. Averaged HERE
        // rather than in the editor because this is where the clock is - and
        // an unsmoothed counter is a number nobody can read.
        {
            static float fps_avg = 0.0f;
            if (dt > 0.0001f) {
                float inst = 1.0f / dt;
                float k = dt / (0.5f + dt);        // ~0.5 s time constant
                fps_avg = fps_avg <= 0.0f ? inst : fps_avg + (inst - fps_avg) * k;
            }
            dai_editor_ui_fps(panels, fps_avg);
        }

        // Unsaved? The asterisk in the hierarchy comes from here.""",
    'host fps clock')
wr('examples/editor_demo.cpp', s)
print('patch72 ok')
