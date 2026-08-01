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

// ---- quaternion <-> euler, display side only ------------------------------
// The document keeps a quaternion; the inspector shows degrees. Converting
// back and forth every frame would drift, so the inspector keeps a cache and
// only re-reads it when the node or its rotation changed from somewhere else.
static void quat_to_euler(dai_quat q, float *deg) {
    // ZYX order, the one every DCC tool's rotation fields use.
    float sinr = 2.0f * (q.w * q.x + q.y * q.z);
    float cosr = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    float roll = std::atan2(sinr, cosr);
    float sinp = 2.0f * (q.w * q.y - q.z * q.x);
    float pitch = std::fabs(sinp) >= 1.0f ? std::copysign(1.5707963f, sinp)
                                          : std::asin(sinp);
    float siny = 2.0f * (q.w * q.z + q.x * q.y);
    float cosy = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    float yaw = std::atan2(siny, cosy);
    const float R2D = 57.2957795f;
    deg[0] = roll * R2D; deg[1] = pitch * R2D; deg[2] = yaw * R2D;
}

static dai_quat euler_to_quat(const float *deg) {
    const float D2R = 3.14159265f / 180.0f;
    float rx = deg[0] * D2R, ry = deg[1] * D2R, rz = deg[2] * D2R;
    auto axis_q = [](float ax, float ay, float az, float a) {
        float sn = std::sin(a * 0.5f);
        return dai_quat{ ax * sn, ay * sn, az * sn, std::cos(a * 0.5f) };
    };
    auto qm = [](dai_quat a, dai_quat b) {
        return dai_quat{ a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
                         a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
                         a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
                         a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z };
    };
    return qm(qm(axis_q(0, 0, 1, rz), axis_q(0, 1, 0, ry)), axis_q(1, 0, 0, rx));
}

static bool quat_eq(dai_quat a, dai_quat b) {
    return std::fabs(a.x - b.x) < 1e-5f && std::fabs(a.y - b.y) < 1e-5f &&
           std::fabs(a.z - b.z) < 1e-5f && std::fabs(a.w - b.w) < 1e-5f;
}

// The collider's shape picker: "From Mesh" leaves the body's shape to the
// asset resolver, the way a MeshCollider in Unity is not a capsule with a
// different name.
const char *const COLLIDER_SHAPES[] = { "Box", "Sphere", "Capsule", "From Mesh" };

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

    // Fold state of the inspector's component blocks. Retained because it
    // cannot be derived from the document - the same reason the hierarchy's
    // folds live here.
    int fold_transform = 1, fold_body = 1, fold_collider = 1, fold_render = 1, fold_script = 0;
    char     script_buf[96] = { 0 };
    dai_node script_buf_node = DAI_INVALID_NODE;

    // ---- scene view / game view ------------------------------------------
    int  view = DAI_VIEW_SCENE;
    int  view_was_playing = 0;      // so Play switches to Game exactly once

    // ---- Edit Collider ---------------------------------------------------
    // The mode Unity's little box-with-handles button turns on: the collider
    // gets six face handles in the scene and dragging one moves that FACE,
    // which changes size and centre together - dragging the right face must
    // not also move the left one.
    int   collider_edit = 0;
    int   col_axis = -1;            // 0,1,2 while a handle is held
    int   col_sign = 1;
    float col_last_x = 0, col_last_y = 0;
    bool  col_tx_open = false;

    char     name_buf[DAI_NODE_NAME_MAX] = { 0 };
    dai_node name_buf_node = DAI_INVALID_NODE;
    char     tag_buf[32] = { 0 };
    dai_node tag_buf_node = DAI_INVALID_NODE;
    // Which node is being renamed right now, and what has been typed so far.
    // The hierarchy shows the label until F2 (or Rename from the menu) turns
    // the row into a text field; Enter and clicking away commit.
    dai_node rename_node = DAI_INVALID_NODE;
    char     rename_buf[DAI_NODE_NAME_MAX] = { 0 };
    int      rename_just_opened = 0;

    // Euler display state: the document stores a quaternion, and reading it
    // back as degrees every frame would fight the user while they type. The
    // cached angles are refreshed whenever the node or its rotation changed
    // from anywhere else (gizmo, undo, play).
    dai_node euler_node = DAI_INVALID_NODE;
    dai_quat euler_cached_q{ 0, 0, 0, 1 };
    float    euler_deg[3] = { 0, 0, 0 };

    // Right click menus. The state is ours because a menu has to survive the
    // frames between "opened" and "clicked an entry".
    dai_ui_popup menu_node{};          // hierarchy row or viewport object
    dai_ui_popup menu_canvas{};        // empty hierarchy space
    dai_node     menu_target = DAI_INVALID_NODE;
    char     asset_buf[96] = { 0 };
    dai_node asset_buf_node = DAI_INVALID_NODE;

    // asset browser: the list is the host's, the selection is ours
    std::vector<const char *> assets;
    int asset_sel = -1;

    // ---- projects --------------------------------------------------------
    // The host owns the disk; the editor owns the clicks. A project here is
    // exactly what it is in Unity: the folder the scenes and assets live in,
    // and "New Project" means naming that folder, nothing more mystical.
    dai_editor_ui_project_list_fn   proj_list = nullptr;
    dai_editor_ui_project_action_fn proj_create = nullptr;
    dai_editor_ui_project_action_fn proj_open = nullptr;
    void *proj_user = nullptr;
    std::vector<std::string> projects;
    std::string proj_current;
    char   proj_name_buf[64] = { 0 };
    int    proj_tab = 0;              // 0 = files, 1 = projects

    // ---- scripts -----------------------------------------------------------
    int (*script_create)(const char *name, void *user) = nullptr;
    void *script_user = nullptr;
    char   script_name_buf[64] = { 0 };

    // ---- mesh inventory --------------------------------------------------
    dai_editor_ui_mesh_name_fn mesh_name = nullptr;
    uint32_t mesh_count = 0;
    void *mesh_user = nullptr;

    // The layout. Windows the user can move, so their rectangles have to
    // survive the frame - and be resettable, because a window dragged off the
    // screen on a monitor you no longer have is otherwise gone for good.
    dai_ui_window win_hierarchy{};
    dai_ui_window win_inspector{};
    dai_ui_window win_project{};
    dai_ui_window win_scene{};          // the viewport IS a window
    dai_ui_window win_settings{};
    int   settings_open = 0;
    // Where fill mode last put the scene window, so a user drag can be told
    // apart from the layout: same rect = filling, different = floated.
    float scene_fx = -1, scene_fy = -1, scene_fw = -1, scene_fh = -1;
    int   prev_frame_down = 0;

    // ---- settings ----------------------------------------------------------
    float settings_font_px = 13.0f;
    int   settings_theme = 0;
    float settings_gizmo_px = 90.0f;
    float settings_snap = 0.0f;
    void (*apply_font)(float px, void *user) = nullptr;
    void *apply_user = nullptr;
    const char *pending_asset = nullptr;   // clicked in the Project window
    int   pending_as_tree = 0;
    bool  layout_ready = false;
    float layout_w = 0, layout_h = 0;
    float view_x = 0, view_y = 0, view_w = 0, view_h = 0;

    // viewport interaction
    bool viewport_dragging = false;
    bool prev_viewport_down = false;
    bool prev_right_down = false;
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
    if (r.tag[0] && r.name[0]) {
        // bounded by hand: snprintf's %s %s of two fixed arrays trips the
        // truncation warning even though the total fits
        size_t used = 0;
        for (const char *c = r.tag; *c && used + 1 < n; ++c) buf[used++] = *c;
        if (used + 1 < n) buf[used++] = ' ';
        for (const char *c = r.name; *c && used + 1 < n; ++c) buf[used++] = *c;
        buf[used] = 0;
        return buf;
    }
    if (r.name[0]) return r.name;
    if (r.tag[0])  return r.tag;
    std::snprintf(buf, n, "node %u", (unsigned)id);
    return buf;
}

bool has_children(dai_doc *d, dai_node n) {
    return dai_doc_children(d, n, nullptr, 0) > 0;
}

