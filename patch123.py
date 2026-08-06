import io

# ===========================================================================
# 1) 2D-Modus. Unitys Knopf, hier auf der 2.
# ===========================================================================
p = 'include/dai_editor.h'
s = io.open(p, encoding='utf-8').read()
old = """DAI_API void dai_editor_camera_get(const dai_editor *e, dai_vec3 *eye, dai_vec3 *target,
                                   float *fov_deg);"""
new = """DAI_API void dai_editor_camera_get(const dai_editor *e, dai_vec3 *eye, dai_vec3 *target,
                                   float *fov_deg);

/* 2D mode: the camera looks straight down -Z, orbiting is off, and the host
 * renders orthographically. Unity's 2D button, and it is a CAMERA mode rather
 * than a project setting on purpose - a 2D game is a 3D scene you are looking
 * at flat, and the day you want to see it in perspective you press the key
 * again instead of converting anything.
 *
 * dai_editor_cam_ortho_height is what the host passes to dai_render_ortho:
 * half the visible height in world units, derived from the pivot distance so
 * that toggling the mode does not change how big things look. */
DAI_API void  dai_editor_cam_2d(dai_editor *e, int on);
DAI_API int   dai_editor_cam_2d_get(const dai_editor *e);
DAI_API float dai_editor_cam_ortho_height(const dai_editor *e);"""
assert s.count(old) == 1, 'camera_get decl not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)

p = 'src/dai_editor.cpp'
s = io.open(p, encoding='utf-8').read()
old = """    int   cam_mode = 0;                // 0 none, 1 look, 2 pan, 3 orbit, 4 dolly"""
new = """    int   cam_2d = 0;                  // Unity's 2D button
    int   cam_mode = 0;                // 0 none, 1 look, 2 pan, 3 orbit, 4 dolly"""
assert s.count(old) == 1, 'cam_mode member not found'
s = s.replace(old, new)

old = """void dai_editor_camera_get(const dai_editor *e, dai_vec3 *eye, dai_vec3 *target,"""
new = """void dai_editor_cam_2d(dai_editor *e, int on) {
    if (!e) return;
    int want = on ? 1 : 0;
    if (e->cam_2d == want) return;
    e->cam_2d = want;
    cam_ensure_angles(e);
    if (want) {
        // Square on to the XY plane. The pivot stays where it was, so the
        // thing you were looking at is the thing you are still looking at -
        // a 2D button that also moves you somewhere else is a button you
        // press once and then undo.
        dai_vec3 pivot = dai_editor_cam_pivot(e);
        e->cam_yaw = 0.0f;
        e->cam_pitch = 0.0f;
        dai_vec3 fwd, right, upv;
        cam_basis(e, &fwd, &right, &upv);
        e->eye = sub(pivot, mul(fwd, e->cam_pivot_dist));
        cam_apply(e);
    }
}
int dai_editor_cam_2d_get(const dai_editor *e) { return e ? e->cam_2d : 0; }

float dai_editor_cam_ortho_height(const dai_editor *e) {
    if (!e) return 5.0f;
    // The height a PERSPECTIVE camera would show at the pivot. Toggling the
    // mode then does not change the size of anything, which is the whole
    // reason this is computed rather than a constant.
    return e->cam_pivot_dist * std::tan(e->fov * PI / 360.0f);
}

void dai_editor_camera_get(const dai_editor *e, dai_vec3 *eye, dai_vec3 *target,"""
assert s.count(old) == 1, 'camera_get def not found'
s = s.replace(old, new)

# Orbit und Look sind in 2D aus - sonst kippt die Ebene weg.
old = """    int mode = 0;
    if (in->key_alt && in->mouse_left)        mode = 3;   // orbit"""
new = """    int mode = 0;
    // In 2D the plane must stay square on, so the two gestures that would
    // tilt it are simply not available: alt+left pans instead of orbiting,
    // and the right button does nothing. Everything else - pan, wheel,
    // dolly - is unchanged, because those are how you move around a plan.
    if (e->cam_2d) {
        if (in->mouse_middle || (in->key_alt && in->mouse_left)) mode = 2;
    } else
    if (in->key_alt && in->mouse_left)        mode = 3;   // orbit"""
