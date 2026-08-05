# -*- coding: utf-8 -*-
import sys, io

def rw(p): return io.open(p, encoding='utf-8').read()
def wr(p, s): io.open(p, 'w', encoding='utf-8').write(s)
def sub1(s, old, new, tag):
    if old not in s: print("MISS", tag); sys.exit(1)
    if s.count(old) != 1: print("AMBIG", tag, s.count(old)); sys.exit(1)
    print("ok", tag); return s.replace(old, new)

# ------------------------------------------------------------ dai_editor.cpp
P = 'src/dai_editor.cpp'
s = rw(P)

s = sub1(s,
"""dai_node dai_editor_pick(dai_editor *e, float mx, float my) {
    if (!e || !e->sync) return DAI_INVALID_NODE;
    dai_scene *scene = dai_doc_sync_scene(e->sync);
    dai_world *world = scene ? dai_scene_world(scene) : nullptr;
    if (!world) return DAI_INVALID_NODE;
    dai_vec3 o, d;
    dai_editor_ray(e, mx, my, &o, &d);
    dai_ray_hit hit{};
    if (!dai_raycast(world, o, d, e->zfar, &hit)) return DAI_INVALID_NODE;
    return dai_doc_sync_node_of_body(e->sync, hit.body);
}""",
"""// Ray against one node's oriented box, in the node's own space. Returns the
// entry distance or a negative number for a miss.
static float ray_box_local(dai_vec3 o, dai_vec3 d, dai_vec3 centre, dai_quat rot,
                           dai_vec3 half, float far_) {
    // Into local space: undo the rotation about the box's centre. A world
    // space AABB would pick a rotated crate by its bounding cube, which is
    // wrong by up to 73% of its volume and feels like clicking thin air.
    dai_quat inv{ -rot.x, -rot.y, -rot.z, rot.w };
    dai_vec3 lo = qrot(inv, sub(o, centre));
    dai_vec3 ld = qrot(inv, d);
    float tmin = 0.0f, tmax = far_;
    const float *pmin = &lo.x, *pdir = &ld.x, *ph = &half.x;
    for (int i = 0; i < 3; ++i) {
        float h = ph[i];
        if (h <= 0.0f) h = 1e-4f;
        if (std::fabs(pdir[i]) < 1e-7f) {
            if (pmin[i] < -h || pmin[i] > h) return -1.0f;   // parallel and outside
            continue;
        }
        float inv_d = 1.0f / pdir[i];
        float t1 = (-h - pmin[i]) * inv_d;
        float t2 = ( h - pmin[i]) * inv_d;
        if (t1 > t2) { float t = t1; t1 = t2; t2 = t; }
        if (t1 > tmin) tmin = t1;
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) return -1.0f;
    }
    return tmin;
}

// Picking is a GRAPHICS question, not a physics one.
//
// It used to be dai_raycast against the physics world, which means a node with
// no collider - a camera, a light, an empty group, anything with "no collider"
// ticked, and every trigger - was invisible to the mouse: the ray went through
// it and hit the floor behind, so clicking a cube "selected the plane". You
// cannot select what you cannot hit, and half the objects in a scene do not
// exist as far as the solver is concerned.
//
// So: test the ray against what is DRAWN. Every node in the document, its own
// box, nearest wins. The physics raycast is kept as a tiebreaker for compound
// bodies, whose real shape the document does not describe.
dai_node dai_editor_pick(dai_editor *e, float mx, float my) {
    if (!e) return DAI_INVALID_NODE;
    dai_doc *d = e->doc;
    if (!d) return DAI_INVALID_NODE;
    dai_vec3 o, dir;
    dai_editor_ray(e, mx, my, &o, &dir);

    std::vector<dai_node> ids(dai_doc_nodes(d, nullptr, 0));
    if (!ids.empty()) dai_doc_nodes(d, ids.data(), (uint32_t)ids.size());

    dai_node best = DAI_INVALID_NODE;
    float best_t = e->zfar;
    for (dai_node n : ids) {
        dai_node_desc r{};
        if (dai_doc_get(d, n, &r) != DAI_OK) continue;
        if (r.hidden) continue;            // you cannot click what is not drawn

        dai_vec3 wp{}, ws{ 1, 1, 1 };
        dai_quat wr{ 0, 0, 0, 1 };
        if (dai_doc_world_transform(d, n, &wp, &wr, &ws) != DAI_OK) {
            wp = r.position; wr = r.rotation; ws = r.scale;
        }
        // While playing, the body is where the object IS - the document still
        // holds the pose from before Play.
        dai_vec3 live{};
        if (dai_editor_live_position(e, n, &live)) wp = live;

        // What the user sees: the render box when the collider was detached
        // from it, the collider otherwise, and a small handle for things that
        // have neither (cameras, lights, empties) so they are clickable at all.
        dai_vec3 half = r.half_extent;
        if (r.render_extent.x || r.render_extent.y || r.render_extent.z)
            half = r.render_extent;
        if (half.x <= 0.0f && half.y <= 0.0f && half.z <= 0.0f)
            half = dai_vec3{ 0.25f, 0.25f, 0.25f };
        half.x = std::fabs(half.x * ws.x);
        half.y = std::fabs(half.y * ws.y);
        half.z = std::fabs(half.z * ws.z);
        dai_vec3 centre = add(wp, qrot(wr, dai_vec3{ r.collider_center.x * ws.x,
                                                     r.collider_center.y * ws.y,
                                                     r.collider_center.z * ws.z }));
        float t = ray_box_local(o, dir, centre, wr, half, best_t);
        if (t >= 0.0f && t < best_t) { best_t = t; best = n; }
    }
    if (best != DAI_INVALID_NODE) return best;

    // Nothing in the document was hit: fall back to the solver, which knows
    // about compound shapes the document cannot describe.
    if (!e->sync) return DAI_INVALID_NODE;
    dai_scene *scene = dai_doc_sync_scene(e->sync);
    dai_world *world = scene ? dai_scene_world(scene) : nullptr;
    if (!world) return DAI_INVALID_NODE;
    dai_ray_hit hit{};
    if (!dai_raycast(world, o, dir, e->zfar, &hit)) return DAI_INVALID_NODE;
    return dai_doc_sync_node_of_body(e->sync, hit.body);
}""", "pick")