void draw_subtree(dai_editor_ui *p, dai_doc *d, dai_node n, int depth) {
    dai_node_desc r{};
    if (dai_doc_get(d, n, &r) != DAI_OK) return;

    int kids = has_children(d, n) ? 1 : 0;
    int open = p->folded.find(n) == p->folded.end() ? 1 : 0;
    int was_open = open;

    if (p->rename_node == n) {
        // This row is a text field right now. Enter commits, clicking away
        // commits too - a rename you have to remember to confirm is a rename
        // you will lose.
        int clicked = dai_ui_tree_rename(p->ui, p->rename_buf, sizeof(p->rename_buf),
                                         depth, kids, kids ? &open : nullptr);
        ++p->visible_rows;
        if (clicked == 1) {
            dai_doc_begin(d, "Rename");
            dai_node_desc cur{};
            if (dai_doc_get(d, n, &cur) == DAI_OK) {
                std::snprintf(cur.name, sizeof(cur.name), "%s", p->rename_buf);
                dai_doc_set(d, n, &cur);
            }
            dai_doc_commit(d);
            dai_editor_resync(p->ed);
            p->rename_node = DAI_INVALID_NODE;
        } else if (clicked == -1) {
            p->rename_node = DAI_INVALID_NODE;
        }
        if (kids && open != was_open) {
            if (open) p->folded.erase(n);
            else      p->folded.insert(n);
        }
    } else {
        char tmp[80];
        const char *label = node_label(r, n, tmp, sizeof(tmp));
        int rc = dai_ui_tree_item_ex(p->ui, label, depth, kids, kids ? &open : nullptr,
                                     dai_editor_is_selected(p->ed, n));
        if (rc & 1) {
            // Ctrl-less toggle is not discoverable; additive selection is left
            // to the viewport, where the modifier keys live.
            dai_editor_select(p->ed, n, 0);
        }
        if (rc & 2) {
            // Right click selects what it opens the menu for - a menu that
            // acts on the OLD selection while the pointer sits on another row
            // deletes the wrong crate.
            dai_editor_select(p->ed, n, 0);
            p->menu_target = n;
            float mx = 0, my = 0;
            dai_ui_mouse(p->ui, &mx, &my, nullptr, nullptr);
            dai_ui_popup_open(&p->menu_node, mx, my);
        }
        ++p->visible_rows;
        if (kids && open != was_open) {
            if (open) p->folded.erase(n);
            else      p->folded.insert(n);
        }
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

int dai_editor_ui_menu_open(const dai_editor_ui *p) {
    return p ? (p->menu_node.open || p->menu_canvas.open) : 0;
}

void dai_editor_ui_rename(dai_editor_ui *p, dai_node n) {
    if (!p) return;
    dai_node_desc r{};
    if (dai_doc_get(dai_editor_doc(p->ed), n, &r) != DAI_OK) return;
    std::snprintf(p->rename_buf, sizeof(p->rename_buf), "%s", r.name);
    p->rename_node = n;
}

void dai_editor_ui_expand_all(dai_editor_ui *p) {
    if (!p) return;
    p->fold_transform = p->fold_body = p->fold_collider = p->fold_render = 1;
}

uint32_t dai_editor_ui_visible_rows(const dai_editor_ui *p) { return p ? p->visible_rows : 0; }

void dai_editor_ui_project_host(dai_editor_ui *p,
                                dai_editor_ui_project_list_fn list,
                                dai_editor_ui_project_action_fn create,
                                dai_editor_ui_project_action_fn open,
                                void *user) {
    if (!p) return;
    p->proj_list = list; p->proj_create = create; p->proj_open = open;
    p->proj_user = user;
    dai_editor_ui_projects_refresh(p);
}

void dai_editor_ui_projects_refresh(dai_editor_ui *p) {
    if (!p) return;
    p->projects.clear();
    if (!p->proj_list) return;
    for (uint32_t i = 0; ; ++i) {
        const char *name = p->proj_list(i, p->proj_user);
        if (!name) break;
        p->projects.push_back(name);
    }
}

void dai_editor_ui_settings_host(dai_editor_ui *p,
                                 void (*apply_font)(float px, void *user),
                                 float current_px, void *user) {
    if (!p) return;
    p->apply_font = apply_font;
    p->apply_user = user;
    p->settings_font_px = current_px;
}

void dai_editor_ui_script_host(dai_editor_ui *p,
                               int (*create)(const char *name, void *user), void *user) {
    if (!p) return;
    p->script_create = create;
    p->script_user = user;
}

const char *dai_editor_ui_project(const dai_editor_ui *p) {
    return p ? p->proj_current.c_str() : "";
}

void dai_editor_ui_mesh_host(dai_editor_ui *p,
                             dai_editor_ui_mesh_name_fn name,
                             uint32_t mesh_count, void *user) {
    if (!p) return;
    p->mesh_name = name; p->mesh_count = mesh_count; p->mesh_user = user;
}

// ------------------------------------------------------------- hierarchy

// The contents, without deciding where they live. The panel version and the
// window version both call this - two copies of a tree walk is how the two
// slowly stop agreeing.
static void hierarchy_body(dai_editor_ui *p, float h) {
    dai_doc *d = dai_editor_doc(p->ed);
    p->visible_rows = 0;
    dai_ui_scroll_begin(p->ui, "hierarchy", h);

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
}

void dai_editor_ui_hierarchy(dai_editor_ui *p, float x, float y, float w, float h) {
    if (!p) return;
    dai_ui_panel_begin(p->ui, x, y, w, h, "Hierarchy");
    hierarchy_body(p, h - 58.0f);
    // Right click on the empty rest of the panel: the canvas menu. A row that
    // was right clicked opened its own menu already, so only fire when none
    // is open.
    if (!p->menu_node.open && !p->menu_canvas.open) {
        float mx = 0, my = 0;
        dai_ui_mouse(p->ui, &mx, &my, nullptr, nullptr);
        if (dai_ui_right_pressed(p->ui) && mx >= x && mx < x + w && my >= y && my < y + h)
            dai_ui_popup_open(&p->menu_canvas, mx, my);
    }
    dai_ui_panel_end(p->ui);
}

// -------------------------------------------------------------- inspector

namespace {

// Unity's inspector is a label column and a value column, and the label column
// is a fraction of the panel, not a fixed number of pixels: at 230 px the
// three XYZ boxes have to give the labels less room than they do at 400.
void fit_label_column(dai_ui *ui) {
    dai_ui_style *st = dai_ui_style_of(ui);
    float w = dai_ui_panel_width(ui) * 0.32f;
    if (w < 52.0f) w = 52.0f;
    if (w > 110.0f) w = 110.0f;
    st->label_w = w;      // kept, not restored: the panel it was measured for
                          // is the one every field after this belongs to
}

// The builtin mesh a shape draws as. Freezing this is what stops "I changed
// the collider to a sphere" from also turning the model into a sphere.
uint32_t mesh_of_shape(int shape) {
    switch (shape) {
    case DAI_SHAPE_SPHERE:  return DAI_MESH_SPHERE;
    case DAI_SHAPE_CAPSULE: return DAI_MESH_CAPSULE;
    default:                return DAI_MESH_BOX;
    }
}

const char *collider_title(int shape) {
    switch (shape) {
    case DAI_SHAPE_SPHERE:  return "Sphere Collider";
    case DAI_SHAPE_CAPSULE: return "Capsule Collider";
    default:                return "Box Collider";
    }
}

bool v3_differs(dai_vec3 a, dai_vec3 b) {
    return a.x != b.x || a.y != b.y || a.z != b.z;
}

} // namespace

static void inspector_body(dai_editor_ui *p) {
    close_field_tx_on_release(p);
    dai_doc *d = dai_editor_doc(p->ed);

    uint32_t sel = dai_editor_selection_count(p->ed);
    if (sel == 0) { dai_ui_label(p->ui, "nothing selected"); return; }
    if (sel > 1) {
        dai_ui_label_fmt(p->ui, "%u nodes selected", sel);
        dai_ui_label(p->ui, "move them with the gizmo");
        return;
    }

    dai_node n = dai_editor_selected(p->ed, 0);
    dai_node_desc r{};
    if (dai_doc_get(d, n, &r) != DAI_OK) return;
    dai_node_desc before = r;

    fit_label_column(p->ui);
    int playing = dai_editor_state_get(p->ed) != DAI_EDITOR_EDIT;

    // ---- the object header: icon, active, name, then tag ------------------
    // Same shape as Unity's: what the thing is, whether it is on, what it is
    // called. The name is a text field you can put a caret in, not a label.
    if (p->name_buf_node != n) {
        std::snprintf(p->name_buf, sizeof(p->name_buf), "%s", r.name);
        p->name_buf_node = n;
    }
    if (dai_ui_input_text(p->ui, "Name", p->name_buf, sizeof(p->name_buf)))
        std::snprintf(r.name, sizeof(r.name), "%s", p->name_buf);

    if (p->tag_buf_node != n) {
        std::snprintf(p->tag_buf, sizeof(p->tag_buf), "%s", r.tag);
        p->tag_buf_node = n;
    }
    if (dai_ui_input_text(p->ui, "Tag", p->tag_buf, sizeof(p->tag_buf)))
        std::snprintf(r.tag, sizeof(r.tag), "%s", p->tag_buf);

    if (p->asset_buf_node != n) {
        std::snprintf(p->asset_buf, sizeof(p->asset_buf), "%s", r.asset);
        p->asset_buf_node = n;
    }
    if (dai_ui_input_text(p->ui, "Asset", p->asset_buf, sizeof(p->asset_buf)))
        std::snprintf(r.asset, sizeof(r.asset), "%s", p->asset_buf);

    // ---- Transform ---------------------------------------------------------
    dai_ui_header_icon(p->ui, DAI_ICON_MOVE, "Transform", &p->fold_transform, nullptr);
    if (p->fold_transform) {
        // While playing the document still holds the pose from before play -
        // that is exactly what makes Stop able to restore it - so a panel that
        // asked the document would show a frozen ghost. Ask the BODY where the
        // object is, and write typed values back to the document.
        dai_vec3 pos = r.position;
        if (playing) dai_editor_live_position(p->ed, n, &pos);
        if (dai_ui_num_vec3(p->ui, "Position", &pos.x, 0.02f)) r.position = pos;

        // Rotation is shown in degrees. The cache is refreshed whenever the
        // quaternion moved from anywhere that is not this field - the gizmo,
        // an undo, the simulation - so typing is never fought by a conversion
        // that rounds differently than the last keystroke.
        if (p->euler_node != n || !quat_eq(p->euler_cached_q, r.rotation)) {
            p->euler_node = n;
            p->euler_cached_q = r.rotation;
            quat_to_euler(r.rotation, p->euler_deg);
        }
        if (dai_ui_num_vec3(p->ui, "Rotation", p->euler_deg, 0.5f)) {
            r.rotation = euler_to_quat(p->euler_deg);
            p->euler_cached_q = r.rotation;
        }
        dai_ui_num_vec3(p->ui, "Scale", &r.scale.x, 0.01f);
    }

    // ---- Mesh Renderer -----------------------------------------------------
    int visible = !r.hidden;
    if (dai_ui_header_icon(p->ui, visible ? DAI_ICON_EYE : DAI_ICON_EYE_OFF, "Mesh Renderer",
                           &p->fold_render, &visible) == 2)
        r.hidden = !visible;
    if (p->fold_render) {
        if (p->mesh_name) {
            const char *cur = r.mesh == 0xFFFFFFFFu ? "(from shape)"
                            : r.mesh == 0xFFFFFFFEu ? "(from asset)"
                            : p->mesh_name(r.mesh, p->mesh_user);
            dai_ui_label_fmt(p->ui, "Mesh: %s", cur ? cur : "?");
            dai_ui_row(p->ui, 20.0f);
            if (dai_ui_button(p->ui, "<") && p->mesh_count > 0) {
                r.mesh = r.mesh >= 0xFFFFFFFEu ? p->mesh_count - 1
                       : (r.mesh + p->mesh_count - 1) % p->mesh_count;
            }
            if (dai_ui_button(p->ui, ">") && p->mesh_count > 0) {
                r.mesh = r.mesh >= 0xFFFFFFFEu ? 0 : (r.mesh + 1) % p->mesh_count;
            }
            if (dai_ui_button(p->ui, "auto")) r.mesh = 0xFFFFFFFFu;
            dai_ui_row_end(p->ui);
        }
        // The size of the DRAWN mesh. Zero means "same as the collider", so a
        // fresh box shows the collider's numbers and stops following it the
        // moment either one is typed into.
        dai_vec3 shown_size = r.render_extent;
        bool follows = !(shown_size.x || shown_size.y || shown_size.z);
        if (follows) shown_size = r.half_extent;
        dai_vec3 full{ shown_size.x * 2.0f, shown_size.y * 2.0f, shown_size.z * 2.0f };
        if (dai_ui_num_vec3(p->ui, "Size", &full.x, 0.01f)) {
            r.render_extent = { full.x * 0.5f, full.y * 0.5f, full.z * 0.5f };
            if (r.mesh == 0xFFFFFFFFu) r.mesh = mesh_of_shape(r.shape);
        }
        // Show what the object IS, not what the document happens to store. A
        // node that never had a colour set carries 0,0,0 and the scene picked
        // one from the palette - showing the zeros makes the first drag paint
        // it black.
        dai_vec3 shown = r.color;
        bool implicit = (r.color.x == 0.0f && r.color.y == 0.0f && r.color.z == 0.0f);
        if (implicit) dai_editor_node_color(p->ed, n, &shown);
        if (dai_ui_num_vec3(p->ui, "Colour", &shown.x, 0.004f) || !implicit)
            r.color = shown;
        dai_ui_num_field(p->ui, "Rough", &r.roughness, 0.005f, 0.02f, 1.0f, "rough");
        dai_ui_num_field(p->ui, "Emissive", &r.emissive, 0.01f, 0.0f, 100.0f, "emissive");
    }

    // ---- Collider ----------------------------------------------------------
    // What the world can hit. NOT what it looks like: the wireframe is green
    // and separate on purpose, and Size here resizes the collision box while
    // the model stays the size it was. Unchecking it removes the COLLISION -
    // never the model, that was the bug where a component toggle made the
    // mesh vanish.
    int has_collider = !r.no_collider && !r.no_body;
    if (dai_ui_header_icon(p->ui, DAI_ICON_BOX, collider_title(r.shape),
                           &p->fold_collider, &has_collider) == 2) {
        r.no_collider = !has_collider;
        if (!has_collider && r.no_rigidbody) r.no_body = 1;
        else r.no_body = 0;
    }
    if (p->fold_collider) {
        if (!has_collider) {
            dai_ui_label(p->ui, "no collider - nothing can hit this");
        } else {
            // Edit Collider: a real button that stays lit while the mode is
            // on - the icon next to a label did not read as something you can
            // press, because nothing about it looked like a button.
            if (dai_ui_toggle_button(p->ui, "Edit Collider", p->collider_edit))
                p->collider_edit = !p->collider_edit;

            int shape_idx = r.shape;
            if (r.mesh == 0xFFFFFFFEu) shape_idx = 3;
            if (dai_ui_option(p->ui, "Shape", &shape_idx, COLLIDER_SHAPES, 4)) {
                // Changing the collider's shape must not reshape the model:
                // pin the mesh to what it is right now first.
                if (r.mesh == 0xFFFFFFFFu) r.mesh = mesh_of_shape(r.shape);
                if (shape_idx == 3) r.mesh = 0xFFFFFFFEu;
                else                r.shape = shape_idx;
            }
            int trig = r.trigger;
            if (dai_ui_checkbox(p->ui, "Is Trigger", &trig)) r.trigger = trig;
            dai_ui_num_vec3(p->ui, "Center", &r.collider_center.x, 0.01f);
            if (shape_idx != 3) {
                // Unity shows the FULL size of the box, not the half extent.
                // The document stores halves, so the field converts.
                dai_vec3 size{ r.half_extent.x * 2.0f, r.half_extent.y * 2.0f,
                               r.half_extent.z * 2.0f };
                if (dai_ui_num_vec3(p->ui, "Size", &size.x, 0.01f))
                    r.half_extent = { size.x * 0.5f, size.y * 0.5f, size.z * 0.5f };
            }
        }
    }

    // ---- Rigidbody ---------------------------------------------------------
    int has_body = !r.no_rigidbody && !r.no_body;
    if (dai_ui_header_icon(p->ui, DAI_ICON_SETTINGS, "Rigidbody", &p->fold_body, &has_body) == 2) {
        r.no_rigidbody = !has_body;
        if (!has_body && r.no_collider) r.no_body = 1;
        else r.no_body = 0;
    }
    if (p->fold_body) {
        if (!has_body) {
            dai_ui_label(p->ui, "no rigidbody - nothing drives this");
            dai_ui_option(p->ui, "Motion", &r.motion, MOTIONS, 3);
            dai_ui_num_field(p->ui, "Friction", &r.friction, 0.005f, 0.0f, 10.0f, "friction");
            dai_ui_num_field(p->ui, "Bounce", &r.restitution, 0.005f, 0.0f, 1.0f, "bounce");
        }
    }

    // THE rule this whole session was about: the collider is not the mesh. The
    // moment the collider's size or offset is touched, the mesh stops
    // following it - by freezing the size it has at that instant, so nothing
    // visibly jumps and the next drag of the green box leaves the model alone.
    if ((v3_differs(r.half_extent, before.half_extent) ||
         v3_differs(r.collider_center, before.collider_center)) &&
        !(r.render_extent.x || r.render_extent.y || r.render_extent.z)) {
        r.render_extent = before.half_extent;
        if (r.mesh == 0xFFFFFFFFu) r.mesh = mesh_of_shape(before.shape);
    }

    // ---- Script -------------------------------------------------------------
    // The path is the behaviour. Creating the file lives in the Project
    // window ("New Script"); assigning it is typing the name or picking the
    // file - the same rule as the Asset field.
    dai_ui_header_icon(p->ui, DAI_ICON_FILE, "Script", &p->fold_script, nullptr);
    if (p->fold_script) {
        if (p->script_buf_node != n) {
            std::snprintf(p->script_buf, sizeof(p->script_buf), "%s", r.script);
            p->script_buf_node = n;
        }
        if (dai_ui_input_text(p->ui, "File", p->script_buf, sizeof(p->script_buf)))
            std::snprintf(r.script, sizeof(r.script), "%s", p->script_buf);
        if (!r.script[0]) dai_ui_label(p->ui, "none - New Script is in the Project window");
    }

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
    for (float *v : { &r.render_extent.x, &r.render_extent.y, &r.render_extent.z })
        if (*v < 0.0f) *v = 0.0f;
    for (float *v : { &r.color.x, &r.color.y, &r.color.z })
        *v = *v < 0.0f ? 0.0f : (*v > 1.0f ? 1.0f : *v);

    if (std::memcmp(&before, &r, sizeof(dai_node_desc)) != 0) {
        begin_field_tx(p, "Edit");
        dai_doc_set(d, n, &r);
        // Straight away, not next frame: the host may not call
        // dai_editor_advance at all, and an inspector whose numbers move the
        // gizmo but not the object is worse than one that does nothing.
        dai_editor_resync(p->ed);
    }
}

void dai_editor_ui_inspector(dai_editor_ui *p, float x, float y, float w, float h) {
    if (!p) return;
    dai_ui_panel_begin(p->ui, x, y, w, h, "Inspector");
    inspector_body(p);
    dai_ui_panel_end(p->ui);
}

// ---------------------------------------------------------------- toolbar

void dai_editor_ui_toolbar(dai_editor_ui *p, float x, float y, float w) {
    if (!p) return;
    dai_doc *d = dai_editor_doc(p->ed);
    int state = dai_editor_state_get(p->ed);

    // 34 px tall and flush with the edges: this is the strip along the top of
    // the window, not a floating panel.
    dai_ui_panel_begin(p->ui, x, y, w, 34.0f, nullptr);
    dai_ui_row(p->ui, 24.0f);

    // Icons, not words. A toolbar of eleven text buttons is 600 px of chrome
    // and still unreadable at a glance; the icons are SVG, rasterised once at
    // the size this interface actually uses, and every one of them carries the
    // word it replaced as a tooltip - an icon only toolbar with no tooltips is
    // a memory test.
    int mode = dai_editor_gizmo_mode_get(p->ed);
    if (dai_ui_icon_button(p->ui, DAI_ICON_MOVE, "Move", mode == DAI_GIZMO_TRANSLATE))
        dai_editor_gizmo_mode(p->ed, DAI_GIZMO_TRANSLATE);
    if (dai_ui_icon_button(p->ui, DAI_ICON_ROTATE, "Rotate", mode == DAI_GIZMO_ROTATE))
        dai_editor_gizmo_mode(p->ed, DAI_GIZMO_ROTATE);
    if (dai_ui_icon_button(p->ui, DAI_ICON_SCALE, "Scale", mode == DAI_GIZMO_SCALE))
        dai_editor_gizmo_mode(p->ed, DAI_GIZMO_SCALE);

    dai_ui_toolbar_gap(p->ui, 10.0f);
    if (dai_ui_icon_button(p->ui, DAI_ICON_UNDO, "Undo", 0)) dai_editor_undo(p->ed);
    if (dai_ui_icon_button(p->ui, DAI_ICON_REDO, "Redo", 0)) dai_editor_redo(p->ed);
    if (dai_ui_icon_button(p->ui, DAI_ICON_COPY, "Duplicate", 0)) dai_editor_duplicate_selection(p->ed);
    if (dai_ui_icon_button(p->ui, DAI_ICON_TRASH, "Delete", 0)) dai_editor_delete_selection(p->ed);

    dai_ui_toolbar_gap(p->ui, 10.0f);
    if (state == DAI_EDITOR_EDIT) {
        if (dai_ui_icon_button(p->ui, DAI_ICON_PLAY, "Play", 0)) dai_editor_play(p->ed);
    } else if (state == DAI_EDITOR_PLAY) {
        if (dai_ui_icon_button(p->ui, DAI_ICON_PAUSE, "Pause", 1)) dai_editor_pause(p->ed);
        if (dai_ui_icon_button(p->ui, DAI_ICON_STOP, "Stop", 0)) dai_editor_stop(p->ed);
    } else {
        if (dai_ui_icon_button(p->ui, DAI_ICON_PLAY, "Resume", 0)) dai_editor_play(p->ed);
        if (dai_ui_icon_button(p->ui, DAI_ICON_STOP, "Stop", 0)) dai_editor_stop(p->ed);
        if (dai_ui_icon_button(p->ui, DAI_ICON_CHECK, "Keep", 0)) dai_editor_apply_sim(p->ed);
    }

    // The way back from a layout the user dragged into a corner.
    dai_ui_toolbar_gap(p->ui, 10.0f);
    if (dai_ui_icon_button(p->ui, DAI_ICON_LAYOUT, "Layout", 0) && p->layout_ready)
        dai_editor_ui_layout_reset(p, p->layout_w, p->layout_h);
    if (dai_ui_icon_button(p->ui, DAI_ICON_SETTINGS, "Settings", p->settings_open))
        p->settings_open = !p->settings_open;
    (void)d;
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
    if (p->collider_edit) return;   // one set of handles at a time
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

// ------------------------------------------------------- colliders in 3D

namespace {

dai_vec3 qrot_v(dai_quat q, dai_vec3 v) {
    dai_vec3 u{ q.x, q.y, q.z };
    dai_vec3 uv{ u.y*v.z - u.z*v.y, u.z*v.x - u.x*v.z, u.x*v.y - u.y*v.x };
    dai_vec3 uuv{ u.y*uv.z - u.z*uv.y, u.z*uv.x - u.x*uv.z, u.x*uv.y - u.y*uv.x };
    return { v.x + 2.0f*(q.w*uv.x + uuv.x),
             v.y + 2.0f*(q.w*uv.y + uuv.y),
             v.z + 2.0f*(q.w*uv.z + uuv.z) };
}
dai_vec3 v_add(dai_vec3 a, dai_vec3 b) { return { a.x+b.x, a.y+b.y, a.z+b.z }; }
dai_vec3 v_mul(dai_vec3 a, float s) { return { a.x*s, a.y*s, a.z*s }; }

// Where the collider actually is: the node's world transform, plus the centre
// offset, with the extents scaled the same way the physics scales them.
struct ColliderBox {
    dai_vec3 center{};      // world
    dai_quat rot{ 0, 0, 0, 1 };
    dai_vec3 half{};        // world units, already scaled
    dai_vec3 scale{ 1, 1, 1 };
    int      shape = 0;
};

bool collider_of(dai_editor_ui *p, dai_node n, ColliderBox *out) {
    dai_doc *d = dai_editor_doc(p->ed);
    dai_node_desc r{};
    if (dai_doc_get(d, n, &r) != DAI_OK) return false;
    if (r.no_body) return false;
    dai_vec3 wp{}, ws{ 1, 1, 1 };
    dai_quat wr{ 0, 0, 0, 1 };
    dai_doc_world_transform(d, n, &wp, &wr, &ws);
    // While playing, the object is where the SIMULATION put it - a wireframe
    // left at the pre-play pose would be a lie drawn in green.
    if (dai_editor_state_get(p->ed) != DAI_EDITOR_EDIT)
        dai_editor_live_position(p->ed, n, &wp);
    dai_vec3 off{ r.collider_center.x * ws.x, r.collider_center.y * ws.y,
                  r.collider_center.z * ws.z };
    out->center = v_add(wp, qrot_v(wr, off));
    out->rot = wr;
    out->scale = ws;
    out->shape = r.shape;
    float ax = std::fabs(ws.x), ay = std::fabs(ws.y), az = std::fabs(ws.z);
    if (r.shape == DAI_SHAPE_SPHERE) {
        float rad = r.half_extent.x * ax;
        out->half = { rad, rad, rad };
    } else if (r.shape == DAI_SHAPE_CAPSULE) {
        float rad = r.half_extent.x * ax;
        out->half = { rad, r.half_extent.y * ay + rad, rad };
    } else {
        out->half = { r.half_extent.x * ax, r.half_extent.y * ay, r.half_extent.z * az };
    }
    return true;
}

void wire_line(dai_editor_ui *p, dai_vec3 a, dai_vec3 b, uint32_t col, float thick) {
    float ax, ay, bx, by;
    if (!dai_editor_project(p->ed, a, &ax, &ay)) return;
    if (!dai_editor_project(p->ed, b, &bx, &by)) return;
    dai_ui_line(p->ui, ax, ay, bx, by, thick, col);
}

void wire_box(dai_editor_ui *p, const ColliderBox &c, uint32_t col, float thick) {
    dai_vec3 corner[8];
    for (int i = 0; i < 8; ++i) {
        dai_vec3 l{ (i & 1) ? c.half.x : -c.half.x,
                    (i & 2) ? c.half.y : -c.half.y,
                    (i & 4) ? c.half.z : -c.half.z };
        corner[i] = v_add(c.center, qrot_v(c.rot, l));
    }
    static const int EDGES[12][2] = { {0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7},
                                      {0,4},{1,5},{2,6},{3,7} };
    for (auto &e : EDGES) wire_line(p, corner[e[0]], corner[e[1]], col, thick);
}

void wire_circle(dai_editor_ui *p, dai_vec3 c, dai_quat rot, dai_vec3 ax, dai_vec3 ay,
                 float r, uint32_t col, float thick) {
    const int N = 24;
    dai_vec3 prev{};
    for (int i = 0; i <= N; ++i) {
        float t = (float)i / (float)N * 6.2831853f;
        dai_vec3 l = v_add(v_mul(ax, std::cos(t) * r), v_mul(ay, std::sin(t) * r));
        dai_vec3 wpt = v_add(c, qrot_v(rot, l));
        if (i) wire_line(p, prev, wpt, col, thick);
        prev = wpt;
    }
}

// Unity's collider green. Not "a green": this exact one, because the point of
// the colour is that it is instantly recognisable as "collision, not model".
const uint32_t WIRE_GREEN = 0xFF8FF08Fu;   // 0xAABBGGRR

void draw_collider(dai_editor_ui *p, const ColliderBox &c, uint32_t col, float thick) {
    if (c.shape == DAI_SHAPE_SPHERE) {
        wire_circle(p, c.center, c.rot, dai_vec3{1,0,0}, dai_vec3{0,1,0}, c.half.x, col, thick);
        wire_circle(p, c.center, c.rot, dai_vec3{0,1,0}, dai_vec3{0,0,1}, c.half.x, col, thick);
        wire_circle(p, c.center, c.rot, dai_vec3{1,0,0}, dai_vec3{0,0,1}, c.half.x, col, thick);
    } else if (c.shape == DAI_SHAPE_CAPSULE) {
        float rad = c.half.x, shaft = c.half.y - rad;
        dai_vec3 top = v_add(c.center, qrot_v(c.rot, dai_vec3{ 0, shaft, 0 }));
        dai_vec3 bot = v_add(c.center, qrot_v(c.rot, dai_vec3{ 0, -shaft, 0 }));
        wire_circle(p, top, c.rot, dai_vec3{1,0,0}, dai_vec3{0,0,1}, rad, col, thick);
        wire_circle(p, bot, c.rot, dai_vec3{1,0,0}, dai_vec3{0,0,1}, rad, col, thick);
        for (int i = 0; i < 4; ++i) {
            float sx = (i == 0) ? rad : (i == 1) ? -rad : 0.0f;
            float sz = (i == 2) ? rad : (i == 3) ? -rad : 0.0f;
            wire_line(p, v_add(top, qrot_v(c.rot, dai_vec3{ sx, 0, sz })),
                         v_add(bot, qrot_v(c.rot, dai_vec3{ sx, 0, sz })), col, thick);
        }
    } else {
        wire_box(p, c, col, thick);
    }
}

// The six face handles: centre of each face, in world space.
void face_handles(const ColliderBox &c, dai_vec3 *out, int *axis, int *sign) {
    int k = 0;
    for (int a = 0; a < 3; ++a) {
        for (int s = -1; s <= 1; s += 2) {
            dai_vec3 l{ 0, 0, 0 };
            (&l.x)[a] = (&c.half.x)[a] * (float)s;
            out[k] = v_add(c.center, qrot_v(c.rot, l));
            axis[k] = a;
            sign[k] = s;
            ++k;
        }
    }
}

} // namespace

int dai_editor_ui_collider_edit(const dai_editor_ui *p) { return p ? p->collider_edit : 0; }
void dai_editor_ui_collider_edit_set(dai_editor_ui *p, int on) { if (p) p->collider_edit = on ? 1 : 0; }

void dai_editor_ui_colliders(dai_editor_ui *p) {
    if (!p || p->view != DAI_VIEW_SCENE) return;
    uint32_t sel = dai_editor_selection_count(p->ed);
    for (uint32_t i = 0; i < sel; ++i) {
        ColliderBox c{};
        if (!collider_of(p, dai_editor_selected(p->ed, i), &c)) continue;
        draw_collider(p, c, WIRE_GREEN, p->collider_edit ? 2.0f : 1.5f);
        if (!p->collider_edit || i != 0) continue;

        // Edit mode: a handle per face, filled when it is the one being held.
        dai_vec3 h[6]; int ax[6], sg[6];
        face_handles(c, h, ax, sg);
        for (int k = 0; k < 6; ++k) {
            float sx, sy;
            if (!dai_editor_project(p->ed, h[k], &sx, &sy)) continue;
            bool live = p->col_axis == ax[k] && p->col_sign == sg[k];
            float s = live ? 4.0f : 3.0f;
            dai_ui_rect(p->ui, sx - s, sy - s, s * 2.0f, s * 2.0f,
                        live ? 0xFFFFFFFFu : WIRE_GREEN);
        }
    }
}

// Dragging a face handle: the FACE moves, so half extent and centre both move
// by half the distance - that is what "the box grew on one side" means, and
// resizing symmetrically instead is the thing that makes Unity users think the
// editor is broken.
static int collider_edit_input(dai_editor_ui *p, float mx, float my, int down) {
    if (!p->collider_edit || dai_editor_selection_count(p->ed) == 0) return 0;
    dai_node n = dai_editor_selected(p->ed, 0);
    ColliderBox c{};
    if (!collider_of(p, n, &c)) return 0;
    dai_doc *d = dai_editor_doc(p->ed);

    if (p->col_axis < 0) {
        if (!down) return 0;
        dai_vec3 h[6]; int ax[6], sg[6];
        face_handles(c, h, ax, sg);
        int best = -1;
        float bestd = 10.0f;      // pixels
        for (int k = 0; k < 6; ++k) {
            float sx, sy;
            if (!dai_editor_project(p->ed, h[k], &sx, &sy)) continue;
            float dx = sx - mx, dy = sy - my;
            float dist = std::sqrt(dx * dx + dy * dy);
            if (dist < bestd) { bestd = dist; best = k; }
        }
        if (best < 0) return 0;
        p->col_axis = ax[best];
        p->col_sign = sg[best];
        p->col_last_x = mx;
        p->col_last_y = my;
        dai_doc_begin(d, "Resize collider");
        p->col_tx_open = true;
        return 1;
    }

    if (!down) {
        p->col_axis = -1;
        if (p->col_tx_open) { dai_doc_commit(d); p->col_tx_open = false; }
        return 1;
    }

    // How many world units one pixel of movement along this axis is worth:
    // project the axis itself and measure it on screen. No camera maths here,
    // and it stays correct at any zoom or angle.
    dai_vec3 axis_w = qrot_v(c.rot, dai_vec3{ p->col_axis == 0 ? 1.0f : 0.0f,
                                              p->col_axis == 1 ? 1.0f : 0.0f,
                                              p->col_axis == 2 ? 1.0f : 0.0f });
    dai_vec3 handle = v_add(c.center, v_mul(axis_w, (&c.half.x)[p->col_axis] * (float)p->col_sign));
    float ax0, ay0, ax1, ay1;
    const float PROBE = 0.5f;
    if (!dai_editor_project(p->ed, handle, &ax0, &ay0)) return 1;
    if (!dai_editor_project(p->ed, v_add(handle, v_mul(axis_w, PROBE)), &ax1, &ay1)) return 1;
    float sdx = ax1 - ax0, sdy = ay1 - ay0;
    float slen = std::sqrt(sdx * sdx + sdy * sdy);
    if (slen < 0.5f) return 1;              // axis points at the camera: nothing to drag
    float mdx = mx - p->col_last_x, mdy = my - p->col_last_y;
    float along = (mdx * sdx + mdy * sdy) / slen;      // pixels along the axis
    float world = along * PROBE / slen * (float)p->col_sign;
    p->col_last_x = mx;
    p->col_last_y = my;
    if (world == 0.0f) return 1;

    dai_node_desc r{};
    if (dai_doc_get(d, n, &r) != DAI_OK) return 1;
    dai_node_desc before = r;
    float s = std::fabs((&c.scale.x)[p->col_axis]);
    if (s < 1e-4f) s = 1.0f;
    float local = world / s;                 // the document stores local units
    float *half = &r.half_extent.x + p->col_axis;
    float *ctr  = &r.collider_center.x + p->col_axis;
    float nh = *half + local * 0.5f;
    if (nh < 0.005f) nh = 0.005f;
    *ctr += (nh - *half) * (float)p->col_sign;
    *half = nh;
    // Same rule as the inspector: touching the collider unpins the mesh.
    if (!(r.render_extent.x || r.render_extent.y || r.render_extent.z)) {
        r.render_extent = before.half_extent;
        if (r.mesh == 0xFFFFFFFFu) r.mesh = mesh_of_shape(before.shape);
    }
    dai_doc_set(d, n, &r);
    dai_editor_resync(p->ed);
    return 1;
}

// ------------------------------------------------------- scene / game view

namespace {

// The camera node: a node tagged "MainCamera". No new document type, because a
// camera IS a transform with a meaning - and a tag is how this document says
// what something means (the same way Unity's tag does).
const char *CAMERA_TAG = "MainCamera";

dai_node find_camera(const dai_editor_ui *p) {
    dai_doc *d = dai_editor_doc(p->ed);
    uint32_t count = dai_doc_count(d);
    if (!count) return DAI_INVALID_NODE;
    std::vector<dai_node> ids(count);
    dai_doc_nodes(d, ids.data(), count);
    for (dai_node id : ids) {
        dai_node_desc r{};
        if (dai_doc_get(d, id, &r) != DAI_OK) continue;
        if (std::strcmp(r.tag, CAMERA_TAG) == 0) return id;
    }
    return DAI_INVALID_NODE;
}

// A rotation whose local -Z points along `dir`. Built from an orthonormal
// basis rather than from yaw/pitch, so it cannot disagree with whatever
// convention the camera code happens to use.
dai_quat look_quat(dai_vec3 dir) {
    float len = std::sqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
    if (len < 1e-6f) return dai_quat{ 0, 0, 0, 1 };
    dai_vec3 back{ -dir.x/len, -dir.y/len, -dir.z/len };
    dai_vec3 up{ 0, 1, 0 };
    if (std::fabs(back.y) > 0.999f) up = dai_vec3{ 0, 0, 1 };
    dai_vec3 right{ up.y*back.z - up.z*back.y, up.z*back.x - up.x*back.z,
                    up.x*back.y - up.y*back.x };
    float rl = std::sqrt(right.x*right.x + right.y*right.y + right.z*right.z);
    right = { right.x/rl, right.y/rl, right.z/rl };
    dai_vec3 u2{ back.y*right.z - back.z*right.y, back.z*right.x - back.x*right.z,
                 back.x*right.y - back.y*right.x };
    // matrix (columns right, u2, back) -> quaternion
    float m00 = right.x, m01 = u2.x, m02 = back.x;
    float m10 = right.y, m11 = u2.y, m12 = back.y;
    float m20 = right.z, m21 = u2.z, m22 = back.z;
    float tr = m00 + m11 + m22;
    dai_quat q{};
    if (tr > 0.0f) {
        float s = std::sqrt(tr + 1.0f) * 2.0f;
        q.w = 0.25f * s; q.x = (m21 - m12) / s; q.y = (m02 - m20) / s; q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m21 - m12) / s; q.x = 0.25f * s; q.y = (m01 + m10) / s; q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m02 - m20) / s; q.x = (m01 + m10) / s; q.y = 0.25f * s; q.z = (m12 + m21) / s;
    } else {
        float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m10 - m01) / s; q.x = (m02 + m20) / s; q.y = (m12 + m21) / s; q.z = 0.25f * s;
    }
    return q;
}

} // namespace