assert s.count(old) == 1, 'mode selection not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)
print('editor: 2D camera mode')

# ===========================================================================
# 2) Der Host: Taste 2, orthographische Projektion, HUD-Texturen.
# ===========================================================================
p = 'examples/editor_demo.cpp'
s = io.open(p, encoding='utf-8').read()

old = """            dai_window_key_down(win, DAI_KEY_BACKSPACE),
        };"""
new = """            dai_window_key_down(win, DAI_KEY_BACKSPACE),
            dai_window_key_down(win, DAI_KEY_2),
        };"""
assert s.count(old) == 1, 'keys array not found'
s = s.replace(old, new)
old = """    int prev_keys[10] = { 0 };"""
new = """    int prev_keys[11] = { 0 };"""
assert s.count(old) == 1, 'prev_keys not found'
s = s.replace(old, new)
old = """        int keys[10] = {"""
new = """        int keys[11] = {"""
assert s.count(old) == 1, 'keys decl not found'
s = s.replace(old, new)

old = """            if (pressed(2)) dai_editor_gizmo_mode(ed, DAI_GIZMO_SCALE);"""
new = """            if (pressed(2)) dai_editor_gizmo_mode(ed, DAI_GIZMO_SCALE);
            // 2 toggles 2D, Unity's button on a key. It is a camera mode:
            // the scene does not change, the way you are looking at it does.
            if (pressed(10)) {
                int on = !dai_editor_cam_2d_get(ed);
                dai_editor_cam_2d(ed, on);
                dai_editor_ui_toast(panels, on ? "2D" : "3D", 1.0f);
            }"""
assert s.count(old) == 1, 'gizmo scale key not found'
s = s.replace(old, new)

old = """        dai_render_world_clip(r, vrx * uis, vry * uis, vrw * uis, vrh * uis);"""
new = """        dai_render_world_clip(r, vrx * uis, vry * uis, vrw * uis, vrh * uis);
        // 2D: an orthographic projection, sized so that switching does not
        // change how big anything looks. 0 puts the perspective back.
        dai_render_ortho(r, dai_editor_cam_2d_get(ed) ? dai_editor_cam_ortho_height(ed) : 0.0f);"""
assert s.count(old) == 1, 'world clip not found'
s = s.replace(old, new)

# --- HUD-Texturen ---------------------------------------------------------
old = """static const char *hud_resolve(const char *text, void *) {"""
new = """// A texture for the HUD, by project path. Cached: the HUD asks once per image
// per frame, and loading a PNG sixty times a second would be a slideshow.
static uint32_t hud_image_cb(const char *path, float *out_w, float *out_h, void *) {
    if (!path || !path[0] || !g_renderer || !g_assets_dir[0]) return 0;
    struct Entry { dai_texture tex; float w, h; };
    static std::unordered_map<std::string, Entry> cache;
    auto it = cache.find(path);
    if (it != cache.end()) {
        if (out_w) *out_w = it->second.w;
        if (out_h) *out_h = it->second.h;
        return it->second.tex;
    }
    char full[700];
    std::snprintf(full, sizeof(full), "%s/%s", g_assets_dir, path);
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
    Entry e{ 0, 0, 0 };
    if (dai_image_load_rgba(full, rgba, &w, &h)) {
        e.tex = dai_render_texture_create(g_renderer, rgba.data(), w, h, 1);
        e.w = (float)w; e.h = (float)h;
    }
    // A failure is cached too: a missing file must cost one failed load, not
    // one per frame for the rest of the session.
    cache[path] = e;
    if (out_w) *out_w = e.w;
    if (out_h) *out_h = e.h;
    return e.tex;
}

static const char *hud_resolve(const char *text, void *) {"""
assert s.count(old) == 1, 'hud_resolve anchor not found'
s = s.replace(old, new)

old = """    dai_editor_ui_tr_host(panels, hud_resolve, nullptr);"""
new = """    dai_editor_ui_tr_host(panels, hud_resolve, nullptr);
    dai_hud_images(hud_image_cb, nullptr);"""
assert s.count(old) == 1, 'tr_host call not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)
print('host: key 2, ortho, hud textures')
