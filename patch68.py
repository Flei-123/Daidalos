#!/usr/bin/env python3
# patch68 - a camera you can actually click, and a preview of what it sees
# while you move it.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p68'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# ========================================= 1. the handle was too small to hit
# 9 screen pixels was chosen to stop a camera in the middle of the sky from
# eating every click near the horizon. It over-corrected: a 9 px cube is 4 px
# either side of a point, and the drawn camera icon is three times that - so
# the thing you are aiming at and the thing that answers are different sizes,
# and the difference is all miss. 16 px matches the icon, which is the only
# number that makes "click the camera" mean what it looks like it means.
s = rd('src/dai_editor.cpp')
s = sub1(s,
"""            float hs = per_px * 9.0f;""",
"""            float hs = per_px * 16.0f;""",
    'bigger handle')
wr('src/dai_editor.cpp', s)

# ================================== 2. what the selected camera can see
s = rd('include/dai_editor_ui.h')
s = sub1(s,
"""DAI_API int  dai_editor_ui_game_camera(const dai_editor_ui *p, dai_vec3 *eye,
                                       dai_vec3 *look, float *fov_deg);""",
"""DAI_API int  dai_editor_ui_game_camera(const dai_editor_ui *p, dai_vec3 *eye,
                                       dai_vec3 *look, float *fov_deg);

/* ---- the camera preview -------------------------------------------------
 *
 * Unity's small window in the corner of the scene view: select a camera, and
 * what that camera sees appears while you move it. Framing a shot by flying
 * the editor camera to where the game camera is, looking, and flying back is
 * the alternative, and it is why nobody frames shots.
 *
 * Returns 1 when there is something to draw: `p` has a camera selected and the
 * Scene view is on screen. The rectangle is in UI (logical) pixels, already
 * inside the scene panel and already drawn as a frame by the editor - the host
 * only has to render the world into it from the camera below. */
DAI_API int dai_editor_ui_camera_preview(const dai_editor_ui *p,
                                         float *x, float *y, float *w, float *h,
                                         dai_vec3 *eye, dai_vec3 *look, float *fov_deg,
                                         float *ortho_size);""",
    'camera preview decl')
wr('include/dai_editor_ui.h', s)

s = rd('src/dai_editor_ui.cpp')
s = sub1(s,
"""float dai_editor_ui_game_ortho(const dai_editor_ui *p) {""",
"""int dai_editor_ui_camera_preview(const dai_editor_ui *p,
                                 float *x, float *y, float *w, float *h,
                                 dai_vec3 *eye, dai_vec3 *look, float *fov_deg,
                                 float *ortho_size) {
    if (!p || !p->ed) return 0;
    if (!p->view_w || !p->view_h) return 0;
    // The Game panel already shows this, full size and continuously; a second
    // copy in the corner of the scene would be two answers to one question.
    if (p->has_game) return 0;
    if (dai_editor_selection_count(p->ed) == 0) return 0;
    dai_node sel = dai_editor_selected(p->ed, 0);
    dai_doc *d = dai_editor_doc(p->ed);
    dai_node_desc r{};
    if (dai_doc_get(d, sel, &r) != DAI_OK || !r.camera) return 0;

    dai_vec3 wp{}, ws{ 1, 1, 1 };
    dai_quat wr{ 0, 0, 0, 1 };
    if (!dai_editor_live_transform(p->ed, sel, &wp, &wr, &ws) &&
        dai_doc_world_transform(d, sel, &wp, &wr, &ws) != DAI_OK) return 0;
    dai_vec3 dir = qrot_v(wr, dai_vec3{ 0, 0, -1 });
    if (eye) *eye = wp;
    if (look) *look = v_add(wp, dir);
    if (fov_deg) *fov_deg = r.camera_fov > 0.0f ? r.camera_fov : 60.0f;
    if (ortho_size) *ortho_size = r.camera == 2 ? (r.camera_size > 0.0f ? r.camera_size : 5.0f)
                                                : 0.0f;

    // Bottom right, 16:9, a quarter of the view's width, with a floor and a
    // ceiling so it is neither a postage stamp on a 4K monitor nor the whole
    // panel on a small one.
    float pw2 = p->view_w * 0.25f;
    if (pw2 < 180.0f) pw2 = 180.0f;
    if (pw2 > 420.0f) pw2 = 420.0f;
    if (pw2 > p->view_w - 40.0f) pw2 = p->view_w - 40.0f;
    float ph2 = pw2 * 9.0f / 16.0f;
    if (ph2 > p->view_h - 60.0f) { ph2 = p->view_h - 60.0f; pw2 = ph2 * 16.0f / 9.0f; }
    if (pw2 < 80.0f || ph2 < 45.0f) return 0;          // no room: no preview
    const float M = 12.0f;
    if (x) *x = p->view_x + p->view_w - pw2 - M;
    if (y) *y = p->view_y + p->view_h - ph2 - M;
    if (w) *w = pw2;
    if (h) *h = ph2;
    return 1;
}

float dai_editor_ui_game_ortho(const dai_editor_ui *p) {""",
    'camera preview impl')