int  dai_editor_ui_view(const dai_editor_ui *p) { return p ? p->view : DAI_VIEW_SCENE; }
void dai_editor_ui_view_set(dai_editor_ui *p, int view) {
    if (p) p->view = view == DAI_VIEW_GAME ? DAI_VIEW_GAME : DAI_VIEW_SCENE;
}

int dai_editor_ui_game_camera(const dai_editor_ui *p, dai_vec3 *eye, dai_vec3 *look,
                              float *fov_deg) {
    if (!p) return 0;
    dai_node cam = find_camera(p);
    if (cam == DAI_INVALID_NODE) return 0;
    dai_doc *d = dai_editor_doc(p->ed);
    dai_vec3 wp{}, ws{ 1, 1, 1 };
    dai_quat wr{ 0, 0, 0, 1 };
    if (dai_doc_world_transform(d, cam, &wp, &wr, &ws) != DAI_OK) return 0;
    if (dai_editor_state_get(p->ed) != DAI_EDITOR_EDIT)
        dai_editor_live_position(p->ed, cam, &wp);
    dai_vec3 dir = qrot_v(wr, dai_vec3{ 0, 0, -1 });
    if (eye) *eye = wp;
    if (look) *look = v_add(wp, dir);
    if (fov_deg) *fov_deg = 60.0f;
    return 1;
}

