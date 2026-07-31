// Editor panels. See include/dai_editor_ui.h.
//
// Reads the document every frame and writes edits straight back to it. There is
// no model of the scene here and no cached widget values - the document already
// is the model, and a second copy would be the thing that goes stale.

#include "dai_editor_ui.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

uint32_t rgba(int r, int g, int b, int a) {
    return (uint32_t)((a << 24) | (b << 16) | (g << 8) | r);
}

const char *const SHAPES[] = { "Box", "Sphere", "Capsule" };
const char *const MOTIONS[] = { "Static", "Kinematic", "Dynamic" };

} // namespace

struct dai_editor_ui {
    dai_editor *ed = nullptr;
    dai_ui     *ui = nullptr;

    std::unordered_set<dai_node> folded;   // default is open, so this stays small
    uint32_t visible_rows = 0;

    // A drag on a numeric field changes the value every frame. Without this the
    // undo stack would fill with one step per frame of the drag.
    bool     field_tx_open = false;
    bool     prev_mouse_down = false;

    char     name_buf[DAI_NODE_NAME_MAX] = { 0 };
    dai_node name_buf_node = DAI_INVALID_NODE;

    // viewport interaction
    bool viewport_dragging = false;
    bool prev_viewport_down = false;
    bool scrubbing = false;
};

namespace {

void begin_field_tx(dai_editor_ui *p, const char *name) {
    if (p->field_tx_open) return;
    dai_doc_begin(dai_editor_doc(p->ed), name);
    p->field_tx_open = true;
}

// Closes the transaction when the mouse comes up, so one drag of a field is one
// undo step no matter how many frames it spanned. Called by the inspector
// itself rather than by the host: a frontend that only draws panels and never
// calls the viewport would otherwise leave a transaction open forever, and an
// open transaction silently blocks undo.
void end_field_tx_if_released(dai_editor_ui *p, int mouse_down) {
    if (p->field_tx_open && !mouse_down) {
        dai_doc_commit(dai_editor_doc(p->ed));
        p->field_tx_open = false;
    }
}

void close_field_tx_on_release(dai_editor_ui *p) {
    int down = 0;
    dai_ui_mouse(p->ui, nullptr, nullptr, &down, nullptr);
    end_field_tx_if_released(p, down);
}

const char *node_label(const dai_node_desc &r, dai_node id, char *buf, size_t n) {
    if (r.name[0]) return r.name;
    std::snprintf(buf, n, "node %u", (unsigned)id);
    return buf;
}

bool has_children(dai_doc *d, dai_node n) {
    return dai_doc_children(d, n, nullptr, 0) > 0;
}

void draw_subtree(dai_editor_ui *p, dai_doc *d, dai_node n, int depth) {
    dai_node_desc r{};
    if (dai_doc_get(d, n, &r) != DAI_OK) return;

    char tmp[80];
    const char *label = node_label(r, n, tmp, sizeof(tmp));
    int kids = has_children(d, n) ? 1 : 0;
    int open = p->folded.find(n) == p->folded.end() ? 1 : 0;
    int was_open = open;

    if (dai_ui_tree_item(p->ui, label, depth, kids, kids ? &open : nullptr,
                         dai_editor_is_selected(p->ed, n))) {
        // Ctrl-less toggle is not discoverable; additive selection is left to
        // the viewport, where the modifier keys live.
        dai_editor_select(p->ed, n, 0);
    }
    ++p->visible_rows;
    if (kids && open != was_open) {
        if (open) p->folded.erase(n);
        else      p->folded.insert(n);
    }
    if (!kids || !open) return;

    uint32_t cn = dai_doc_children(d, n, nullptr, 0);
    std::vector<dai_node> kid_ids(cn);
    if (cn) dai_doc_children(d, n, kid_ids.data(), cn);
    for (dai_node k : kid_ids) draw_subtree(p, d, k, depth + 1);
}

} // namespace