# camera anchor, for the resize jump
s = sub1(s,
"""int dai_editor_cam_update(dai_editor *e, const dai_editor_cam_input *in) {""",
"""// Re-anchor the drag origin without moving the camera.
//
// The window backend reports the mouse in RENDER pixels, scaled from window
// pixels by render_size/window_size. Resize the window and that factor changes,
// so the same physical pointer reports a different coordinate - and since a
// resize is done with the button HELD, the camera saw a huge delta and spun.
// The host calls this on the frame the render target changed size.
void dai_editor_cam_anchor(dai_editor *e, float mx, float my) {
    if (!e) return;
    e->cam_last_x = mx;
    e->cam_last_y = my;
}

int dai_editor_cam_update(dai_editor *e, const dai_editor_cam_input *in) {""", "cam_anchor")
wr(P, s)

P = 'include/dai_editor.h'
s = rw(P)
s = sub1(s,
"DAI_API void dai_editor_ray(const dai_editor *e, float mouse_x, float mouse_y,",
"""/* Re-anchor the camera drag to this pointer position without moving it. Call
 * it when the mouse coordinate SYSTEM changed rather than the mouse - a window
 * resize rescales it, and a resize is done with the button held, so without
 * this the scene camera spins as the window is dragged. */
DAI_API void dai_editor_cam_anchor(dai_editor *e, float mouse_x, float mouse_y);

DAI_API void dai_editor_ray(const dai_editor *e, float mouse_x, float mouse_y,""", "cam_anchor decl")
wr(P, s)
print("patch7 done")