dai_node dai_editor_ui_add_camera(dai_editor_ui *p) {
    if (!p) return DAI_INVALID_NODE;
    dai_doc *d = dai_editor_doc(p->ed);
    // Where the editor camera is looking: "align with view", the only way
    // anybody actually places a camera.
    float cx = p->layout_w > 0 ? p->layout_w * 0.5f : 640.0f;
    float cy = p->layout_h > 0 ? p->layout_h * 0.5f : 360.0f;
    dai_vec3 o{}, dir{ 0, 0, -1 };
    dai_editor_ray(p->ed, cx, cy, &o, &dir);
    dai_node_desc r = dai_node_desc_default();
    std::snprintf(r.name, sizeof(r.name), "Main Camera");
    std::snprintf(r.tag, sizeof(r.tag), "%s", CAMERA_TAG);
    r.position = o;
    r.rotation = look_quat(dir);
    r.no_body = 1;          // a camera is a transform, not a thing to collide with
    r.hidden = 1;
    dai_doc_begin(d, "Add camera");
    dai_node n = dai_doc_add(d, &r);
    dai_doc_commit(d);
    dai_editor_resync(p->ed);
    dai_editor_select(p->ed, n, 0);
    return n;
}

// The camera's frustum, drawn in the scene view the way every editor draws it:
// you cannot aim something you cannot see.
static void draw_cameras(dai_editor_ui *p) {
    dai_doc *d = dai_editor_doc(p->ed);
    uint32_t count = dai_doc_count(d);
    if (!count) return;
    std::vector<dai_node> ids(count);
    dai_doc_nodes(d, ids.data(), count);
    for (dai_node id : ids) {
        dai_node_desc r{};
        if (dai_doc_get(d, id, &r) != DAI_OK) continue;
        if (std::strcmp(r.tag, CAMERA_TAG) != 0) continue;
        dai_vec3 wp{}, ws{ 1, 1, 1 };
        dai_quat wr{ 0, 0, 0, 1 };
        dai_doc_world_transform(d, id, &wp, &wr, &ws);
        bool sel = dai_editor_is_selected(p->ed, id) != 0;
        uint32_t col = sel ? 0xFFFFFFFFu : 0xFFB0B0B0u;
        const float NEAR_D = 0.35f, FAR_D = 1.6f, HALF = 0.55f;
        dai_vec3 c[8];
        int k = 0;
        for (float depth : { NEAR_D, FAR_D }) {
            float hw = HALF * depth, hh = hw * 0.56f;
            for (int i = 0; i < 4; ++i) {
                float sx = (i == 0 || i == 3) ? -hw : hw;
                float sy = (i < 2) ? hh : -hh;
                c[k++] = v_add(wp, qrot_v(wr, dai_vec3{ sx, sy, -depth }));
            }
        }
        for (int i = 0; i < 4; ++i) {
            wire_line(p, c[i], c[(i + 1) % 4], col, 1.5f);
            wire_line(p, c[4 + i], c[4 + (i + 1) % 4], col, 1.5f);
            wire_line(p, c[i], c[4 + i], col, 1.5f);
        }
        wire_line(p, wp, c[0], col, 1.5f);
        wire_line(p, wp, c[2], col, 1.5f);
    }
}