extern "C" {

dai_editor_ui *dai_editor_ui_create(dai_editor *editor, dai_ui *ui) {
    if (!editor || !ui) return nullptr;
    dai_editor_ui *p = new dai_editor_ui();
    p->ed = editor;
    p->ui = ui;
    return p;
}

void dai_editor_ui_destroy(dai_editor_ui *p) { delete p; }

uint32_t dai_editor_ui_visible_rows(const dai_editor_ui *p) { return p ? p->visible_rows : 0; }

// ------------------------------------------------------------- hierarchy

void dai_editor_ui_hierarchy(dai_editor_ui *p, float x, float y, float w, float h) {
    if (!p) return;
    dai_doc *d = dai_editor_doc(p->ed);
    p->visible_rows = 0;

    dai_ui_panel_begin(p->ui, x, y, w, h, "Hierarchy");
    dai_ui_scroll_begin(p->ui, "hierarchy", h - 58.0f);

    uint32_t n = dai_doc_count(d);
    std::vector<dai_node> all(n);
    if (n) dai_doc_nodes(d, all.data(), n);
    for (dai_node id : all) {
        dai_node_desc r{};
        if (dai_doc_get(d, id, &r) != DAI_OK) continue;
        if (r.parent != DAI_INVALID_NODE) continue;      // roots drive the recursion
        draw_subtree(p, d, id, 0);
    }

    dai_ui_scroll_end(p->ui);
    dai_ui_panel_end(p->ui);
}

// -------------------------------------------------------------- inspector

void dai_editor_ui_inspector(dai_editor_ui *p, float x, float y, float w, float h) {
    if (!p) return;
    close_field_tx_on_release(p);
    dai_doc *d = dai_editor_doc(p->ed);
    dai_ui_panel_begin(p->ui, x, y, w, h, "Inspector");

    uint32_t sel = dai_editor_selection_count(p->ed);
    if (sel == 0) {
        dai_ui_label(p->ui, "nothing selected");
        dai_ui_panel_end(p->ui);
        return;
    }
    if (sel > 1) {
        dai_ui_label_fmt(p->ui, "%u nodes selected", sel);
        dai_ui_label(p->ui, "move them with the gizmo");
        dai_ui_panel_end(p->ui);
        return;
    }

    dai_node n = dai_editor_selected(p->ed, 0);
    dai_node_desc r{};
    if (dai_doc_get(d, n, &r) != DAI_OK) { dai_ui_panel_end(p->ui); return; }
    dai_node_desc before = r;

    if (p->name_buf_node != n) {
        std::snprintf(p->name_buf, sizeof(p->name_buf), "%s", r.name);
        p->name_buf_node = n;
    }
    if (dai_ui_input_text(p->ui, "Name", p->name_buf, sizeof(p->name_buf))) {
        std::snprintf(r.name, sizeof(r.name), "%s", p->name_buf);
    }

    dai_ui_separator(p->ui);
    dai_ui_drag_vec3(p->ui, "Position", &r.position.x, 0.02f);

    // Euler angles are a display convenience only: the document stores a
    // quaternion, and converting back and forth every frame would drift. The
    // fields are relative, so they only ever apply a delta.
    float euler[3] = { 0, 0, 0 };
    if (dai_ui_drag_vec3(p->ui, "Rotate", euler, 0.5f)) {
        float rx = euler[0] * 3.14159265f / 180.0f;
        float ry = euler[1] * 3.14159265f / 180.0f;
        float rz = euler[2] * 3.14159265f / 180.0f;
        auto axis_q = [](float ax, float ay, float az, float a) {
            float s = std::sin(a * 0.5f);
            return dai_quat{ ax * s, ay * s, az * s, std::cos(a * 0.5f) };
        };
        auto qm = [](dai_quat a, dai_quat b) {
            return dai_quat{ a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
                             a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
                             a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
                             a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z };
        };
        dai_quat delta = qm(qm(axis_q(1,0,0,rx), axis_q(0,1,0,ry)), axis_q(0,0,1,rz));
        r.rotation = qm(delta, r.rotation);
    }
    dai_ui_drag_vec3(p->ui, "Scale", &r.scale.x, 0.01f);

    dai_ui_separator(p->ui);
    dai_ui_option(p->ui, "Shape", &r.shape, SHAPES, 3);
    dai_ui_option(p->ui, "Motion", &r.motion, MOTIONS, 3);
    dai_ui_drag_vec3(p->ui, "Extent", &r.half_extent.x, 0.01f);
    dai_ui_drag_float(p->ui, "Friction", &r.friction, 0.005f);
    dai_ui_drag_float(p->ui, "Bounce", &r.restitution, 0.005f);
    int no_body = r.no_body;
    if (dai_ui_checkbox(p->ui, "no rigid body (group)", &no_body)) r.no_body = no_body;

    dai_ui_separator(p->ui);
    dai_ui_drag_vec3(p->ui, "Colour", &r.color.x, 0.004f);
    dai_ui_drag_float(p->ui, "Rough", &r.roughness, 0.005f);
    dai_ui_drag_float(p->ui, "Emissive", &r.emissive, 0.01f);
    int hidden = r.hidden;
    if (dai_ui_checkbox(p->ui, "hidden", &hidden)) r.hidden = hidden;

    // Clamp here rather than in the widgets: these are physical quantities and
    // a negative roughness or a zero scale would reach the renderer as garbage.
    if (r.roughness < 0.02f) r.roughness = 0.02f;
    if (r.roughness > 1.0f) r.roughness = 1.0f;
    if (r.friction < 0.0f) r.friction = 0.0f;
    if (r.restitution < 0.0f) r.restitution = 0.0f;
    if (r.emissive < 0.0f) r.emissive = 0.0f;
    for (float *v : { &r.scale.x, &r.scale.y, &r.scale.z })
        if (std::fabs(*v) < 0.001f) *v = 0.001f;
    for (float *v : { &r.half_extent.x, &r.half_extent.y, &r.half_extent.z })
        if (*v < 0.001f) *v = 0.001f;
    for (float *v : { &r.color.x, &r.color.y, &r.color.z })
        *v = *v < 0.0f ? 0.0f : (*v > 1.0f ? 1.0f : *v);

    if (std::memcmp(&before, &r, sizeof(dai_node_desc)) != 0) {
        begin_field_tx(p, "Edit");
        dai_doc_set(d, n, &r);
    }
    dai_ui_panel_end(p->ui);
}

// ---------------------------------------------------------------- toolbar

void dai_editor_ui_toolbar(dai_editor_ui *p, float x, float y, float w) {
    if (!p) return;
    dai_doc *d = dai_editor_doc(p->ed);
    int state = dai_editor_state_get(p->ed);

    dai_ui_panel_begin(p->ui, x, y, w, 44.0f, nullptr);
    dai_ui_row(p->ui, 28.0f);

    int mode = dai_editor_gizmo_mode_get(p->ed);
    if (dai_ui_button(p->ui, mode == DAI_GIZMO_TRANSLATE ? "[Move]" : "Move"))
        dai_editor_gizmo_mode(p->ed, DAI_GIZMO_TRANSLATE);
    if (dai_ui_button(p->ui, mode == DAI_GIZMO_ROTATE ? "[Rotate]" : "Rotate"))
        dai_editor_gizmo_mode(p->ed, DAI_GIZMO_ROTATE);
    if (dai_ui_button(p->ui, mode == DAI_GIZMO_SCALE ? "[Scale]" : "Scale"))
        dai_editor_gizmo_mode(p->ed, DAI_GIZMO_SCALE);

    if (dai_ui_button(p->ui, "Undo")) dai_editor_undo(p->ed);
    if (dai_ui_button(p->ui, "Redo")) dai_editor_redo(p->ed);
    if (dai_ui_button(p->ui, "Duplicate")) dai_editor_duplicate_selection(p->ed);
    if (dai_ui_button(p->ui, "Delete")) dai_editor_delete_selection(p->ed);

    if (state == DAI_EDITOR_EDIT) {
        if (dai_ui_button(p->ui, "Play")) dai_editor_play(p->ed);
    } else if (state == DAI_EDITOR_PLAY) {
        if (dai_ui_button(p->ui, "Pause")) dai_editor_pause(p->ed);
        if (dai_ui_button(p->ui, "Stop")) dai_editor_stop(p->ed);
    } else {
        if (dai_ui_button(p->ui, "Resume")) dai_editor_play(p->ed);
        if (dai_ui_button(p->ui, "Stop")) dai_editor_stop(p->ed);
        if (dai_ui_button(p->ui, "Keep")) dai_editor_apply_sim(p->ed);
    }

    const char *undo_name = dai_editor_undo_name(p->ed);
    dai_ui_label_fmt(p->ui, "%u nodes | undo: %s",
                     dai_doc_count(d), undo_name && *undo_name ? undo_name : "-");
    dai_ui_panel_end(p->ui);
}

// --------------------------------------------------------------- timeline

void dai_editor_ui_timeline(dai_editor_ui *p, float x, float y, float w) {
    if (!p) return;
    if (dai_editor_state_get(p->ed) == DAI_EDITOR_EDIT) return;

    dai_tick first = dai_editor_timeline_first(p->ed);
    dai_tick last = dai_editor_timeline_last(p->ed);
    dai_tick cur = dai_editor_timeline_tick(p->ed);
    if (last <= first) return;

    const float H = 44.0f;
    dai_ui_panel_begin(p->ui, x, y, w, H, nullptr);
    float tx = x + 10.0f, tw = w - 20.0f, ty = y + 24.0f;
    dai_ui_rect(p->ui, tx, ty, tw, 8.0f, rgba(38, 42, 52, 255));

    float span = (float)(last - first);
    float t = span > 0 ? (float)(cur - first) / span : 1.0f;
    dai_ui_rect(p->ui, tx, ty, tw * t, 8.0f, rgba(80, 150, 235, 255));
    dai_ui_rect(p->ui, tx + tw * t - 2.0f, ty - 5.0f, 4.0f, 18.0f, rgba(240, 240, 245, 255));

    dai_ui_text(p->ui, tx, y + 3.0f, "timeline (drag to scrub)", rgba(150, 156, 170, 255));
    char buf[96];
    std::snprintf(buf, sizeof(buf), "tick %llu  (%llu..%llu)",
                  (unsigned long long)cur, (unsigned long long)first, (unsigned long long)last);
    float bw = dai_ui_text_width(p->ui, buf);
    dai_ui_text(p->ui, x + w - bw - 10.0f, y + 3.0f, buf, rgba(150, 156, 170, 255));

    // Scrubbing: dragging anywhere on the track seeks. Pause first, because
    // stepping the simulation forward while the user drags backwards fights
    // the pointer and the playhead jitters.
    float mx = 0, my = 0;
    int down = 0, pressed = 0;
    dai_ui_mouse(p->ui, &mx, &my, &down, &pressed);
    bool over_track = mx >= tx - 6.0f && mx <= tx + tw + 6.0f && my >= y && my <= y + H;
    if (down && (over_track || p->scrubbing)) {
        if (pressed && over_track) p->scrubbing = true;
        if (p->scrubbing) {
            if (dai_editor_state_get(p->ed) == DAI_EDITOR_PLAY) dai_editor_pause(p->ed);
            float u = (mx - tx) / (tw > 0 ? tw : 1.0f);
            u = u < 0 ? 0 : (u > 1 ? 1 : u);
            dai_tick want = first + (dai_tick)(u * span + 0.5f);
            dai_editor_scrub(p->ed, want);
        }
    } else if (!down) {
        p->scrubbing = false;
    }
    dai_ui_panel_end(p->ui);
}

// ------------------------------------------------------------------ gizmo

void dai_editor_ui_gizmo(dai_editor_ui *p) {
    if (!p) return;
    uint32_t n = dai_editor_gizmo_lines(p->ed, nullptr, 0);
    if (!n) return;
    std::vector<dai_gizmo_line> lines(n);
    dai_editor_gizmo_lines(p->ed, lines.data(), n);
    for (const dai_gizmo_line &l : lines) {
        float ax, ay, bx, by;
        if (!dai_editor_project(p->ed, l.a, &ax, &ay)) continue;
        if (!dai_editor_project(p->ed, l.b, &bx, &by)) continue;
        auto ch = [](float v) { return (uint32_t)(v < 0 ? 0 : (v > 1 ? 255 : v * 255.0f + 0.5f)); };
        uint32_t col = ch(l.color.x) | (ch(l.color.y) << 8) | (ch(l.color.z) << 16) | 0xFF000000u;
        dai_ui_line(p->ui, ax, ay, bx, by, l.highlighted ? 4.0f : 2.5f, col);
    }
}

// ------------------------------------------------------- viewport input

int dai_editor_ui_viewport_input(dai_editor_ui *p, float mx, float my, int mouse_down) {
    if (!p) return 0;
    end_field_tx_if_released(p, mouse_down);

    bool over_ui = dai_ui_wants_mouse(p->ui) != 0;
    bool pressed = mouse_down && !p->prev_viewport_down;
    bool released = !mouse_down && p->prev_viewport_down;
    p->prev_viewport_down = mouse_down != 0;

    if (p->viewport_dragging) {
        if (mouse_down) dai_editor_drag_update(p->ed, mx, my);
        if (released) { dai_editor_drag_end(p->ed); p->viewport_dragging = false; }
        return 1;
    }
    // A press that started over a panel must not fall through to the scene, or
    // clicking a button would also deselect whatever was selected.
    if (over_ui) return 0;

    if (pressed) {
        int axis = dai_editor_gizmo_hit(p->ed, mx, my);
        if (axis != DAI_AXIS_NONE) {
            dai_editor_drag_begin(p->ed, axis, mx, my);
            p->viewport_dragging = dai_editor_dragging(p->ed) != 0;
            return 1;
        }
        dai_node hit = dai_editor_pick(p->ed, mx, my);
        dai_editor_select(p->ed, hit, 0);       // empty space clears the selection
        return 1;
    }
    dai_editor_gizmo_hover(p->ed, mx, my);
    return 0;
}

int dai_editor_ui_viewport(dai_editor_ui *p, const dai_editor_cam_input *in) {
    if (!p || !in) return 0;

    // A camera gesture that started in the viewport keeps going even when the
    // pointer wanders over a panel - releasing the button outside should not
    // leave the camera stuck mid-orbit.
    bool over_ui = dai_ui_wants_mouse(p->ui) != 0;
    dai_editor_cam_input ci = *in;
    if (over_ui && !dai_editor_cam_active(p->ed)) {
        ci.mouse_right = 0;
        ci.mouse_middle = 0;
        ci.wheel = 0.0f;
        if (ci.key_alt) ci.mouse_left = 0;
    }
    int cam_used = dai_editor_cam_update(p->ed, &ci);
    if (cam_used) {
        // Cancel a half finished object drag rather than letting the camera and
        // the gizmo fight over the same pointer.
        if (p->viewport_dragging) {
            dai_editor_drag_cancel(p->ed);
            p->viewport_dragging = false;
        }
        p->prev_viewport_down = 0;
        return 1;
    }
    // Alt is the camera's modifier; a left click with it held is never a pick.
    int left = (in->key_alt) ? 0 : in->mouse_left;
    return dai_editor_ui_viewport_input(p, in->mouse_x, in->mouse_y, left);
}

// ------------------------------------------------------------------ frame

void dai_editor_ui_frame(dai_editor_ui *p, float vw, float vh) {
    if (!p) return;
    const float SIDE = 240.0f;
    dai_editor_ui_toolbar(p, 8.0f, 8.0f, vw - 16.0f);
    dai_editor_ui_hierarchy(p, 8.0f, 60.0f, SIDE, vh * 0.5f);
    dai_editor_ui_inspector(p, vw - SIDE - 8.0f, 60.0f, SIDE, vh - 128.0f);
    dai_editor_ui_timeline(p, 8.0f + SIDE + 8.0f, vh - 52.0f, vw - 2.0f * (SIDE + 16.0f));
    dai_editor_ui_gizmo(p);
}

} // extern "C"