# The frame and the caption, drawn by the editor so the host only renders.
s = sub1(s,
"""    if (dai_dock_panel(p->dock, "Game", &px, &py, &pw, &ph)) {""",
"""    {
        // The preview's chrome. Drawn AFTER the scene panel has reported its
        // rectangle and before the host renders into it - the world lands on
        // top of this plate and inside this border.
        float cx2, cy2, cw2, ch2;
        if (dai_editor_ui_camera_preview(p, &cx2, &cy2, &cw2, &ch2,
                                         nullptr, nullptr, nullptr, nullptr)) {
            const dai_ui_style *cs2 = dai_ui_style_of(ui);
            dai_ui_layer_push(ui, DAI_LAYER_WINDOW + 4);
            dai_ui_rect(ui, cx2 - 1.0f, cy2 - 1.0f, cw2 + 2.0f, ch2 + 2.0f, cs2->panel_border);
            dai_ui_rect(ui, cx2, cy2, cw2, ch2, rgba(0x0A, 0x0A, 0x0C, 255));
            float lh2 = dai_ui_text_height(ui) + 6.0f;
            dai_ui_rect(ui, cx2, cy2 - lh2, cw2, lh2, (cs2->chrome & 0x00FFFFFFu) | 0xE6000000u);
            dai_ui_text(ui, cx2 + 6.0f, cy2 - lh2 + 3.0f, "Camera Preview", cs2->text_dim);
            dai_ui_layer_pop(ui);
        }
    }
    if (dai_dock_panel(p->dock, "Game", &px, &py, &pw, &ph)) {""",
    'camera preview chrome')
wr('src/dai_editor_ui.cpp', s)

# ============================================== 3. the host renders into it
s = rd('examples/editor_demo.cpp')
s = sub1(s,
"""                if (dai_editor_ui_game_camera(panels, &ge, &gl, &gf)) {
                    dai_render_camera2(r, ge, gl, dai_vec3{ 0, 1, 0 }, gf);
                    dai_render_ortho2(r, game_ortho);""",
"""                if (dai_editor_ui_game_camera(panels, &ge, &gl, &gf)) {
                    dai_render_camera2(r, ge, gl, dai_vec3{ 0, 1, 0 }, gf);
                    dai_render_ortho2(r, game_ortho);
                    (void)0;""",
    'game view marker (unchanged)')

s = sub1(s,
"""        dai_vec3 eye, look;
        {   // read the camera back out of the editor so both agree exactly""",
"""        // The camera preview, in the corner of the scene view. It uses the
        // renderer's SECOND view, the same one the Game panel uses - and the
        // editor refuses to offer a preview while that panel is open, so the
        // two can never want it at the same time.
        {
            float cx3, cy3, cw3, ch3, cfov = 60.0f, cortho = 0.0f;
            dai_vec3 ceye{}, clook{};
            if (dai_editor_ui_camera_preview(panels, &cx3, &cy3, &cw3, &ch3,
                                             &ceye, &clook, &cfov, &cortho)) {
                dai_render_camera2(r, ceye, clook, dai_vec3{ 0, 1, 0 }, cfov);
                dai_render_ortho2(r, cortho);
                dai_render_world_clip2(r, cx3 * uis, cy3 * uis, cw3 * uis, ch3 * uis);
            }
        }

        dai_vec3 eye, look;
        {   // read the camera back out of the editor so both agree exactly""",
    'render camera preview')
wr('examples/editor_demo.cpp', s)
print('patch68 ok')