// One tab of the Scene/Game bar. Drawn by hand rather than with dai_ui_button
// because this bar sits on the chrome, outside any panel, and it has to claim
// the click so it does not also land in the 3D view underneath.
static int view_tab(dai_editor_ui *p, float x, float y, float w, float h,
                    const char *label, int active) {
    dai_ui *ui = p->ui;
    const dai_ui_style *st = dai_ui_style_of(ui);
    float mx = 0, my = 0;
    int down = 0, pressed = 0;
    dai_ui_mouse(ui, &mx, &my, &down, &pressed);
    bool over = mx >= x && mx < x + w && my >= y && my < y + h;
    if (over) dai_ui_claim_mouse(ui);
    dai_ui_rect(ui, x, y, w, h,
                active ? st->panel : (over ? st->titlebar_focused : st->titlebar));
    if (active) dai_ui_rect(ui, x, y, w, 2.0f, st->accent);
    float tw = dai_ui_text_width(ui, label);
    dai_ui_text(ui, x + (w - tw) * 0.5f, y + 3.0f, label,
                active ? st->text : st->text_dim);
    return over && pressed;
}

// -------------------------------------------------------------- settings

static void settings_body(dai_editor_ui *p) {
    dai_ui *ui = p->ui;
    fit_label_column(ui);

    dai_ui_label(ui, "Appearance");
    // Font size is the ONE thing the host owns (it made the font and the
    // texture), so the editor asks. Everything else it can do itself.
    int size_idx = 0;
    if (p->settings_font_px >= 15.5f) size_idx = 2;
    else if (p->settings_font_px >= 13.5f) size_idx = 1;
    static const char *const SIZES[] = { "Klein (12)", "Normal (13)", "Gross (16)" };
    if (dai_ui_option(ui, "UI size", &size_idx, SIZES, 3)) {
        float px = size_idx == 0 ? 12.0f : size_idx == 1 ? 13.0f : 16.0f;
        p->settings_font_px = px;
        if (p->apply_font) p->apply_font(px, p->apply_user);
    }

    static const char *const THEMES[] = { "Unity Dark", "Darker", "Slate" };
    if (dai_ui_option(ui, "Theme", &p->settings_theme, THEMES, 3)) {
        dai_ui_style *st = dai_ui_style_of(ui);
        if (p->settings_theme == 1) {          // one step darker everywhere
            *st = dai_ui_style_default();
            st->panel = 0xFF2A2A2Au; st->track = 0xFF212121u; st->chrome = 0xFF0E0E0Eu;
            st->button = 0xFF454545u; st->button_hover = 0xFF525252u;
            st->titlebar = 0xFF1B1B1Bu; st->titlebar_focused = 0xFF262626u;
        } else if (p->settings_theme == 2) {   // the blue-grey end of dark
            *st = dai_ui_style_default();
            st->panel = 0xFF3B4148u; st->track = 0xFF2E3339u; st->chrome = 0xFF191D22u;
            st->button = 0xFF565E68u; st->button_hover = 0xFF646D78u;
            st->titlebar = 0xFF252A30u; st->titlebar_focused = 0xFF31383Fu;
        } else {
            *st = dai_ui_style_default();
        }
    }

    dai_ui_separator(ui);
    dai_ui_label(ui, "Viewport");
    float speed = dai_editor_cam_speed_get(p->ed);
    if (dai_ui_num_field(ui, "Cam speed", &speed, 0.05f, 0.1f, 200.0f, "camspeed"))
        dai_editor_cam_speed(p->ed, speed);
    float gizmo = dai_editor_gizmo_scale(p->ed);   // world units; the setter takes px
    (void)gizmo;
    float gpx = p->settings_gizmo_px;
    if (dai_ui_num_field(ui, "Gizmo px", &gpx, 1.0f, 30.0f, 300.0f, "gizmosize")) {
        p->settings_gizmo_px = gpx;
        dai_editor_gizmo_size(p->ed, gpx);
    }
    float snap = p->settings_snap;
    if (dai_ui_num_field(ui, "Snap step", &snap, 0.01f, 0.0f, 100.0f, "snapstep")) {
        p->settings_snap = snap;
        dai_editor_snap(p->ed, snap, 15.0f, 0.1f);
    }

    dai_ui_separator(ui);
    dai_ui_label(ui, "Values apply immediately.");
    dai_ui_label(ui, "UI size needs a restart of the text it already drew");
    dai_ui_label(ui, "to reshape - the host reloads the font.");
}

