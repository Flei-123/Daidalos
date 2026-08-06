import io

# ===========================================================================
# 1) Die Scene-Kamera drehte sich, wenn man den Tab wechselte.
#
# Der Host las die Kamera zurueck, indem er einen STRAHL durch die Mitte des
# Viewport-Rechtecks schoss. Ist der Scene-Tab nicht sichtbar, ist dieses
# Rechteck 0x0 - der "Strahl durch die Mitte" ist dann der Strahl durch die
# Ecke (0,0), und genau diese Richtung schrieb der Host als Blickrichtung
# zurueck. Einmal weg und zurueck, und die Kamera hatte sich um ein halbes
# Sichtfeld gedreht.
#
# Zwei Dinge dagegen: der Editor gibt seine Kamera jetzt direkt heraus, statt
# sie sich aus einem Strahl rekonstruieren zu lassen, und ein Frame ohne
# sichtbaren Viewport fasst sie gar nicht erst an.
# ===========================================================================
p = 'include/dai_editor.h'
s = io.open(p, encoding='utf-8').read()
old = """DAI_API dai_vec3 dai_editor_cam_pivot(const dai_editor *e);"""
new = """DAI_API dai_vec3 dai_editor_cam_pivot(const dai_editor *e);

/* The camera, as the editor holds it. Exists because the host used to rebuild
 * it from a ray through the middle of the viewport - and a viewport that is
 * not on screen is 0x0, so "the middle" was the corner and the camera turned
 * by half a field of view every time the Scene tab was hidden. A value you
 * can ask for cannot be reconstructed wrongly. */
DAI_API void dai_editor_camera_get(const dai_editor *e, dai_vec3 *eye, dai_vec3 *target,
                                   float *fov_deg);"""
assert s.count(old) == 1, 'cam_pivot decl not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)

p = 'src/dai_editor.cpp'
s = io.open(p, encoding='utf-8').read()
old = """dai_vec3 dai_editor_cam_pivot(const dai_editor *e) {"""
new = """void dai_editor_camera_get(const dai_editor *e, dai_vec3 *eye, dai_vec3 *target,
                           float *fov_deg) {
    if (!e) return;
    if (eye) *eye = e->eye;
    if (target) *target = e->target;
    if (fov_deg) *fov_deg = e->fov;
}

dai_vec3 dai_editor_cam_pivot(const dai_editor *e) {"""
assert s.count(old) == 1, 'cam_pivot def not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)

p = 'examples/editor_demo.cpp'
s = io.open(p, encoding='utf-8').read()
old = """        dai_vec3 eye, look;
        {   // read the camera back out of the editor so both agree exactly
            float cx, cy, cw2, ch2;
            dai_editor_ui_viewport_rect(panels, &cx, &cy, &cw2, &ch2);
            dai_vec3 o, d;
            dai_editor_ray(ed, cx + cw2 * 0.5f, cy + ch2 * 0.5f, &o, &d);
            eye = o;
            look = dai_vec3{ o.x + d.x, o.y + d.y, o.z + d.z };
        }"""
new = """        // The camera, asked for directly. This used to shoot a ray through the
        // middle of the viewport and call the result "forward" - which works
        // until the viewport is 0x0, and it IS 0x0 whenever the Scene tab is
        // not the visible one. Then the middle is the corner, the corner ray
        // became the camera's direction, and switching to the Script tab and
        // back turned the view by half a field of view.
        dai_vec3 eye{}, look{};
        dai_editor_camera_get(ed, &eye, &look, nullptr);"""
assert s.count(old) == 1, 'camera readback not found'
s = s.replace(old, new)

old = """        dai_editor_camera(ed, eye, look, dai_vec3{ 0, 1, 0 }, 55.0f, 0.1f, 300.0f,
                          vrw, vrh);"""
new = """        // ...and it is only written back when there IS a viewport. A frame in
        // which the scene is not on screen has nothing to say about the
        // camera, and saying it anyway is what broke this.
        if (vrw > 0.0f && vrh > 0.0f)
            dai_editor_camera(ed, eye, look, dai_vec3{ 0, 1, 0 }, 55.0f, 0.1f, 300.0f,
                              vrw, vrh);"""
assert s.count(old) == 1, 'camera write not found'
s = s.replace(old, new)

# ===========================================================================
# 2) Play leert die Console.
# ===========================================================================
old = """            if (st_now == DAI_EDITOR_PLAY && !g_scripts_live) scripts_start();"""
new = """            if (st_now == DAI_EDITOR_PLAY && !g_scripts_live) {
                // A run starts with an empty console. Otherwise the first
                // error of THIS run is somewhere below the errors of the last
                // three, and the only way to tell them apart is the clock.
                if (g_panels_for_log) dai_editor_ui_log_clear(g_panels_for_log);
                scripts_start();
            }"""
assert s.count(old) == 1, 'scripts_start call not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)
print('camera drift, console clear on play')

# ===========================================================================
# 3) Static: es TUT etwas - es ist der Bewegungstyp. Nur sagte das nichts.
# ===========================================================================
p = 'src/dai_editor_ui.cpp'
s = io.open(p, encoding='utf-8').read()
old = """            dai_ui_text(ui2, cx + 18.0f, cy + 1.0f, "Static", st2->text_dim);
            if (over_s && p2) r.motion = is_static ? DAI_DYNAMIC : DAI_STATIC;"""
new = """            dai_ui_text(ui2, cx + 18.0f, cy + 1.0f, "Static", st2->text_dim);
            // Unity's Static is a hint for batching and lightmaps. Here it is
            // the MOTION TYPE, which is the same promise made honestly: a
            // static body never moves, so the solver can leave it alone.
            //
            // Turning it off restores KINEMATIC rather than DYNAMIC when the
            // object has no rigidbody - a collider with nothing driving it
            // that suddenly falls through the floor is not what unticking a
            // box should mean.
            if (over_s && p2)
                r.motion = is_static ? (r.no_rigidbody ? DAI_KINEMATIC : DAI_DYNAMIC)
                                     : DAI_STATIC;
            if (over_s) {
                const char *tip = is_static
                    ? "Static: this body never moves. Physics skips it, and nothing "
                      "a script does to its transform will be simulated."
                    : "Not static: the body is simulated. Tick this for floors, "
                      "walls and anything that should never be pushed.";
                dai_ui_tooltip_at(ui2, cx, hy + 4.0f, sw, 26.0f, tip);
            }"""
assert s.count(old) == 1, 'static toggle not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)
print('static toggle explains itself')
