// Editor core. See include/dai_editor.h.
//
// No renderer, no UI, no Vulkan: this turns pixels into rays, rays into
// selections and drags into document edits. Undo lives in dai_doc, because the
// document is what is being edited - see the header comment there for why that
// is the difference between "undo delete works" and "undo delete is a TODO".

#include "dai_editor.h"

#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

const float PI = 3.14159265358979f;

dai_vec3 sub(dai_vec3 a, dai_vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
dai_vec3 add(dai_vec3 a, dai_vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
dai_vec3 mul(dai_vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
float dot(dai_vec3 a, dai_vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
dai_vec3 cross(dai_vec3 a, dai_vec3 b) {
    return { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}
float length(dai_vec3 v) { return std::sqrt(dot(v, v)); }
dai_vec3 norm(dai_vec3 v) {
    float l = length(v);
    return l > 1e-8f ? mul(v, 1.0f / l) : dai_vec3{ 0, 0, 0 };
}
dai_quat qmul(dai_quat a, dai_quat b) {
    return { a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
             a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
             a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
             a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z };
}
dai_quat qaxis(dai_vec3 axis, float angle) {
    dai_vec3 a = norm(axis);
    float s = std::sin(angle * 0.5f);
    return { a.x*s, a.y*s, a.z*s, std::cos(angle * 0.5f) };
}
dai_vec3 qrot(dai_quat q, dai_vec3 v) {
    dai_vec3 u{ q.x, q.y, q.z };
    dai_vec3 uv = cross(u, v);
    dai_vec3 uuv = cross(u, uv);
    return { v.x + 2.0f*(q.w*uv.x + uuv.x),
             v.y + 2.0f*(q.w*uv.y + uuv.y),
             v.z + 2.0f*(q.w*uv.z + uuv.z) };
}
float snap_to(float v, float step) { return step > 0.0f ? std::round(v / step) * step : v; }

dai_vec3 axis_vector(int axis) {
    switch (axis) {
    case DAI_AXIS_X: return { 1, 0, 0 };
    case DAI_AXIS_Y: return { 0, 1, 0 };
    case DAI_AXIS_Z: return { 0, 0, 1 };
    default: return { 0, 0, 0 };
    }
}
dai_vec3 axis_color(int axis) {
    switch (axis) {
    case DAI_AXIS_X:  return { 0.90f, 0.25f, 0.25f };
    case DAI_AXIS_Y:  return { 0.35f, 0.85f, 0.30f };
    case DAI_AXIS_Z:  return { 0.30f, 0.50f, 0.95f };
    case DAI_AXIS_XY: return { 0.85f, 0.85f, 0.30f };
    case DAI_AXIS_XZ: return { 0.85f, 0.40f, 0.85f };
    case DAI_AXIS_YZ: return { 0.30f, 0.85f, 0.85f };
    default:          return { 0.90f, 0.90f, 0.90f };
    }
}
// The two in-plane axes of a plane handle.
void plane_axes(int axis, dai_vec3 *u, dai_vec3 *v) {
    switch (axis) {
    case DAI_AXIS_XY: *u = { 1, 0, 0 }; *v = { 0, 1, 0 }; break;
    case DAI_AXIS_XZ: *u = { 1, 0, 0 }; *v = { 0, 0, 1 }; break;
    default:          *u = { 0, 1, 0 }; *v = { 0, 0, 1 }; break;   // YZ
    }
}

struct DragItem {
    dai_node n;
    dai_vec3 wpos;       // where the object WAS when the drag started - live,
                         // not documented: during play the document holds the
                         // pre-play pose and adding a mouse delta to that is
                         // how a dragged object used to teleport
    dai_quat wrot;
    dai_vec3 scale;      // local
    // Where this drag wants the object to end up. Filled every update; while
    // playing it is written to the BODY after the resync, because during play
    // the body is the truth and the document must stay untouched.
    dai_vec3 target_pos;
    dai_quat target_rot;
    bool     has_target = false;
};

} // namespace

struct dai_editor {
    dai_doc      *doc = nullptr;
    dai_doc_sync *sync = nullptr;

    dai_vec3 eye{ 0, 5, 10 }, target{ 0, 0, 0 }, up{ 0, 1, 0 };
    float fov = 55.0f, znear = 0.1f, zfar = 500.0f;
    float vw = 1280.0f, vh = 720.0f;
    // Where the viewport's pixel (0,0) sits on the surface. The 3D view lives
    // inside the scene WINDOW, so the pixel the user clicked and the pixel
    // the camera maths mean are only the same when this offset is applied.
    float vx = 0.0f, vy = 0.0f;

    // Camera orientation is kept as yaw/pitch, not as a target point. Storing a
    // target and rotating it drifts: repeated look-around would slowly change
    // the distance to it, and the roll would creep away from level.
    float cam_yaw = 0.0f, cam_pitch = 0.0f;
    float cam_pivot_dist = 10.0f;      // where orbit and dolly aim
    float cam_speed = 6.0f;
    bool  cam_angles_valid = false;
    int   cam_mode = 0;                // 0 none, 1 look, 2 pan, 3 orbit, 4 dolly
    float cam_last_x = 0, cam_last_y = 0;
    bool  cam_prev_focus = false;

    std::vector<dai_node> selection;
    int mode = DAI_GIZMO_TRANSLATE;
    float snap_translate = 0.0f, snap_rotate = 0.0f, snap_scale = 0.0f;
    float gizmo_pixels = 90.0f;
    int hover_axis = DAI_AXIS_NONE;

    int  state = DAI_EDITOR_EDIT;
    dai_tick play_start_tick = 0;
    // The document as it was when Play was pressed. Everything done while the
    // simulation runs - a gizmo drag, a typed position, a new object - is a
    // rehearsal, and Stop throws it away. Without this, moving a crate during
    // play wrote through to the document and Stop "restored" the scene to the
    // moved crate, which is the one thing play mode must never do.
    dai_doc_state *play_state = nullptr;

    bool dragging = false;
    bool drag_screen_rotate = false;   // ring seen edge-on, fall back to screen angle
    float drag_screen_start = 0.0f;
    float drag_screen_sign = 1.0f;
    int  drag_axis = DAI_AXIS_NONE;
    dai_vec3 drag_start_point{};
    dai_vec3 drag_plane_normal{};
    dai_vec3 drag_center{};
    float    drag_gizmo_len = 1.0f;
    std::vector<DragItem> drag_items;
};

namespace {

dai_world *editor_world(const dai_editor *e) {
    dai_scene *sc = e->sync ? dai_doc_sync_scene(e->sync) : nullptr;
    return sc ? dai_scene_world(sc) : nullptr;
}

// where a ray meets a plane; false when they are parallel or it is behind us
bool ray_plane(dai_vec3 o, dai_vec3 d, dai_vec3 p, dai_vec3 n, dai_vec3 *out) {
    float denom = dot(d, n);
    if (std::fabs(denom) < 1e-6f) return false;
    float t = dot(sub(p, o), n) / denom;
    if (t < 0.0f) return false;
    *out = add(o, mul(d, t));
    return true;
}

void camera_basis(const dai_editor *e, dai_vec3 *fwd, dai_vec3 *right, dai_vec3 *upv) {
    *fwd = norm(sub(e->target, e->eye));
    *right = norm(cross(*fwd, e->up));
    *upv = cross(*right, *fwd);
}

void resync(dai_editor *e) { if (e->sync) dai_doc_sync_apply(e->sync); }

// Distance in pixels from p to the segment ab, all in screen space.
float seg_distance(float px, float py, float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay;
    float len2 = dx*dx + dy*dy;
    float t = len2 > 1e-6f ? ((px - ax)*dx + (py - ay)*dy) / len2 : 0.0f;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float cx = ax + dx*t, cy = ay + dy*t;
    return std::sqrt((px - cx)*(px - cx) + (py - cy)*(py - cy));
}

bool point_in_quad(float px, float py, const float *qx, const float *qy) {
    // Convex quad, consistent winding test. The projected handle stays convex
    // for any camera, so this is exact rather than an approximation.
    int sign = 0;
    for (int i = 0; i < 4; ++i) {
        int j = (i + 1) & 3;
        float cross_z = (qx[j] - qx[i]) * (py - qy[i]) - (qy[j] - qy[i]) * (px - qx[i]);
        int s = (cross_z > 0) ? 1 : (cross_z < 0 ? -1 : 0);
        if (!s) continue;
        if (!sign) sign = s;
        else if (sign != s) return false;
    }
    return sign != 0;
}

void capture(dai_editor *e, std::vector<DragItem> &out) {
    out.clear();
    for (dai_node n : e->selection) {
        dai_node_desc rec{};
        if (dai_doc_get(e->doc, n, &rec) != DAI_OK) continue;
        DragItem it{};
        it.n = n;
        it.scale = rec.scale;
        // The pose the object is ACTUALLY at, which during play is the body and
        // not the document. Capturing the document here is what made a dragged
        // object "spawn" higher every time: the crate had fallen to y = 6.65
        // while the document still said 8.0, so a 1.85 m drag put it at 9.85 -
        // a 3.2 m jump - and the next drag started from 9.85 again.
        if (!dai_editor_live_transform(e, n, &it.wpos, &it.wrot, nullptr))
            dai_doc_world_transform(e->doc, n, &it.wpos, &it.wrot, nullptr);
        out.push_back(it);
    }
}

// The body sits at the COLLIDER; a centre offset moved it away from the node.
// A pose meant for the OBJECT therefore has to be pushed back out by that
// offset before it can be written to the body, or every drag during play would
// walk the object off by one offset.
void set_live_transform(dai_editor *e, dai_node n, dai_vec3 pos, dai_quat rot) {
    if (!e->sync) return;
    dai_scene *sc = dai_doc_sync_scene(e->sync);
    dai_entity ent = dai_doc_sync_entity(e->sync, n);
    if (!sc || !ent) return;
    dai_body b = dai_scene_body(sc, ent);
    if (b == DAI_INVALID_BODY) return;      // render-only node: nothing to move
    dai_node_desc rec{};
    if (dai_doc_get(e->doc, n, &rec) == DAI_OK &&
        (rec.collider_center.x || rec.collider_center.y || rec.collider_center.z)) {
        dai_vec3 ws{ 1, 1, 1 };
        dai_doc_world_transform(e->doc, n, nullptr, nullptr, &ws);
        dai_vec3 off = qrot(rot, dai_vec3{ rec.collider_center.x * ws.x,
                                           rec.collider_center.y * ws.y,
                                           rec.collider_center.z * ws.z });
        pos = add(pos, off);
    }
    dai_body_set_transform(editor_world(e), b, pos, rot);
}

// Where a drag writes its result. Editing: the document, which the sync layer
// then pushes into the world - that is what makes the edit undoable. Playing or
// paused: the BODY, and the document is left exactly as Play found it, so Stop
// still restores the scene and the drag stays the rehearsal it is meant to be.
void drag_write(dai_editor *e, DragItem &it, dai_vec3 pos, const dai_quat *rot) {
    it.target_pos = pos;
    it.target_rot = rot ? *rot : it.wrot;
    it.has_target = true;
    if (e->state != DAI_EDITOR_EDIT) return;    // pushed to the body after resync
    dai_doc_set_world_position(e->doc, it.n, pos);
    if (rot) dai_doc_set_world_rotation(e->doc, it.n, *rot);
}

// A node whose ancestor is also selected must not be moved twice: the ancestor
// already carries it. Without this, dragging a parent and child together moves
// the child at double speed - a classic editor bug.
bool ancestor_selected(const dai_editor *e, dai_node n) {
    dai_node_desc rec{};
    if (dai_doc_get(e->doc, n, &rec) != DAI_OK) return false;
    dai_node p = rec.parent;
    uint32_t guard = dai_doc_count(e->doc) + 1;
    while (p && guard--) {
        if (dai_editor_is_selected(e, p)) return true;
        if (dai_doc_get(e->doc, p, &rec) != DAI_OK) break;
        p = rec.parent;
    }
    return false;
}

} // namespace

extern "C" {

dai_editor *dai_editor_create(dai_doc *doc, dai_doc_sync *sync) {
    if (!doc) return nullptr;
    dai_editor *e = new dai_editor();
    e->doc = doc;
    e->sync = sync;
    return e;
}

void dai_editor_destroy(dai_editor *e) {
    if (e && e->play_state) dai_doc_state_free(e->play_state);
    delete e;
}
dai_doc *dai_editor_doc(const dai_editor *e) { return e ? e->doc : nullptr; }

void dai_editor_camera_viewport(dai_editor *e, float vw, float vh) {
    // Just the size. Going through dai_editor_camera to change it would also
    // reset the orbit angles from eye/target, so a window resize would jerk the
    // camera - which is not what resizing a window should do.
    if (!e) return;
    if (vw > 0) e->vw = vw;
    if (vh > 0) e->vh = vh;
}
void dai_editor_camera_viewport_rect(dai_editor *e, float x, float y, float w, float h) {
    if (!e) return;
    if (w > 0) e->vw = w;
    if (h > 0) e->vh = h;
    e->vx = x;
    e->vy = y;
}

void dai_editor_camera(dai_editor *e, dai_vec3 eye, dai_vec3 target, dai_vec3 up,
                       float fov, float znear, float zfar, float vw, float vh) {
    if (!e) return;
    e->eye = eye; e->target = target; e->up = up;
    e->fov = fov; e->znear = znear; e->zfar = zfar;
    if (vw > 0) e->vw = vw;
    if (vh > 0) e->vh = vh;
    // Setting the camera from outside has to update the angles too, or the
    // first look-around would snap back to wherever the camera used to point.
    dai_vec3 d = sub(target, eye);
    float len = length(d);
    if (len > 1e-5f) {
        e->cam_pivot_dist = len;
        d = mul(d, 1.0f / len);
        e->cam_yaw = std::atan2(d.x, -d.z);
        e->cam_pitch = std::asin(d.y < -1.0f ? -1.0f : (d.y > 1.0f ? 1.0f : d.y));
        e->cam_angles_valid = true;
    }
}

void dai_editor_ray(const dai_editor *e, float mx, float my, dai_vec3 *origin, dai_vec3 *dir) {
    if (!e) return;
    // Camera basis plus the pixel's angle. No matrix inverse, and it cannot
    // disagree with dai_editor_project below, which uses the same basis.
    dai_vec3 fwd, right, upv;
    camera_basis(e, &fwd, &right, &upv);
    float t = std::tan(e->fov * PI / 360.0f);
    float aspect = e->vw / (e->vh > 0 ? e->vh : 1.0f);
    float ndc_x = (2.0f * (mx - e->vx) / e->vw) - 1.0f;
    float ndc_y = 1.0f - (2.0f * (my - e->vy) / e->vh); // pixels grow downwards
    dai_vec3 d = norm(add(add(fwd, mul(right, ndc_x * t * aspect)), mul(upv, ndc_y * t)));
    if (origin) *origin = e->eye;
    if (dir) *dir = d;
}

int dai_editor_project(const dai_editor *e, dai_vec3 world, float *out_x, float *out_y) {
    if (!e) return 0;
    dai_vec3 fwd, right, upv;
    camera_basis(e, &fwd, &right, &upv);
    dai_vec3 v = sub(world, e->eye);
    float z = dot(v, fwd);
    if (z <= e->znear) return 0;                       // behind the camera
    float t = std::tan(e->fov * PI / 360.0f);
    float aspect = e->vw / (e->vh > 0 ? e->vh : 1.0f);
    float ndc_x = dot(v, right) / (z * t * aspect);
    float ndc_y = dot(v, upv) / (z * t);
    if (out_x) *out_x = e->vx + (ndc_x + 1.0f) * 0.5f * e->vw;
    if (out_y) *out_y = e->vy + (1.0f - ndc_y) * 0.5f * e->vh;
    return 1;
}

// ------------------------------------------------------------- selection

dai_node dai_editor_pick(dai_editor *e, float mx, float my) {
    if (!e || !e->sync) return DAI_INVALID_NODE;
    dai_scene *scene = dai_doc_sync_scene(e->sync);
    dai_world *world = scene ? dai_scene_world(scene) : nullptr;
    if (!world) return DAI_INVALID_NODE;
    dai_vec3 o, d;
    dai_editor_ray(e, mx, my, &o, &d);
    dai_ray_hit hit{};
    if (!dai_raycast(world, o, d, e->zfar, &hit)) return DAI_INVALID_NODE;
    return dai_doc_sync_node_of_body(e->sync, hit.body);
}

void dai_editor_select(dai_editor *e, dai_node n, int additive) {
    if (!e) return;
    if (!additive) e->selection.clear();
    if (n == DAI_INVALID_NODE) return;
    for (size_t i = 0; i < e->selection.size(); ++i)
        if (e->selection[i] == n) {
            if (additive) e->selection.erase(e->selection.begin() + (long)i);   // toggle
            return;
        }
    e->selection.push_back(n);
}

void dai_editor_deselect_all(dai_editor *e) { if (e) e->selection.clear(); }
uint32_t dai_editor_selection_count(const dai_editor *e) {
    return e ? (uint32_t)e->selection.size() : 0;
}
dai_node dai_editor_selected(const dai_editor *e, uint32_t i) {
    return (e && i < e->selection.size()) ? e->selection[i] : DAI_INVALID_NODE;
}
int dai_editor_is_selected(const dai_editor *e, dai_node n) {
    if (!e) return 0;
    for (dai_node s : e->selection) if (s == n) return 1;
    return 0;
}

dai_vec3 dai_editor_selection_center(const dai_editor *e) {
    dai_vec3 c{ 0, 0, 0 };
    if (!e || e->selection.empty()) return c;
    uint32_t n = 0;
    for (dai_node id : e->selection) {
        dai_vec3 p{};
        // Live, not documented: during play the gizmo must sit ON the body,
        // or the falling crate leaves its handle hanging where play started.
        if (dai_editor_live_position(e, id, &p)) { c = add(c, p); ++n; }
    }
    return n ? mul(c, 1.0f / (float)n) : c;
}

// ----------------------------------------------------------------- gizmo

void dai_editor_gizmo_mode(dai_editor *e, int mode) { if (e) e->mode = mode; }
int  dai_editor_gizmo_mode_get(const dai_editor *e) { return e ? e->mode : 0; }

void dai_editor_snap(dai_editor *e, float t, float r, float sc) {
    if (!e) return;
    e->snap_translate = t; e->snap_rotate = r; e->snap_scale = sc;
}

void dai_editor_gizmo_size(dai_editor *e, float pixels) {
    if (e && pixels > 1.0f) e->gizmo_pixels = pixels;
}

float dai_editor_gizmo_scale(const dai_editor *e) {
    if (!e || e->selection.empty()) return 0.0f;
    dai_vec3 c = dai_editor_selection_center(e);
    dai_vec3 fwd, right, upv;
    camera_basis(e, &fwd, &right, &upv);
    // Depth along the view direction, not straight line distance: using the
    // latter makes the gizmo shrink as it moves towards the screen edges.
    float depth = dot(sub(c, e->eye), fwd);
    if (depth < e->znear) depth = e->znear;
    float world_per_pixel = 2.0f * depth * std::tan(e->fov * PI / 360.0f) / (e->vh > 0 ? e->vh : 1.0f);
    return e->gizmo_pixels * world_per_pixel;
}

uint32_t dai_editor_gizmo_lines(const dai_editor *e, dai_gizmo_line *out, uint32_t max) {
    if (!e || e->selection.empty()) return 0;
    dai_vec3 c = dai_editor_selection_center(e);
    float len = dai_editor_gizmo_scale(e);
    if (len <= 0.0f) return 0;
    int active = e->dragging ? e->drag_axis : e->hover_axis;

    uint32_t w = 0;
    auto emit = [&](dai_vec3 a, dai_vec3 b, int axis) {
        if (out && w < max) {
            dai_gizmo_line &l = out[w];
            l.a = a; l.b = b;
            l.axis = axis;
            l.highlighted = (axis == active) ? 1 : 0;
            l.color = axis_color(axis);
            if (l.highlighted) l.color = { 1.0f, 0.85f, 0.20f };
        }
        ++w;
    };

    if (e->mode == DAI_GIZMO_ROTATE) {
        // Three rings. Segments rather than an analytic circle so the frontend
        // only ever needs to draw lines - one primitive for the whole gizmo.
        const int SEG = 32;
        for (int a = DAI_AXIS_X; a <= DAI_AXIS_Z; ++a) {
            dai_vec3 n = axis_vector(a);
            dai_vec3 u = norm(cross(n, std::fabs(n.y) > 0.9f ? dai_vec3{ 1, 0, 0 } : dai_vec3{ 0, 1, 0 }));
            dai_vec3 v = cross(n, u);
            for (int i = 0; i < SEG; ++i) {
                float t0 = 2.0f * PI * i / SEG, t1 = 2.0f * PI * (i + 1) / SEG;
                dai_vec3 p0 = add(c, add(mul(u, std::cos(t0) * len), mul(v, std::sin(t0) * len)));
                dai_vec3 p1 = add(c, add(mul(u, std::cos(t1) * len), mul(v, std::sin(t1) * len)));
                emit(p0, p1, a);
            }
        }
        return out ? (w < max ? w : max) : w;
    }

    for (int a = DAI_AXIS_X; a <= DAI_AXIS_Z; ++a) {
        dai_vec3 dir = axis_vector(a);
        dai_vec3 tip = add(c, mul(dir, len));
        emit(c, tip, a);
        if (e->mode == DAI_GIZMO_SCALE) {
            // A little box at the tip, so translate and scale never look alike.
            dai_vec3 u, v;
            plane_axes(a == DAI_AXIS_X ? DAI_AXIS_YZ : (a == DAI_AXIS_Y ? DAI_AXIS_XZ : DAI_AXIS_XY), &u, &v);
            float k = len * 0.06f;
            dai_vec3 p00 = add(tip, add(mul(u, -k), mul(v, -k)));
            dai_vec3 p10 = add(tip, add(mul(u,  k), mul(v, -k)));
            dai_vec3 p11 = add(tip, add(mul(u,  k), mul(v,  k)));
            dai_vec3 p01 = add(tip, add(mul(u, -k), mul(v,  k)));
            emit(p00, p10, a); emit(p10, p11, a); emit(p11, p01, a); emit(p01, p00, a);
        } else {
            // Arrow head: two short back-swept lines, cheap and readable.
            dai_vec3 u, v;
            plane_axes(a == DAI_AXIS_X ? DAI_AXIS_YZ : (a == DAI_AXIS_Y ? DAI_AXIS_XZ : DAI_AXIS_XY), &u, &v);
            dai_vec3 back = add(c, mul(dir, len * 0.85f));
            emit(tip, add(back, mul(u, len * 0.05f)), a);
            emit(tip, add(back, mul(u, len * -0.05f)), a);
            emit(tip, add(back, mul(v, len * 0.05f)), a);
            emit(tip, add(back, mul(v, len * -0.05f)), a);
        }
    }

    // Plane handles: small squares in the corner between two axes.
    for (int a = DAI_AXIS_XY; a <= DAI_AXIS_YZ; ++a) {
        dai_vec3 u, v;
        plane_axes(a, &u, &v);
        float in = len * 0.25f, out_ = len * 0.50f;
        dai_vec3 p00 = add(c, add(mul(u, in),   mul(v, in)));
        dai_vec3 p10 = add(c, add(mul(u, out_), mul(v, in)));
        dai_vec3 p11 = add(c, add(mul(u, out_), mul(v, out_)));
        dai_vec3 p01 = add(c, add(mul(u, in),   mul(v, out_)));
        emit(p00, p10, a); emit(p10, p11, a); emit(p11, p01, a); emit(p01, p00, a);
    }
    return out ? (w < max ? w : max) : w;
}

int dai_editor_gizmo_hit(const dai_editor *e, float mx, float my) {
    if (!e || e->selection.empty()) return DAI_AXIS_NONE;
    dai_vec3 c = dai_editor_selection_center(e);
    float len = dai_editor_gizmo_scale(e);
    if (len <= 0.0f) return DAI_AXIS_NONE;

    // Plane handles first: they sit between the axes and are the smaller
    // target, so an axis line crossing them must not steal the click.
    if (e->mode != DAI_GIZMO_ROTATE) {
        for (int a = DAI_AXIS_XY; a <= DAI_AXIS_YZ; ++a) {
            dai_vec3 u, v;
            plane_axes(a, &u, &v);
            float in = len * 0.25f, out_ = len * 0.50f;
            dai_vec3 corner[4] = {
                add(c, add(mul(u, in),   mul(v, in))),
                add(c, add(mul(u, out_), mul(v, in))),
                add(c, add(mul(u, out_), mul(v, out_))),
                add(c, add(mul(u, in),   mul(v, out_))),
            };
            float qx[4], qy[4];
            bool ok = true;
            for (int i = 0; i < 4 && ok; ++i) ok = dai_editor_project(e, corner[i], &qx[i], &qy[i]) != 0;
            if (ok && point_in_quad(mx, my, qx, qy)) return a;
        }
    }

    // Then the nearest line, using exactly the geometry the frontend draws -
    // hit test and picture cannot drift apart this way.
    uint32_t n = dai_editor_gizmo_lines(e, nullptr, 0);
    std::vector<dai_gizmo_line> lines(n);
    if (n) dai_editor_gizmo_lines(e, lines.data(), n);

    const float TOL = 9.0f;
    float best = TOL;
    int best_axis = DAI_AXIS_NONE;
    for (const dai_gizmo_line &l : lines) {
        if (l.axis >= DAI_AXIS_XY && l.axis <= DAI_AXIS_YZ) continue;   // handled above
        float ax, ay, bx, by;
        if (!dai_editor_project(e, l.a, &ax, &ay)) continue;
        if (!dai_editor_project(e, l.b, &bx, &by)) continue;
        float d = seg_distance(mx, my, ax, ay, bx, by);
        if (d < best) { best = d; best_axis = l.axis; }
    }
    return best_axis;
}

void dai_editor_gizmo_hover(dai_editor *e, float mx, float my) {
    if (!e || e->dragging) return;
    e->hover_axis = dai_editor_gizmo_hit(e, mx, my);
}
int dai_editor_gizmo_hovered(const dai_editor *e) { return e ? e->hover_axis : DAI_AXIS_NONE; }

// ---------------------------------------------------------------- editing

void dai_editor_drag_begin(dai_editor *e, int axis, float mx, float my) {
    if (!e || e->selection.empty() || axis == DAI_AXIS_NONE || e->dragging) return;
    capture(e, e->drag_items);
    if (e->drag_items.empty()) return;

    dai_vec3 centre = dai_editor_selection_center(e);
    dai_vec3 o, d;
    dai_editor_ray(e, mx, my, &o, &d);

    dai_vec3 a = axis_vector(axis);
    dai_vec3 to_cam = norm(sub(e->eye, centre));
    dai_vec3 n;
    if (e->mode == DAI_GIZMO_ROTATE && axis <= DAI_AXIS_Z) {
        n = a;                                   // spin in the plane of the ring
    } else if (axis <= DAI_AXIS_Z) {
        // Contains the axis and faces the camera as much as possible. Picking
        // a fixed plane instead makes a Y drag useless when you look along it.
        n = norm(cross(a, cross(to_cam, a)));
    } else if (axis == DAI_AXIS_XY) { n = { 0, 0, 1 };
    } else if (axis == DAI_AXIS_XZ) { n = { 0, 1, 0 };
    } else if (axis == DAI_AXIS_YZ) { n = { 1, 0, 0 };
    } else { n = to_cam; }
    if (dot(n, n) < 1e-8f) n = to_cam;

    // A ring seen exactly edge-on has no usable drag plane - the ray never
    // meets it. Refusing to rotate would be correct and useless, so fall back
    // to the angle the cursor sweeps around the gizmo on screen, which is what
    // the user is aiming at anyway.
    e->drag_screen_rotate = false;
    if (e->mode == DAI_GIZMO_ROTATE && std::fabs(dot(d, n)) < 0.12f) {
        float cx, cy;
        if (!dai_editor_project(e, centre, &cx, &cy)) return;
        e->drag_screen_rotate = true;
        e->drag_screen_start = std::atan2(my - cy, mx - cx);
        // Screen y grows downwards, so a positive turn about an axis pointing
        // at the camera reads as a *decreasing* atan2.
        e->drag_screen_sign = (dot(n, to_cam) >= 0.0f) ? -1.0f : 1.0f;
    }

    dai_vec3 hit{};
    if (!e->drag_screen_rotate && !ray_plane(o, d, centre, n, &hit)) return;

    e->dragging = true;
    e->drag_axis = axis;
    e->drag_plane_normal = n;
    e->drag_start_point = hit;
    e->drag_center = centre;
    e->drag_gizmo_len = dai_editor_gizmo_scale(e);
    if (e->drag_gizmo_len <= 1e-4f) e->drag_gizmo_len = 1.0f;

    const char *name = e->mode == DAI_GIZMO_ROTATE ? "Rotate"
                     : e->mode == DAI_GIZMO_SCALE  ? "Scale" : "Move";
    dai_doc_begin(e->doc, name);   // everything until drag_end is one undo step
}

// Move, rotate and scale each have their own early returns, so the push to the
// scene lives in a wrapper rather than at the end of one of them.
static void drag_update_impl(dai_editor *e, float mx, float my);

void dai_editor_drag_update(dai_editor *e, float mx, float my) {
    if (!e || !e->dragging) return;
    drag_update_impl(e, mx, my);
    // Now, not at drag_end. The document moves the moment the pointer does;
    // without this the object sits still until the button is released and then
    // teleports - dragging a number instead of a thing. dai_doc_sync_apply only
    // touches nodes whose revision changed, so a drag costs the handful that
    // are actually moving.
    resync(e);
    // While playing the body is the truth, and it is written AFTER the resync
    // on purpose: a scale change rebuilds the body at the document pose, and
    // this is what puts it back where the object is. Nothing here touches the
    // document, so Stop still restores the scene exactly.
    if (e->state != DAI_EDITOR_EDIT)
        for (DragItem &it : e->drag_items)
            if (it.has_target && !ancestor_selected(e, it.n))
                set_live_transform(e, it.n, it.target_pos, it.target_rot);
}

static void drag_update_impl(dai_editor *e, float mx, float my) {
    if (!e || !e->dragging) return;

    if (e->drag_screen_rotate) {
        float cx, cy;
        if (!dai_editor_project(e, e->drag_center, &cx, &cy)) return;
        float angle = (std::atan2(my - cy, mx - cx) - e->drag_screen_start) * e->drag_screen_sign;
        if (e->snap_rotate > 0.0f) angle = snap_to(angle, e->snap_rotate * PI / 180.0f);
        dai_vec3 axis = e->drag_axis <= DAI_AXIS_Z ? axis_vector(e->drag_axis)
                                                   : e->drag_plane_normal;
        dai_quat q = qaxis(axis, angle);
        for (DragItem &it : e->drag_items) {
            if (ancestor_selected(e, it.n)) continue;
            dai_vec3 rel = qrot(q, sub(it.wpos, e->drag_center));
            dai_quat wr = qmul(q, it.wrot);
            drag_write(e, it, add(e->drag_center, rel), &wr);
        }
        return;
    }

    dai_vec3 o, d, hit;
    dai_editor_ray(e, mx, my, &o, &d);
    if (!ray_plane(o, d, e->drag_center, e->drag_plane_normal, &hit)) return;

    // Everything is computed from the captured start state, never accumulated
    // from the previous frame: accumulation drifts, and a slow drag would end
    // up somewhere a fast one did not.
    if (e->mode == DAI_GIZMO_ROTATE) {
        dai_vec3 axis = e->drag_axis <= DAI_AXIS_Z ? axis_vector(e->drag_axis)
                                                   : e->drag_plane_normal;
        dai_vec3 v0 = sub(e->drag_start_point, e->drag_center);
        dai_vec3 v1 = sub(hit, e->drag_center);
        if (length(v0) < 1e-5f || length(v1) < 1e-5f) return;
        float angle = std::atan2(dot(cross(v0, v1), axis), dot(v0, v1));
        if (e->snap_rotate > 0.0f)
            angle = snap_to(angle, e->snap_rotate * PI / 180.0f);
        dai_quat q = qaxis(axis, angle);
        for (DragItem &it : e->drag_items) {
            if (ancestor_selected(e, it.n)) continue;
            dai_vec3 rel = qrot(q, sub(it.wpos, e->drag_center));
            dai_quat wr = qmul(q, it.wrot);
            drag_write(e, it, add(e->drag_center, rel), &wr);
        }
        return;
    }

    if (e->mode == DAI_GIZMO_SCALE) {
        float factor;
        if (e->drag_axis <= DAI_AXIS_Z) {
            dai_vec3 a = axis_vector(e->drag_axis);
            float d0 = dot(sub(e->drag_start_point, e->drag_center), a);
            float d1 = dot(sub(hit, e->drag_center), a);
            // Difference over gizmo length, not d1/d0: grabbing the handle near
            // the centre would divide by almost zero and fling the object.
            factor = 1.0f + (d1 - d0) / e->drag_gizmo_len;
        } else {
            float l0 = length(sub(e->drag_start_point, e->drag_center));
            float l1 = length(sub(hit, e->drag_center));
            factor = 1.0f + (l1 - l0) / e->drag_gizmo_len;
        }
        if (e->snap_scale > 0.0f) factor = snap_to(factor, e->snap_scale);
        if (factor < 0.01f) factor = 0.01f;

        dai_vec3 mask{ 1, 1, 1 };
        if (e->drag_axis <= DAI_AXIS_Z) {
            dai_vec3 a = axis_vector(e->drag_axis);
            mask = { 1.0f + (factor - 1.0f) * a.x,
                     1.0f + (factor - 1.0f) * a.y,
                     1.0f + (factor - 1.0f) * a.z };
        } else {
            mask = { factor, factor, factor };
        }
        for (DragItem &it : e->drag_items) {
            if (ancestor_selected(e, it.n)) continue;
            dai_node_desc rec{};
            if (dai_doc_get(e->doc, it.n, &rec) != DAI_OK) continue;
            rec.scale = { it.scale.x * mask.x, it.scale.y * mask.y, it.scale.z * mask.z };
            // Size is the one thing the simulation does not own, so it goes to
            // the document even during play - a collision shape is immutable
            // and the sync layer has to rebuild the body to resize it. That
            // rebuild spawns the body at the DOCUMENT pose, which during play
            // is the pre-play one, so the target below puts it back where the
            // object actually is. Stop still discards all of it.
            dai_doc_set(e->doc, it.n, &rec);
            // Multiple objects scale away from the shared centre; a single one
            // stays put because it *is* the centre.
            dai_vec3 rel = sub(it.wpos, e->drag_center);
            drag_write(e, it,
                add(e->drag_center, dai_vec3{ rel.x * mask.x, rel.y * mask.y, rel.z * mask.z }),
                nullptr);
        }
        return;
    }

    dai_vec3 delta = sub(hit, e->drag_start_point);
    if (e->drag_axis <= DAI_AXIS_Z) {
        dai_vec3 a = axis_vector(e->drag_axis);
        delta = mul(a, dot(delta, a));               // constrain to the axis
    }
    if (e->snap_translate > 0.0f) {
        delta.x = snap_to(delta.x, e->snap_translate);
        delta.y = snap_to(delta.y, e->snap_translate);
        delta.z = snap_to(delta.z, e->snap_translate);
    }
    for (DragItem &it : e->drag_items) {
        if (ancestor_selected(e, it.n)) continue;
        drag_write(e, it, add(it.wpos, delta), nullptr);
    }
}

void dai_editor_drag_end(dai_editor *e) {
    if (!e || !e->dragging) return;
    e->dragging = false;
    e->drag_axis = DAI_AXIS_NONE;
    e->drag_screen_rotate = false;
    e->drag_items.clear();
    dai_doc_commit(e->doc);        // drops the step entirely if nothing moved
    resync(e);
}

void dai_editor_drag_cancel(dai_editor *e) {
    if (!e || !e->dragging) return;
    e->dragging = false;
    e->drag_axis = DAI_AXIS_NONE;
    e->drag_screen_rotate = false;
    e->drag_items.clear();
    dai_doc_abort(e->doc);         // restores the state captured at drag_begin
    resync(e);
}

int dai_editor_dragging(const dai_editor *e) { return (e && e->dragging) ? 1 : 0; }

void dai_editor_move_selection(dai_editor *e, dai_vec3 delta) {
    if (!e || e->selection.empty()) return;
    // The same rule the gizmo follows: while playing the object is where the
    // simulation put it, so a nudge is relative to THAT and lands on the body.
    // Reading the document here moved things relative to the pre-play pose,
    // which looks like the object jumping somewhere else entirely.
    if (e->state != DAI_EDITOR_EDIT) {
        for (dai_node n : e->selection) {
            if (ancestor_selected(e, n)) continue;
            dai_vec3 p{};
            dai_quat r{ 0, 0, 0, 1 };
            if (!dai_editor_live_transform(e, n, &p, &r, nullptr)) continue;
            set_live_transform(e, n, add(p, delta), r);
        }
        return;
    }
    dai_doc_begin(e->doc, "Move");
    for (dai_node n : e->selection) {
        if (ancestor_selected(e, n)) continue;
        dai_vec3 p{};
        if (dai_doc_world_transform(e->doc, n, &p, nullptr, nullptr) != DAI_OK) continue;
        dai_doc_set_world_position(e->doc, n, add(p, delta));
    }
    dai_doc_commit(e->doc);
    resync(e);
}

dai_result dai_editor_delete_selection(dai_editor *e) {
    if (!e || e->selection.empty()) return DAI_ERR_NOT_FOUND;
    dai_doc_begin(e->doc, "Delete");
    for (dai_node n : e->selection) dai_doc_remove(e->doc, n);   // children go too
    dai_doc_commit(e->doc);
    e->selection.clear();
    resync(e);
    return DAI_OK;
}

uint32_t dai_editor_duplicate_selection(dai_editor *e) {
    if (!e || e->selection.empty()) return 0;

    // Collect the selection plus every descendant, parents first, so a copied
    // child can be reparented onto its copied parent instead of the original.
    std::vector<dai_node> roots;
    for (dai_node n : e->selection) if (!ancestor_selected(e, n)) roots.push_back(n);
    std::vector<dai_node> all = roots;
    for (size_t i = 0; i < all.size(); ++i) {
        uint32_t cn = dai_doc_children(e->doc, all[i], nullptr, 0);
        std::vector<dai_node> kids(cn);
        if (cn) dai_doc_children(e->doc, all[i], kids.data(), cn);
        for (dai_node k : kids) all.push_back(k);
    }

    dai_doc_begin(e->doc, "Duplicate");
    std::unordered_map<dai_node, dai_node> map;
    std::vector<dai_node> copies;
    for (dai_node n : all) {
        dai_node_desc rec{};
        if (dai_doc_get(e->doc, n, &rec) != DAI_OK) continue;
        // A copy gets a new id, and the palette colour comes FROM the id -
        // so duplicating a node that never had a colour chosen repainted it.
        // Bake the colour it is actually drawn in: a copy must look like the
        // original, that is the entire point of copying it.
        if (rec.color.x == 0.0f && rec.color.y == 0.0f && rec.color.z == 0.0f)
            dai_editor_node_color(e, n, &rec.color);
        auto it = map.find(rec.parent);
        if (it != map.end()) rec.parent = it->second;
        dai_node c = dai_doc_add(e->doc, &rec);
        if (!c) continue;
        map[n] = c;
        copies.push_back(c);
    }
    dai_doc_commit(e->doc);

    e->selection.clear();
    for (dai_node n : roots) {
        auto it = map.find(n);
        if (it != map.end()) e->selection.push_back(it->second);
    }
    resync(e);
    return (uint32_t)copies.size();
}

} // extern "C"

// ------------------------------------------------------- viewport camera

namespace {

const float PITCH_LIMIT = 1.5533f;      // 89 degrees; at 90 the basis degenerates

void cam_apply(dai_editor *e) {
    // Rebuild the look-at target from the angles. up stays world up, so the
    // horizon never rolls - which is what Unity does and what people expect.
    float cp = std::cos(e->cam_pitch), sp = std::sin(e->cam_pitch);
    dai_vec3 fwd{ std::sin(e->cam_yaw) * cp, sp, -std::cos(e->cam_yaw) * cp };
    e->target = add(e->eye, mul(fwd, e->cam_pivot_dist));
}

void cam_ensure_angles(dai_editor *e) {
    if (e->cam_angles_valid) return;
    dai_vec3 d = sub(e->target, e->eye);
    float len = length(d);
    if (len > 1e-5f) {
        e->cam_pivot_dist = len;
        d = mul(d, 1.0f / len);
        e->cam_yaw = std::atan2(d.x, -d.z);
        e->cam_pitch = std::asin(d.y < -1.0f ? -1.0f : (d.y > 1.0f ? 1.0f : d.y));
    }
    e->cam_angles_valid = true;
}

void cam_basis(const dai_editor *e, dai_vec3 *fwd, dai_vec3 *right, dai_vec3 *upv) {
    float cp = std::cos(e->cam_pitch), sp = std::sin(e->cam_pitch);
    *fwd = dai_vec3{ std::sin(e->cam_yaw) * cp, sp, -std::cos(e->cam_yaw) * cp };
    *right = norm(cross(*fwd, dai_vec3{ 0, 1, 0 }));
    *upv = cross(*right, *fwd);
}

// Rough world size of the selection, so F frames it instead of ending up
// inside it or a hundred metres away.
float selection_radius(const dai_editor *e, dai_vec3 centre) {
    float r = 0.0f;
    for (dai_node n : e->selection) {
        dai_node_desc rec{};
        if (dai_doc_get(e->doc, n, &rec) != DAI_OK) continue;
        dai_vec3 p{}, sc{ 1, 1, 1 };
        dai_doc_world_transform(e->doc, n, &p, nullptr, &sc);
        float ext = std::fabs(rec.half_extent.x * sc.x);
        ext = std::fmax(ext, std::fabs(rec.half_extent.y * sc.y));
        ext = std::fmax(ext, std::fabs(rec.half_extent.z * sc.z));
        float d = length(sub(p, centre)) + ext;
        if (d > r) r = d;
    }
    return r;
}

} // namespace

extern "C" {

void dai_editor_cam_speed(dai_editor *e, float s) {
    if (e && s > 0.0f) e->cam_speed = s;
}
float dai_editor_cam_speed_get(const dai_editor *e) { return e ? e->cam_speed : 0.0f; }
int dai_editor_cam_active(const dai_editor *e) { return (e && e->cam_mode != 0) ? 1 : 0; }

dai_vec3 dai_editor_cam_pivot(const dai_editor *e) {
    if (!e) return dai_vec3{ 0, 0, 0 };
    dai_vec3 fwd, right, upv;
    cam_basis(e, &fwd, &right, &upv);
    return add(e->eye, mul(fwd, e->cam_pivot_dist));
}

void dai_editor_cam_focus(dai_editor *e) {
    if (!e) return;
    cam_ensure_angles(e);

    dai_vec3 centre{ 0, 0, 0 };
    float radius = 2.0f;
    if (dai_editor_selection_count(e) > 0) {
        centre = dai_editor_selection_center(e);
        radius = selection_radius(e, centre);
    } else {
        // Nothing selected: frame the whole document, the way F does with an
        // empty selection.
        uint32_t n = dai_doc_count(e->doc);
        std::vector<dai_node> all(n);
        if (n) dai_doc_nodes(e->doc, all.data(), n);
        dai_vec3 lo{ 1e9f, 1e9f, 1e9f }, hi{ -1e9f, -1e9f, -1e9f };
        uint32_t counted = 0;
        for (dai_node id : all) {
            dai_node_desc rec{};
            if (dai_doc_get(e->doc, id, &rec) != DAI_OK || rec.no_body) continue;
            dai_vec3 p{};
            dai_doc_world_transform(e->doc, id, &p, nullptr, nullptr);
            lo = { std::fmin(lo.x, p.x), std::fmin(lo.y, p.y), std::fmin(lo.z, p.z) };
            hi = { std::fmax(hi.x, p.x), std::fmax(hi.y, p.y), std::fmax(hi.z, p.z) };
            ++counted;
        }
        if (counted) {
            centre = mul(add(lo, hi), 0.5f);
            radius = length(sub(hi, lo)) * 0.5f;
        }
    }
    if (radius < 0.5f) radius = 0.5f;

    // Distance that fits the sphere in the vertical fov, with a little air.
    float half = e->fov * PI / 360.0f;
    float dist = radius / std::fmax(0.05f, std::sin(half)) * 1.15f;
    dai_vec3 fwd, right, upv;
    cam_basis(e, &fwd, &right, &upv);
    e->cam_pivot_dist = dist;
    e->eye = sub(centre, mul(fwd, dist));
    cam_apply(e);
}

int dai_editor_cam_update(dai_editor *e, const dai_editor_cam_input *in) {
    if (!e || !in) return 0;
    cam_ensure_angles(e);

    float dt = in->dt > 0.0f ? in->dt : 1.0f / 60.0f;
    if (dt > 0.1f) dt = 0.1f;              // a hitch must not teleport the camera

    if (in->key_focus && !e->cam_prev_focus) dai_editor_cam_focus(e);
    e->cam_prev_focus = in->key_focus != 0;

    // Which mode a press starts. Checked in Unity's precedence: alt combos
    // first, then plain right for flythrough, then middle for pan.
    int mode = 0;
    if (in->key_alt && in->mouse_left)        mode = 3;   // orbit
    else if (in->key_alt && in->mouse_right)  mode = 4;   // dolly by dragging
    else if (in->mouse_right)                 mode = 1;   // look around
    else if (in->mouse_middle)                mode = 2;   // pan

    // Any change of mode re-anchors the drag origin. Only checking "pressed
    // while idle" leaves the old mode running when the user switches buttons
    // without releasing - alt+left after a middle drag would keep panning, and
    // the first frame would jump by the whole distance between the two presses.
    if (mode != e->cam_mode) {
        e->cam_mode = mode;
        e->cam_last_x = in->mouse_x;
        e->cam_last_y = in->mouse_y;
    }

    float dx = in->mouse_x - e->cam_last_x;
    float dy = in->mouse_y - e->cam_last_y;
    e->cam_last_x = in->mouse_x;
    e->cam_last_y = in->mouse_y;

    dai_vec3 fwd, right, upv;
    cam_basis(e, &fwd, &right, &upv);
    const float LOOK = 0.005f;             // radians per pixel

    if (e->cam_mode == 1) {                // look around, position unchanged
        e->cam_yaw += dx * LOOK;
        e->cam_pitch -= dy * LOOK;
    } else if (e->cam_mode == 3) {         // orbit around the pivot
        dai_vec3 pivot = add(e->eye, mul(fwd, e->cam_pivot_dist));
        e->cam_yaw += dx * LOOK;
        e->cam_pitch -= dy * LOOK;
        if (e->cam_pitch > PITCH_LIMIT) e->cam_pitch = PITCH_LIMIT;
        if (e->cam_pitch < -PITCH_LIMIT) e->cam_pitch = -PITCH_LIMIT;
        cam_basis(e, &fwd, &right, &upv);
        e->eye = sub(pivot, mul(fwd, e->cam_pivot_dist));
    } else if (e->cam_mode == 2) {         // pan in the screen plane
        // Scale by distance so the point under the cursor keeps up regardless
        // of how far away the scene is.
        float k = e->cam_pivot_dist * std::tan(e->fov * PI / 360.0f) * 2.0f / (e->vh > 0 ? e->vh : 1.0f);
        e->eye = add(e->eye, mul(right, -dx * k));
        e->eye = add(e->eye, mul(upv, dy * k));
    } else if (e->cam_mode == 4) {         // alt+right drag = dolly
        float k = (dx + -dy) * 0.01f * std::fmax(1.0f, e->cam_pivot_dist);
        e->eye = add(e->eye, mul(fwd, k));
    }

    if (e->cam_pitch > PITCH_LIMIT) e->cam_pitch = PITCH_LIMIT;
    if (e->cam_pitch < -PITCH_LIMIT) e->cam_pitch = -PITCH_LIMIT;
    cam_basis(e, &fwd, &right, &upv);

    if (e->cam_mode == 1) {
        // Flythrough movement. Q down, E up - Unity's way round.
        float speed = e->cam_speed * (in->key_shift ? 3.0f : 1.0f);
        dai_vec3 move{ 0, 0, 0 };
        if (in->key_w) move = add(move, fwd);
        if (in->key_s) move = sub(move, fwd);
        if (in->key_d) move = add(move, right);
        if (in->key_a) move = sub(move, right);
        if (in->key_e) move = add(move, dai_vec3{ 0, 1, 0 });
        if (in->key_q) move = sub(move, dai_vec3{ 0, 1, 0 });
        if (dot(move, move) > 1e-8f) e->eye = add(e->eye, mul(norm(move), speed * dt));

        // While flying, the wheel sets the speed instead of moving - this is
        // the binding people miss most when a clone gets it wrong.
        if (in->wheel != 0.0f) {
            e->cam_speed *= std::pow(1.2f, in->wheel);
            if (e->cam_speed < 0.05f) e->cam_speed = 0.05f;
            if (e->cam_speed > 500.0f) e->cam_speed = 500.0f;
        }
    } else if (in->wheel != 0.0f) {
        // Dolly. Proportional to distance, so approaching something never
        // overshoots past it in one notch.
        float step = in->wheel * 0.12f * std::fmax(0.5f, e->cam_pivot_dist);
        e->eye = add(e->eye, mul(fwd, step));
        e->cam_pivot_dist -= step;
        if (e->cam_pivot_dist < 0.5f) e->cam_pivot_dist = 0.5f;
    }

    cam_apply(e);
    return e->cam_mode != 0 ? 1 : 0;
}

} // extern "C"

extern "C" {

// -------------------------------------------------------------- play mode

void dai_editor_play(dai_editor *e) {
    if (!e || e->state == DAI_EDITOR_PLAY) return;
    dai_world *w = editor_world(e);
    if (!w) return;
    if (e->state == DAI_EDITOR_EDIT) {
        if (e->dragging) dai_editor_drag_cancel(e);
        if (e->play_state) dai_doc_state_free(e->play_state);
        e->play_state = dai_doc_state_capture(e->doc);
        resync(e);                       // start from what the document says
        // +1 on purpose. A snapshot holds the state at the START of its tick,
        // before that tick's commands run - and the bodies the resync just
        // created belong to the current tick. Seeking to it would therefore
        // land before they existed and wipe the scene.
        e->play_start_tick = dai_current_tick(w) + 1;
    }
    e->state = DAI_EDITOR_PLAY;
}

void dai_editor_pause(dai_editor *e) {
    if (e && e->state == DAI_EDITOR_PLAY) e->state = DAI_EDITOR_PAUSED;
}

void dai_editor_stop(dai_editor *e) {
    if (!e || e->state == DAI_EDITOR_EDIT) return;
    e->state = DAI_EDITOR_EDIT;
    if (e->dragging) dai_editor_drag_cancel(e);
    // Undo everything that was done WHILE playing. Unity does exactly this,
    // and for the same reason: you move things around during play to see what
    // happens, not to edit the scene. dai_editor_apply_sim is the explicit
    // "keep it" button, and it is the only way anything survives Stop.
    if (e->play_state) {
        dai_doc_state_restore(e->doc, e->play_state);
        dai_doc_state_free(e->play_state);
        e->play_state = nullptr;
    }
    // The document is untouched, so putting the world back is just a matter of
    // telling the sync layer that everything it believes is stale.
    if (e->sync) {
        dai_doc_sync_reset(e->sync);
        dai_doc_sync_apply(e->sync);
    }
}

int dai_editor_state_get(const dai_editor *e) { return e ? e->state : DAI_EDITOR_EDIT; }

int dai_editor_live_transform(const dai_editor *e, dai_node n,
                              dai_vec3 *pos, dai_quat *rot, dai_vec3 *scale) {
    if (!e) return 0;
    dai_node_desc rec{};
    if (dai_doc_get(e->doc, n, &rec) != DAI_OK) return 0;

    // Scale is never simulated - Jolt shapes are immutable, so a resize is a
    // rebuild, not a motion - and it therefore always comes from the document,
    // in both states.
    dai_vec3 wp{}, ws{ 1, 1, 1 };
    dai_quat wr{ 0, 0, 0, 1 };
    dai_doc_world_transform(e->doc, n, &wp, &wr, &ws);
    if (scale) *scale = ws;

    // Editing: the document is the truth and the scene follows it, so asking
    // costs a doc lookup. Playing or paused: the document still holds the
    // pre-play pose - that is the whole reason Stop can be exact - and the
    // truth is the body, so anything asking "where is the object, and which
    // way is it facing" must ask the body, or it draws a frozen ghost of where
    // play started.
    if (e->state == DAI_EDITOR_EDIT) {
        if (pos) *pos = wp;
        if (rot) *rot = wr;
        return 1;
    }
    if (!e->sync) return 0;
    dai_entity ent = dai_doc_sync_entity(e->sync, n);
    dai_scene *sc = dai_doc_sync_scene(e->sync);
    if (!ent || !sc) return 0;
    dai_body b = dai_scene_body(sc, ent);
    if (!b) return 0;       // render-only node: nothing simulates it, the doc pose stands
    dai_transform t{};
    if (dai_body_get(editor_world(e), b, &t) != DAI_OK) return 0;

    if (rot) *rot = t.rotation;
    if (!pos) return 1;
    *pos = t.position;
    // A collider centre offset puts the body somewhere the object is not. The
    // caller asks where the OBJECT is. The offset is undone with the BODY's
    // rotation, not the document's - undoing it with a stale orientation was
    // the second half of the same bug, and it moved the wireframe sideways as
    // well as leaving it unturned.
    if (rec.collider_center.x || rec.collider_center.y || rec.collider_center.z) {
        dai_vec3 off = qrot(t.rotation, dai_vec3{ rec.collider_center.x * ws.x,
                                                  rec.collider_center.y * ws.y,
                                                  rec.collider_center.z * ws.z });
        pos->x -= off.x; pos->y -= off.y; pos->z -= off.z;
    }
    return 1;
}

// Kept as the narrow question it always was, now answered by the full one:
// two implementations of "where is this really" is exactly how the wireframe
// and the mesh drifted apart in the first place.
int dai_editor_live_position(const dai_editor *e, dai_node n, dai_vec3 *out) {
    if (!e || !out) return 0;
    return dai_editor_live_transform(e, n, out, nullptr, nullptr);
}

void dai_editor_node_label(const dai_editor *e, dai_node n, char *buf, size_t len) {
    if (!buf || !len) return;
    buf[0] = 0;
    if (!e) { std::snprintf(buf, len, "?"); return; }
    dai_node_desc d{};
    if (dai_doc_get(e->doc, n, &d) != DAI_OK) { std::snprintf(buf, len, "?"); return; }
    if (d.tag[0] && d.name[0]) std::snprintf(buf, len, "%s %s", d.tag, d.name);
    else if (d.tag[0])           std::snprintf(buf, len, "%s", d.tag);
    else if (d.name[0])          std::snprintf(buf, len, "%s", d.name);
    else                         std::snprintf(buf, len, "Node %u", n);
}

int dai_editor_node_color(dai_editor *e, dai_node n, dai_vec3 *out) {
    if (!e || !out || !e->sync) return 0;
    dai_entity ent = dai_doc_sync_entity(e->sync, n);
    dai_scene *sc = dai_doc_sync_scene(e->sync);
    if (!ent || !sc) return 0;
    return dai_scene_color(sc, ent, out) == DAI_OK ? 1 : 0;
}

uint32_t dai_editor_resync(dai_editor *e) {
    if (!e || !e->sync) return 0;
    return dai_doc_sync_apply(e->sync);
}

uint32_t dai_editor_advance(dai_editor *e, double real_seconds, float *out_alpha) {
    if (out_alpha) *out_alpha = 1.0f;
    if (!e) return 0;
    // Not playing? Then the document is the truth and the scene follows it.
    // Without this, a frontend that edits the document directly - typing a
    // number into the inspector rather than dragging the gizmo - moved the
    // gizmo (which reads the document) while the object stayed exactly where
    // it was, because only the drag path called resync.
    if (e->state != DAI_EDITOR_PLAY) { resync(e); return 0; }
    dai_world *w = editor_world(e);
    if (!w) return 0;
    return dai_advance(w, real_seconds, out_alpha);
}

uint32_t dai_editor_apply_sim(dai_editor *e) {
    if (!e || !e->sync) return 0;
    uint32_t n = dai_doc_sync_pull(e->sync, "Apply simulation");
    // "Keep" means keep: the play snapshot would otherwise put everything back
    // the moment Stop is pressed, undoing the very thing this button did. It
    // stays undoable through the undo stack, which is where it belongs.
    if (e->play_state) {
        dai_doc_state_free(e->play_state);
        e->play_state = dai_doc_state_capture(e->doc);
    }
    return n;
}

// --------------------------------------------------------------- timeline

dai_tick dai_editor_timeline_first(const dai_editor *e) {
    dai_world *w = editor_world(e);
    if (!w) return 0;
    dai_tick oldest = dai_oldest_snapshot(w);
    // Never offer to scrub back past the moment play started: before that the
    // ring holds ticks from a previous run, and landing there would look like
    // the editor undid an edit.
    return oldest > e->play_start_tick ? oldest : e->play_start_tick;
}

dai_tick dai_editor_timeline_last(const dai_editor *e) {
    dai_world *w = editor_world(e);
    return w ? dai_current_tick(w) : 0;
}

dai_tick dai_editor_timeline_tick(const dai_editor *e) {
    return dai_editor_timeline_last(e);
}

int dai_editor_scrub(dai_editor *e, dai_tick tick) {
    if (!e || e->state == DAI_EDITOR_EDIT) return 0;
    dai_world *w = editor_world(e);
    if (!w) return 0;
    dai_tick now = dai_current_tick(w);
    if (tick == now) return 1;
    if (tick < dai_editor_timeline_first(e)) return 0;
    return dai_seek_to(w, tick) == DAI_OK ? 1 : 0;
}

// ------------------------------------------------------------------- undo

int dai_editor_undo(dai_editor *e) {
    if (!e || !dai_doc_undo(e->doc)) return 0;
    // A node that came back has a new entity behind it, so anything the
    // frontend cached is stale - resync before the next frame draws.
    resync(e);
    for (size_t i = e->selection.size(); i-- > 0; )
        if (!dai_doc_valid(e->doc, e->selection[i]))
            e->selection.erase(e->selection.begin() + (long)i);
    return 1;
}

int dai_editor_redo(dai_editor *e) {
    if (!e || !dai_doc_redo(e->doc)) return 0;
    resync(e);
    for (size_t i = e->selection.size(); i-- > 0; )
        if (!dai_doc_valid(e->doc, e->selection[i]))
            e->selection.erase(e->selection.begin() + (long)i);
    return 1;
}

uint32_t dai_editor_undo_depth(const dai_editor *e) { return e ? dai_doc_undo_depth(e->doc) : 0; }
uint32_t dai_editor_redo_depth(const dai_editor *e) { return e ? dai_doc_redo_depth(e->doc) : 0; }
const char *dai_editor_undo_name(const dai_editor *e) { return e ? dai_doc_undo_name(e->doc) : ""; }

} // extern "C"