// ------------------------------------------------------- viewport input

int dai_editor_ui_viewport_input(dai_editor_ui *p, float mx, float my, int mouse_down) {
    if (!p) return 0;
    end_field_tx_if_released(p, mouse_down);

    bool over_ui = dai_ui_wants_mouse(p->ui) != 0;
    bool pressed = mouse_down && !p->prev_viewport_down;
    bool released = !mouse_down && p->prev_viewport_down;
    p->prev_viewport_down = mouse_down != 0;

    // The game view is what the player sees. Clicking in it must not pick,
    // move or deselect anything - that is the scene view's job.
    if (p->view != DAI_VIEW_SCENE) return 0;

    // Edit Collider handles outrank the gizmo: while that mode is on, the
    // handles ARE the thing you are aiming at.
    if (p->col_axis >= 0) {
        collider_edit_input(p, mx, my, mouse_down);
        return 1;
    }
    if (p->collider_edit && pressed && !over_ui && collider_edit_input(p, mx, my, mouse_down))
        return 1;

    if (p->viewport_dragging) {
        if (mouse_down) dai_editor_drag_update(p->ed, mx, my);
        if (released) { dai_editor_drag_end(p->ed); p->viewport_dragging = false; }
        return 1;
    }
    // A press that started over a panel must not fall through to the scene, or
    // clicking a button would also deselect whatever was selected.
    if (over_ui) return 0;

    if (pressed) {
        int axis = p->collider_edit ? DAI_AXIS_NONE : dai_editor_gizmo_hit(p->ed, mx, my);
        if (axis != DAI_AXIS_NONE) {
            dai_editor_drag_begin(p->ed, axis, mx, my);
            p->viewport_dragging = dai_editor_dragging(p->ed) != 0;
            return 1;
        }
        dai_node hit = dai_editor_pick(p->ed, mx, my);
        dai_editor_select(p->ed, hit, 0);       // empty space clears the selection
        return 1;
    }
    if (!p->collider_edit) dai_editor_gizmo_hover(p->ed, mx, my);
    return 0;
}

int dai_editor_ui_viewport(dai_editor_ui *p, const dai_editor_cam_input *in) {
    if (!p || !in) return 0;

    // The game view is not navigable - it is the player's camera, and dragging
    // it around would be editing the scene by accident.
    if (p->view != DAI_VIEW_SCENE) {
        p->prev_right_down = in->mouse_right != 0;
        p->prev_viewport_down = in->mouse_left != 0;
        return 0;
    }

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

    // The right button is two things in one place, and they are told apart by
    // movement: hold it and the camera looks around, TAP it and the object
    // under the pointer gets a menu. A tap that already opened a menu still
    // opens it; only the look around is deferred.
    int right_tap = in->mouse_right && !p->prev_right_down;
    p->prev_right_down = in->mouse_right != 0;
    if (right_tap && !over_ui && !p->menu_canvas.open && !p->menu_node.open) {
        dai_node hit = dai_editor_pick(p->ed, in->mouse_x, in->mouse_y);
        if (hit != DAI_INVALID_NODE) {
            dai_editor_select(p->ed, hit, 0);
            p->menu_target = hit;
            dai_ui_popup_open(&p->menu_node, in->mouse_x, in->mouse_y);
        } else {
            dai_ui_popup_open(&p->menu_canvas, in->mouse_x, in->mouse_y);
        }
        // Swallow the rest of this right click: the frame the menu appears,
        // the camera must not also start turning - otherwise every menu opens
        // with the world already rotated a degree.
        return 1;
    }

    return dai_editor_ui_viewport_input(p, in->mouse_x, in->mouse_y, left);
}

// ------------------------------------------------------------------ frame

void dai_editor_ui_asset_list(dai_editor_ui *p, const char *const *paths, uint32_t count) {
    if (!p) return;
    p->assets.clear();
    if (paths && count) p->assets.assign(paths, paths + count);
    if (p->asset_sel >= (int)p->assets.size()) p->asset_sel = -1;
}

int dai_editor_ui_asset_selected(const dai_editor_ui *p) { return p ? p->asset_sel : -1; }

static int assets_body(dai_editor_ui *p, float h, const char **out_path, int *out_as_tree);

int dai_editor_ui_assets(dai_editor_ui *p, float x, float y, float w, float h,
                         const char **out_path, int *out_as_tree) {
    if (out_path) *out_path = nullptr;
    if (out_as_tree) *out_as_tree = 0;
    if (!p || !p->ui) return 0;

    dai_ui_panel_begin(p->ui, x, y, w, h, "Assets");
    int r = assets_body(p, h, out_path, out_as_tree);
    dai_ui_panel_end(p->ui);
    return r;
}

