// Editor core. See include/dai_editor.h.
//
// No renderer, no UI, no Vulkan: this turns pixels into rays, rays into
// selections and edits into undoable commands. The native editor and any other
// frontend share it.

#include "dai_editor.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Snapshot {              // one entity's transform, before and after
    dai_entity entity;
    dai_body   body;
    dai_vec3   pos_before, pos_after;
    dai_quat   rot_before, rot_after;
};

struct Command {
    std::string name;
    std::vector<Snapshot> items;
};

dai_vec3 sub(dai_vec3 a, dai_vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
dai_vec3 add(dai_vec3 a, dai_vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
dai_vec3 mul(dai_vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
float dot(dai_vec3 a, dai_vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
dai_vec3 cross(dai_vec3 a, dai_vec3 b) {
    return { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}
dai_vec3 norm(dai_vec3 v) {
    float l = std::sqrt(dot(v, v));
    return l > 1e-8f ? mul(v, 1.0f / l) : dai_vec3{ 0, 0, 0 };
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

} // namespace

struct dai_editor {
    dai_scene *scene = nullptr;
    dai_world *world = nullptr;

    dai_vec3 eye{ 0, 5, 10 }, target{ 0, 0, 0 }, up{ 0, 1, 0 };
    float fov = 55.0f, znear = 0.1f, zfar = 500.0f;
    float vw = 1280.0f, vh = 720.0f;

    std::vector<dai_entity> selection;
    int mode = DAI_GIZMO_TRANSLATE;
    float snap_translate = 0.0f, snap_rotate = 0.0f;

    // drag state
    bool dragging = false;
    int drag_axis = DAI_AXIS_NONE;
    dai_vec3 drag_start_point{};
    dai_vec3 drag_plane_normal{};
    std::vector<Snapshot> drag_items;

    std::vector<Command> undo_stack, redo_stack;
};

namespace {

// where a ray meets a plane; false when they are parallel
bool ray_plane(dai_vec3 o, dai_vec3 d, dai_vec3 p, dai_vec3 n, dai_vec3 *out) {
    float denom = dot(d, n);
    if (std::fabs(denom) < 1e-6f) return false;
    float t = dot(sub(p, o), n) / denom;
    if (t < 0.0f) return false;
    *out = add(o, mul(d, t));
    return true;
}

void capture(dai_editor *e, std::vector<Snapshot> &out) {
    out.clear();
    for (dai_entity ent : e->selection) {
        dai_body b = dai_scene_body(e->scene, ent);
        if (!b) continue;
        dai_transform t{};
        if (dai_body_get(e->world, b, &t) != DAI_OK) continue;
        Snapshot s{};
        s.entity = ent; s.body = b;
        s.pos_before = s.pos_after = t.position;
        s.rot_before = s.rot_after = t.rotation;
        out.push_back(s);
    }
}

void push_command(dai_editor *e, const char *name, const std::vector<Snapshot> &items) {
    bool changed = false;
    for (const Snapshot &s : items) {
        if (std::fabs(s.pos_after.x - s.pos_before.x) > 1e-6f ||
            std::fabs(s.pos_after.y - s.pos_before.y) > 1e-6f ||
            std::fabs(s.pos_after.z - s.pos_before.z) > 1e-6f) { changed = true; break; }
    }
    if (!changed) return;                     // a click that moved nothing is not an undo step
    Command c;
    c.name = name;
    c.items = items;
    e->undo_stack.push_back(std::move(c));
    e->redo_stack.clear();                    // the classic rule: editing forks the future
}

void apply(dai_editor *e, const Snapshot &s, bool after) {
    dai_body_set_transform(e->world, s.body, after ? s.pos_after : s.pos_before,
                           after ? s.rot_after : s.rot_before);
    dai_body_set_velocity(e->world, s.body, dai_vec3{ 0,0,0 }, dai_vec3{ 0,0,0 });
}

} // namespace

extern "C" {

dai_editor *dai_editor_create(dai_scene *scene) {
    if (!scene) return nullptr;
    dai_editor *e = new dai_editor();
    e->scene = scene;
    e->world = dai_scene_world(scene);
    return e;
}

void dai_editor_destroy(dai_editor *e) { delete e; }

void dai_editor_camera(dai_editor *e, dai_vec3 eye, dai_vec3 target, dai_vec3 up,
                       float fov, float znear, float zfar, float vw, float vh) {
    if (!e) return;
    e->eye = eye; e->target = target; e->up = up;
    e->fov = fov; e->znear = znear; e->zfar = zfar;
    if (vw > 0) e->vw = vw;
    if (vh > 0) e->vh = vh;
}

void dai_editor_ray(const dai_editor *e, float mx, float my, dai_vec3 *origin, dai_vec3 *dir) {
    if (!e) return;
    // forward/right/up of the camera, then offset by the pixel's angle. No
    // matrix inverse needed, and it cannot disagree with the renderer as long
    // as both use the same fov convention (vertical).
    dai_vec3 fwd = norm(sub(e->target, e->eye));
    dai_vec3 right = norm(cross(fwd, e->up));
    dai_vec3 upv = cross(right, fwd);
    float t = std::tan(e->fov * 3.14159265f / 360.0f);
    float aspect = e->vw / (e->vh > 0 ? e->vh : 1.0f);
    float ndc_x = (2.0f * mx / e->vw) - 1.0f;
    float ndc_y = 1.0f - (2.0f * my / e->vh);          // pixels grow downwards
    dai_vec3 d = norm(add(add(fwd, mul(right, ndc_x * t * aspect)), mul(upv, ndc_y * t)));
    if (origin) *origin = e->eye;
    if (dir) *dir = d;
}

dai_entity dai_editor_pick(dai_editor *e, float mx, float my) {
    if (!e) return DAI_INVALID_ENTITY;
    dai_vec3 o, d;
    dai_editor_ray(e, mx, my, &o, &d);
    dai_ray_hit hit{};
    if (!dai_raycast(e->world, o, d, e->zfar, &hit)) return DAI_INVALID_ENTITY;
    // the scene knows which entity owns a body; walk it rather than storing a
    // second map that could go stale
    uint32_t count = dai_scene_count(e->scene) + 8;
    for (uint32_t i = 1; i <= count; ++i)
        if (dai_scene_body(e->scene, i) == hit.body) return i;
    return DAI_INVALID_ENTITY;
}

void dai_editor_select(dai_editor *e, dai_entity ent, int additive) {
    if (!e) return;
    if (!additive) e->selection.clear();
    if (ent == DAI_INVALID_ENTITY) return;
    for (size_t i = 0; i < e->selection.size(); ++i)
        if (e->selection[i] == ent) {
            if (additive) e->selection.erase(e->selection.begin() + (long)i);   // toggle
            return;
        }
    e->selection.push_back(ent);
}

void dai_editor_deselect_all(dai_editor *e) { if (e) e->selection.clear(); }
uint32_t dai_editor_selection_count(const dai_editor *e) { return e ? (uint32_t)e->selection.size() : 0; }
dai_entity dai_editor_selected(const dai_editor *e, uint32_t i) {
    return (e && i < e->selection.size()) ? e->selection[i] : DAI_INVALID_ENTITY;
}
int dai_editor_is_selected(const dai_editor *e, dai_entity ent) {
    if (!e) return 0;
    for (dai_entity s : e->selection) if (s == ent) return 1;
    return 0;
}

dai_vec3 dai_editor_selection_center(const dai_editor *e) {
    dai_vec3 c{ 0, 0, 0 };
    if (!e || e->selection.empty()) return c;
    uint32_t n = 0;
    for (dai_entity ent : e->selection) {
        dai_body b = dai_scene_body(e->scene, ent);
        dai_transform t{};
        if (b && dai_body_get(e->world, b, &t) == DAI_OK) { c = add(c, t.position); ++n; }
    }
    return n ? mul(c, 1.0f / (float)n) : c;
}

void dai_editor_gizmo_mode(dai_editor *e, int mode) { if (e) e->mode = mode; }
int  dai_editor_gizmo_mode_get(const dai_editor *e) { return e ? e->mode : 0; }
void dai_editor_snap(dai_editor *e, float t, float r) {
    if (!e) return;
    e->snap_translate = t; e->snap_rotate = r;
}

void dai_editor_drag_begin(dai_editor *e, int axis, float mx, float my) {
    if (!e || e->selection.empty() || axis == DAI_AXIS_NONE) return;
    capture(e, e->drag_items);
    if (e->drag_items.empty()) return;

    dai_vec3 centre = dai_editor_selection_center(e);
    dai_vec3 o, d;
    dai_editor_ray(e, mx, my, &o, &d);

    // Drag plane: contains the axis and faces the camera as much as possible.
    // Picking the plane naively (say, always XZ) makes a Y drag useless when
    // the camera looks down it.
    dai_vec3 a = axis_vector(axis);
    dai_vec3 to_cam = norm(sub(e->eye, centre));
    dai_vec3 n = (axis <= DAI_AXIS_Z) ? norm(cross(a, cross(to_cam, a))) : a;
    if (axis == DAI_AXIS_XZ) n = { 0, 1, 0 };
    if (axis == DAI_AXIS_XY) n = { 0, 0, 1 };
    if (axis == DAI_AXIS_YZ) n = { 1, 0, 0 };
    if (axis == DAI_AXIS_ALL) n = to_cam;
    if (dot(n, n) < 1e-8f) n = to_cam;

    dai_vec3 hit;
    if (!ray_plane(o, d, centre, n, &hit)) return;
    e->dragging = true;
    e->drag_axis = axis;
    e->drag_plane_normal = n;
    e->drag_start_point = hit;
}

void dai_editor_drag_update(dai_editor *e, float mx, float my) {
    if (!e || !e->dragging) return;
    dai_vec3 centre = dai_editor_selection_center(e);
    dai_vec3 o, d, hit;
    dai_editor_ray(e, mx, my, &o, &d);
    if (!ray_plane(o, d, centre, e->drag_plane_normal, &hit)) return;

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
    for (Snapshot &s : e->drag_items) {
        s.pos_after = add(s.pos_before, delta);
        apply(e, s, true);
    }
}

void dai_editor_drag_end(dai_editor *e) {
    if (!e || !e->dragging) return;
    e->dragging = false;
    e->drag_axis = DAI_AXIS_NONE;
    push_command(e, "Move", e->drag_items);
    e->drag_items.clear();
}

int dai_editor_dragging(const dai_editor *e) { return (e && e->dragging) ? 1 : 0; }

void dai_editor_move_selection(dai_editor *e, dai_vec3 delta) {
    if (!e || e->selection.empty()) return;
    std::vector<Snapshot> items;
    capture(e, items);
    for (Snapshot &s : items) {
        s.pos_after = add(s.pos_before, delta);
        apply(e, s, true);
    }
    push_command(e, "Move", items);
}

dai_result dai_editor_delete_selection(dai_editor *e) {
    if (!e || e->selection.empty()) return DAI_ERR_NOT_FOUND;
    // Deleting is deliberately NOT undoable yet: bringing a body back means
    // recreating it with the same handle, which the scene layer cannot promise
    // today. Saying so beats a broken undo.
    for (dai_entity ent : e->selection) dai_scene_remove(e->scene, ent);
    e->selection.clear();
    e->undo_stack.clear();
    e->redo_stack.clear();
    return DAI_OK;
}

int dai_editor_undo(dai_editor *e) {
    if (!e || e->undo_stack.empty()) return 0;
    Command c = e->undo_stack.back();
    e->undo_stack.pop_back();
    for (const Snapshot &s : c.items) apply(e, s, false);
    e->redo_stack.push_back(c);
    return 1;
}

int dai_editor_redo(dai_editor *e) {
    if (!e || e->redo_stack.empty()) return 0;
    Command c = e->redo_stack.back();
    e->redo_stack.pop_back();
    for (const Snapshot &s : c.items) apply(e, s, true);
    e->undo_stack.push_back(c);
    return 1;
}

uint32_t dai_editor_undo_depth(const dai_editor *e) { return e ? (uint32_t)e->undo_stack.size() : 0; }
uint32_t dai_editor_redo_depth(const dai_editor *e) { return e ? (uint32_t)e->redo_stack.size() : 0; }
const char *dai_editor_undo_name(const dai_editor *e) {
    return (e && !e->undo_stack.empty()) ? e->undo_stack.back().name.c_str() : "";
}

} // extern "C"