static int assets_body(dai_editor_ui *p, float h, const char **out_path, int *out_as_tree) {
    if (p->assets.empty()) {
        // An empty browser and a browser nobody filled look the same to the
        // user, so say which it is.
        dai_ui_label(p->ui, "nothing mounted");
        return 0;
    }

    dai_ui_label_fmt(p->ui, "%u files", (unsigned)p->assets.size());
    dai_ui_separator(p->ui);

    // Only as many rows as fit. A folder with two hundred models must not push
    // the buttons off the bottom of the panel, and a list that quietly runs
    // past the edge is worse than one that says how much it is not showing.
    const float ROW = 22.0f;
    uint32_t fits = (uint32_t)((h - 110.0f) / ROW);
    if (fits < 1) fits = 1;
    uint32_t shown = (uint32_t)p->assets.size() < fits ? (uint32_t)p->assets.size() : fits;
    for (uint32_t i = 0; i < shown; ++i) {
        const char *full = p->assets[i] ? p->assets[i] : "";
        const char *slash = std::strrchr(full, '/');
        const char *label = slash ? slash + 1 : full;
        int selected = (int)i == p->asset_sel;
        char row[128];
        std::snprintf(row, sizeof(row), "%s%s", selected ? "> " : "  ", label);
        if (dai_ui_button(p->ui, row)) p->asset_sel = selected ? -1 : (int)i;
    }
    if (shown < p->assets.size())
        dai_ui_label_fmt(p->ui, "... %u more", (unsigned)(p->assets.size() - shown));

    dai_ui_separator(p->ui);
    if (p->asset_sel < 0 || p->asset_sel >= (int)p->assets.size()) {
        dai_ui_label(p->ui, "pick one");
        return 0;
    }

    const char *pick = p->assets[(size_t)p->asset_sel];
    dai_ui_label(p->ui, pick ? pick : "");
    int placed = 0;
    // A script is not placed, it is ATTACHED - the same click as typing its
    // name into the Script block, which is the only place it can mean
    // anything.
    size_t plen = pick ? std::strlen(pick) : 0;
    if (plen > 3 && std::strcmp(pick + plen - 3, ".js") == 0) {
        if (dai_ui_button(p->ui, "Assign to selection") &&
            dai_editor_selection_count(p->ed) > 0) {
            dai_node n = dai_editor_selected(p->ed, 0);
            dai_node_desc r{};
            if (dai_doc_get(dai_editor_doc(p->ed), n, &r) == DAI_OK) {
                dai_doc_begin(dai_editor_doc(p->ed), "Assign script");
                std::snprintf(r.script, sizeof(r.script), "%s", pick);
                dai_doc_set(dai_editor_doc(p->ed), n, &r);
                dai_doc_commit(dai_editor_doc(p->ed));
                p->script_buf_node = DAI_INVALID_NODE;   // refetch the field
            }
        }
        return 0;
    }
    dai_ui_row(p->ui, 22.0f);
    // Two buttons because the difference is physical, not cosmetic: one body
    // for the whole model, or one body per piece.
    if (dai_ui_button(p->ui, "Place")) {
        if (out_path) *out_path = pick;
        if (out_as_tree) *out_as_tree = 0;
        placed = 1;
    }
    if (dai_ui_button(p->ui, "As tree")) {
        if (out_path) *out_path = pick;
        if (out_as_tree) *out_as_tree = 1;
        placed = 1;
    }
    dai_ui_row_end(p->ui);
    return placed;
}

void dai_editor_ui_layout_dump(const dai_editor_ui *p, char *out, size_t n) {
    if (!p || !out || !n) return;
    const dai_ui_window *ws[5] = { &p->win_hierarchy, &p->win_project, &p->win_inspector,
                                   &p->win_scene, &p->win_settings };
    const char *names[5] = { "Hierarchy", "Project", "Inspector", "Scene", "Settings" };
    size_t used = 0;
    for (int i = 0; i < 5 && used + 80 < n; ++i) {
        const dai_ui_window *w = ws[i];
        int wro = std::snprintf(out + used, n - used,
                                "%s dock=%d slot=%d vp=%d fl=%d open=%d %.0f,%.0f %.0fx%.0f | ",
                                names[i], w->dock, w->dock_slot, w->viewport, w->floated,
                                w->open, w->x, w->y, w->w, w->h);
        used += wro > 0 ? (size_t)wro : 0;
    }
}

void dai_editor_ui_layout_reset(dai_editor_ui *p, float vw, float vh) {
    if (!p) return;
    const float SIDE = 230.0f;
    // Docked, like the layout every 3D editor opens with: hierarchy over
    // project on the left, inspector down the right. Dragging a title bar
    // pulls a window out of its dock, dropping it near an edge puts it back.
    p->win_hierarchy = dai_ui_window_docked(DAI_DOCK_LEFT, 1, SIDE);
    p->win_project   = dai_ui_window_docked(DAI_DOCK_LEFT, 2, SIDE);
    p->win_inspector = dai_ui_window_docked(DAI_DOCK_RIGHT, 0, SIDE);
    // The scene view is a window like the others. It starts in fill mode:
    // not docked to an edge, not floated - covering whatever is left.
    p->win_scene = dai_ui_window_make(0, 0, 400, 300);
    p->win_scene.viewport = 1;
    p->win_scene.floated = 0;
    p->win_scene.min_w = 200.0f;
    p->win_scene.min_h = 120.0f;
    p->win_settings = dai_ui_window_make(120, 80, 340, 320);
    p->win_settings.open = 0;
    p->scene_fx = -1;
    p->layout_ready = true;
    p->layout_w = vw; p->layout_h = vh;
}

void dai_editor_ui_viewport_rect(const dai_editor_ui *p, float *x, float *y, float *w, float *h) {
    if (!p) return;
    if (x) *x = p->view_x;
    if (y) *y = p->view_y;
    if (w) *w = p->view_w;
    if (h) *h = p->view_h;
}

// The two right click menus of the hierarchy and the viewport. They are run
// LAST in the frame so they paint above every window, and they mutate through
// the document like every other edit.
static void run_context_menus(dai_editor_ui *p) {
    dai_doc *d = dai_editor_doc(p->ed);

    static const dai_ui_menu_item NODE_ITEMS[] = {
        { DAI_ICON_PLUS, "Rename", "F2" },
        { DAI_ICON_COPY, "Duplicate", "Ctrl+D" },
        { DAI_ICON_TRASH, "Delete", "Del" },
    };
    int pick = dai_ui_popup_menu(p->ui, &p->menu_node, NODE_ITEMS, 3);
    if (pick >= 0 && p->menu_target != DAI_INVALID_NODE) {
        if (pick == 0) {
            dai_editor_ui_rename(p, p->menu_target);
        } else if (pick == 1) {
            dai_editor_select(p->ed, p->menu_target, 0);
            dai_editor_duplicate_selection(p->ed);
        } else if (pick == 2) {
            dai_editor_select(p->ed, p->menu_target, 0);
            dai_editor_delete_selection(p->ed);
        }
        p->menu_target = DAI_INVALID_NODE;
    }

    // Everything "Create" can mean, in one place. An empty node is a group:
    // no body, nothing drawn - it exists to carry children and a transform.
    static const dai_ui_menu_item CANVAS_ITEMS[] = {
        { DAI_ICON_PLUS, "Create Empty", nullptr },
        { DAI_ICON_BOX, "New Box", nullptr },
        { DAI_ICON_SPHERE, "New Sphere", nullptr },
        { DAI_ICON_CAPSULE, "New Capsule", nullptr },
        { DAI_ICON_CAMERA, "New Camera", nullptr },
    };
    int cpick = dai_ui_popup_menu(p->ui, &p->menu_canvas, CANVAS_ITEMS, 5);
    if (cpick == 4) { dai_editor_ui_add_camera(p); return; }
    if (cpick >= 0) {
        dai_node_desc r = dai_node_desc_default();
        const char *undo = "New box";
        if (cpick == 0) {
            std::snprintf(r.name, sizeof(r.name), "GameObject");
            r.no_body = 1;
            r.hidden = 1;           // an empty has no mesh to draw
            undo = "Create empty";
        } else {
            const char *names[4] = { "", "Box", "Sphere", "Capsule" };
            int shapes[4] = { 0, DAI_SHAPE_BOX, DAI_SHAPE_SPHERE, DAI_SHAPE_CAPSULE };
            std::snprintf(r.name, sizeof(r.name), "%s", names[cpick]);
            r.shape = shapes[cpick];
            r.motion = DAI_DYNAMIC;
            r.half_extent = { 0.5f, 0.5f, 0.5f };
            r.position = { 0, 0.5f, 0 };
            undo = names[cpick] == names[2] ? "New sphere" : (cpick == 3 ? "New capsule" : "New box");
        }
        dai_doc_begin(d, undo);
        dai_node n = dai_doc_add(d, &r);
        dai_doc_commit(d);
        dai_editor_resync(p->ed);
        dai_editor_select(p->ed, n, 0);
    }
}

void dai_editor_ui_frame(dai_editor_ui *p, float vw, float vh) {
    if (!p) return;
    dai_ui *ui = p->ui;
    const dai_ui_style *st = dai_ui_style_of(ui);
    const float TOP = 34.0f, BOTTOM = 24.0f;

    if (!p->layout_ready) dai_editor_ui_layout_reset(p, vw, vh);
    // Docked windows divide up everything between the two bars.
    dai_ui_dock_area(ui, 0.0f, TOP, vw, vh - TOP - BOTTOM);
    if (p->layout_w != vw || p->layout_h != vh) {
        // Keep the right hand column glued to the right edge on a resize.
        // Anything else leaves the inspector floating in the middle of a wider
        // window, which is exactly what a 21:9 monitor did to it.
        float dx = vw - p->layout_w;
        if (p->win_inspector.x + p->win_inspector.w > p->layout_w - 40.0f)
            p->win_inspector.x += dx;
        float dh = vh - p->layout_h;
        if (p->win_inspector.y + p->win_inspector.h > p->layout_h - 60.0f)
            p->win_inspector.h += dh;
        if (p->win_project.y + p->win_project.h > p->layout_h - 60.0f)
            p->win_project.h += dh;
        p->layout_w = vw; p->layout_h = vh;
    }

    // The chrome: solid bars top and bottom. Everything between them that no
    // window covers is the scene view - the editor is a frame around a hole.
    dai_ui_rect(ui, 0, 0, vw, TOP, st->chrome);
    dai_ui_rect(ui, 0, vh - BOTTOM, vw, BOTTOM, st->chrome);
    dai_ui_rect(ui, 0, vh - BOTTOM, vw, 1.0f, st->panel_border);

    dai_editor_ui_toolbar(p, 0.0f, 0.0f, vw);

    if (dai_ui_window_begin(ui, "Hierarchy", &p->win_hierarchy))
        hierarchy_body(p, p->win_hierarchy.h - 40.0f);
    dai_ui_window_end(ui);

    if (dai_ui_window_begin(ui, "Project", &p->win_project)) {
        // Two tabs: the current project's files, and the projects themselves.
        // A project in this engine is a folder of scenes and assets - it has
        // to exist before "nothing mounted" can become anything, which is why
        // the tab is here and not in a launcher.
        dai_ui_row(ui, 22.0f);
        if (dai_ui_button(ui, p->proj_tab == 0 ? "[Files]" : "Files")) p->proj_tab = 0;
        if (dai_ui_button(ui, p->proj_tab == 1 ? "[Projects]" : "Projects")) {
            p->proj_tab = 1;
            dai_editor_ui_projects_refresh(p);
        }
        dai_ui_row_end(ui);
        dai_ui_separator(ui);

        if (p->proj_tab == 1) {
            if (p->proj_current.empty())
                dai_ui_label(ui, "no project open");
            else
                dai_ui_label_fmt(ui, "open: %s", p->proj_current.c_str());
            dai_ui_separator(ui);
            if (p->projects.empty())
                dai_ui_label(ui, p->proj_list ? "no projects yet" : "no project host");
            for (const std::string &name : p->projects) {
                if (dai_ui_button(ui, name.c_str()) && p->proj_open) {
                    if (p->proj_open(name.c_str(), p->proj_user)) {
                        p->proj_current = name;
                        p->proj_tab = 0;
                    }
                }
            }
            dai_ui_separator(ui);
            dai_ui_label(ui, "New project");
            dai_ui_input_text(ui, "Name", p->proj_name_buf, sizeof(p->proj_name_buf));
            if (dai_ui_button(ui, "Create + open") && p->proj_create) {
                if (p->proj_name_buf[0] &&
                    p->proj_create(p->proj_name_buf, p->proj_user)) {
                    p->proj_current = p->proj_name_buf;
                    p->proj_name_buf[0] = 0;
                    dai_editor_ui_projects_refresh(p);
                    p->proj_tab = 0;
                }
            }
        } else {
            const char *pick = nullptr; int as_tree = 0;
            if (assets_body(p, p->win_project.h, &pick, &as_tree)) {
                p->pending_asset = pick;
                p->pending_as_tree = as_tree;
            }
            // Creating a script is a file, and the file is the host's - the
            // editor owns the click, the host owns the folder.
            if (p->script_create) {
                dai_ui_separator(ui);
                dai_ui_label(ui, "New script");
                dai_ui_input_text(ui, "Name", p->script_name_buf, sizeof(p->script_name_buf));
                if (dai_ui_button(ui, "Create script") && p->script_name_buf[0]) {
                    if (p->script_create(p->script_name_buf, p->script_user))
                        p->script_name_buf[0] = 0;
                }
            }
        }
    }
    dai_ui_window_end(ui);

    if (dai_ui_window_begin(ui, "Inspector", &p->win_inspector))
        inspector_body(p);
    dai_ui_window_end(ui);

    // ---- the scene window -------------------------------------------------
    // The viewport is a WINDOW: it docks like every other window, resizes like
    // every other window, and tearing it off the layout floats it - which is
    // the answer to "Scene und Game sind eigentlich auch Fenster".
    float fx = 0, fy = 0, fw = vw, fh = vh;
    dai_ui_free_area(ui, &fx, &fy, &fw, &fh);
    if (fy < TOP) { fh -= (TOP - fy); fy = TOP; }
    if (fy + fh > vh - BOTTOM) fh = vh - BOTTOM - fy;

    // Fill mode versus floated: if the window is not where the layout last
    // put it, the user moved it - and it floats from then on. Docking it
    // clears the flag, because a docked window is placed by the dock.
    if (p->win_scene.dock != DAI_DOCK_NONE) p->win_scene.floated = 0;
    bool filling = p->win_scene.dock == DAI_DOCK_NONE && !p->win_scene.floated;
    if (filling && p->scene_fx >= 0.0f &&
        (std::fabs(p->win_scene.x - p->scene_fx) > 2.0f ||
         std::fabs(p->win_scene.y - p->scene_fy) > 2.0f))
        p->win_scene.floated = 1;
    // Dropped back over the middle of the layout: fill again.
    {
        float mx = 0, my = 0;
        int down = 0;
        dai_ui_mouse(ui, &mx, &my, &down, nullptr);
        if (!down && p->prev_frame_down && p->win_scene.floated &&
            p->win_scene.dock == DAI_DOCK_NONE &&
            mx > fx + fw * 0.25f && mx < fx + fw * 0.75f &&
            my > fy + fh * 0.25f && my < fy + fh * 0.75f)
            p->win_scene.floated = 0;
        p->prev_frame_down = down;
    }
    filling = p->win_scene.dock == DAI_DOCK_NONE && !p->win_scene.floated;
    if (filling) {
        p->win_scene.x = fx; p->win_scene.y = fy;
        p->win_scene.w = fw; p->win_scene.h = fh;
    }

    if (dai_ui_window_begin(ui, "Scene", &p->win_scene)) {
        // No widgets: the body IS the scene. The tabs sit on the title bar,
        // drawn after window_end at the window's own layer.
    }
    dai_ui_window_end(ui);
    {
        int layer = dai_ui_window_layer(ui, "Scene");
        dai_ui_layer_set(ui, layer);
        float bar = 19.0f;
        float tx = p->win_scene.x + 22.0f;
        // Play switches to Game and Stop switches back, exactly once each.
        int playing = dai_editor_state_get(p->ed) != DAI_EDITOR_EDIT;
        if (playing && !p->view_was_playing) p->view = DAI_VIEW_GAME;
        if (!playing && p->view_was_playing) p->view = DAI_VIEW_SCENE;
        p->view_was_playing = playing;
        if (view_tab(p, tx, p->win_scene.y, 58.0f, bar, "Scene", p->view == DAI_VIEW_SCENE))
            p->view = DAI_VIEW_SCENE;
        if (view_tab(p, tx + 59.0f, p->win_scene.y, 58.0f, bar, "Game", p->view == DAI_VIEW_GAME))
            p->view = DAI_VIEW_GAME;
        dai_ui_layer_set(ui, 0);
    }
    {
        float bar = 19.0f;
        p->view_x = p->win_scene.x + 1.0f;
        p->view_y = p->win_scene.y + bar;
        p->view_w = p->win_scene.w - 2.0f;
        p->view_h = p->win_scene.h - bar;
        if (p->view_w < 8.0f) p->view_w = 8.0f;
        if (p->view_h < 8.0f) p->view_h = 8.0f;
    }

    if (p->view == DAI_VIEW_GAME && !dai_editor_ui_game_camera(p, nullptr, nullptr, nullptr)) {
        // Unity's message, and it means the same thing here.
        const char *msg = "No camera in the scene - right click the viewport, New Camera";
        float tw = dai_ui_text_width(ui, msg);
        dai_ui_text(ui, p->view_x + (p->view_w - tw) * 0.5f,
                    p->view_y + p->view_h * 0.5f, msg, st->text_dim);
    }

    p->scene_fx = p->win_scene.x; p->scene_fy = p->win_scene.y;
    p->scene_fw = p->win_scene.w; p->scene_fh = p->win_scene.h;

    dai_editor_ui_timeline(p, p->view_x + 8.0f, vh - BOTTOM - 50.0f, p->view_w - 16.0f);
    dai_editor_ui_status(p, 0.0f, vh - BOTTOM, vw, BOTTOM);
    if (p->view == DAI_VIEW_SCENE) {
        // The wireframes are UI lines over the 3D - clipped to the window the
        // 3D lives in, or an object behind the inspector draws its gizmo on
        // the inspector.
        dai_ui_clip_begin(ui, p->view_x, p->view_y, p->view_w, p->view_h);
        dai_editor_ui_colliders(p);
        draw_cameras(p);
        dai_editor_ui_gizmo(p);
        dai_ui_clip_end(ui);
    }

    // Settings, on top of the rest.
    if (p->settings_open) {
        p->win_settings.open = 1;
        if (dai_ui_window_begin(ui, "Settings", &p->win_settings))
            settings_body(p);
        dai_ui_window_end(ui);
    } else {
        p->win_settings.open = 0;
    }

    run_context_menus(p);
}

void dai_editor_ui_status(dai_editor_ui *p, float x, float y, float w, float h) {
    if (!p) return;
    dai_ui *ui = p->ui;
    const dai_ui_style *st = dai_ui_style_of(ui);
    dai_doc *d = dai_editor_doc(p->ed);
    int state = dai_editor_state_get(p->ed);
    const char *state_name = state == DAI_EDITOR_PLAY ? "PLAY"
                           : state == DAI_EDITOR_EDIT ? "EDIT" : "PAUSED";
    char buf[192];
    std::snprintf(buf, sizeof(buf), "%s   %u nodes   selection %u   undo: %s",
                  state_name, dai_doc_count(d), dai_editor_selection_count(p->ed),
                  dai_editor_undo_name(p->ed) && *dai_editor_undo_name(p->ed)
                      ? dai_editor_undo_name(p->ed) : "-");
    dai_ui_text(ui, x + 8.0f, y + 3.0f, buf, st->text_dim);
    char right[96];
    std::snprintf(right, sizeof(right), "viewport %.0fx%.0f", p->view_w, p->view_h);
    float rw = dai_ui_text_width(ui, right);
    dai_ui_text(ui, x + w - rw - 8.0f, y + 3.0f, right, st->text_dim);
}

int dai_editor_ui_take_asset(dai_editor_ui *p, const char **out_path, int *out_as_tree) {
    if (!p || !p->pending_asset) return 0;
    if (out_path) *out_path = p->pending_asset;
    if (out_as_tree) *out_as_tree = p->pending_as_tree;
    p->pending_asset = nullptr;
    p->pending_as_tree = 0;
    return 1;
}

} // extern "C"
