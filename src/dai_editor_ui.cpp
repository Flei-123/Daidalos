// Editor panels. See include/dai_editor_ui.h.
//
// Reads the document every frame and writes edits straight back to it. There is
// no model of the scene here and no cached widget values - the document already
// is the model, and a second copy would be the thing that goes stale.

#include "dai_editor_ui.h"
#include "dai_tr.h"
#include "dai_dock.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
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

// The nearest spelling to `near_deg` of the same orientation. See above.
static void quat_to_euler_near(dai_quat q, const float *near_deg, float *deg) {
    float a[3];
    quat_to_euler(q, a);
    // The other family: flip roll and yaw by half a turn, mirror pitch.
    float b[3] = { a[0] + 180.0f, 180.0f - a[1], a[2] + 180.0f };

    auto unwrap = [](float v, float target) {
        // v + 360k, k chosen so the result is within half a turn of target.
        float d = v - target;
        float k = d / 360.0f;
        k = k >= 0.0f ? std::floor(k + 0.5f) : std::ceil(k - 0.5f);
        return v - k * 360.0f;
    };
    float cost_a = 0.0f, cost_b = 0.0f;
    for (int i = 0; i < 3; ++i) {
        a[i] = unwrap(a[i], near_deg[i]);
        b[i] = unwrap(b[i], near_deg[i]);
        cost_a += std::fabs(a[i] - near_deg[i]);
        cost_b += std::fabs(b[i] - near_deg[i]);
    }
    const float *win = cost_b + 0.001f < cost_a ? b : a;
    for (int i = 0; i < 3; ++i) deg[i] = win[i];
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
const char *const COLLIDER_SHAPES[] = { "Box", "Sphere", "Capsule", "Compound", "Cylinder",
                                        "From Mesh" };
// The index in that list is the dai_shape value, except for the last entry -
// "From Mesh" is not a shape, it is the mesh answering instead.
const int COLLIDER_FROM_MESH = 5;

} // namespace

// What the inspector shows for a material. Deliberately a copy of the fields
// rather than the material struct itself: this file does not include the
// material header, because the day it does is the day the editor UI cannot be
// built without the material format.
struct dai_matfile_view {
    float color[3];
    float roughness;
    float metallic;
    float emissive;
};

struct dai_editor_ui {
    dai_editor *ed = nullptr;
    dai_ui     *ui = nullptr;

    std::unordered_set<dai_node> folded;   // default is open, so this stays small
    // The hierarchy's filter. A scene of four hundred objects is a scene you
    // scroll, and scrolling is not finding.
    char hier_filter[64] = { 0 };
    int  hier_flat = 0;      // filtering: draw the row, not the subtree
    uint32_t visible_rows = 0;

    // A drag on a numeric field changes the value every frame. Without this the
    // undo stack would fill with one step per frame of the drag.
    bool     field_tx_open = false;
    bool     prev_mouse_down = false;

    // Fold state of the inspector's component blocks. Retained because it
    // cannot be derived from the document - the same reason the hierarchy's
    // folds live here.
    int fold_transform = 1, fold_body = 1, fold_collider = 1, fold_render = 1;
    int fold_button = 1;
    std::vector<int> fold_scripts;   // one fold per attached script (Unity: each is its own component)
    char     script_buf[512] = { 0 };   // a full script list fits, not one path
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
    // "Open this file in whatever edits it" - the host decides what that is
    // (VS Code if it can find one, the OS default otherwise). NULL = do nothing.
    dai_editor_ui_rename_fn open_asset = nullptr;
    void *open_asset_user = nullptr;
    // Real directories on disk, fed by the host: the tree is otherwise derived
    // from FILE paths alone, so an empty folder was invisible - "New Folder"
    // vanished the moment it was created, and a renamed one with it.
    std::vector<const char *> folders_disk;
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

    // The Files half, Unity's two column browser: the folder tree left, the
    // chosen folder's contents right. The tree is derived from the flat asset
    // list every frame - only this UI state survives between frames.
    std::string proj_dir;                       // the folder being shown
    std::set<std::string> proj_folds = { "" };  // expanded folders, root always
    float proj_tree_w = 170.0f;
    float proj_tree_scroll = 0.0f, proj_list_scroll = 0.0f;
    int   proj_tree_drag = 0;
    char  proj_search[64] = { 0 };

    // ---- scripts -----------------------------------------------------------
    int (*script_create)(const char *name, void *user) = nullptr;
    int (*folder_create)(const char *name, void *user) = nullptr;
    int (*material_create)(const char *name, void *user) = nullptr;
    void *material_user = nullptr;
    void *script_user = nullptr;
    char   script_name_buf[64] = { 0 };
    int    script_focus = 0;
    int    want_save = 0, want_refresh = 0;
    // What the About block shows. The editor knows none of it - the host owns
    // the disk and the updater - so it is pushed in and simply displayed.
    char about_projects[320] = { 0 };
    char about_assets[320] = { 0 };
    char about_status[160] = { 0 };
    int  want_update_check = 0;
    // Set when a material was assigned or the browser was refreshed: the host
    // re-reads the .daimat files and pushes their numbers onto the nodes that
    // point at them. Read and cleared like the other one-shots.
    int want_material_apply = 0;
    // Drag & drop of a .js from the Project list onto a node (Unity: dropping
    // a script on an object ADDS a script component to it).
    std::string drag_pending;           // row the press started on
    std::string drag_script;            // moved far enough: actively dragged
    float       drag_px = 0, drag_py = 0;
    dai_node    hover_node = DAI_INVALID_NODE;  // hierarchy row under the pointer
    // Inline rename of a just-created asset (Unity's create flow).
    std::string rename_click;
    std::string last_pick;         // last row clicked in the Project window           // row the Project right click landed on
    std::string rename_asset;
    char        rename_asset_buf[160] = { 0 };
    int         rename_seen_active = 0;
    // Prefabs: the document layer can already save and instantiate them
    // (dai_doc_prefab_save / _instantiate). The editor just never offered it.
    dai_editor_ui_rename_fn prefab_save = nullptr;   // (node_name, rel_path) -> 1
    dai_editor_ui_rename_fn asset_rename = nullptr;
    void       *rename_user = nullptr;
    // Importing from outside the project: the desktop's file manager drops
    // onto the Project window, the host does the copying.
    dai_editor_ui_rename_fn asset_import = nullptr;
    void       *import_user = nullptr;
    // Deleting a file is not undoable, so it asks first - and the asking is a
    // small popup rather than a modal, because a modal in an editor stops the
    // world for a question about one file.
    dai_editor_ui_rename_fn asset_delete = nullptr;
    void       *delete_user = nullptr;
    std::string delete_ask;            // what Delete is about to remove
    dai_ui_popup menu_delete{};

    // ---- the built-in script editor ------------------------------------
    // One entry per open file. The buffer is the file plus room to type in:
    // a code editor that stops accepting characters at the file's original
    // length is a code editor you use once.
    struct OpenScript {
        std::string       path;
        std::vector<char> buf;
        dai_ui_code_state st{};
        int               dirty = 0;
    };
    std::vector<OpenScript> scripts_open;
    int  script_tab = 0;
    int  script_external = 0;          // 0 = edit here, 1 = hand to VS Code
    dai_editor_ui_read_fn  file_read = nullptr;
    dai_editor_ui_write_fn file_write = nullptr;
    void *file_user = nullptr;
    // A row of the Project window dragged onto a FOLDER moves it there. The
    // row that is aimed at is worked out while the folders are drawn and
    // consumed at the end of the frame, where the release is handled - the
    // two cannot be the same place, because the folder rows are drawn long
    // before anyone knows whether the button came up over one of them.
    std::string proj_drop_dir;
    int         proj_drop_ok = 0;
    // Which folder ROW of the listing is selected. Unity's rule, and every
    // file manager's: one click picks a folder up (rename it, drag it, see
    // what it is), two clicks go inside. Entering on the first click makes
    // the folder unselectable - there is no gesture left that means "this
    // one" - which is why renaming a folder needed the tree or a right click.
    std::string proj_sel_folder;
    // What the inspector is inspecting when it is not inspecting an object.
    // Unity's rule: the inspector shows THE selection, and a file in the
    // project window is a selection like any other. Set by a click in the
    // browser, cleared by a click in the hierarchy or the viewport.
    std::string inspect_asset;
    std::string inspect_loaded;        // the path the buffer below belongs to
    std::vector<char> inspect_text;    // the file, when it is one we read
    dai_matfile_view inspect_mat{};    // parsed, when it is a material
    int inspect_mat_ok = 0;
    int inspect_dirty = 0;
    // Scenes as files (Unity: several per project, opened by click).
    dai_editor_ui_project_list_fn   scene_list = nullptr;
    dai_editor_ui_project_action_fn scene_open = nullptr;
    dai_editor_ui_project_action_fn scene_save_as = nullptr;
    void       *scene_user = nullptr;
    char        scene_name_buf[128] = { 0 };
    // Node references for scripts: "// @param target" declares a field, a
    // hierarchy node dragged onto the assign block fills it.
    dai_editor_ui_params_fn params_fn = nullptr;
    void       *params_user = nullptr;
    dai_node    drag_node = DAI_INVALID_NODE;
    dai_node    drag_node_pending = DAI_INVALID_NODE;
    dai_node    drag_ref_target = DAI_INVALID_NODE;  // selection when the drag began
    float       drag_nx = 0, drag_ny = 0;
    int         param_hover_entry = -1;
    char        param_hover_key[64] = { 0 };

    // A press is not a click yet. Selecting on the press is what made DRAGGING
    // a row repaint the inspector: you grab an object to move it and the panel
    // behind you has already switched to it. The pick waits here and is
    // committed when the button comes up without a drag having started.
    int          click_kind = 0;        // 0 none, 1 folder, 2 file, 3 node
    std::string  click_path;            // the folder or file it landed on
    int          click_dbl = 0;         // it was the second click of a pair
    dai_node     click_node = DAI_INVALID_NODE;
    int          click_ctrl = 0;        // ctrl was held: add to the selection
    int          click_shift = 0;       // shift was held: take everything between
    // The anchor a shift-click measures from. One per list, because the two
    // lists are two orderings - and "everything between" only means anything
    // inside one of them.
    dai_node     range_anchor_node = DAI_INVALID_NODE;
    std::string  range_anchor_asset;
    // Files picked in the Project window. The folder selection stays single:
    // a range of folders is not a thing anyone does, and the browser navigates
    // by folder.
    std::vector<std::string> asset_multi;
    dai_node     ping_node = DAI_INVALID_NODE;  // an object field was clicked

    // The object picker behind a reference field's target button.
    // ---- localisation ------------------------------------------------------
    // Held as a table in memory and written back per language on Save. The
    // host owns the files (it knows the assets folder); this owns the grid.
    struct LocEntry { std::string key; std::vector<std::string> vals; };
    std::vector<std::string> loc_langs;      // codes, in file order
    std::vector<std::string> loc_names;      // what each calls itself
    std::vector<LocEntry>    loc_rows;
    float loc_scroll = 0.0f;
    int   loc_dirty = 0;
    int   loc_loaded = 0;
    char  loc_newkey[96] = { 0 };
    char  loc_newlang[16] = { 0 };
    int   loc_edit_row = -1, loc_edit_col = -1;
    char  loc_edit_buf[256] = { 0 };
    // The host fills the table and writes it back - it is the one that knows
    // where Assets/Strings is.
    int  (*loc_load)(void *user) = nullptr;
    int  (*loc_save)(void *user) = nullptr;
    void *loc_user = nullptr;

    dai_ui_searchlist obj_list{};
    dai_ui_searchlist font_list{};
    dai_node          font_pick_node = DAI_INVALID_NODE;
    dai_node    obj_pick_node = DAI_INVALID_NODE;
    char obj_pick_type[32] = { 0 };   // the field's declared type, "" = any
    int         obj_pick_entry = -1;
    char        obj_pick_key[64] = { 0 };

    // ---- mesh inventory --------------------------------------------------
    dai_editor_ui_mesh_name_fn mesh_name = nullptr;
    uint32_t mesh_count = 0;
    void *mesh_user = nullptr;

    // The layout. Windows the user can move, so their rectangles have to
    // survive the frame - and be resettable, because a window dragged off the
    // screen on a monitor you no longer have is otherwise gone for good.
    // The layout is a dock TREE now (see dai_dock.h): panels tile, they never
    // overlap, and Scene/Game are two tabs of one leaf like every other pair.
    dai_dock *dock = nullptr;
    int   settings_open = 0;
    dai_ui_popup menu_project{};      // right click in the project window
    dai_ui_popup menu_addcomp{};      // (superseded by addcomp_list below)
    dai_ui_searchlist addcomp_list{}; // Add Component, Unity style: search +
                                      // filtered list, scripts included
    dai_ui_popup menu_mesh{};         // the mesh object field's picker
    dai_node    mesh_menu_node = DAI_INVALID_NODE;
    dai_node    addcomp_node = DAI_INVALID_NODE;   // who Add Component was opened for
    dai_ui_searchlist mat_list{};     // the material row's picker
    dai_node    mat_menu_node = DAI_INVALID_NODE;
    int         mat_menu_slot = 0;
    dai_ui_popup menu_comp{};         // right click on a component header
    int  comp_menu_target = -1;       // 0 transform 1 rigidbody 2 collider 3 camera 4 light 5 sprite 6 audio 7 text 8 image 9 button
    dai_ui_popup menu_window{};       // the Window menu: bring a panel back
    char clipboard[4096] = { 0 };     // objects, fields, console lines, free text
    unsigned clip_rev = 0;            // bumped on every set - the host mirrors
                                      // changes into the OS clipboard
    int  clip_kind = -1;
    int  last_ctrl_held = 0;          // refreshed from the cam input each frame
    int  last_shift_held = 0;         // ...and shift, which picks RANGES
    int  fold_camera = 1, fold_light = 1, fold_sprite = 1, fold_audio = 1;
    int  fold_text = 1;
    int  fold_freeze = 1;
    int  fold_image = 1;
    dai_ui_searchlist image_list{};
    dai_node          image_pick_node = DAI_INVALID_NODE;
    // The project's active language, so the inspector previews a "@key" as
    // the words a player would see. Owned by the host - it knows which files
    // exist - and pushed in with dai_editor_ui_strings().
    const char *(*tr_fn)(const char *text, void *user) = nullptr;
    void *tr_user = nullptr;
    char audio_buf[64] = { 0 };
    dai_node audio_buf_node = DAI_INVALID_NODE;
    // Console: one ring buffer for engine messages and script print().
    struct LogLine { int level; std::string text; uint32_t count; };
    std::vector<LogLine> log;
    int  log_show[3] = { 1, 1, 1 };   // info / warning / error
    int  log_collapse = 1;
    float log_scroll = 0.0f;
    // Unity's console is two panels: the list, and the full text of the ONE
    // line you clicked. Messages are longer than a row - a script error
    // carries a file, a line and a reason - and a list that clips them is a
    // list you have to copy out of to read.
    int   log_sel = -1;            // index into `log`, -1 = nothing picked
    float log_detail = 0.0f;       // height of the detail pane, 0 = closed
    float log_detail_scroll = 0.0f;
    // Audio mixer: four busses, the set every game ends up with.
    float bus_gain[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    int   bus_mute[4] = { 0, 0, 0, 0 };
    char scene_label[128] = { 0 };    // the active scene, shown as the hierarchy root
    int  scene_dirty = 0;             // the host owns the file; it says when
    char toast[160] = { 0 };          // "saved main.daidalos" and friends
    float toast_left = 0.0f;          // seconds it stays up
    int  rename_drawn = 0;            // the rename row existed THIS frame
    int  scene_root_folded = 0;

    // ---- settings ----------------------------------------------------------
    float settings_font_px = 13.0f;
    void (*apply_scale)(float, void *) = nullptr;
    void *apply_scale_user = nullptr;
    float settings_ui_scale = 0.0f;   // 0 = follow the display
    int   fold_materials = 1;      // the Materials array, open like Unity's
    int   reveal_selection = 0;    // scroll the hierarchy to the selection
    int   reveal_row_wanted = 0;
    int   settings_tab = 0;
    void (*proj_settings_host)(void *user) = nullptr;
    int   settings_theme = 0;
    float def_friction = 0.6f, def_restitution = 0.0f;   // host pushes project defaults
    float settings_gizmo_px = 90.0f;
    float settings_snap = 0.0f;
    void (*apply_font)(float px, void *user) = nullptr;
    void *apply_user = nullptr;
    const char *pending_asset = nullptr;   // clicked in the Project window
    // Where a dragged asset was let go, in the world. A double click has no
    // such place, which is what the flag is for.
    int      pending_at_valid = 0;
    dai_vec3 pending_at{ 0, 0, 0 };
    // Prefab mode: the name shown in the bar, and the two one-shot flags the
    // host reads.
    std::string prefab_mode;
    std::string prefab_open_want;
    int         prefab_exit_want = 0;
    int   pending_as_tree = 0;
    bool  layout_ready = false;
    float layout_w = 0, layout_h = 0;
    float view_x = 0, view_y = 0, view_w = 0, view_h = 0;
    // The Game panel next to the Scene panel: two independent views, not one
    // viewport with a split personality.
    int   has_game = 0;
    float game_x = 0, game_y = 0, game_w = 0, game_h = 0;

    // viewport interaction
    int  component_remove = 0;   // pending Remove Component: 1 rigidbody, 2 collider, 3 camera, 4 light, 5 sprite, 6 audio
    dai_ui_popup menu_layout{};       // named layouts: save current, pick one
    dai_ui_popup menu_gizmos{};       // scene view toolbar: gizmo visibility
    dai_ui_popup menu_scecam{};       // scene view toolbar: camera settings
    char layout_name_buf[64] = { 0 };
    int  gizmo_grid = 1;            // the floor grid in the scene view
    int  gizmo_fps = 1;             // the frame counter in the corner
    float fps_now = 0.0f;           // pushed by the host, already smoothed
    int  gizmo_colliders = 1;
    int  gizmo_cameras = 1;
    int  floating_outside = 1;      // floating panels may leave the main window
    void (*layout_save_host)(const char *name, const char *text, size_t n, void *user) = nullptr;
    int  (*layout_load_host)(const char *name, char *out, size_t n, void *user) = nullptr;
    void *layout_host_user = nullptr;
    bool viewport_dragging = false;
    bool prev_viewport_down = false;
    bool prev_right_down = false;
    bool scrubbing = false;
};

// ---- asset paths: the list is flat, the browser is a tree -------------------
static std::string parent_of(const std::string &s) {
    size_t slash = s.find_last_of('/');
    return slash == std::string::npos ? std::string() : s.substr(0, slash);
}
static std::string base_of(const std::string &s) {
    size_t slash = s.find_last_of('/');
    return slash == std::string::npos ? s : s.substr(slash + 1);
}

// Scripts stack on a node like Unity components. The document field is one
// string, so the list travels ';'-separated - every old single-script file
// stays valid.
static std::vector<std::string> script_list(const char *s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char *c = s; c && *c; ++c) {
        if (*c == ';') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += *c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
static void script_join(char *out, size_t n, const std::vector<std::string> &list) {
    if (!n) return;
    out[0] = 0;
    for (const std::string &s : list) {
        if (out[0]) std::strncat(out, ";", n - std::strlen(out) - 1);
        std::strncat(out, s.c_str(), n - std::strlen(out) - 1);
    }
}
// What a file IS, by extension - so the browser reads at a glance.
static const char *icon_for_asset(const std::string &path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return DAI_ICON_FILE;
    std::string e = path.substr(dot + 1);
    for (char &c : e) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    // The SAME glyph the component header uses (dai_ui_header_icon_col with
    // DAI_ICON_C_SCRIPT). A file in the Project window and the component it
    // becomes when you drop it on an object are one thing; showing them as two
    // different pictures makes the reader do a translation that has no content.
    if (e == "js" || e == "ts")                       return DAI_ICON_C_SCRIPT;
    if (e == "cpp" || e == "cc" || e == "cxx" || e == "h" || e == "hpp") return DAI_ICON_C_SCRIPT;
    if (e == "glb" || e == "gltf" || e == "obj")      return DAI_ICON_MODEL;
    if (e == "wav" || e == "ogg" || e == "mp3" || e == "flac") return DAI_ICON_AUDIO;
    if (e == "png" || e == "jpg" || e == "jpeg" || e == "tga" || e == "svg") return DAI_ICON_IMAGE;
    if (e == "daidalos" || e == "prefab")             return DAI_ICON_C_PREFAB;
    if (e == "daimat")                                return DAI_ICON_MATERIAL;
    if (e == "daistr")                                return DAI_ICON_C_TEXT;
    if (e == "ttf" || e == "otf" || e == "ttc")       return DAI_ICON_C_TEXT;
    return DAI_ICON_FILE;
}

// The colour that goes with the icon above. The component headers in the
// inspector are colour coded already - a script header is gold, a camera is
// pale blue - and a file in the Project window is the same thing before it is
// attached to anything. Grey-on-grey rows make you read every name; colour
// lets the eye find the material among forty textures without reading at all.
static uint32_t icon_color_for_asset(const std::string &path, uint32_t fallback) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return fallback;
    std::string e = path.substr(dot + 1);
    for (char &c : e) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (e == "js" || e == "ts" || e == "cpp" || e == "cc" || e == "cxx" ||
        e == "h" || e == "hpp")                       return rgba(0xF2, 0xC1, 0x4E, 255);  // script gold
    if (e == "daimat")                                return rgba(0xC8, 0x8A, 0xE0, 255);  // material violet
    if (e == "daistr")                                return rgba(0x9C, 0xD6, 0x8C, 255);  // strings green
    if (e == "ttf" || e == "otf" || e == "ttc")       return rgba(0xD8, 0xC2, 0x7A, 255);  // font sand
    if (e == "daidalos" || e == "prefab")             return rgba(0x6C, 0xB2, 0xF0, 255);  // prefab blue
    if (e == "png" || e == "jpg" || e == "jpeg" || e == "tga" || e == "svg")
                                                      return rgba(0x6F, 0xCB, 0x9F, 255);  // texture green
    if (e == "glb" || e == "gltf" || e == "obj")      return rgba(0xE0, 0x93, 0x6C, 255);  // model amber
    if (e == "wav" || e == "ogg" || e == "mp3" || e == "flac")
                                                      return rgba(0xE8, 0x84, 0x9B, 255);  // audio rose
    return fallback;
}

// A file the engine can run on an object: QuickJS, or a native C++ behaviour
// (see dai_native.h). Both attach the same way and both are components.
// Anything an external editor can open, as opposed to something the scene
// places. A .cpp is both a behaviour and text; the check above wins.
// A .daimat. Asked here rather than through dai_material_is_file so this file
// keeps compiling without the material header - the editor UI has no business
// knowing what is IN a material, only which files are ones.
static bool is_scene_file(const std::string &path) {
    const std::string ext = ".daidalos";
    return path.size() > ext.size() &&
           path.compare(path.size() - ext.size(), ext.size(), ext) == 0;
}

static bool is_scene_asset(const std::string &path) {
    return is_scene_file(path) &&
           (path.compare(0, 7, "Scenes/") == 0 || path.compare(0, 7, "scenes/") == 0);
}

static bool is_material_file(const std::string &path) {
    const std::string ext = ".daimat";
    if (path.size() <= ext.size()) return false;
    std::string tail = path.substr(path.size() - ext.size());
    for (char &c : tail) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return tail == ext;
}

static bool is_text_file(const std::string &path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string e = path.substr(dot + 1);
    for (char &c : e) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return e == "txt" || e == "md" || e == "json" || e == "h" || e == "hpp" ||
           e == "glsl";
}

static bool is_behaviour_file(const std::string &path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string e = path.substr(dot + 1);
    for (char &c : e) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return e == "js" || e == "cpp" || e == "cc" || e == "cxx";
}

static bool script_name_ok(const std::string &s) {
    if (s.empty()) return false;
    if (s.find("..") != std::string::npos) return false;
    // Windows-illegal filename characters are the only real constraint; a
    // space is perfectly legal - rejecting it is what made renaming
    // "New Folder" into anything (or keeping its name) silently fail.
    for (char c : s)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|')
            return false;
    return true;
}
// Reparenting keeps the object exactly where it is on screen: the document
// stores LOCAL transforms, so the new local one is the old world transform
// expressed in the new parent's frame. Without this every drag in the
// hierarchy teleported the object by the parent's offset.
static dai_quat q_conj(dai_quat q) { return { -q.x, -q.y, -q.z, q.w }; }
static dai_quat q_mul(dai_quat a, dai_quat b) {
    return { a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
             a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
             a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
             a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z };
}
static dai_vec3 q_rot(dai_quat q, dai_vec3 v) {
    dai_vec3 u{ q.x, q.y, q.z };
    dai_vec3 uv{ u.y*v.z - u.z*v.y, u.z*v.x - u.x*v.z, u.x*v.y - u.y*v.x };
    dai_vec3 uuv{ u.y*uv.z - u.z*uv.y, u.z*uv.x - u.x*uv.z, u.x*uv.y - u.y*uv.x };
    return { v.x + 2.0f*(q.w*uv.x + uuv.x), v.y + 2.0f*(q.w*uv.y + uuv.y),
             v.z + 2.0f*(q.w*uv.z + uuv.z) };
}
// A node may not become a child of its own descendant - that builds a cycle
// the tree walk never returns from.
static bool is_descendant(dai_doc *d, dai_node maybe_child, dai_node of) {
    for (int guard = 0; guard < 256; ++guard) {
        dai_node_desc r{};
        if (dai_doc_get(d, maybe_child, &r) != DAI_OK) return false;
        if (r.parent == DAI_INVALID_NODE) return false;
        if (r.parent == of) return true;
        maybe_child = r.parent;
    }
    return true;   // pathological depth: refuse rather than loop
}
// Every node in the order the hierarchy DRAWS them: depth first, children
// under their parent, folded subtrees skipped. A shift-range means "the rows
// between these two", and rows are what is on screen - selecting by id order
// would grab objects that are nowhere near each other.
static void hierarchy_order(const dai_editor_ui *p, dai_doc *d, dai_node parent,
                            std::vector<dai_node> &out) {
    std::vector<dai_node> all((size_t)dai_doc_count(d));
    uint32_t n = all.empty() ? 0 : dai_doc_nodes(d, all.data(), (uint32_t)all.size());
    for (uint32_t i = 0; i < n; ++i) {
        dai_node_desc r{};
        if (dai_doc_get(d, all[i], &r) != DAI_OK) continue;
        if (r.parent != parent) continue;
        out.push_back(all[i]);
        if (p->folded.count(all[i])) continue;      // collapsed: its rows are not on screen
        hierarchy_order(p, d, all[i], out);
    }
}

static void select_range_nodes(dai_editor_ui *p, dai_node a, dai_node b, int keep) {
    dai_doc *d = dai_editor_doc(p->ed);
    std::vector<dai_node> rows;
    hierarchy_order(p, d, DAI_INVALID_NODE, rows);
    int ia = -1, ib = -1;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i] == a) ia = (int)i;
        if (rows[i] == b) ib = (int)i;
    }
    if (ia < 0 || ib < 0) { dai_editor_select(p->ed, b, keep); return; }
    if (ia > ib) { int t = ia; ia = ib; ib = t; }
    // Shift alone REPLACES the selection with the range - shift+ctrl adds to
    // it. That is the Windows rule, and the one muscle memory expects.
    if (!keep) dai_editor_deselect_all(p->ed);
    for (int i = ia; i <= ib; ++i) dai_editor_select(p->ed, rows[(size_t)i], 1);
}

// Dropping a dragged node re-parents THE WHOLE SELECTION when the dragged one
// is part of it. Selecting eight crates and dropping them into a group has to
// move eight crates; moving one and leaving seven behind is not a gesture
// anybody meant to make.
//
// A node whose ancestor is also selected is skipped: the ancestor carries it
// already, and re-parenting both would pull the child out of its parent and
// then move it twice.
static void reparent_dragged(dai_editor_ui *p, dai_node parent);
static void reparent_node(dai_editor_ui *p, dai_node child, dai_node parent);

// The files of the open folder, in the order the browser lists them. A shift
// range is "the rows between these two", and rows means what is on screen.
static std::vector<std::string> project_visible_files(const dai_editor_ui *p) {
    std::vector<std::string> out;
    for (const char *a : p->assets) {
        if (!a) continue;
        std::string full = a;
        if (parent_of(full) != p->proj_dir) continue;
        out.push_back(full);
    }
    std::sort(out.begin(), out.end());
    return out;
}

static void reparent_dragged(dai_editor_ui *p, dai_node parent) {
    dai_doc *d = dai_editor_doc(p->ed);
    if (!dai_editor_is_selected(p->ed, p->drag_node) ||
        dai_editor_selection_count(p->ed) <= 1) {
        reparent_node(p, p->drag_node, parent);
        return;
    }
    std::vector<dai_node> sel;
    for (uint32_t i = 0; i < dai_editor_selection_count(p->ed); ++i)
        sel.push_back(dai_editor_selected(p->ed, i));
    for (dai_node n : sel) {
        bool ancestor_in_set = false;
        for (dai_node other : sel)
            if (other != n && is_descendant(d, n, other)) { ancestor_in_set = true; break; }
        if (ancestor_in_set) continue;
        reparent_node(p, n, parent);
    }
}

static void reparent_node(dai_editor_ui *p, dai_node child, dai_node parent) {
    dai_doc *d = dai_editor_doc(p->ed);
    if (child == parent) return;
    dai_node_desc r{};
    if (dai_doc_get(d, child, &r) != DAI_OK) return;
    if (parent != DAI_INVALID_NODE && is_descendant(d, parent, child)) return;
    if (r.parent == parent) return;

    dai_vec3 cw{}, cs{ 1, 1, 1 }; dai_quat cr{ 0, 0, 0, 1 };
    dai_doc_world_transform(d, child, &cw, &cr, &cs);
    dai_doc_begin(d, "Reparent");
    r.parent = parent;
    if (parent == DAI_INVALID_NODE) {
        r.position = cw; r.rotation = cr; r.scale = cs;
    } else {
        dai_vec3 pw{}, ps{ 1, 1, 1 }; dai_quat pr{ 0, 0, 0, 1 };
        dai_doc_world_transform(d, parent, &pw, &pr, &ps);
        dai_quat inv = q_conj(pr);
        dai_vec3 delta{ cw.x - pw.x, cw.y - pw.y, cw.z - pw.z };
        dai_vec3 loc = q_rot(inv, delta);
        r.position = { ps.x != 0 ? loc.x / ps.x : loc.x,
                       ps.y != 0 ? loc.y / ps.y : loc.y,
                       ps.z != 0 ? loc.z / ps.z : loc.z };
        r.rotation = q_mul(inv, cr);
        r.scale = { ps.x != 0 ? cs.x / ps.x : cs.x,
                    ps.y != 0 ? cs.y / ps.y : cs.y,
                    ps.z != 0 ? cs.z / ps.z : cs.z };
    }
    dai_doc_set(d, child, &r);
    dai_doc_commit(d);
    dai_editor_resync(p->ed);
}

// The drop: a dragged .js lands on a node and becomes another component.
static void script_attach(dai_editor_ui *p, dai_node n, const std::string &path) {
    dai_doc *d = dai_editor_doc(p->ed);
    dai_node_desc r{};
    if (dai_doc_get(d, n, &r) != DAI_OK) return;
    std::vector<std::string> list = script_list(r.script);
    for (const std::string &s : list) if (s == path) return;   // already attached
    list.push_back(path);
    dai_doc_begin(d, "Add script");
    script_join(r.script, sizeof(r.script), list);
    dai_doc_set(d, n, &r);
    dai_doc_commit(d);
}

// An entry is "path.js" or "path.js{key=value,key=value}" - the brace block
// carries the references the inspector assigned.
static std::string entry_path(const std::string &e) {
    size_t b = e.find('{');
    return b == std::string::npos ? e : e.substr(0, b);
}
static std::string entry_param(const std::string &e, const std::string &key) {
    size_t b = e.find('{');
    if (b == std::string::npos) return std::string();
    size_t en = e.find('}', b);
    std::string inner = e.substr(b + 1, en == std::string::npos ? en : en - b - 1);
    size_t pos = 0;
    while (pos < inner.size()) {
        size_t comma = inner.find(',', pos);
        std::string kv = inner.substr(pos, comma == std::string::npos ? comma : comma - pos);
        size_t eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == key) return kv.substr(eq + 1);
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return std::string();
}
// One "// @param" declaration, as the host reports it: "float:speed=6".
// A missing type means node, which is what every declaration meant before
// types existed - so every script written until now keeps its fields.
enum ParamType { PARAM_NODE = 0, PARAM_FLOAT, PARAM_INT, PARAM_BOOL, PARAM_STRING,
                 PARAM_HEADER };
// `tip` is the description shown while the pointer rests on the row - Unity's
// [Tooltip]. A header is not a field: it is the [Header("...")] line above one,
// and it carries its text in `name`.
// `ntype` is the DECLARED TYPE of a node reference - Unity's rule, where a
// field spelled `Rigidbody target` offers only rigidbodies in its picker and
// refuses everything else on a drag. Empty means "any object", which is what
// every reference meant before types existed.
struct ParamDecl { std::string name, def, tip, ntype; int type = PARAM_NODE; };

// Which component a typed reference asks for, and what to call it on screen.
// Kept as one table so the label, the icon and the test can never drift apart.
struct NodeTypeInfo { const char *key, *label, *icon; };
static const NodeTypeInfo NODE_TYPES[] = {
    { "transform", "Transform", DAI_ICON_C_TRANSFORM },
    { "camera",    "Camera",    DAI_ICON_C_CAMERA },
    { "light",     "Light",     DAI_ICON_C_LIGHT },
    { "rigidbody", "Rigidbody", DAI_ICON_C_BODY },
    { "body",      "Rigidbody", DAI_ICON_C_BODY },
    { "collider",  "Collider",  DAI_ICON_C_COLLIDER },
    { "sprite",    "Sprite",    DAI_ICON_C_SPRITE },
    { "audio",     "Audio",     DAI_ICON_C_AUDIO },
    { "mesh",      "Mesh",      DAI_ICON_C_MESH },
};
static const NodeTypeInfo *node_type_of(const std::string &k) {
    if (k.empty()) return nullptr;
    for (const NodeTypeInfo &t : NODE_TYPES)
        if (k == t.key) return &t;
    return nullptr;
}
// Does this node carry what the field asks for? A Transform is every node -
// in Unity too, which is why `Transform` fields accept anything.
static bool node_has_type(const dai_node_desc &r, const std::string &k) {
    if (k.empty() || k == "object" || k == "node" || k == "transform") return true;
    if (k == "camera")    return r.camera != 0;
    if (k == "light")     return r.light != 0;
    if (k == "rigidbody" || k == "body") return !r.no_rigidbody && !r.no_body;
    if (k == "collider")  return !r.no_collider && !r.no_body;
    if (k == "sprite")    return r.sprite != 0;
    if (k == "audio")     return r.audio_event[0] != 0 || r.audio_autoplay || r.audio_bus;
    if (k == "mesh")      return 1;   /* every node draws something */
    return true;
}

static std::vector<ParamDecl> parse_params(const char *csv) {
    std::vector<ParamDecl> out;
    std::string cur;
    for (const char *c = csv ? csv : ""; ; ++c) {
        if (*c == ',' || !*c) {
            if (!cur.empty()) {
                ParamDecl d;
                std::string rest = cur;
                size_t colon = rest.find(':');
                if (colon != std::string::npos) {
                    std::string t = rest.substr(0, colon);
                    rest = rest.substr(colon + 1);
                    if (t == "float" || t == "number") d.type = PARAM_FLOAT;
                    else if (t == "int")               d.type = PARAM_INT;
                    else if (t == "bool")              d.type = PARAM_BOOL;
                    else if (t == "string" || t == "text") d.type = PARAM_STRING;
                    else if (t == "header")            d.type = PARAM_HEADER;
                    else {
                        d.type = PARAM_NODE;
                        // Anything else IS the reference's type. An unknown
                        // word behaves as "any object" rather than dropping
                        // the field: a typo must not make a slot disappear.
                        if (t != "node" && t != "object") d.ntype = t;
                    }
                }
                // "|" ends the declaration and begins its description.
                size_t bar = rest.find('|');
                if (bar != std::string::npos) {
                    d.tip = rest.substr(bar + 1);
                    rest = rest.substr(0, bar);
                }
                size_t eq = rest.find('=');
                if (eq != std::string::npos) { d.def = rest.substr(eq + 1); rest = rest.substr(0, eq); }
                d.name = rest;
                if (!d.name.empty()) out.push_back(d);
            }
            cur.clear();
            if (!*c) break;
        } else cur += *c;
    }
    return out;
}

static void entry_set_param(std::string &e, const std::string &key, const std::string &val) {
    std::string path = entry_path(e);
    std::string inner;
    size_t b = e.find('{');
    if (b != std::string::npos) {
        size_t en = e.find('}', b);
        inner = e.substr(b + 1, en == std::string::npos ? en : en - b - 1);
    }
    std::string out;
    size_t pos = 0;
    bool done = false;
    while (pos < inner.size()) {
        size_t comma = inner.find(',', pos);
        std::string kv = inner.substr(pos, comma == std::string::npos ? comma : comma - pos);
        size_t eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == key) { kv = key + "=" + val; done = true; }
        if (!kv.empty()) { if (!out.empty()) out += ","; out += kv; }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    if (!done) { if (!out.empty()) out += ","; out += key + "=" + val; }
    e = path + "{" + out + "}";
}

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

// Just the name. The tag used to be glued in front of it ("MainCamera
// Camera"), which reads as part of the name, is not editable there, and costs
// the column its whole job. The tag lives in the inspector, where it is a
// field; what the object IS is now the row's icon.
const char *node_label(const dai_node_desc &r, dai_node id, char *buf, size_t n) {
    if (r.name[0]) return r.name;
    if (r.tag[0])  return r.tag;
    std::snprintf(buf, n, "node %u", (unsigned)id);
    return buf;
}

// The icon that says what a node is, in the order a person would answer the
// question: a camera is a camera even when it also has a script.
const char *node_icon(const dai_node_desc &r) {
    if (r.camera)         return DAI_ICON_CAMERA;
    if (r.light)          return DAI_ICON_LIGHT;
    if (r.audio_event[0]) return DAI_ICON_VOLUME;
    if (r.sprite)         return DAI_ICON_SPRITE;
    if (r.asset[0])       return DAI_ICON_MODEL;
    if (r.no_body && r.no_collider && r.no_rigidbody && !r.asset[0])
        return r.script[0] ? DAI_ICON_SCRIPT : DAI_ICON_EMPTY;
    switch (r.shape) {
    case DAI_SHAPE_SPHERE:  return DAI_ICON_SPHERE;
    case DAI_SHAPE_CAPSULE: return DAI_ICON_CAPSULE;
    default:                return DAI_ICON_CUBE;
    }
}

bool has_children(dai_doc *d, dai_node n) {
    return dai_doc_children(d, n, nullptr, 0) > 0;
}

void draw_subtree(dai_editor_ui *p, dai_doc *d, dai_node n, int depth);

// One row, no children. What the filter shows: a match three levels down is a
// match, and opening its two parents to reach it is exactly what the search
// box exists to avoid.
void draw_subtree_row_only(dai_editor_ui *p, dai_doc *d, dai_node n) {
    p->hier_flat = 1;
    draw_subtree(p, d, n, 0);
    p->hier_flat = 0;
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
        // "Focus selection" has to work when the row is below the fold.
        if (p->reveal_row_wanted && dai_editor_is_selected(p->ed, n)) {
            float rvx = 0, rvy = 0;
            dai_ui_cursor_pos(p->ui, &rvx, &rvy);
            dai_ui_scroll_reveal(p->ui, rvy, 20.0f);
            p->reveal_row_wanted = 0;
        }
        // Unity's one visual rule for prefabs, and it is a good one: the NAME
        // is blue. Not a badge, not a second column - the thing you are
        // already reading tells you this object came from a file, so "why did
        // my change come back" is answered before it is asked.
        if (r.prefab[0]) dai_ui_tree_label_color(p->ui, rgba(0x6C, 0xB6, 0xF5, 255));
        int rc = dai_ui_tree_item_icon(p->ui, node_icon(r), label, depth, kids,
                                       kids ? &open : nullptr,
                                       dai_editor_is_selected(p->ed, n) ||
                                       (p->click_kind == 3 && p->click_node == n));
        if (rc & 4) {
            p->hover_node = n;   // a dragged script aims at this row
            // ...and while a node IS being dragged, say so ON the row. A
            // re-parent whose target only becomes visible after the button
            // comes up is a re-parent you undo half the time - and the
            // hierarchy is where people actually build the scene graph.
            if (p->drag_node != DAI_INVALID_NODE && p->drag_node != n) {
                const dai_ui_style *hs = dai_ui_style_of(p->ui);
                float lx = 0, ly = 0, lw = 0, lh = 0;
                dai_ui_last_rect(p->ui, &lx, &ly, &lw, &lh);
                dai_ui_rect(p->ui, lx, ly, lw, lh, (hs->accent & 0x00FFFFFFu) | 0x55000000u);
                dai_ui_rect_outline(p->ui, lx, ly, lw, lh, 1.0f, hs->accent);
                dai_ui_cursor_set(p->ui, DAI_CURSOR_HAND);
            }
            int lpe = 0;
            dai_ui_mouse(p->ui, nullptr, nullptr, nullptr, &lpe);
            if (lpe && p->drag_pending.empty()) {
                p->drag_node_pending = n;
                p->drag_ref_target = dai_editor_selection_count(p->ed) > 0
                                   ? dai_editor_selected(p->ed, 0) : DAI_INVALID_NODE;
                dai_ui_mouse(p->ui, &p->drag_nx, &p->drag_ny, nullptr, nullptr);
            }
        }
        if (rc & 1) {
            // Armed, not done: the selection changes when the button comes up
            // and nothing was dragged. Ctrl still ADDS, the way Unity
            // multi-selects; a plain click replaces.
            p->click_kind = 3;
            p->click_node = n;
            p->click_path.clear();
            p->click_dbl = 0;
            p->click_ctrl = p->last_ctrl_held ? 1 : 0;
            p->click_shift = p->last_shift_held ? 1 : 0;
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
    if (!p->hier_flat)
        for (dai_node k : kid_ids) draw_subtree(p, d, k, depth + 1);
}

} // namespace

extern "C" {

dai_editor_ui *dai_editor_ui_create(dai_editor *editor, dai_ui *ui) {
    if (!editor || !ui) return nullptr;
    dai_editor_ui *p = new dai_editor_ui();
    p->ed = editor;
    p->ui = ui;
    p->dock = dai_dock_create();
    // Scene and Game are two views of ONE rendered world, and the host draws
    // that world once per frame into a single rectangle - the two tabs stay
    // in one leaf no matter where the user drags them.
    // Scene and Game were once one viewport wearing two hats - the lock kept
    // them tabbed together because they could never show different things.
    // With two real views the lock is the bug: dragging one dragged both.
    // (dai_dock_lock_pair deliberately NOT called anymore.)
    return p;
}

void dai_editor_ui_destroy(dai_editor_ui *p) { if (p) dai_dock_destroy(p->dock);
    delete p; }

int dai_editor_ui_menu_open(const dai_editor_ui *p) {
    return p ? (p->menu_node.open || p->menu_canvas.open || p->menu_project.open ||
                dai_ui_popup_active(p->ui)) : 0;
}

void dai_editor_ui_log(dai_editor_ui *p, int level, const char *text) {
    if (!p || !text) return;
    if (level < 0) level = 0;
    if (level > 2) level = 2;
    // Collapse repeats the way every console does: a script erroring once per
    // frame must not push everything else out of the buffer.
    if (!p->log.empty() && p->log.back().level == level && p->log.back().text == text) {
        p->log.back().count++;
        return;
    }
    p->log.push_back({ level, text, 1 });
    if (p->log.size() > 2000) p->log.erase(p->log.begin(), p->log.begin() + 500);
}

uint32_t dai_editor_ui_log_tail(const dai_editor_ui *p, char *buf, uint32_t buf_size) {
    if (!buf || buf_size < 2) return 0;
    buf[0] = 0;
    if (!p) return 0;
    // Walk BACKWARDS to find how many of the newest lines fit, then write them
    // in order. Nothing is allocated: this runs inside a crash handler, where
    // the heap is exactly the thing that might be broken.
    uint32_t need = 0;
    size_t first = p->log.size();
    while (first > 0) {
        const auto &l = p->log[first - 1];
        uint32_t line = (uint32_t)l.text.size() + 1;
        if (need + line >= buf_size) break;
        need += line;
        --first;
    }
    uint32_t used = 0;
    for (size_t i = first; i < p->log.size(); ++i) {
        const std::string &t = p->log[i].text;
        uint32_t n = (uint32_t)t.size();
        if (used + n + 2 >= buf_size) break;
        std::memcpy(buf + used, t.data(), n);
        used += n;
        buf[used++] = '\n';
    }
    buf[used] = 0;
    return used;
}

void dai_editor_ui_log_clear(dai_editor_ui *p) { if (p) p->log.clear(); }

float dai_editor_ui_bus_gain(const dai_editor_ui *p, int bus) {
    if (!p || bus < 0 || bus > 3) return 1.0f;
    return p->bus_mute[bus] ? 0.0f : p->bus_gain[bus];
}

void dai_editor_ui_clipboard_set(dai_editor_ui *p, int kind, const char *text) {
    if (!p) return;
    p->clip_kind = kind;
    ++p->clip_rev;
    std::snprintf(p->clipboard, sizeof(p->clipboard), "%s", text ? text : "");
}
unsigned dai_editor_ui_clipboard_rev(const dai_editor_ui *p) { return p ? p->clip_rev : 0; }
const char *dai_editor_ui_clipboard_get(const dai_editor_ui *p, int *kind) {
    if (!p || !p->clipboard[0]) { if (kind) *kind = -1; return nullptr; }
    if (kind) *kind = p->clip_kind;
    return p->clipboard;
}
int dai_editor_ui_clipboard_has(const dai_editor_ui *p) { return p && p->clipboard[0]; }

// ---------------------------------------------------------------- the HUD
//
// The game's own text, drawn on the picture. Two decisions worth writing down:
//
// ANCHORS, NOT POSITIONS. A score pinned to x=1720 is off screen the moment
// the window is 1280 wide, and a Game view is never the size of the finished
// window. So a label says "top right, 12 px in" and survives every size.
//
// IT IS CLIPPED TO THE VIEW. In the editor the Game view is a panel among
// panels; a HUD that spilled past its edge would draw over the Hierarchy.
// Where a HUD gets its pictures. A global rather than a parameter because
// both hosts set it once at startup and neither ever changes it, and threading
// it through dai_hud_draw would put it in the signature of a function whose
// whole point is that the editor and the runtime call it identically.
// What the last draw laid out, so the editor can put a handle on it. A frame
// of latency by construction - the rectangle is from the frame before the one
// being built - and that is fine: it moves when the thing moves.
// `kind` because a node can have BOTH an Image and a Text, and they are two
// rectangles in two places. Without it the editor found whichever came first
// and the other one could not be selected or resized at all.
enum { HUD_KIND_IMAGE = 0, HUD_KIND_TEXT = 1 };
// `editable` marks the copy drawn over the SCENE view. The Game view is what
// the player sees - resize grips have no business there, and they showed up
// because both draws land in the same list and nothing told them apart.
struct HudRect { dai_node n; int kind; int editable; float x, y, w, h; };
static std::vector<HudRect> g_hud_rects;
static std::vector<HudRect> g_hud_rects_prev;
// The HUD is drawn more than once per frame now - once over the Scene view so
// it can be edited where the rest of the scene is edited, once into the Game
// view because that is what the player sees. Both have to end up in the SAME
// list, or the second call throws the first one's rectangles away and half the
// UI stops being clickable depending on which panel is open.
static bool g_hud_frame_open = false;
// Set by the host around the draw that is being EDITED (the Scene view's).
static int g_hud_editable = 0;
void dai_hud_editable(int on) { g_hud_editable = on ? 1 : 0; }

void dai_hud_frame(void) { g_hud_frame_open = false; }

// What is under the pointer, topmost first: a later node draws over an earlier
// one, so the search runs backwards. Uses the PREVIOUS frame's rectangles -
// the current frame's are still being built when input is read.
int dai_hud_pick(float mx, float my, dai_node *out) {
    for (size_t i = g_hud_rects_prev.size(); i-- > 0; ) {
        const HudRect &r = g_hud_rects_prev[i];
        if (!r.editable) continue;
        if (r.w <= 0.0f || r.h <= 0.0f) continue;
        if (mx < r.x - 2.0f || mx >= r.x + r.w + 2.0f) continue;
        if (my < r.y - 2.0f || my >= r.y + r.h + 2.0f) continue;
        if (out) *out = r.n;
        return 1;
    }
    return 0;
}

// Every rectangle a node has, oldest first. `index` 0 is its Image, 1 its
// Text - or 0 is the Text when there is no Image. Returns 0 past the end, so
// a caller loops until it stops.
int dai_hud_rect_nth(dai_node n, int index, int *kind,
                     float *x, float *y, float *w, float *h) {
    int seen = 0;
    for (const HudRect &r : g_hud_rects_prev) {
        if (r.n != n || !r.editable) continue;
        if (seen++ != index) continue;
        if (kind) *kind = r.kind;
        if (x) *x = r.x;
        if (y) *y = r.y;
        if (w) *w = r.w;
        if (h) *h = r.h;
        return 1;
    }
    return 0;
}

int dai_hud_rect_of(dai_node n, float *x, float *y, float *w, float *h) {
    for (const HudRect &r : g_hud_rects_prev) {
        if (r.n != n || !r.editable) continue;
        if (x) *x = r.x;
        if (y) *y = r.y;
        if (w) *w = r.w;
        if (h) *h = r.h;
        return 1;
    }
    return 0;
}

// ---- buttons ---------------------------------------------------------
// Only ON while the game view is being drawn during Play. The same HUD is
// drawn a second time over the Scene view for editing, and a button that
// answered the pointer there would fire every time you tried to move it.
static int   g_hud_interactive = 0;
static dai_node g_hud_held = DAI_INVALID_NODE;
static std::vector<dai_node> g_hud_clicks;

void dai_hud_interactive(int on) { g_hud_interactive = on ? 1 : 0; }

uint32_t dai_hud_take_clicks(dai_node *out, uint32_t max) {
    uint32_t n = (uint32_t)g_hud_clicks.size();
    if (n > max) n = max;
    if (out) for (uint32_t i = 0; i < n; ++i) out[i] = g_hud_clicks[i];
    g_hud_clicks.clear();
    return n;
}

static uint32_t hud_rgb_of(const dai_vec3 &v) {
    auto ch = [](float x) -> uint32_t {
        float c = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
        return (uint32_t)(c * 255.0f + 0.5f);
    };
    return 0xFF000000u | (ch(v.z) << 16) | (ch(v.y) << 8) | ch(v.x);
}
static uint32_t hud_tint_mul(uint32_t c, float f) {
    auto ch = [&](int sh) -> uint32_t {
        float v = (float)((c >> sh) & 0xFFu) * f;
        if (v > 255.0f) v = 255.0f;
        if (v < 0.0f) v = 0.0f;
        return (uint32_t)(v + 0.5f) << sh;
    };
    return (c & 0xFF000000u) | ch(0) | ch(8) | ch(16);
}
// 0 idle, 1 hovered, 2 held. Registers the click on RELEASE INSIDE - letting
// go somewhere else is a cancelled click, and that is not a detail: it is the
// only way out once a button has been pressed by accident.
static int hud_button_state(dai_ui *ui, dai_node id,
                            float bx, float by, float bw, float bh) {
    if (!g_hud_interactive) return 0;
    float mx = 0.0f, my = 0.0f;
    int down = 0, press = 0;
    dai_ui_mouse(ui, &mx, &my, &down, &press);
    bool over = mx >= bx && mx < bx + bw && my >= by && my < by + bh;
    if (press && over) g_hud_held = id;
    if (!down && g_hud_held == id) {
        if (over) g_hud_clicks.push_back(id);
        g_hud_held = DAI_INVALID_NODE;
    }
    if (!over) return 0;
    return (down && g_hud_held == id) ? 2 : 1;
}
// The tint a button wants, given the one it would have had. Without colours of
// its own it brightens under the pointer and darkens while held - what every
// toolkit does, and what nobody has to be told.
static uint32_t hud_button_tint(const dai_node_desc &r, int state, uint32_t base) {
    if (state == 1) {
        if (r.button_hover.x || r.button_hover.y || r.button_hover.z)
            return hud_rgb_of(r.button_hover);
        return hud_tint_mul(base, 1.18f);
    }
    if (state == 2) {
        if (r.button_press.x || r.button_press.y || r.button_press.z)
            return hud_rgb_of(r.button_press);
        return hud_tint_mul(base, 0.78f);
    }
    return base;
}

// ---- {variables} in a label -----------------------------------------
// "Points: {score}" is the thing every HUD wants and the thing every engine
// makes you write a script for. The host resolves the name against the
// script running on THAT node; anything it does not know is left standing,
// so in the editor you see the placeholder where the number will be.
static dai_hud_var_fn g_hud_var = nullptr;
static void          *g_hud_var_user = nullptr;
void dai_hud_vars(dai_hud_var_fn fn, void *user) { g_hud_var = fn; g_hud_var_user = user; }

static void hud_expand_vars(dai_node n, const char *src, std::string &out) {
    out.clear();
    for (size_t i = 0; src[i]; ) {
        if (src[i] != '{') { out += src[i++]; continue; }
        size_t j = i + 1;
        while (src[j] && src[j] != '}' && src[j] != '{' && j - i < 64) ++j;
        if (src[j] != '}' || j == i + 1) { out += src[i++]; continue; }
        std::string name(src + i + 1, src + j);
        char val[128] = { 0 };
        if (g_hud_var && g_hud_var(n, name.c_str(), val, sizeof(val), g_hud_var_user)) {
            out += val;
        } else {
            out.append(src + i, src + j + 1);   // unknown: leave it visible
        }
        i = j + 1;
    }
}

static dai_hud_image_fn g_hud_image = nullptr;
static void            *g_hud_image_user = nullptr;
void dai_hud_images(dai_hud_image_fn fn, void *user) {
    g_hud_image = fn; g_hud_image_user = user;
}

// ---- rich text -------------------------------------------------------
// The spelling people already type, Unity's: <b>, <u>, <s>, <color=#RRGGBB>
// and <br>. Anything that is not one of those is left EXACTLY as it was
// typed - "<3" is a heart, "a < b" is a comparison, and a parser that eats
// them is a parser people switch off.
//
// There is no <i>. Slanting a glyph needs a slanted glyph and the atlas holds
// one shape per character; the honest answer is an italic .ttf in the Font
// field. <i> is accepted and does nothing rather than silently printing
// "<i>" in the middle of a sentence.
enum { HUD_B = 1, HUD_U = 2, HUD_S = 4 };
struct HudStyle { uint8_t f; uint32_t col; };   // col 0 = the label's own

// #RGB, #RRGGBB, #RRGGBBAA or one of the names everybody tries first.
// 0 means "not a colour" - the label's own is kept.
static uint32_t hud_parse_col(const char *v) {
    while (*v == ' ' || *v == '"' || *v == '\'') ++v;
    if (*v == '#') {
        ++v;
        uint32_t val = 0;
        int n = 0;
        for (; n < 8; ++n) {
            char c = v[n];
            int d;
            if (c >= '0' && c <= '9')      d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            val = (val << 4) | (uint32_t)d;
        }
        uint32_t r8 = 0, g8 = 0, b8 = 0, a8 = 255;
        if (n == 3) {
            r8 = ((val >> 8) & 0xF) * 17; g8 = ((val >> 4) & 0xF) * 17; b8 = (val & 0xF) * 17;
        } else if (n == 6) {
            r8 = (val >> 16) & 0xFF; g8 = (val >> 8) & 0xFF; b8 = val & 0xFF;
        } else if (n == 8) {
            r8 = (val >> 24) & 0xFF; g8 = (val >> 16) & 0xFF; b8 = (val >> 8) & 0xFF; a8 = val & 0xFF;
        } else {
            return 0;
        }
        return (a8 << 24) | (b8 << 16) | (g8 << 8) | r8;
    }
    struct Named { const char *name; uint32_t argb; };
    static const Named NAMES[] = {
        { "red", 0xFF0000FFu }, { "green", 0xFF00FF00u }, { "blue", 0xFFFF0000u },
        { "white", 0xFFFFFFFFu }, { "black", 0xFF000000u }, { "yellow", 0xFF00FFFFu },
        { "cyan", 0xFFFFFF00u }, { "magenta", 0xFFFF00FFu }, { "orange", 0xFF00A5FFu },
        { "grey", 0xFF808080u }, { "gray", 0xFF808080u },
    };
    for (const Named &nm : NAMES) {
        size_t l = std::strlen(nm.name);
        if (std::strncmp(v, nm.name, l) == 0 && (v[l] == 0 || v[l] == ' ' || v[l] == '"'))
            return nm.argb;
    }
    return 0;
}

// The words with the tags taken out, plus one style per remaining character.
// Per CHARACTER and not per run, because the wrap happens afterwards and a
// run that a line break falls inside has to survive being cut in two.
static void hud_rich(const char *src, std::string &plain, std::vector<HudStyle> &out) {
    plain.clear();
    out.clear();
    uint8_t f = 0;
    uint32_t col = 0;
    std::vector<uint32_t> stack;
    for (size_t i = 0; src[i]; ) {
        if (src[i] == '<') {
            size_t j = i + 1;
            while (src[j] && src[j] != '>' && j - i < 48) ++j;
            if (src[j] == '>') {
                std::string low;
                for (size_t q = i + 1; q < j; ++q) {
                    char c = src[q];
                    low += (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
                }
                bool known = true;
                if      (low == "b")  f |= HUD_B;
                else if (low == "/b") f = (uint8_t)(f & ~HUD_B);
                else if (low == "u")  f |= HUD_U;
                else if (low == "/u") f = (uint8_t)(f & ~HUD_U);
                else if (low == "s" || low == "strike")   f |= HUD_S;
                else if (low == "/s" || low == "/strike") f = (uint8_t)(f & ~HUD_S);
                else if (low == "i" || low == "/i" || low == "em" || low == "/em") { }
                else if (low == "br" || low == "br/" || low == "/br") {
                    plain += '\n';
                    out.push_back(HudStyle{ f, col });
                } else if (low.compare(0, 6, "color=") == 0) {
                    uint32_t c = hud_parse_col(low.c_str() + 6);
                    if (c) { stack.push_back(col); col = c; }
                    else known = false;
                } else if (low == "/color") {
                    if (!stack.empty()) { col = stack.back(); stack.pop_back(); }
                    else col = 0;
                } else {
                    known = false;
                }
                if (known) { i = j + 1; continue; }
            }
        }
        plain += src[i];
        out.push_back(HudStyle{ f, col });
        ++i;
    }
}

// Breaks `text` into lines that fit `maxw` at scale `k`. maxw <= 0 means "do
// not wrap" - a score is one line however long the number gets.
static void hud_wrap(dai_ui *ui, const std::string &text, float maxw, float k,
                     std::vector<std::string> &out) {
    out.clear();
    std::string para;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            if (maxw <= 0.0f) { out.push_back(para); para.clear(); continue; }
            // Greedy wrap at spaces; a word longer than the box is cut rather
            // than allowed to stick out, because sticking out is what a box
            // exists to prevent.
            std::string line;
            size_t pos = 0;
            while (pos <= para.size()) {
                size_t sp = para.find(' ', pos);
                std::string word = para.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
                std::string cand = line.empty() ? word : line + " " + word;
                if (dai_ui_text_width(ui, cand.c_str()) * k <= maxw || line.empty()) {
                    line = cand;
                } else {
                    out.push_back(line);
                    line = word;
                }
                if (sp == std::string::npos) break;
                pos = sp + 1;
            }
            out.push_back(line);
            para.clear();
            continue;
        }
        para += text[i];
    }
}

void dai_hud_draw(dai_ui *ui, dai_doc *doc, float x, float y, float w, float h,
                  float scale, dai_hud_resolve_fn resolve, void *user) {
    if (!ui || !doc || w <= 0.0f || h <= 0.0f) return;
    if (!(scale > 0.0f)) scale = 1.0f;

    std::vector<dai_node> ids(dai_doc_count(doc));
    uint32_t n = ids.empty() ? 0 : dai_doc_nodes(doc, ids.data(), (uint32_t)ids.size());
    if (!n) return;

    if (!g_hud_frame_open) {
        g_hud_rects_prev.swap(g_hud_rects);
        g_hud_rects.clear();
        g_hud_frame_open = true;
    }

    dai_ui_clip_begin(ui, x, y, w, h);
    for (uint32_t i = 0; i < n; ++i) {
        dai_node_desc r{};
        if (dai_doc_get(doc, ids[i], &r) != DAI_OK) continue;
        // Worked out at the Image and reused by the Text, so a button with a
        // background AND a label reacts as ONE thing: both tint together, and
        // the rectangle that answers the pointer is the background's.
        int btn = 0;
        // `disabled` is the OBJECT: off means off, label included.
        // ---- Image ---------------------------------------------------------
        // Drawn before the text of the same frame, so a label on a panel is a
        // label on a panel and not behind it. Both live on their own node in
        // practice; when they share one, the picture is the background.
        if (r.image_on && !r.disabled && r.image[0] && g_hud_image) {
            float iw = 0.0f, ih = 0.0f;
            uint32_t tex = g_hud_image(r.image, &iw, &ih, g_hud_image_user);
            {
                float dw = r.image_w > 0.0f ? r.image_w : (iw > 0.0f ? iw : 64.0f);
                float dh = r.image_h > 0.0f ? r.image_h : (ih > 0.0f ? ih : 64.0f);
                dw *= scale; dh *= scale;
                int ia = r.image_anchor < 0 ? 0 : (r.image_anchor > 8 ? 8 : r.image_anchor);
                const float IPAD = 8.0f * scale;
                float icol = (float)(ia % 3), irow = (float)(ia / 3);
                float ix = x + IPAD + (w - 2.0f * IPAD - dw) * (icol * 0.5f) + r.image_x * scale;
                float iy = y + IPAD + (h - 2.0f * IPAD - dh) * (irow * 0.5f) + r.image_y * scale;
                uint32_t tint = 0xFFFFFFFFu;
                if (r.image_color.x != 0.0f || r.image_color.y != 0.0f || r.image_color.z != 0.0f) {
                    auto ch = [](float v) -> uint32_t {
                        float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                        return (uint32_t)(c * 255.0f + 0.5f);
                    };
                    tint = 0xFF000000u | (ch(r.image_color.z) << 16) |
                           (ch(r.image_color.y) << 8) | ch(r.image_color.x);
                }
                if (r.button_on && !r.disabled) {
                    btn = hud_button_state(ui, ids[i], ix, iy, dw, dh);
                    tint = hud_button_tint(r, btn, tint);
                }
                if (tex) {
                    dai_ui_image_at(ui, tex, ix, iy, dw, dh, 0, 0, 1, 1, tint);
                } else {
                    // The file is named and it did not load. Before this the
                    // component drew NOTHING - no picture, no rectangle, no
                    // message - so "I see no image in the UI" was the only
                    // symptom there was, and it could not even be selected to
                    // ask what was wrong. Now it shows where it would be and
                    // says what it wanted.
                    dai_ui_rect(ui, ix, iy, dw, dh, 0x30FFFFFFu);
                    dai_ui_rect_outline(ui, ix, iy, dw, dh, 1.0f, 0xFF3D84D8u);
                    dai_ui_line(ui, ix, iy, ix + dw, iy + dh, 1.0f, 0x803D84D8u);
                    dai_ui_line(ui, ix + dw, iy, ix, iy + dh, 1.0f, 0x803D84D8u);
                    const char *bn = r.image;
                    for (const char *q = r.image; *q; ++q)
                        if (*q == '/' || *q == '\\') bn = q + 1;
                    dai_ui_text(ui, ix + 4.0f, iy + 3.0f, "image not loaded", 0xFFFFFFFFu);
                    (void)bn;
                    dai_ui_text(ui, ix + 4.0f, iy + 3.0f + dai_ui_text_height(ui) + 2.0f,
                                bn, 0xFFC8C8C8u);
                }
                // Registered either way: an element you cannot select is an
                // element you cannot fix.
                g_hud_rects.push_back(HudRect{ ids[i], HUD_KIND_IMAGE, g_hud_editable, ix, iy, dw, dh });
            }
        }

        if (!r.text_on || r.disabled) continue;
        // `hidden` deliberately does NOT hide the label. It is the MESH
        // renderer's checkbox, and every node here draws a box unless it is
        // ticked - so a label-only object is a hidden one BY CONSTRUCTION.
        // Honouring `hidden` here made a HUD node a choice between "shows a
        // grey box in the middle of the game" and "shows nothing", which is
        // no choice at all. The Text component is its own renderer; the two
        // switches are about two different things.

        char shown[256];
        const char *txt = r.text;
        if (resolve) {
            const char *out = resolve(r.text, user);
            if (out) { std::snprintf(shown, sizeof(shown), "%s", out); txt = shown; }
        }
        if (!txt || !txt[0]) continue;

        // {score} -> what the script says. Before the markup, so a value can
        // carry its own colour tag, and before the wrap, because a number is
        // not the same width as its name.
        std::string expanded;
        if (std::strchr(txt, '{')) {
            hud_expand_vars(ids[i], txt, expanded);
            txt = expanded.c_str();
        }

        // Tags out, styles kept. Everything below works on the WORDS - the
        // wrap, the widths, the autosize - because "<b>" is not two
        // characters wide on screen and a layout that measures it is wrong by
        // exactly the length of the markup.
        std::string plain;
        std::vector<HudStyle> rich;
        hud_rich(txt, plain, rich);
        if (plain.empty()) continue;

        float px = r.text_size > 0.0f ? r.text_size : 24.0f;
        // The atlas is one size; a label asks for another. dai_ui_text_scaled
        // draws at the ratio, which is what makes a 48 px title possible
        // without a second font.
        float k = px / (dai_ui_text_height(ui) > 0.0f ? dai_ui_text_height(ui) : 13.0f);

        // The box, if there is one. Without it the label is as wide as its
        // words; with it the text wraps, and with autosize it also shrinks
        // until it fits - which is the only honest answer to "the German
        // translation is a third longer and the box did not grow".
        float boxw = r.text_w * scale, boxh = r.text_h * scale;
        std::vector<std::string> lines;
        float lh = 0.0f, block_h = 0.0f, widest = 0.0f;
        for (int attempt = 0; attempt < 40; ++attempt) {
            hud_wrap(ui, plain, boxw > 0.0f ? boxw : 0.0f, k, lines);
            lh = dai_ui_text_height(ui) * k * 1.25f;
            block_h = lh * (float)lines.size();
            widest = 0.0f;
            for (const std::string &l : lines) {
                float lw = dai_ui_text_width(ui, l.c_str()) * k;
                if (lw > widest) widest = lw;
            }
            if (!r.text_autosize || boxh <= 0.0f) break;
            if (block_h <= boxh && (boxw <= 0.0f || widest <= boxw)) break;
            // 4% a step: enough to converge in a few dozen tries, small enough
            // that the result is not visibly quantised.
            k *= 0.96f;
            if (k * dai_ui_text_height(ui) < 6.0f) break;   // a floor: unreadable is not a fit
        }
        // Inside a box the block is laid out against the BOX, not the words -
        // otherwise a centred paragraph re-centres itself every time a word
        // changes length.
        if (boxw > 0.0f) widest = boxw;
        if (boxh > 0.0f && block_h < boxh) block_h = boxh;

        // 0..8 in reading order: column is anchor%3, row is anchor/3.
        int a = r.text_anchor < 0 ? 0 : (r.text_anchor > 8 ? 8 : r.text_anchor);
        const float PAD = 8.0f * scale;
        float col = (float)(a % 3), row = (float)(a / 3);
        float bx = x + PAD + (w - 2.0f * PAD - widest) * (col * 0.5f);
        float by = y + PAD + (h - 2.0f * PAD - block_h) * (row * 0.5f);
        bx += r.text_x * scale;
        by += r.text_y * scale;

        // 0,0,0 means "no colour chosen" here as everywhere else in the
        // document, and for text the useful default is white.
        uint32_t col32;
        if (r.text_color.x == 0.0f && r.text_color.y == 0.0f && r.text_color.z == 0.0f)
            col32 = 0xFFFFFFFFu;
        else {
            auto ch = [](float v) -> uint32_t {
                float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                return (uint32_t)(c * 255.0f + 0.5f);
            };
            col32 = 0xFF000000u | (ch(r.text_color.z) << 16) | (ch(r.text_color.y) << 8) | ch(r.text_color.x);
        }

        g_hud_rects.push_back(HudRect{ ids[i], HUD_KIND_TEXT, g_hud_editable, bx, by, widest, block_h });
        // A label with no background is still a button; the words ARE the
        // rectangle. Only worked out here when the Image did not already do it.
        if (r.button_on && !r.disabled) {
            if (!r.image_on || !r.image[0])
                btn = hud_button_state(ui, ids[i], bx, by, widest, block_h);
            col32 = hud_button_tint(r, btn, col32);
        }
        // Where each wrapped line sits in the unwrapped words, so a line
        // knows which styles are its own. Searched forward rather than
        // counted: the wrap drops the space it broke at, and counting would
        // drift by one character per line for the rest of the paragraph.
        size_t cur = 0;
        for (size_t li = 0; li < lines.size(); ++li) {
            const std::string &ln = lines[li];
            size_t at = plain.find(ln, cur);
            if (at == std::string::npos) at = cur;
            cur = at + ln.size();

            // Centre and right anchors centre EACH line inside the block, so
            // a two line centred title looks centred rather than ragged.
            float lw = dai_ui_text_width(ui, ln.c_str()) * k;
            // The block's own alignment wins over the anchor's. 0 keeps the
            // old behaviour: lines follow whichever corner the block is
            // pinned to, which is right until it is not.
            float af = (r.text_align == 1) ? 0.0f
                     : (r.text_align == 2) ? 0.5f
                     : (r.text_align == 3) ? 1.0f : (col * 0.5f);
            float lx = bx + (widest - lw) * af;
            float ly = by + lh * (float)li;
            float th = dai_ui_text_height(ui) * k;
            float rule = th * 0.075f < 1.0f ? 1.0f : th * 0.075f;

            // Runs of one style. Plain text is ONE run, so the common case is
            // the same two draws it always was.
            size_t s0 = 0;
            float rx = lx;
            while (s0 < ln.size()) {
                HudStyle sy = at + s0 < rich.size() ? rich[at + s0] : HudStyle{ 0, 0 };
                size_t s1 = s0 + 1;
                while (s1 < ln.size()) {
                    HudStyle s2 = at + s1 < rich.size() ? rich[at + s1] : HudStyle{ 0, 0 };
                    if (s2.f != sy.f || s2.col != sy.col) break;
                    ++s1;
                }
                std::string run = ln.substr(s0, s1 - s0);
                float rw = dai_ui_text_width(ui, run.c_str()) * k;
                uint32_t rc = sy.col ? sy.col : col32;
                // A one pixel shadow. Not decoration: white text on a bright
                // sky is unreadable, and every game HUD in existence does this.
                dai_ui_text_scaled(ui, rx + 1.0f * scale, ly + 1.0f * scale, run.c_str(),
                                   0xB0000000u, k);
                dai_ui_text_scaled(ui, rx, ly, run.c_str(), rc, k);
                // Bold out of one face: the same run again, a hair to the
                // right. Not a real bold cut and it does not pretend to be
                // one - it is what a single atlas can honestly do.
                if (sy.f & HUD_B) dai_ui_text_scaled(ui, rx + 0.9f, ly, run.c_str(), rc, k);
                if (sy.f & HUD_U) dai_ui_rect(ui, rx, ly + th * 0.98f, rw, rule, rc);
                if (sy.f & HUD_S) dai_ui_rect(ui, rx, ly + th * 0.55f, rw, rule, rc);
                rx += rw;
                s0 = s1;
            }
        }
    }
    dai_ui_clip_end(ui);
}

void dai_editor_ui_tr_host(dai_editor_ui *p, dai_hud_resolve_fn fn, void *user) {
    if (!p) return;
    p->tr_fn = fn; p->tr_user = user;
}

void dai_editor_ui_loc_host(dai_editor_ui *p, dai_editor_ui_loc_fn load,
                            dai_editor_ui_loc_fn save, void *user) {
    if (!p) return;
    p->loc_load = load; p->loc_save = save; p->loc_user = user;
}
void dai_editor_ui_loc_begin(dai_editor_ui *p) {
    if (!p) return;
    p->loc_langs.clear(); p->loc_names.clear(); p->loc_rows.clear();
    p->loc_dirty = 0;
}
void dai_editor_ui_loc_lang(dai_editor_ui *p, const char *code, const char *name) {
    if (!p || !code || !code[0]) return;
    for (const std::string &l : p->loc_langs) if (l == code) return;
    p->loc_langs.push_back(code);
    p->loc_names.push_back(name && name[0] ? name : code);
    for (auto &e : p->loc_rows) e.vals.resize(p->loc_langs.size());
}
void dai_editor_ui_loc_set(dai_editor_ui *p, const char *key, const char *lang,
                           const char *text) {
    if (!p || !key || !key[0] || !lang) return;
    int col = -1;
    for (size_t i = 0; i < p->loc_langs.size(); ++i) if (p->loc_langs[i] == lang) col = (int)i;
    if (col < 0) return;
    for (auto &e : p->loc_rows)
        if (e.key == key) { e.vals.resize(p->loc_langs.size()); e.vals[(size_t)col] = text ? text : ""; return; }
    dai_editor_ui::LocEntry e;
    e.key = key;
    e.vals.assign(p->loc_langs.size(), std::string());
    e.vals[(size_t)col] = text ? text : "";
    p->loc_rows.push_back(e);
    std::sort(p->loc_rows.begin(), p->loc_rows.end(),
              [](const dai_editor_ui::LocEntry &a, const dai_editor_ui::LocEntry &b) {
                  return a.key < b.key;
              });
}
uint32_t dai_editor_ui_loc_lang_count(const dai_editor_ui *p) {
    return p ? (uint32_t)p->loc_langs.size() : 0;
}
const char *dai_editor_ui_loc_lang_at(const dai_editor_ui *p, uint32_t i, const char **out_name) {
    if (!p || i >= p->loc_langs.size()) return nullptr;
    if (out_name) *out_name = p->loc_names[i].c_str();
    return p->loc_langs[i].c_str();
}
uint32_t dai_editor_ui_loc_key_count(const dai_editor_ui *p) {
    return p ? (uint32_t)p->loc_rows.size() : 0;
}
const char *dai_editor_ui_loc_key_at(const dai_editor_ui *p, uint32_t i) {
    return (p && i < p->loc_rows.size()) ? p->loc_rows[i].key.c_str() : nullptr;
}
const char *dai_editor_ui_loc_get(const dai_editor_ui *p, uint32_t key_i, uint32_t lang_i) {
    if (!p || key_i >= p->loc_rows.size()) return "";
    const auto &v = p->loc_rows[key_i].vals;
    return lang_i < v.size() ? v[lang_i].c_str() : "";
}

void dai_editor_ui_toast(dai_editor_ui *p, const char *text, float seconds) {
    if (!p) return;
    std::snprintf(p->toast, sizeof(p->toast), "%s", text ? text : "");
    p->toast_left = seconds > 0.0f ? seconds : 2.0f;
}

void dai_editor_ui_scene_dirty(dai_editor_ui *p, int dirty) {
    if (p) p->scene_dirty = dirty ? 1 : 0;
}
int dai_editor_ui_scene_dirty_get(const dai_editor_ui *p) {
    return p ? p->scene_dirty : 0;
}

void dai_editor_ui_scene_label(dai_editor_ui *p, const char *name) {
    if (!p) return;
    std::snprintf(p->scene_label, sizeof(p->scene_label), "%s", name ? name : "");
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

void dai_editor_ui_rename_host(dai_editor_ui *p, dai_editor_ui_rename_fn fn, void *user) {
    if (!p) return;
    p->asset_rename = fn; p->rename_user = user;
}

void dai_editor_ui_import_host(dai_editor_ui *p, dai_editor_ui_rename_fn fn, void *user) {
    if (!p) return;
    p->asset_import = fn; p->import_user = user;
}

void dai_editor_ui_file_host(dai_editor_ui *p, dai_editor_ui_read_fn r,
                             dai_editor_ui_write_fn w, void *user) {
    if (!p) return;
    p->file_read = r; p->file_write = w; p->file_user = user;
}

void dai_editor_ui_script_editor_pref(dai_editor_ui *p, int external) {
    if (p) p->script_external = external ? 1 : 0;
}
int dai_editor_ui_script_editor_pref_get(const dai_editor_ui *p) {
    return p ? p->script_external : 0;
}

int dai_editor_ui_script_open(dai_editor_ui *p, const char *rel_path) {
    if (!p || !rel_path || !*rel_path) return 0;
    // Already open? Then this is "show me that one", which is what clicking a
    // file a second time means in every editor there is.
    for (size_t i = 0; i < p->scripts_open.size(); ++i)
        if (p->scripts_open[i].path == rel_path) {
            p->script_tab = (int)i;
            dai_editor_ui_panel_open(p, "Script");
            dai_dock_focus(p->dock, "Script");
            return 1;
        }
    if (!p->file_read) {
        dai_editor_ui_toast(p, "this build cannot read files", 2.0f);
        return 0;
    }
    dai_editor_ui::OpenScript o;
    o.path = rel_path;
    // Room to type. A file opened at exactly its own size is read-only in
    // practice, and nothing says so.
    o.buf.assign(96 * 1024, 0);
    uint32_t got = p->file_read(rel_path, o.buf.data(), (uint32_t)o.buf.size() - 1,
                                p->file_user);
    if (!got && o.buf[0] == 0) {
        // An empty file is fine; an unreadable one is not, and the difference
        // is whether the host said anything.
        o.buf[0] = 0;
    }
    p->scripts_open.push_back(std::move(o));
    p->script_tab = (int)p->scripts_open.size() - 1;
    p->scripts_open.back().st.want_focus = 1;
    dai_editor_ui_panel_open(p, "Script");
    dai_dock_focus(p->dock, "Script");
    return 1;
}

size_t dai_editor_ui_scripts_open_save(const dai_editor_ui *p, char *buf, size_t n) {
    if (!p) return 0;
    std::string t;
    // The active tab first, so restoring it needs no index to go stale.
    if (p->script_tab >= 0 && p->script_tab < (int)p->scripts_open.size())
        t += p->scripts_open[(size_t)p->script_tab].path + "\n";
    for (size_t i = 0; i < p->scripts_open.size(); ++i) {
        if ((int)i == p->script_tab) continue;
        t += p->scripts_open[i].path + "\n";
    }
    if (buf && n) {
        size_t c = t.size() < n - 1 ? t.size() : n - 1;
        std::memcpy(buf, t.data(), c);
        buf[c] = 0;
    }
    return t.size();
}

void dai_editor_ui_scripts_open_load(dai_editor_ui *p, const char *text) {
    if (!p || !text) return;
    std::string one;
    for (const char *c = text; ; ++c) {
        if (*c && *c != '\n') { if (*c != '\r') one += *c; continue; }
        if (!one.empty()) {
            // A file that has since been deleted simply does not come back;
            // an editor that refuses to start because of it would be worse.
            dai_editor_ui_script_open(p, one.c_str());
            one.clear();
        }
        if (!*c) break;
    }
    // The first line was the active one, and opening pushes to the end - so
    // after the load the active tab is index 0 again.
    if (!p->scripts_open.empty()) p->script_tab = 0;
}

int dai_editor_ui_script_save(dai_editor_ui *p) {
    if (!p || p->scripts_open.empty()) return 0;
    if (p->script_tab < 0 || p->script_tab >= (int)p->scripts_open.size()) return 0;
    auto &cur = p->scripts_open[(size_t)p->script_tab];
    if (!p->file_write) {
        dai_editor_ui_toast(p, "this build cannot write files", 2.0f);
        return 0;
    }
    if (p->file_write(cur.path.c_str(), cur.buf.data(), p->file_user)) {
        cur.dirty = 0;
        char msg[192];
        std::snprintf(msg, sizeof(msg), "saved %s", base_of(cur.path).c_str());
        dai_editor_ui_toast(p, msg, 1.5f);
        p->want_refresh = 1;
        return 1;
    }
    dai_editor_ui_toast(p, "could not write that file", 2.5f);
    return 0;
}

void dai_editor_ui_delete_host(dai_editor_ui *p, dai_editor_ui_rename_fn fn, void *user) {
    if (!p) return;
    p->asset_delete = fn; p->delete_user = user;
}

int dai_editor_ui_delete_project_pick(dai_editor_ui *p) {
    if (!p) return 0;
    // Only when the Project window is the one under the pointer: Delete in the
    // scene means the selected OBJECT, and the two must never be confused.
    if (!dai_ui_root_hovered(p->ui, "Project")) return 0;
    std::string target;
    if (!p->proj_sel_folder.empty()) target = p->proj_sel_folder;
    else if (p->asset_sel >= 0 && p->asset_sel < (int)p->assets.size() &&
             p->assets[(size_t)p->asset_sel])
        target = p->assets[(size_t)p->asset_sel];
    else if (!p->last_pick.empty())      target = p->last_pick;   // the tree, or a right click
    if (target.empty()) {
        dai_editor_ui_toast(p, "select a file or folder first", 1.8f);
        return 1;      // still ours: Delete over the browser is never the scene's
    }
    if (!p->asset_delete) {
        dai_editor_ui_toast(p, "this build cannot delete files", 2.0f);
        return 1;                       // still ours: do not delete the object
    }
    p->delete_ask = target;
    float mx = 0, my = 0;
    dai_ui_mouse(p->ui, &mx, &my, nullptr, nullptr);
    dai_ui_popup_open(&p->menu_delete, mx, my);
    return 1;
}

int dai_editor_ui_drop_files(dai_editor_ui *p, const char *paths_nl, float x, float y) {
    if (!p || !paths_nl || !*paths_nl) return 0;
    // Only the Project window takes files. Asking the dock where it is - and
    // not remembering it from the draw - is the rule everywhere else in here:
    // a drop arrives between frames, and last frame's rectangle is the only
    // one that exists.
    float px = 0, py = 0, pw = 0, ph = 0;
    if (!dai_dock_panel_rect(p->dock, "Project", 0, &px, &py, &pw, &ph)) return 0;
    if (x < px || x >= px + pw || y < py || y >= py + ph) return 0;
    if (!p->asset_import) {
        dai_editor_ui_toast(p, "no import host - this build cannot copy files in", 3.0f);
        return 0;
    }
    // The Projects half of the window has no folder to import INTO; a drop
    // there means the files, so switch to them first.
    p->proj_tab = 0;
    int n = 0, seen = 0;
    std::string one;
    for (const char *c = paths_nl; ; ++c) {
        if (*c && *c != '\n') { one += *c; continue; }
        while (!one.empty() && (one.back() == '\r' || one.back() == ' ')) one.pop_back();
        if (!one.empty()) {
            ++seen;
            std::string b = one;
            size_t sl = b.find_last_of("/\\");
            if (sl != std::string::npos) b = b.substr(sl + 1);
            if (!b.empty()) {
                std::string dest = p->proj_dir.empty() ? b : p->proj_dir + "/" + b;
                if (p->asset_import(one.c_str(), dest.c_str(), p->import_user)) ++n;
            }
            one.clear();
        }
        if (!*c) break;
    }
    char msg[128];
    if (n > 0) {
        p->want_refresh = 1;
        std::snprintf(msg, sizeof(msg), n == 1 ? "imported %d item into %s"
                                               : "imported %d items into %s",
                      n, p->proj_dir.empty() ? "Assets" : p->proj_dir.c_str());
        dai_editor_ui_toast(p, msg, 2.0f);
    } else if (seen > 0) {
        dai_editor_ui_toast(p, "nothing imported - could not copy it in", 2.5f);
    }
    return n > 0;
}

void dai_editor_ui_material_host(dai_editor_ui *p,
                                 int (*create)(const char *name, void *user),
                                 void *user) {
    if (!p) return;
    p->material_create = create;
    p->material_user = user;
}

void dai_editor_ui_prefab_host(dai_editor_ui *p, dai_editor_ui_rename_fn fn) {
    if (p) p->prefab_save = fn;
}

void dai_editor_ui_open_asset_host(dai_editor_ui *p, dai_editor_ui_rename_fn fn, void *user) {
    if (!p) return;
    p->open_asset = fn;
    p->open_asset_user = user;
}

void dai_editor_ui_params_host(dai_editor_ui *p, dai_editor_ui_params_fn fn, void *user) {
    if (!p) return;
    p->params_fn = fn; p->params_user = user;
}

void dai_editor_ui_scene_host(dai_editor_ui *p,
                              dai_editor_ui_project_list_fn list,
                              dai_editor_ui_project_action_fn open,
                              dai_editor_ui_project_action_fn save_as,
                              void *user) {
    if (!p) return;
    p->scene_list = list; p->scene_open = open; p->scene_save_as = save_as;
    p->scene_user = user;
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

void dai_editor_ui_physics_defaults(dai_editor_ui *p, float friction, float restitution) {
    if (!p) return;
    p->def_friction = friction;
    p->def_restitution = restitution;
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

void dai_editor_ui_scale_host(dai_editor_ui *p,
                              void (*apply_scale)(float scale, void *user),
                              float current, void *user) {
    if (!p) return;
    p->apply_scale = apply_scale;
    p->apply_scale_user = user;
    p->settings_ui_scale = current;
}

void dai_editor_ui_project_settings_host(dai_editor_ui *p, void (*draw)(void *user), void *user) {
    if (!p) return;
    p->proj_settings_host = draw;
    if (user) p->proj_user = user;
}

void dai_editor_ui_folder_host(dai_editor_ui *p,
                               int (*create)(const char *name, void *user), void *user) {
    if (!p) return;
    p->folder_create = create;
    if (user) p->script_user = user;
}

int dai_editor_ui_take_save(dai_editor_ui *p) {
    if (!p || !p->want_save) return 0;
    p->want_save = 0;
    return 1;
}

int dai_editor_ui_take_material_apply(dai_editor_ui *p) {
    if (!p || !p->want_material_apply) return 0;
    p->want_material_apply = 0;
    return 1;
}

void dai_editor_ui_about(dai_editor_ui *p, const char *projects_dir,
                         const char *assets_dir, const char *update_status) {
    if (!p) return;
    if (projects_dir) std::snprintf(p->about_projects, sizeof(p->about_projects), "%s", projects_dir);
    if (assets_dir)   std::snprintf(p->about_assets, sizeof(p->about_assets), "%s", assets_dir);
    if (update_status) std::snprintf(p->about_status, sizeof(p->about_status), "%s", update_status);
}

int dai_editor_ui_take_update_check(dai_editor_ui *p) {
    if (!p || !p->want_update_check) return 0;
    p->want_update_check = 0;
    return 1;
}

int dai_editor_ui_take_refresh(dai_editor_ui *p) {
    if (!p || !p->want_refresh) return 0;
    p->want_refresh = 0;
    return 1;
}

size_t dai_editor_ui_layout_save(const dai_editor_ui *p, char *buf, size_t n) {
    if (!p) return 0;
    return dai_dock_to_text(p->dock, buf, n);
}

dai_result dai_editor_ui_layout_load(dai_editor_ui *p, const char *text) {
    if (!p || !text) return DAI_ERR_INVALID_ARG;
    dai_result r = dai_dock_from_text(p->dock, text);
    if (r == DAI_OK) p->layout_ready = true;
    return r;
}

static void project_expand_to(dai_editor_ui *p, const std::string &dir);
/* The Project window's small button, used by the inspector's prefab bar too -
 * one button shape for the whole editor beats two that nearly match. */
static int browser_button(dai_editor_ui *p, float x, float y, float w, float h,
                          const char *label);

int dai_editor_ui_rename_project_pick(dai_editor_ui *p) {
    if (!p) return 0;
    std::string pick = p->last_pick;
    if (pick.empty() && p->asset_sel >= 0 && p->asset_sel < (int)p->assets.size())
        pick = p->assets[(size_t)p->asset_sel] ? p->assets[(size_t)p->asset_sel] : "";
    if (pick.empty()) return 0;
    // The rename field is drawn in the list of the CURRENT folder, so open
    // the parent first or the row never exists and the rename cancels itself.
    p->rename_asset = pick;
    p->proj_dir = parent_of(pick);
    project_expand_to(p, p->proj_dir);
    std::string base = base_of(pick);
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0) base.resize(dot);
    std::snprintf(p->rename_asset_buf, sizeof(p->rename_asset_buf), "%s", base.c_str());
    p->rename_seen_active = 0;
    p->proj_tab = 0;
    return 1;
}

void dai_editor_ui_panel_open(dai_editor_ui *p, const char *title) {
    if (!p || !title) return;
    dai_dock_focus(p->dock, title);
}

void dai_editor_ui_mesh_host(dai_editor_ui *p,
                             dai_editor_ui_mesh_name_fn name,
                             uint32_t mesh_count, void *user) {
    if (!p) return;
    p->mesh_name = name; p->mesh_count = mesh_count; p->mesh_user = user;
}

// No panel is open twenty times. The cap is a seatbelt, not a feature: a dock
// that ever answers "here is another instance" forever must cost a missing
// panel, never a hung editor.
#define DAI_MAX_PANEL_INSTANCES 8

// ------------------------------------------------------------- hierarchy

// The contents, without deciding where they live. The panel version and the
// window version both call this - two copies of a tree walk is how the two
// slowly stop agreeing.
static dai_vec3 spawn_point(dai_editor_ui *p, dai_doc *d, float half_y);


static void hierarchy_body(dai_editor_ui *p, float h) {
    dai_doc *d = dai_editor_doc(p->ed);
    p->visible_rows = 0;
    p->param_hover_entry = -1;          // same, for reference assign fields
    // Which row the pointer is over is a fact about THIS frame. Left standing
    // from the last one, the empty space below the tree quietly still means
    // "the last row you touched" - and every drop into nothing re-parented
    // onto the bottom object.
    p->hover_node = DAI_INVALID_NODE;
    // The filter, above the tree and outside the scroll: a search box that
    // scrolls away with its results is a search box you lose.
    {
        char fbuf[64];
        std::snprintf(fbuf, sizeof(fbuf), "%s", p->hier_filter);
        if (dai_ui_input_text(p->ui, "Search", fbuf, sizeof(fbuf)))
            std::snprintf(p->hier_filter, sizeof(p->hier_filter), "%s", fbuf);
    }
    dai_ui_scroll_begin(p->ui, "hierarchy", h);
    p->reveal_row_wanted = p->reveal_selection;

    // The scene itself is the root row, the way Unity puts the .unity file
    // above everything: it says WHICH scene is open, and dropping a node on
    // it is how a child becomes a root again.
    int have_root = p->scene_label[0] != 0;
    int root_open = !p->scene_root_folded;
    if (have_root) {
        // The asterisk every editor uses, in the one row that names the file.
        // Without it "did I save that" has no answer except pressing Ctrl+S
        // again and hoping.
        char label[172];
        std::snprintf(label, sizeof(label), "%s%s", p->scene_label,
                      p->scene_dirty ? " *" : "");
        int rc = dai_ui_tree_item_ex(p->ui, label, 0, 1, &root_open, 0);
        p->scene_root_folded = !root_open;
        if (rc & 4) {
            p->hover_node = DAI_SCENE_ROOT_NODE;   // drop here = unparent
            if (p->drag_node != DAI_INVALID_NODE) {
                const dai_ui_style *hs = dai_ui_style_of(p->ui);
                float lx = 0, ly = 0, lw = 0, lh = 0;
                dai_ui_last_rect(p->ui, &lx, &ly, &lw, &lh);
                dai_ui_rect(p->ui, lx, ly, lw, lh, (hs->accent & 0x00FFFFFFu) | 0x55000000u);
                dai_ui_rect_outline(p->ui, lx, ly, lw, lh, 1.0f, hs->accent);
                dai_ui_cursor_set(p->ui, DAI_CURSOR_HAND);
            }
        }
        ++p->visible_rows;
    }
    if (!have_root || root_open) {
        uint32_t n = dai_doc_count(d);
        std::vector<dai_node> all(n);
        if (n) dai_doc_nodes(d, all.data(), n);
        if (p->hier_filter[0]) {
            // While filtering the tree is FLAT. A match three levels down is
            // a match; making you open its two parents to see it is the thing
            // you opened the search box to avoid. Case insensitive, because
            // nobody remembers whether they called it Player or player.
            std::string needle;
            for (const char *q = p->hier_filter; *q; ++q)
                needle += (char)((*q >= 'A' && *q <= 'Z') ? *q + 32 : *q);
            int shown = 0;
            for (dai_node id : all) {
                dai_node_desc r{};
                if (dai_doc_get(d, id, &r) != DAI_OK) continue;
                std::string hay;
                for (const char *q = r.name; *q; ++q)
                    hay += (char)((*q >= 'A' && *q <= 'Z') ? *q + 32 : *q);
                if (hay.find(needle) == std::string::npos) continue;
                draw_subtree_row_only(p, d, id);
                ++shown;
            }
            if (!shown) dai_ui_label(p->ui, "nothing matches");
        } else {
            for (dai_node id : all) {
                dai_node_desc r{};
                if (dai_doc_get(d, id, &r) != DAI_OK) continue;
                if (r.parent != DAI_INVALID_NODE) continue;  // roots drive the recursion
                draw_subtree(p, d, id, have_root ? 1 : 0);
            }
        }
    }
    p->reveal_selection = 0;
    // Room to scroll past the last row. The same reason as the inspector:
    // the bottom row sat ON the edge, and its right click menu opened
    // downwards into a panel border.
    dai_ui_spacing(p->ui, 120.0f);
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
    case DAI_SHAPE_SPHERE:   return DAI_MESH_SPHERE;
    case DAI_SHAPE_CAPSULE:  return DAI_MESH_CAPSULE;
    case DAI_SHAPE_CYLINDER: return DAI_MESH_CYLINDER;
    default:                 return DAI_MESH_BOX;
    }
}

const char *collider_title(int shape) {
    switch (shape) {
    case DAI_SHAPE_SPHERE:   return "Sphere Collider";
    case DAI_SHAPE_CAPSULE:  return "Capsule Collider";
    case DAI_SHAPE_CYLINDER: return "Cylinder Collider";
    default:                 return "Box Collider";
    }
}

bool v3_differs(dai_vec3 a, dai_vec3 b) {
    return a.x != b.x || a.y != b.y || a.z != b.z;
}

} // namespace

// The inspector for a FILE. Everything an asset can say about itself without
// the editor having to understand its contents - and, for the two kinds where
// it can do better than that, the fields themselves.
static void asset_inspector_body(dai_editor_ui *p) {
    dai_ui *ui = p->ui;
    const dai_ui_style *st = dai_ui_style_of(ui);
    const std::string &path = p->inspect_asset;
    std::string base = base_of(path);
    std::string ext;
    {
        size_t dot = base.find_last_of('.');
        if (dot != std::string::npos) ext = base.substr(dot + 1);
        for (char &c : ext) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    bool is_folder = ext.empty();

    // ---- the header card, the same shape the object inspector uses --------
    {
        float hx, hy;
        dai_ui_cursor_pos(ui, &hx, &hy);
        dai_ui_advance(ui, 0, 34.0f);
        float hw = dai_ui_panel_width(ui) - st->padding * 2;
        dai_ui_rrect(ui, hx, hy, hw, 34.0f, 5.0f, rgba(0x3A, 0x3A, 0x3A, 255));
        dai_ui_rect_outline(ui, hx, hy, hw, 34.0f, 1.0f, st->panel_border);
        dai_ui_rect(ui, hx, hy + 33.0f, hw, 1.0f, st->accent);
        const char *ic = is_folder ? DAI_ICON_FOLDER : icon_for_asset(path);
        dai_ui_rrect(ui, hx + 6.0f, hy + 5.0f, 24.0f, 24.0f, 4.0f, st->track);
        if (ic && dai_ui_has_icon(ui, ic))
            dai_ui_icon_at(ui, ic, hx + 10.0f, hy + 9.0f, 16.0f,
                           is_folder ? rgba(0xD8, 0xB4, 0x6A, 255)
                                     : icon_color_for_asset(path, st->accent));
        dai_ui_text(ui, hx + 38.0f, hy + 5.0f, base.c_str(), st->text);
        const char *kind = is_folder ? "Folder"
                         : is_material_file(path) ? "Material"
                         : is_scene_asset(path) ? "Scene"
                         : is_scene_file(path) ? "Prefab"
                         : is_behaviour_file(path) ? "Behaviour"
                         : (ext == "png" || ext == "jpg" || ext == "jpeg") ? "Texture"
                         : (ext == "glb" || ext == "gltf") ? "Model"
                         : "File";
        dai_ui_text(ui, hx + 38.0f, hy + 19.0f, kind, st->text_dim);
    }
    dai_ui_label(p->ui, path.c_str());
    dai_ui_separator(p->ui);
    if (is_folder) {
        dai_ui_label(p->ui, "double click it in the browser to go inside");
        return;
    }

    // ---- the file itself, read once per selection -------------------------
    if (p->inspect_loaded != path) {
        p->inspect_loaded = path;
        p->inspect_text.assign(64 * 1024, 0);
        p->inspect_mat_ok = 0;
        p->inspect_dirty = 0;
        uint32_t got = 0;
        if (p->file_read)
            got = p->file_read(path.c_str(), p->inspect_text.data(),
                               (uint32_t)p->inspect_text.size() - 1, p->file_user);
        p->inspect_text[got < p->inspect_text.size() ? got : p->inspect_text.size() - 1] = 0;
        if (is_material_file(path) && got) {
            // Parsed here rather than through the material module, for the
            // same reason the struct is a copy: four numbers off a text file
            // is not worth a dependency in the other direction.
            dai_matfile_view v{ { 0.8f, 0.8f, 0.8f }, 0.5f, 0.0f, 0.0f };
            const char *c = p->inspect_text.data();
            while (*c) {
                const char *nl = std::strchr(c, '\n');
                std::string line(c, nl ? (size_t)(nl - c) : std::strlen(c));
                c = nl ? nl + 1 : c + line.size();
                if (line.compare(0, 6, "color ") == 0)
                    std::sscanf(line.c_str() + 6, "%f %f %f", &v.color[0], &v.color[1], &v.color[2]);
                else if (line.compare(0, 10, "roughness ") == 0)
                    std::sscanf(line.c_str() + 10, "%f", &v.roughness);
                else if (line.compare(0, 9, "metallic ") == 0)
                    std::sscanf(line.c_str() + 9, "%f", &v.metallic);
                else if (line.compare(0, 9, "emissive ") == 0)
                    std::sscanf(line.c_str() + 9, "%f", &v.emissive);
            }
            p->inspect_mat = v;
            p->inspect_mat_ok = 1;
        }
    }
    size_t bytes = std::strlen(p->inspect_text.data());

    // ---- what each kind can say for itself --------------------------------
    if (p->inspect_mat_ok) {
        dai_ui_label(p->ui, "Surface");
        int changed = 0;
        changed |= dai_ui_color(p->ui, "Color", p->inspect_mat.color, "matcol");
        changed |= dai_ui_num_field(p->ui, "Roughness", &p->inspect_mat.roughness, 0.01f, 0.02f, 1.0f, "matrough");
        changed |= dai_ui_num_field(p->ui, "Metallic", &p->inspect_mat.metallic, 0.01f, 0.0f, 1.0f, "matmetal");
        changed |= dai_ui_num_field(p->ui, "Emissive", &p->inspect_mat.emissive, 0.05f, 0.0f, 40.0f, "matemis");
        if (changed) p->inspect_dirty = 1;
        // The swatch: a number is not a colour.
        {
            float sx, sy;
            dai_ui_cursor_pos(p->ui, &sx, &sy);
            dai_ui_advance(p->ui, 0, 26.0f);
            float sw = dai_ui_panel_width(p->ui) - st->padding * 2;
            uint32_t col = rgba((int)(p->inspect_mat.color[0] * 255.0f),
                                (int)(p->inspect_mat.color[1] * 255.0f),
                                (int)(p->inspect_mat.color[2] * 255.0f), 255);
            dai_ui_rrect(p->ui, sx, sy, sw, 22.0f, 4.0f, col);
            dai_ui_rect_outline(p->ui, sx, sy, sw, 22.0f, 1.0f, st->panel_border);
        }
        if (dai_ui_button_fit(p->ui, p->inspect_dirty ? "Save material *" : "Save material")) {
            char text[512];
            std::snprintf(text, sizeof(text),
                          "daidalos-material 1\ncolor %g %g %g\nroughness %g\n"
                          "metallic %g\nemissive %g\n",
                          (double)p->inspect_mat.color[0], (double)p->inspect_mat.color[1],
                          (double)p->inspect_mat.color[2], (double)p->inspect_mat.roughness,
                          (double)p->inspect_mat.metallic, (double)p->inspect_mat.emissive);
            if (p->file_write && p->file_write(path.c_str(), text, p->file_user)) {
                p->inspect_dirty = 0;
                p->inspect_loaded.clear();          // re-read it next frame
                p->want_material_apply = 1;         // every object wearing it
                dai_editor_ui_toast(p, "material saved", 1.5f);
            } else {
                dai_editor_ui_toast(p, "could not write that file", 2.5f);
            }
        }
        dai_ui_separator(p->ui);
    } else if (is_behaviour_file(path)) {
        // The fields it declares - the same list the object inspector draws
        // when this script is attached to something.
        if (p->params_fn) {
            char keys[4096] = { 0 };
            p->params_fn(path.c_str(), keys, sizeof(keys), p->params_user);
            std::vector<ParamDecl> decls = parse_params(keys);
            unsigned nfields = 0;
            for (const ParamDecl &pd : decls) if (pd.type != PARAM_HEADER) ++nfields;
            dai_ui_label_fmt(p->ui, "Serialized fields: %u", nfields);
            for (const ParamDecl &pd : decls) {
                static const char *TN[] = { "node", "float", "int", "bool", "string" };
                if (pd.type == PARAM_HEADER) {
                    dai_ui_advance(p->ui, 0, 4.0f);
                    dai_ui_label(p->ui, pd.name.c_str());
                    continue;
                }
                dai_ui_label_fmt(p->ui, "   %s %s%s%s", TN[pd.type], pd.name.c_str(),
                                 pd.def.empty() ? "" : " = ", pd.def.c_str());
                if (!pd.tip.empty()) dai_ui_help(p->ui, pd.tip.c_str());
            }
        }
        int lines = 1;
        for (const char *c = p->inspect_text.data(); *c; ++c) if (*c == '\n') ++lines;
        dai_ui_label_fmt(p->ui, "%d lines", lines);
        if (dai_ui_button_fit(p->ui, "Edit")) dai_editor_ui_script_open(p, path.c_str());
        dai_ui_separator(p->ui);
    } else if (is_scene_file(path)) {
        // A scene file and a prefab file are the same format; the count of
        // "node" lines is what either of them is made of.
        int nodes = 0;
        const char *c = p->inspect_text.data();
        while ((c = std::strstr(c, "node ")) != nullptr) { ++nodes; c += 5; }
        dai_ui_label_fmt(p->ui, "%d object(s) inside", nodes);
        if (is_scene_asset(path)) {
            if (dai_ui_button_fit(p->ui, "Open scene")) p->prefab_open_want = path;
        } else {
            if (dai_ui_button_fit(p->ui, "Open prefab")) p->prefab_open_want = path;
            dai_ui_label(p->ui, "drag it into the scene to place one");
        }
        dai_ui_separator(p->ui);
    }

    if (bytes) dai_ui_label_fmt(p->ui, "%u bytes", (unsigned)bytes);
    else       dai_ui_label(p->ui, "binary, or not readable as text");
    if (dai_ui_button_fit(p->ui, "Open externally") && p->open_asset)
        p->open_asset(nullptr, path.c_str(), p->open_asset_user);
}

static void inspector_body(dai_editor_ui *p) {
    close_field_tx_on_release(p);
    dai_doc *d = dai_editor_doc(p->ed);

    uint32_t sel = dai_editor_selection_count(p->ed);
    // A file is a selection too. It loses to an object, so clicking in the
    // hierarchy always wins - and clicking in the browser clears the object.
    if (sel == 0 && !p->inspect_asset.empty()) { asset_inspector_body(p); return; }
    if (sel == 0) { dai_ui_label(p->ui, "nothing selected"); return; }
    if (sel > 1) {
        // Unity's rule, and the only workable one: show the FIRST object and
        // write every field you touch into all of them. The alternative -
        // "N objects selected", nothing editable - means setting the same
        // friction on twelve crates is twelve trips through the hierarchy.
        //
        // Fields that are not touched are not written, so eight crates keep
        // eight different positions until the moment you drag a position
        // field. That falls out of the diff at the bottom of this function:
        // it compares what the panel produced against what it started with.
        dai_ui_label_fmt(p->ui, "Editing %u objects - a field you change is written to all", sel);
        dai_ui_separator(p->ui);
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
    // One line, Unity's object header: is it on, what is it, what is it
    // called. Three stacked fields for that read like a form, and the
    // inspector is not a form.
    // Refreshed when the DOCUMENT's name no longer matches the buffer and the
    // field is not being typed into: a rename from the hierarchy or an undo
    // has to reach the inspector, and the buffer must not fight the user's
    // keystrokes while it does.
    if (p->name_buf_node != n ||
        (std::strcmp(p->name_buf, r.name) != 0 && !dai_ui_text_active(p->ui))) {
        std::snprintf(p->name_buf, sizeof(p->name_buf), "%s", r.name);
        p->name_buf_node = n;
    }
    if (p->tag_buf_node != n) {
        std::snprintf(p->tag_buf, sizeof(p->tag_buf), "%s", r.tag);
        p->tag_buf_node = n;
    }
    if (p->asset_buf_node != n) {
        std::snprintf(p->asset_buf, sizeof(p->asset_buf), "%s", r.asset);
        p->asset_buf_node = n;
    }
    {
        dai_ui *ui2 = p->ui;
        const dai_ui_style *st2 = dai_ui_style_of(ui2);
        float hx, hy;
        dai_ui_cursor_pos(ui2, &hx, &hy);
        dai_ui_advance(ui2, 0, 34.0f);
        float hw = dai_ui_panel_width(ui2) - st2->padding * 2;
        float mx2 = 0, my2 = 0;
        int d2 = 0, p2 = 0;
        dai_ui_mouse(ui2, &mx2, &my2, &d2, &p2);
        // A card, not a strip: the object header is the one thing in the
        // inspector that says WHAT you are editing, and it was the same
        // height as a numeric field.
        dai_ui_rrect(ui2, hx, hy, hw, 34.0f, 5.0f, rgba(0x3A, 0x3A, 0x3A, 255));
        dai_ui_rect_outline(ui2, hx, hy, hw, 34.0f, 1.0f, st2->panel_border);
        dai_ui_rect(ui2, hx, hy + 33.0f, hw, 1.0f, st2->accent);

        // the active checkbox
        float bx = hx + 8.0f, by = hy + 10.0f, bsz = 14.0f;
        bool over_box = mx2 >= bx && mx2 < bx + bsz && my2 >= by && my2 < by + bsz;
        dai_ui_rect(ui2, bx, by, bsz, bsz, over_box ? st2->button_hover : st2->track);
        dai_ui_rect_outline(ui2, bx, by, bsz, bsz, 1.0f, st2->panel_border);
        {
            int on = !r.disabled;
            if (on) {
                dai_ui_line(ui2, bx + 3.0f, by + 7.0f, bx + 6.0f, by + 10.5f, 2.0f, st2->text);
                dai_ui_line(ui2, bx + 6.0f, by + 10.5f, bx + 11.5f, by + 3.5f, 2.0f, st2->text);
            }
            // The OBJECT, not its renderer. They were the same flag, which
            // is why ticking a camera back on switched its Mesh Renderer on.
            if (over_box && p2)
                r.disabled = !r.disabled;
        }
        // The kind, in a tile of its own - Unity's inspector puts the icon
        // on a plate so the eye finds it before it reads anything.
        dai_ui_rrect(ui2, hx + 28.0f, hy + 5.0f, 24.0f, 24.0f, 4.0f, st2->track);
        dai_ui_icon_at(ui2, node_icon(r), hx + 32.0f, hy + 9.0f, 16.0f, st2->accent);

        // Static, on the right, where Unity has it. It is the motion type
        // here, which is the same promise: this thing does not move.
        float sw = dai_ui_text_width(ui2, "Static") + 22.0f;
        float sx = hx + hw - sw - 6.0f;
        {
            float cx = sx, cy = hy + 10.0f;
            bool over_s = mx2 >= cx && mx2 < cx + sw && my2 >= hy + 4.0f && my2 < hy + 30.0f;
            dai_ui_rect(ui2, cx, cy, 14.0f, 14.0f, over_s ? st2->button_hover : st2->track);
            dai_ui_rect_outline(ui2, cx, cy, 14.0f, 14.0f, 1.0f, st2->panel_border);
            int is_static = r.motion == DAI_STATIC;
            if (is_static) {
                dai_ui_line(ui2, cx + 3.0f, cy + 7.0f, cx + 6.0f, cy + 10.5f, 2.0f, st2->text);
                dai_ui_line(ui2, cx + 6.0f, cy + 10.5f, cx + 11.5f, cy + 3.5f, 2.0f, st2->text);
            }
            dai_ui_text(ui2, cx + 18.0f, cy + 1.0f, "Static", st2->text_dim);
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
            }
        }

        // Written back only when it actually changed, or every frame would
        // count as an edit and every rename from elsewhere would be undone.
        float nfw = sx - (hx + 58.0f) - 8.0f;
        if (nfw < 60.0f) nfw = 60.0f;
        if (dai_ui_text_field(ui2, "objname", hx + 58.0f, hy + 8.0f, nfw, 18.0f,
                              p->name_buf, sizeof(p->name_buf), nullptr))
            std::snprintf(r.name, sizeof(r.name), "%s", p->name_buf);
    }
    dai_ui_spacing(p->ui, 2.0f);
    // Tag and asset share one secondary line; the asset field only exists
    // when the node has an asset to show.
    // Tag on its own line, asset only when there is one. Two full width
    // fields squeezed into one row is how the asset field ended up off the
    // right edge of the panel.
    if (dai_ui_input_text(p->ui, "Tag", p->tag_buf, sizeof(p->tag_buf)))
        std::snprintf(r.tag, sizeof(r.tag), "%s", p->tag_buf);
    if (r.asset[0]) {
        if (dai_ui_input_text(p->ui, "Asset", p->asset_buf, sizeof(p->asset_buf)))
            std::snprintf(r.asset, sizeof(r.asset), "%s", p->asset_buf);
    }
    // ---- Prefab -------------------------------------------------------------
    // A node with a prefab path IS an instance: its children are not stored in
    // this scene, they are expanded from that file. Unity puts a blue bar and
    // the source name at the top of the inspector for exactly this, because
    // "why did my edit come back" has one answer and it is this line.
    if (r.prefab[0]) {
        dai_ui *ui3 = p->ui;
        const dai_ui_style *st3 = dai_ui_style_of(ui3);
        float hx3, hy3;
        dai_ui_cursor_pos(ui3, &hx3, &hy3);
        float hw3 = dai_ui_panel_width(ui3) - st3->padding * 2;
        float hh3 = dai_ui_text_height(ui3) + 10.0f;
        dai_ui_advance(ui3, 0, hh3 + 2.0f);
        dai_ui_rrect(ui3, hx3, hy3, hw3, hh3, 4.0f, rgba(0x25, 0x3A, 0x52, 255));
        dai_ui_rect(ui3, hx3, hy3, 3.0f, hh3, st3->accent);
        float tx3 = hx3 + 9.0f;
        if (dai_ui_has_icon(ui3, DAI_ICON_C_PREFAB)) {
            dai_ui_icon_at(ui3, DAI_ICON_C_PREFAB, tx3, hy3 + (hh3 - 14.0f) * 0.5f, 14.0f,
                           st3->accent);
            tx3 += 19.0f;
        }
        char pl3[160];
        std::snprintf(pl3, sizeof(pl3), "Prefab  %s", base_of(r.prefab).c_str());
        dai_ui_text(ui3, tx3, hy3 + (hh3 - dai_ui_text_height(ui3)) * 0.5f, pl3, st3->text);
        // Select shows the source in the Project window; Unpack breaks the
        // link and keeps the objects, which is Unity's "Unpack Prefab".
        float bw3 = dai_ui_text_width(ui3, "Unpack") + 16.0f;
        float sw3 = dai_ui_text_width(ui3, "Select") + 16.0f;
        float by3 = hy3 + (hh3 - 18.0f) * 0.5f;
        if (browser_button(p, hx3 + hw3 - bw3 - 4.0f, by3, bw3, 18.0f, "Unpack")) {
            r.prefab[0] = 0;
            dai_editor_ui_toast(p, "prefab link removed - the objects stay", 2.0f);
        }
        if (browser_button(p, hx3 + hw3 - bw3 - sw3 - 10.0f, by3, sw3, 18.0f, "Select")) {
            p->proj_dir = parent_of(r.prefab);
            project_expand_to(p, p->proj_dir);
            p->proj_list_scroll = 0.0f;
            for (size_t ai = 0; ai < p->assets.size(); ++ai)
                if (p->assets[ai] && r.prefab == p->assets[ai]) { p->asset_sel = (int)ai; break; }
            dai_editor_ui_panel_open(p, "Project");
            dai_dock_focus(p->dock, "Project");
        }
    }
    dai_ui_separator(p->ui);

    // ---- Transform ---------------------------------------------------------
    // Right click on the header copies the whole transform - three vectors a
    // designer would otherwise retype by hand.
    if (dai_ui_header_icon_col(p->ui, DAI_ICON_C_TRANSFORM, rgba(0x6C, 0xA9, 0xF5, 255), "Transform", &p->fold_transform, nullptr) == 3) {
        p->comp_menu_target = 0;
        float cmx = 0, cmy = 0;
        dai_ui_mouse(p->ui, &cmx, &cmy, nullptr, nullptr);
        dai_ui_popup_open(&p->menu_comp, cmx, cmy);
    }
    if (p->fold_transform) {
        // While playing the document still holds the pose from before play -
        // that is exactly what makes Stop able to restore it - so a panel that
        // asked the document would show a frozen ghost. Ask the BODY where the
        // object is, and write typed values back to the document.
        dai_vec3 pos = r.position;
        dai_quat rot = r.rotation;
        if (playing) {
            // Live means BOTH: position came from the body, but rotation kept
            // showing the document's pre-play ghost - "rotation never updates".
            dai_quat lr; dai_vec3 ls;
            if (dai_editor_live_transform(p->ed, n, &pos, &lr, &ls)) rot = lr;
            else dai_editor_live_position(p->ed, n, &pos);
        }
        if (dai_ui_num_vec3(p->ui, "Position", &pos.x, 0.02f)) r.position = pos;

        // Rotation is shown in degrees. The cache is refreshed whenever the
        // quaternion moved from anywhere that is not this field - the gizmo,
        // an undo, the simulation - so typing is never fought by a conversion
        // that rounds differently than the last keystroke.
        if (p->euler_node != n) {
            // A different object: no previous reading to stay near.
            p->euler_node = n;
            p->euler_cached_q = rot;
            quat_to_euler(rot, p->euler_deg);
        } else if (!quat_eq(p->euler_cached_q, rot)) {
            // The same object, turned from somewhere else - the gizmo, an
            // undo, the simulation. Re-read it in the spelling closest to what
            // is already on screen, so one axis of gizmo drag moves one field.
            p->euler_cached_q = rot;
            quat_to_euler_near(rot, p->euler_deg, p->euler_deg);
        }
        if (dai_ui_num_vec3(p->ui, "Rotation", p->euler_deg, 0.5f)) {
            r.rotation = euler_to_quat(p->euler_deg);
            p->euler_cached_q = r.rotation;
        }
        dai_ui_num_vec3(p->ui, "Scale", &r.scale.x, 0.01f);
    }

    // ---- Mesh Renderer -----------------------------------------------------
    int visible = !r.hidden;
    if (dai_ui_header_icon_col(p->ui, DAI_ICON_C_MESH, rgba(0x4F, 0xD1, 0xC5, 255), "Mesh Renderer",
                           &p->fold_render, &visible) == 2)
        r.hidden = !visible;
    if (p->fold_render) {
        if (p->mesh_name) {
            const char *cur = r.mesh == 0xFFFFFFFFu ? "(from shape)"
                            : r.mesh == 0xFFFFFFFEu ? "(from asset)"
                            : p->mesh_name(r.mesh, p->mesh_user);
            // Unity's object field: the current value, and a target button
            // that opens the list of everything you could put there. The old
            // "< >" pair made picking the fifth mesh a five click guessing
            // game with no way to see what the other four were.
            if (dai_ui_object_field(p->ui, "Mesh", cur ? cur : "None", DAI_ICON_CUBE)) {
                float mx2 = 0, my2 = 0;
                dai_ui_mouse(p->ui, &mx2, &my2, nullptr, nullptr);
                dai_ui_popup_open(&p->menu_mesh, mx2 - 150.0f, my2);
                p->mesh_menu_node = n;
            }
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
        // Colour used to sit here. It belongs to the MATERIAL - a surface
        // property next to the material that owns it is two places the same
        // fact can disagree, and the array below is where materials live.
        // ---- Materials, Unity's array -------------------------------------
        // The roughness and emissive fields that used to sit here moved IN:
        // a surface property next to the material that owns it is two places
        // the same fact can disagree.
        //
        // The layout is Unity's, element by element: the header line carries
        // the size, the rows carry a handle and the field, and the +/- lives
        // under the list on the right.
        {
            std::vector<std::string> mats = script_list(r.materials);
            if (mats.empty()) mats.push_back("Default");
            int mcount = (int)mats.size();
            if (dai_ui_array_begin(p->ui, "Materials", &mcount, &p->fold_materials, 1, 8)) {
                // The size field can be typed into: match the list to it
                // before drawing the rows, or the last row shows a slot that
                // does not exist yet.
                while ((int)mats.size() < mcount) mats.push_back("Default");
                while ((int)mats.size() > mcount) mats.pop_back();
                for (size_t mi = 0; mi < mats.size(); ++mi) {
                    if (dai_ui_array_object_row(p->ui, (int)mi, mats[mi].c_str(),
                                                DAI_ICON_C_MATERIAL)) {
                        float mx2 = 0, my2 = 0;
                        dai_ui_mouse(p->ui, &mx2, &my2, nullptr, nullptr);
                        dai_ui_searchlist_open(&p->mat_list, mx2 - 150.0f, my2);
                        p->mat_list.wants_focus = 1;
                        std::snprintf(p->mat_list.hint, sizeof(p->mat_list.hint),
                                      "Search materials...");
                        p->mat_menu_node = n;
                        p->mat_menu_slot = (int)mi;
                    }
                }
                int delta = dai_ui_array_end(p->ui, (int)mats.size(), 1, 8);
                if (delta > 0) mats.push_back("Default");
                else if (delta < 0 && mats.size() > 1) mats.pop_back();
            } else {
                while ((int)mats.size() < mcount) mats.push_back("Default");
                while ((int)mats.size() > mcount) mats.pop_back();
            }
            script_join(r.materials, sizeof(r.materials), mats);
        }
    }

    // ---- Collider ----------------------------------------------------------
    // What the world can hit. NOT what it looks like: the wireframe is green
    // and separate on purpose, and Size here resizes the collision box while
    // the model stays the size it was. Unchecking it removes the COLLISION -
    // never the model, that was the bug where a component toggle made the
    // mesh vanish.
    int has_collider = !r.no_collider && !r.no_body;
    // A component that is not there is NOT DRAWN. It used to sit there with an
    // empty tick box and the line "no collider - nothing can hit this", which
    // is why removing one looked like it only greyed it out: the header stayed
    // for ever and there was no way to make it go away.
    //
    // The tick box is gone with it. In this document "has a collider" IS the
    // only state there is - there is no separate enabled flag - so a box that
    // claimed to be one was two names for the same bit.
    if (has_collider) {
        // The tick box is ENABLED, not EXISTS. Both, because they are two
        // different questions: "off for a moment while I test something" is
        // not "gone", and the header has to stay put for the first one.
        int con = (r.disabled_comps & DAI_COMP_COLLIDER) == 0;
        int hrc = dai_ui_header_icon_col(p->ui, DAI_ICON_C_COLLIDER, rgba(0x7A, 0xD9, 0x7A, 255), collider_title(r.shape),
                                     &p->fold_collider, &con);
        if (hrc == 2) {
            r.disabled_comps = con ? (r.disabled_comps & ~DAI_COMP_COLLIDER)
                                   : (r.disabled_comps | DAI_COMP_COLLIDER);
        } else if (hrc == 3) {
            p->comp_menu_target = 2;
            float cmx = 0, cmy = 0;
            dai_ui_mouse(p->ui, &cmx, &cmy, nullptr, nullptr);
            dai_ui_popup_open(&p->menu_comp, cmx, cmy);
        }
    }
    if (p->fold_collider) {
        if (!has_collider) {
            /* not there: nothing is drawn, see above */
        } else {
            // Edit Collider: a real button that stays lit while the mode is
            // on - the icon next to a label did not read as something you can
            // press, because nothing about it looked like a button.
            if (dai_ui_toggle_button(p->ui, "Edit Collider", p->collider_edit))
                p->collider_edit = !p->collider_edit;

            int shape_idx = r.shape;
            if (r.mesh == 0xFFFFFFFEu) shape_idx = COLLIDER_FROM_MESH;
            if (dai_ui_option(p->ui, "Shape", &shape_idx, COLLIDER_SHAPES, 6)) {
                // Changing the collider's shape must not reshape the model:
                // pin the mesh to what it is right now first.
                if (r.mesh == 0xFFFFFFFFu) r.mesh = mesh_of_shape(r.shape);
                if (shape_idx == COLLIDER_FROM_MESH) r.mesh = 0xFFFFFFFEu;
                else                                 r.shape = shape_idx;
            }
            int trig = r.trigger;
            if (dai_ui_checkbox(p->ui, "Is Trigger", &trig)) r.trigger = trig;
            dai_ui_num_vec3(p->ui, "Center", &r.collider_center.x, 0.01f);
            if (shape_idx != COLLIDER_FROM_MESH) {
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
    if (has_body) {
        int bon = (r.disabled_comps & DAI_COMP_RIGIDBODY) == 0;
        int hrc = dai_ui_header_icon_col(p->ui, DAI_ICON_C_BODY, rgba(0xA7, 0x9B, 0xF0, 255), "Rigidbody", &p->fold_body, &bon);
        if (hrc == 2) {
            r.disabled_comps = bon ? (r.disabled_comps & ~DAI_COMP_RIGIDBODY)
                                   : (r.disabled_comps | DAI_COMP_RIGIDBODY);
        } else if (hrc == 3) {
            p->comp_menu_target = 1;
            float cmx = 0, cmy = 0;
            dai_ui_mouse(p->ui, &cmx, &cmy, nullptr, nullptr);
            dai_ui_popup_open(&p->menu_comp, cmx, cmy);
        }
    }
    if (p->fold_body) {
        if (!has_body) {
            // Inverted before this: the fields were INSIDE the "off" branch,
            // so switching the rigidbody off is what made them appear.
            /* not there: nothing is drawn */
        } else {
            dai_ui_option(p->ui, "Motion", &r.motion, MOTIONS, 3);
            dai_ui_help(p->ui, "Dynamic: moved by physics. Kinematic: moved by "
                               "script, pushes others. Static: never moves.");
            // No mass field on purpose: mass = density x shape volume.
            // 0 falls back to water (1000) in dai_engine.
            // Every one of these is a physical quantity with a unit, and a
            // number with no unit is a number you have to guess at: "Friction
            // 10" was read as a percentage more than once.
            dai_ui_num_field(p->ui, "Density", &r.density, 10.0f, 0.0f, 100000.0f, "density");
            dai_ui_help(p->ui, "kg/m3. Mass = density x collider volume. 0 = water (1000). "
                               "Wood 600, concrete 2400, steel 7850.");
            dai_ui_num_field(p->ui, "Friction", &r.friction, 0.005f, 0.0f, 10.0f, "friction");
            dai_ui_help(p->ui, "Coefficient mu, not a percentage. 0 = ice, 0.3 = wet road, "
                               "0.6 = wood, 1.0 = rubber. Combined with the other body's mu.");
            dai_ui_num_field(p->ui, "Bounce", &r.restitution, 0.005f, 0.0f, 1.0f, "bounce");
            dai_ui_help(p->ui, "Restitution 0..1. 0 = stays put, 0.8 = basketball, "
                               "1 = keeps all its energy.");

            // ---- Constraints ------------------------------------------
            // Unity's, and in Unity's PLACE: a foldout inside the Rigidbody,
            // not a block of its own. Freezing an axis is a property of the
            // BODY - a heading of its own made it read as one more component
            // to add, and it appeared under objects whose Rigidbody was
            // three sections further up.
            //
            // Frozen means the SOLVER may not move it; a script setting the
            // transform still can.
            if (r.motion == DAI_DYNAMIC) {
                // Unity's shape, down to the indent: a small triangle, then
                // two labelled rows of three ticks. It used to be a full
                // header - which read as another component - and the ticks
                // were captioned "X##fpx", because the id half of the label
                // was being drawn on screen.
                int c_open = !p->fold_freeze;
                dai_ui_subheader(p->ui, "Constraints", &c_open);
                p->fold_freeze = !c_open;
                if (c_open) {
                    dai_ui_axis_toggles(p->ui, "Freeze Position", &r.freeze,
                                        DAI_FREEZE_POS_X, DAI_FREEZE_POS_Y, DAI_FREEZE_POS_Z);
                    dai_ui_axis_toggles(p->ui, "Freeze Rotation", &r.freeze,
                                        DAI_FREEZE_ROT_X, DAI_FREEZE_ROT_Y, DAI_FREEZE_ROT_Z);
                    // The two combinations anyone actually types out by hand.
                    dai_ui_row(p->ui, 0.0f);
                    if (dai_ui_button_fit(p->ui, "Upright")) r.freeze |= DAI_FREEZE_UPRIGHT;
                    if (dai_ui_button_fit(p->ui, "2D plane")) r.freeze |= DAI_FREEZE_2D;
                    if (r.freeze && dai_ui_button_fit(p->ui, "Clear")) r.freeze = 0;
                    dai_ui_row_end(p->ui);
                }
            }
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

    // ---- Scripts ------------------------------------------------------------
    // Scripts are COMPONENTS, the Unity rule: each .js on the node is its own
    // block and several stack. The document stores them ';'-separated in the
    // one script field, so every old single-script file stays valid. There is
    // deliberately no assign button: a script attaches by DRAG, onto the node
    // in the hierarchy or anywhere in this panel.
    {
        std::vector<std::string> slist = script_list(r.script);
        // Only while something IS being dragged. As a permanent line it sat
        // under every object in the project saying the same thing forever,
        // which is not a hint, it is furniture.
        if (slist.empty()) { }
        if (p->fold_scripts.size() != slist.size())
            p->fold_scripts.assign(slist.size(), 1);
        int remove_at = -1;
        for (size_t si = 0; si < slist.size(); ++si) {
            // Every script is its own component, the Unity rule: its own
            // header, its own fold, remove on the header's context click.
            std::string label = base_of(entry_path(slist[si]));
            int hrc = dai_ui_header_icon_col(p->ui, DAI_ICON_C_SCRIPT, rgba(0xF2, 0xC1, 0x4E, 255), label.c_str(),
                                         &p->fold_scripts[si], nullptr);
            if (hrc == 3) remove_at = (int)si;   // right click removes
            if (!p->fold_scripts[si]) continue;
            // "// @param [type] name [= default]" lines in the file are the
            // object's serialized fields: a widget each, edited here, stored
            // on the node, handed to the script at Play.
            if (p->params_fn) {
                char keys[4096] = { 0 };
                p->params_fn(entry_path(slist[si]).c_str(), keys, sizeof(keys), p->params_user);
                std::string entry_before = slist[si];
                for (const ParamDecl &pd : parse_params(keys)) {
                    if (pd.type == PARAM_HEADER) {
                        // Unity's [Header("...")]: air, then a bright line. It
                        // is not a field and has no value - it is the reason
                        // the eight fields under it belong together.
                        dai_ui_advance(p->ui, 0, 6.0f);
                        dai_ui_label(p->ui, pd.name.c_str());
                        continue;
                    }
                    std::string val = entry_param(slist[si], pd.name);
                    if (val.empty()) val = pd.def;      // the file's own default
                    char fid[96];
                    std::snprintf(fid, sizeof(fid), "p%zu_%s", si, pd.name.c_str());
                    if (pd.type == PARAM_FLOAT || pd.type == PARAM_INT) {
                        float fv = (float)std::atof(val.c_str());
                        float was = fv;
                        if (dai_ui_num_field(p->ui, pd.name.c_str(), &fv,
                                             pd.type == PARAM_INT ? 1.0f : 0.1f,
                                             0.0f, 0.0f, fid) || fv != was) {
                            char nb[48];
                            if (pd.type == PARAM_INT) std::snprintf(nb, sizeof(nb), "%d", (int)(fv + (fv < 0 ? -0.5f : 0.5f)));
                            else                      std::snprintf(nb, sizeof(nb), "%g", (double)fv);
                            entry_set_param(slist[si], pd.name, nb);
                        }
                    } else if (pd.type == PARAM_BOOL) {
                        int bv = (val == "true" || val == "1") ? 1 : 0;
                        if (dai_ui_checkbox(p->ui, pd.name.c_str(), &bv))
                            entry_set_param(slist[si], pd.name, bv ? "true" : "false");
                    } else if (pd.type == PARAM_STRING) {
                        char sb[160];
                        std::snprintf(sb, sizeof(sb), "%s", val.c_str());
                        if (dai_ui_input_text(p->ui, pd.name.c_str(), sb, sizeof(sb))) {
                            // ',' '=' '{' '}' and ';' are the separators this
                            // is stored between - a value carrying one would
                            // split the field list in half.
                            std::string clean;
                            for (char c : std::string(sb))
                                clean += (c == ',' || c == '=' || c == '{' ||
                                          c == '}' || c == ';') ? ' ' : c;
                            entry_set_param(slist[si], pd.name, clean);
                        }
                    } else {
                        // A node reference, drawn the way Unity draws one: the
                        // value with its type in brackets, and a target button
                        // that opens a searchable list of every object in the
                        // scene. Dragging one in still works - that gesture is
                        // faster when you can see both windows, and the picker
                        // is what you reach for when you cannot.
                        const NodeTypeInfo *nt = node_type_of(pd.ntype);
                        const char *tname = nt ? nt->label : "Object";
                        // Unity writes the value and its type: "Main Camera
                        // (Transform)". The type is not decoration - it is the
                        // contract the picker and the drop both answer to.
                        std::string disp = (val.empty() ? std::string("None") : val)
                                         + " (" + tname + ")";
                        // An assignment that no longer fits - the camera lost
                        // its Camera component, the file was hand edited - is
                        // said out loud instead of being quietly wrong.
                        int bad = 0;
                        if (!val.empty() && nt) {
                            dai_node vn = dai_doc_find(d, val.c_str());
                            dai_node_desc vr{};
                            if (vn == DAI_INVALID_NODE) bad = 1;
                            else if (dai_doc_get(d, vn, &vr) == DAI_OK && !node_has_type(vr, pd.ntype))
                                bad = 1;
                            if (bad) disp = val + "  (not a " + tname + ")";
                        }
                        int orc = dai_ui_object_field(p->ui, pd.name.c_str(), disp.c_str(),
                                                      nt ? nt->icon : DAI_ICON_C_TRANSFORM);
                        if (!pd.tip.empty()) dai_ui_help(p->ui, pd.tip.c_str());
                        if (orc == 2) {
                            float mx3 = 0, my3 = 0;
                            dai_ui_mouse(p->ui, &mx3, &my3, nullptr, nullptr);
                            dai_ui_searchlist_open(&p->obj_list, mx3 - 210.0f, my3);
                            p->obj_list.wants_focus = 1;
                            std::snprintf(p->obj_list.hint, sizeof(p->obj_list.hint),
                                          "Search objects...");
                            p->obj_pick_node = n;
                            p->obj_pick_entry = (int)si;
                            std::snprintf(p->obj_pick_key, sizeof(p->obj_pick_key),
                                          "%s", pd.name.c_str());
                            std::snprintf(p->obj_pick_type, sizeof(p->obj_pick_type),
                                          "%s", pd.ntype.c_str());
                        } else if (orc == 1 && !val.empty()) {
                            // Clicking the field shows what it points at.
                            p->ping_node = dai_doc_find(d, val.c_str());
                        }
                        const char *hot2 = dai_ui_hot_label(p->ui);
                        if (hot2 && std::strcmp(hot2, pd.name.c_str()) == 0) {
                            p->param_hover_entry = (int)si;
                            std::snprintf(p->param_hover_key, sizeof(p->param_hover_key),
                                          "%s", pd.name.c_str());
                        }
                    }
                    if (!pd.tip.empty() && pd.type != PARAM_NODE)
                        dai_ui_help(p->ui, pd.tip.c_str());
                }
                if (slist[si] != entry_before) script_join(r.script, sizeof(r.script), slist);
            }
        }
        if (remove_at >= 0) {
            slist.erase(slist.begin() + remove_at);
            script_join(r.script, sizeof(r.script), slist);
        }
    }

    // ---- Camera -------------------------------------------------------------
    // The 2D/3D switch lives here: orthographic + a flat world is a 2D game.
    if (r.camera) {
        int on = 1;
        {
            int hrc = dai_ui_header_icon_col(p->ui, DAI_ICON_C_CAMERA, rgba(0x9F, 0xB4, 0xD8, 255), "Camera", &p->fold_camera, &on);
            if (hrc == 2 && !on) r.camera = 0;
            else if (hrc == 3) {
                p->comp_menu_target = 3;
                float cmx = 0, cmy = 0;
                dai_ui_mouse(p->ui, &cmx, &cmy, nullptr, nullptr);
                dai_ui_popup_open(&p->menu_comp, cmx, cmy);
            }
        }
        if (p->fold_camera && r.camera) {
            static const char *const PROJ[] = { "Perspective", "Orthographic" };
            int proj = r.camera == 2 ? 1 : 0;
            if (dai_ui_option(p->ui, "Projection", &proj, PROJ, 2)) r.camera = proj ? 2 : 1;
            if (r.camera == 2) dai_ui_num_field(p->ui, "Size", &r.camera_size, 0.05f, 0.1f, 1000.0f, "camsize");
            else               dai_ui_num_field(p->ui, "FOV", &r.camera_fov, 0.25f, 5.0f, 170.0f, "camfov");
        }
    }

    // ---- Light --------------------------------------------------------------
    if (r.light) {
        int on = 1;
        {
            int hrc = dai_ui_header_icon_col(p->ui, DAI_ICON_C_LIGHT, rgba(0xF5, 0xD7, 0x6E, 255), "Light", &p->fold_light, &on);
            if (hrc == 2 && !on) r.light = 0;
            else if (hrc == 3) {
                p->comp_menu_target = 4;
                float cmx = 0, cmy = 0;
                dai_ui_mouse(p->ui, &cmx, &cmy, nullptr, nullptr);
                dai_ui_popup_open(&p->menu_comp, cmx, cmy);
            }
        }
        if (p->fold_light && r.light) {
            static const char *const LT[] = { "Point", "Spot", "Directional" };
            int lt = r.light - 1;
            if (lt < 0) lt = 0;
            if (lt > 2) lt = 2;
            if (dai_ui_option(p->ui, "Type", &lt, LT, 3)) r.light = lt + 1;
            // A light with no colour chosen is white, and the picker has to
            // show white rather than black - 0,0,0 means "unset" everywhere
            // in this document, and a swatch that says black about a white
            // light is a swatch that lies.
            {
                float lc[3] = { r.light_color.x, r.light_color.y, r.light_color.z };
                if (lc[0] == 0.0f && lc[1] == 0.0f && lc[2] == 0.0f) { lc[0] = lc[1] = lc[2] = 1.0f; }
                if (dai_ui_color(p->ui, "Colour", lc, "lightcol"))
                    r.light_color = dai_vec3{ lc[0], lc[1], lc[2] };
            }
            dai_ui_num_field(p->ui, "Intensity", &r.light_intensity, 0.01f, 0.0f, 100.0f, "lpower");
            if (r.light != 3) dai_ui_num_field(p->ui, "Range", &r.light_range, 0.05f, 0.01f, 1000.0f, "lrange");
            if (r.light == 2) dai_ui_num_field(p->ui, "Cone", &r.light_cone, 0.5f, 1.0f, 89.0f, "lcone");
        }
    }

    // ---- Sprite (2D) --------------------------------------------------------
    if (r.sprite) {
        int on = 1;
        {
            int hrc = dai_ui_header_icon_col(p->ui, DAI_ICON_C_SPRITE, rgba(0x6F, 0xC7, 0xEA, 255), "Sprite", &p->fold_sprite, &on);
            if (hrc == 2 && !on) r.sprite = 0;
            else if (hrc == 3) {
                p->comp_menu_target = 5;
                float cmx = 0, cmy = 0;
                dai_ui_mouse(p->ui, &cmx, &cmy, nullptr, nullptr);
                dai_ui_popup_open(&p->menu_comp, cmx, cmy);
            }
        }
        if (p->fold_sprite && r.sprite) {
            dai_ui_label(p->ui, "texture comes from the Asset field above");
            dai_ui_num_vec3(p->ui, "Size", &r.sprite_size.x, 0.01f);
        }
    }

    // ---- Image (UI) ----------------------------------------------------------
    // The screen space picture: a panel, a heart, a crosshair. This is what
    // the old Sprite checkbox was meant to be - that one set a flag NOTHING
    // read, so it drew nothing, ever.
    if (r.image_on) {
        int on = 1;
        {
            int hrc = dai_ui_header_icon_col(p->ui, DAI_ICON_C_SPRITE, rgba(0x6F, 0xC7, 0xEA, 255),
                                             "Image (UI)", &p->fold_image, &on);
            if (hrc == 2 && !on) r.image_on = 0;
            else if (hrc == 3) {
                p->comp_menu_target = 8;
                float cmx = 0, cmy = 0;
                dai_ui_mouse(p->ui, &cmx, &cmy, nullptr, nullptr);
                dai_ui_popup_open(&p->menu_comp, cmx, cmy);
            }
        }
        if (p->fold_image && r.image_on) {
            std::string idisp = r.image[0] ? base_of(r.image) : std::string("None (Texture)");
            int irc = dai_ui_object_field(p->ui, "Image", idisp.c_str(), DAI_ICON_IMAGE);
            if (irc == 2) {
                float imx = 0, imy = 0;
                dai_ui_mouse(p->ui, &imx, &imy, nullptr, nullptr);
                dai_ui_searchlist_open(&p->image_list, imx - 210.0f, imy);
                p->image_list.wants_focus = 1;
                std::snprintf(p->image_list.hint, sizeof(p->image_list.hint), "Search textures...");
                p->image_pick_node = n;
            }
            float iw = r.image_w, ih = r.image_h;
            if (dai_ui_num_field(p->ui, "Width", &iw, 1.0f, 0.0f, 8000.0f, "imgw")) r.image_w = iw;
            if (dai_ui_num_field(p->ui, "Height", &ih, 1.0f, 0.0f, 8000.0f, "imgh")) r.image_h = ih;
            r.image_w = iw; r.image_h = ih;
            dai_ui_help(p->ui, "0 = the file's own size in pixels.");
            {
                float ic[3] = { r.image_color.x, r.image_color.y, r.image_color.z };
                if (ic[0] == 0.0f && ic[1] == 0.0f && ic[2] == 0.0f) { ic[0] = ic[1] = ic[2] = 1.0f; }
                if (dai_ui_color(p->ui, "Tint", ic, "imgcol"))
                    r.image_color = dai_vec3{ ic[0], ic[1], ic[2] };
            }
            static const char *const ANCHOR2[] = {
                "Top left", "Top", "Top right", "Left", "Centre", "Right",
                "Bottom left", "Bottom", "Bottom right",
            };
            dai_ui_option(p->ui, "Anchor", &r.image_anchor, ANCHOR2, 9);
            float ioff[2] = { r.image_x, r.image_y };
            if (dai_ui_num_field(p->ui, "Offset X", &ioff[0], 1.0f, -8000.0f, 8000.0f, "imgox"))
                r.image_x = ioff[0];
            if (dai_ui_num_field(p->ui, "Offset Y", &ioff[1], 1.0f, -8000.0f, 8000.0f, "imgoy"))
                r.image_y = ioff[1];
            r.image_x = ioff[0]; r.image_y = ioff[1];
        }
    }

    // ---- Button (UI) --------------------------------------------------------
    // Deliberately AFTER Image and before Text: it is the thing that makes
    // those two answer the pointer, and reading the inspector top to bottom
    // is then the same order as building one - a background, a reaction, a
    // label.
    if (r.button_on) {
        int on = 1;
        {
            int hrc = dai_ui_header_icon_col(p->ui, DAI_ICON_C_SPRITE, rgba(0x7E, 0xD3, 0x9B, 255),
                                             "Button (UI)", &p->fold_button, &on);
            if (hrc == 2 && !on) r.button_on = 0;
            else if (hrc == 3) {
                p->comp_menu_target = 9;
                float cmx = 0, cmy = 0;
                dai_ui_mouse(p->ui, &cmx, &cmy, nullptr, nullptr);
                dai_ui_popup_open(&p->menu_comp, cmx, cmy);
            }
        }
        if (p->fold_button && r.button_on) {
            if (!r.image_on && !r.text_on)
                dai_ui_label(p->ui, "add an Image or a Text - a button needs something to be");
            {
                char abuf[64];
                std::snprintf(abuf, sizeof(abuf), "%s", r.button_action);
                if (dai_ui_input_text(p->ui, "On click", abuf, sizeof(abuf)))
                    std::snprintf(r.button_action, sizeof(r.button_action), "%s", abuf);
            }
            dai_ui_help(p->ui, "The function in this object's script. Empty = onClick().");
            {
                float hc[3] = { r.button_hover.x, r.button_hover.y, r.button_hover.z };
                if (dai_ui_color(p->ui, "Hover", hc, "btnhov"))
                    r.button_hover = dai_vec3{ hc[0], hc[1], hc[2] };
            }
            dai_ui_help(p->ui, "black = automatic: 18% brighter under the pointer.");
            {
                float pc[3] = { r.button_press.x, r.button_press.y, r.button_press.z };
                if (dai_ui_color(p->ui, "Pressed", pc, "btnprs"))
                    r.button_press = dai_vec3{ pc[0], pc[1], pc[2] };
            }
            dai_ui_help(p->ui, "black = automatic: 22% darker while held.");
            dai_ui_label(p->ui, "fires on release INSIDE - let go outside cancels");
        }
    }

    // ---- Text ---------------------------------------------------------------
    // The game's own UI. Drawn on the picture, not in the world: a score
    // belongs to a corner of the screen, and a corner is where it has to stay
    // when the window is a different size.
    if (r.text_on) {
        int on = 1;
        {
            int hrc = dai_ui_header_icon_col(p->ui, DAI_ICON_C_TEXT, rgba(0x9C, 0xD6, 0x8C, 255),
                                             "Text", &p->fold_text, &on);
            if (hrc == 2 && !on) r.text_on = 0;
            else if (hrc == 3) {
                p->comp_menu_target = 7;
                float cmx = 0, cmy = 0;
                dai_ui_mouse(p->ui, &cmx, &cmy, nullptr, nullptr);
                dai_ui_popup_open(&p->menu_comp, cmx, cmy);
            }
        }
        if (p->fold_text && r.text_on) {
            // Multi-line: Enter puts in a line break instead of committing.
            // A HUD label is a paragraph as often as it is a word, and
            // "<br>" was the only way to get a second line.
            char tbuf[192];
            std::snprintf(tbuf, sizeof(tbuf), "%s", r.text);
            if (dai_ui_input_multiline(p->ui, "Text", tbuf, sizeof(tbuf), 3))
                std::snprintf(r.text, sizeof(r.text), "%s", tbuf);
            dai_ui_help(p->ui, "The words, or \"@key\" to look them up in the "
                               "project's string table");
            dai_ui_label(p->ui, "markup: <b> <u> <s> <color=#ff0>   Enter = new line");
            dai_ui_label(p->ui, "variables: Points: {score}");
            dai_ui_help(p->ui, "{name} is read from this object's script while it plays: "
                               "a global, a field on state, or a @param. In the editor "
                               "the placeholder stays visible so you can see where it goes.");
            dai_ui_help(p->ui, "HTML-style tags, Unity's spelling. No <i>: an italic "
                               "glyph needs an italic font - put one in Font below. "
                               "Anything that is not a known tag stays as typed.");
            // What a PLAYER would see. A key on its own tells you the label
            // is localised; it does not tell you whether the table has it.
            if (r.text[0] == '@') {
                const char *shown = p->tr_fn ? p->tr_fn(r.text, p->tr_user) : nullptr;
                char line[240];
                if (!p->tr_fn)
                    std::snprintf(line, sizeof(line), "no string table loaded - it will show the key");
                else if (shown && shown[0] && std::strcmp(shown, r.text + 1) != 0)
                    std::snprintf(line, sizeof(line), "shows: %s", shown);
                else
                    std::snprintf(line, sizeof(line),
                                  "no entry for %s in the current language - it will show the key",
                                  r.text + 1);
                dai_ui_label(p->ui, line);
            }
            if (r.text_size <= 0.0f) r.text_size = 24.0f;
            dai_ui_num_field(p->ui, "Size", &r.text_size, 0.5f, 4.0f, 400.0f, "textsize");
            // The typeface. A .ttf in the project, or empty for the one the
            // editor itself uses - a label has to draw before anyone has
            // gone looking for a font, or the component looks broken on the
            // day it is added.
            {
                std::string fdisp = r.text_font[0] ? base_of(r.text_font)
                                                   : std::string("Default (system UI)");
                int frc = dai_ui_object_field(p->ui, "Font", fdisp.c_str(), DAI_ICON_C_TEXT);
                if (frc == 2) {
                    float fmx = 0, fmy = 0;
                    dai_ui_mouse(p->ui, &fmx, &fmy, nullptr, nullptr);
                    dai_ui_searchlist_open(&p->font_list, fmx - 210.0f, fmy);
                    p->font_list.wants_focus = 1;
                    std::snprintf(p->font_list.hint, sizeof(p->font_list.hint), "Search fonts...");
                    p->font_pick_node = n;
                }
            }
            {
                float tc[3] = { r.text_color.x, r.text_color.y, r.text_color.z };
                if (tc[0] == 0.0f && tc[1] == 0.0f && tc[2] == 0.0f) { tc[0] = tc[1] = tc[2] = 1.0f; }
                if (dai_ui_color(p->ui, "Color", tc, "textcol"))
                    r.text_color = dai_vec3{ tc[0], tc[1], tc[2] };
            }
            static const char *const ANCHOR[] = {
                "Top left", "Top", "Top right",
                "Left", "Centre", "Right",
                "Bottom left", "Bottom", "Bottom right",
            };
            dai_ui_option(p->ui, "Anchor", &r.text_anchor, ANCHOR, 9);
            static const char *const TALIGN[] = { "Auto", "Left", "Centre", "Right" };
            dai_ui_seg_buttons(p->ui, "Alignment", &r.text_align, TALIGN, 4);
            dai_ui_help(p->ui, "Where the LINES sit inside the block. The anchor says "
                               "where the block sits on screen - two different questions.");
            // The box. 0 wide means "as wide as the words", which is what a
            // score wants; a real width is what a subtitle wants.
            float bw2 = r.text_w, bh2 = r.text_h;
            if (dai_ui_num_field(p->ui, "Box W", &bw2, 1.0f, 0.0f, 8000.0f, "textbw")) r.text_w = bw2;
            if (dai_ui_num_field(p->ui, "Box H", &bh2, 1.0f, 0.0f, 8000.0f, "textbh")) r.text_h = bh2;
            r.text_w = bw2; r.text_h = bh2;
            dai_ui_help(p->ui, "0 = no box: one line, as wide as the words. With a width "
                               "the text wraps; with a height, autosize can shrink it.");
            int fit = r.text_autosize;
            if (dai_ui_checkbox(p->ui, "Autosize", &fit)) r.text_autosize = fit;
            dai_ui_help(p->ui, "Shrink the text until it fits the box. Needs a box height. "
                               "This is what saves a layout when a translation is a third "
                               "longer than the language it was drawn for.");
            float off[2] = { r.text_x, r.text_y };
            if (dai_ui_num_field(p->ui, "Offset X", &off[0], 1.0f, -8000.0f, 8000.0f, "textox"))
                r.text_x = off[0];
            if (dai_ui_num_field(p->ui, "Offset Y", &off[1], 1.0f, -8000.0f, 8000.0f, "textoy"))
                r.text_y = off[1];
            r.text_x = off[0]; r.text_y = off[1];
        }
    }

    // ---- Audio Source -------------------------------------------------------
    if (r.audio_event[0] || r.audio_autoplay || r.audio_bus) {
        int on = 1;
        {
            int hrc = dai_ui_header_icon_col(p->ui, DAI_ICON_C_AUDIO, rgba(0xF2, 0x9D, 0x5B, 255), "Audio Source", &p->fold_audio, &on);
            if (hrc == 2 && !on) { r.audio_event[0] = 0; r.audio_autoplay = 0; r.audio_bus = 0; }
            else if (hrc == 3) {
                p->comp_menu_target = 6;
                float cmx = 0, cmy = 0;
                dai_ui_mouse(p->ui, &cmx, &cmy, nullptr, nullptr);
                dai_ui_popup_open(&p->menu_comp, cmx, cmy);
            }
        }
        if (p->fold_audio && (r.audio_event[0] || r.audio_autoplay || r.audio_bus)) {
            if (p->audio_buf_node != n) {
                std::snprintf(p->audio_buf, sizeof(p->audio_buf), "%s", r.audio_event);
                p->audio_buf_node = n;
            }
            if (dai_ui_input_text(p->ui, "Event", p->audio_buf, sizeof(p->audio_buf)))
                std::snprintf(r.audio_event, sizeof(r.audio_event), "%s", p->audio_buf);
            static const char *const BUSN[] = { "Master", "Music", "SFX", "UI" };
            dai_ui_option(p->ui, "Bus", &r.audio_bus, BUSN, 4);
            dai_ui_num_field(p->ui, "Volume", &r.audio_volume, 0.01f, 0.0f, 1.0f, "avol");
            int lp = r.audio_loop, ap = r.audio_autoplay;
            if (dai_ui_checkbox(p->ui, "Loop", &lp)) r.audio_loop = lp;
            if (dai_ui_checkbox(p->ui, "Play on start", &ap)) r.audio_autoplay = ap;
        }
    }

    // ---- Remove Component ---------------------------------------------------
    // Add and Remove are siblings, not the same toggle in two coats: a right
    // click on the component title removes it, like Unity's gear menu.
    if (p->component_remove > 0) {
        dai_doc_begin(d, "Remove Component");
        if (p->component_remove == 1) r.no_rigidbody = 1;
        else if (p->component_remove == 2) r.no_collider = 1;
        else if (p->component_remove == 3) r.camera = 0;
        else if (p->component_remove == 4) r.light = 0;
        else if (p->component_remove == 5) r.sprite = 0;
        else if (p->component_remove == 6) {
            r.audio_event[0] = 0; r.audio_autoplay = 0; r.audio_bus = 0;
        }
        r.no_body = (r.no_rigidbody && r.no_collider) ? 1 : 0;
        dai_editor_ui_toast(p, "component removed", 2.0f);
        p->component_remove = 0;
    }

    // ---- Add Component ------------------------------------------------------
    // Unity's button, and the honest version of it: a Daidalos node HAS its
    // slots (a body, a collider, scripts) - adding one switches it on and
    // gives it sane values, removing one switches it off. The mesh is never
    // touched by either, which is the bug this replaced.
    dai_ui_separator(p->ui);
    if (dai_ui_button_fit(p->ui, "Add Component...")) {
        float mx2 = 0, my2 = 0;
        dai_ui_mouse(p->ui, &mx2, &my2, nullptr, nullptr);
        dai_ui_searchlist_open(&p->addcomp_list, mx2, my2);
        p->addcomp_list.wants_focus = 1;
        std::snprintf(p->addcomp_list.hint, sizeof(p->addcomp_list.hint),
                      "Search components...");
        p->addcomp_node = dai_editor_selected(p->ed, 0);
    }

    // Room to scroll past the end. Without it the last widget sat ON the
    // bottom edge, and anything that opens DOWNWARDS - the colour wheel is
    // the obvious one - had nowhere to go: its numbers were simply cut off
    // and there was no way to scroll to them.
    dai_ui_spacing(p->ui, 220.0f);

    // While a hierarchy node is being dragged it BECAME the selection on
    // press - but the reference belongs to the object the user was looking
    // at. Its drop fields stay open here until the button comes up.
    if (p->drag_node != DAI_INVALID_NODE && p->drag_ref_target != DAI_INVALID_NODE &&
        p->params_fn) {
        dai_doc *doc2 = dai_editor_doc(p->ed);
        dai_node_desc tr{}, dr{};
        if (dai_doc_get(doc2, p->drag_ref_target, &tr) == DAI_OK && tr.script[0]) {
            std::string dn = "node";
            if (dai_doc_get(doc2, p->drag_node, &dr) == DAI_OK && dr.name[0]) dn = dr.name;
            dai_ui_separator(p->ui);
            dai_ui_label_fmt(p->ui, "Assign %s to:", dn.c_str());
            std::vector<std::string> tlist = script_list(tr.script);
            const char *hot = dai_ui_hot_label(p->ui);
            for (size_t si = 0; si < tlist.size(); ++si) {
                char keys[4096] = { 0 };
                p->params_fn(entry_path(tlist[si]).c_str(), keys, sizeof(keys), p->params_user);
                for (const ParamDecl &pd : parse_params(keys)) {
                    if (pd.type != PARAM_NODE) continue;   // a float takes no object
                    std::string val = entry_param(tlist[si], pd.name);
                    char pl[384];
                    std::snprintf(pl, sizeof(pl), "%s: %s", pd.name.c_str(),
                                  val.empty() ? "none" : val.c_str());
                    dai_ui_button(p->ui, pl);
                    if (hot && std::strcmp(hot, pl) == 0) {
                        p->param_hover_entry = (int)si;
                        std::snprintf(p->param_hover_key, sizeof(p->param_hover_key),
                                      "%s", pd.name.c_str());
                    }
                }
            }
        }
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
        // ...and the same CHANGE - not the same record - into the rest of the
        // selection. Copying the whole struct would give twelve crates one
        // position, one name and one parent, which is not editing twelve
        // crates, it is replacing eleven of them.
        if (sel > 1) {
            for (uint32_t si = 1; si < sel; ++si) {
                dai_node other = dai_editor_selected(p->ed, si);
                dai_node_desc t{};
                if (dai_doc_get(d, other, &t) != DAI_OK) continue;
                dai_node_desc t0 = t;
// memcpy, not '=': half of these fields are char arrays, and an array is
// the one type in C++ that cannot be assigned.
#define DAI_MF(field) \
    if (std::memcmp(&before.field, &r.field, sizeof(r.field)) != 0) \
        std::memcpy(&t.field, &r.field, sizeof(r.field))
                // NOT name and NOT parent: those two are what makes an object
                // that object, and there is no reading of "multi-edit" where
                // eight objects should end up with one name.
                DAI_MF(tag);
                DAI_MF(position); DAI_MF(rotation); DAI_MF(scale);
                DAI_MF(shape); DAI_MF(motion); DAI_MF(half_extent); DAI_MF(trigger);
                DAI_MF(collider_center); DAI_MF(density); DAI_MF(friction);
                DAI_MF(restitution); DAI_MF(no_sleeping); DAI_MF(freeze);
                DAI_MF(no_body); DAI_MF(no_collider); DAI_MF(no_rigidbody);
                DAI_MF(script);
                DAI_MF(camera); DAI_MF(camera_fov); DAI_MF(camera_size);
                DAI_MF(light); DAI_MF(light_color); DAI_MF(light_range);
                DAI_MF(light_intensity); DAI_MF(light_cone);
                DAI_MF(sprite); DAI_MF(sprite_size);
                DAI_MF(text_on); DAI_MF(text); DAI_MF(text_size); DAI_MF(text_color);
                DAI_MF(text_anchor); DAI_MF(text_x); DAI_MF(text_y); DAI_MF(text_font);
                DAI_MF(audio_event); DAI_MF(audio_bus); DAI_MF(audio_volume);
                DAI_MF(audio_loop); DAI_MF(audio_autoplay);
                DAI_MF(mesh); DAI_MF(materials); DAI_MF(asset);
                DAI_MF(render_extent); DAI_MF(color); DAI_MF(roughness);
                DAI_MF(emissive); DAI_MF(render_flags);
                DAI_MF(hidden); DAI_MF(disabled);
#undef DAI_MF
                if (std::memcmp(&t0, &t, sizeof(t)) != 0) dai_doc_set(d, other, &t);
            }
        }
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

    // Frame the selection, the way F does in the viewport - and reveal it in
    // the hierarchy, because "where is that object" is two questions.
    dai_ui_toolbar_gap(p->ui, 10.0f);
    if (dai_ui_icon_button(p->ui, DAI_ICON_TARGET, "Focus selection (F)", 0) &&
        dai_editor_selection_count(p->ed) > 0) {
        dai_editor_cam_focus(p->ed);
        p->reveal_selection = 1;
    }

    // Layouts: several named arrangements, Unity's layout dropdown.
    dai_ui_toolbar_gap(p->ui, 10.0f);
    if (dai_ui_icon_button(p->ui, DAI_ICON_LAYOUT, "Layout", p->menu_layout.open)) {
        float lx = 0, ly = 0;
        dai_ui_mouse(p->ui, &lx, &ly, nullptr, nullptr);
        dai_ui_popup_open(&p->menu_layout, lx, ly);
    }
    if (dai_ui_icon_button(p->ui, DAI_ICON_SETTINGS, "Settings", 0)) {
        // Wherever it is - a tab behind another one, a floating window, or
        // nowhere yet - one click brings it to the front. Toggling a flag and
        // hoping was how it ended up selected but never focused.
        p->settings_open = 1;
        dai_dock_open(p->dock, "Settings");
        dai_dock_focus(p->dock, "Settings");
    }
    // Window menu: every panel the editor knows, one click to bring it back.
    // Closing a panel used to be one-way - it was gone until restart.
    if (dai_ui_icon_button(p->ui, DAI_ICON_WINDOW, "Window", 0)) {
        float wx = 0, wy = 0;
        dai_ui_mouse(p->ui, &wx, &wy, nullptr, nullptr);
        dai_ui_popup_open(&p->menu_window, wx, wy);
    }
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
        if (!dai_editor_project_seg(p->ed, l.a, l.b, &ax, &ay, &bx, &by)) continue;
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
    // Where the object REALLY is, orientation included. Asking for the live
    // position and then keeping the document's rotation is what left the green
    // outline behind a spinning body - measured at 114 degrees of lie.
    if (!dai_editor_live_transform(p->ed, n, &wp, &wr, &ws))
        dai_doc_world_transform(d, n, &wp, &wr, &ws);
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
    } else if (r.shape == DAI_SHAPE_CYLINDER) {
        out->half = { r.half_extent.x * ax, r.half_extent.y * ay, r.half_extent.x * ax };
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
    // Clipped against the near plane, so wires stay drawn when the camera
    // flies through a collider or the camera-frustum overlay.
    if (!dai_editor_project_seg(p->ed, a, b, &ax, &ay, &bx, &by)) return;
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

// Part of a circle. The capsule needs it: its caps are half circles, and
// drawing them as whole ones puts a ring through the middle of each dome.
void wire_arc(dai_editor_ui *p, dai_vec3 c, dai_quat rot, dai_vec3 ax, dai_vec3 ay,
              float r, float t0, float t1, uint32_t col, float thick) {
    const int N = 16;
    dai_vec3 prev{};
    for (int i = 0; i <= N; ++i) {
        float t = t0 + (t1 - t0) * (float)i / (float)N;
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
        // THE CAPS. Without these four arcs the outline is two circles joined
        // by four straight lines - which is a cylinder, drawn around a shape
        // that collides like a capsule. The wire said one thing and the
        // physics did another, and the wire is what you look at.
        const float PI = 3.14159265f;
        wire_arc(p, top, c.rot, dai_vec3{1,0,0}, dai_vec3{0,1,0},  rad, 0.0f, PI, col, thick);
        wire_arc(p, top, c.rot, dai_vec3{0,0,1}, dai_vec3{0,1,0},  rad, 0.0f, PI, col, thick);
        wire_arc(p, bot, c.rot, dai_vec3{1,0,0}, dai_vec3{0,-1,0}, rad, 0.0f, PI, col, thick);
        wire_arc(p, bot, c.rot, dai_vec3{0,0,1}, dai_vec3{0,-1,0}, rad, 0.0f, PI, col, thick);
        for (int i = 0; i < 4; ++i) {
            float sx = (i == 0) ? rad : (i == 1) ? -rad : 0.0f;
            float sz = (i == 2) ? rad : (i == 3) ? -rad : 0.0f;
            wire_line(p, v_add(top, qrot_v(c.rot, dai_vec3{ sx, 0, sz })),
                         v_add(bot, qrot_v(c.rot, dai_vec3{ sx, 0, sz })), col, thick);
        }
    } else if (c.shape == DAI_SHAPE_CYLINDER) {
        float rad = c.half.x, half_h = c.half.y;
        dai_vec3 top = v_add(c.center, qrot_v(c.rot, dai_vec3{ 0, half_h, 0 }));
        dai_vec3 bot = v_add(c.center, qrot_v(c.rot, dai_vec3{ 0, -half_h, 0 }));
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

void dai_editor_ui_fps(dai_editor_ui *p, float fps) {
    if (p) p->fps_now = fps;
}

uint32_t dai_editor_ui_grid_lines(const dai_editor_ui *p, float *out, uint32_t max_points) {
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
}

// The floor grid, the thing that makes "infinite empty space" readable as a
// floor: a metre-grid on y=0 through the same project path the gizmo uses.
// NOTE: the 2D overlay version is retired - the host draws the grid as
// world-space lines with depth testing (dai_editor_ui_grid_lines), because a
// screen-space overlay shines through every object in the scene.
void dai_editor_ui_grid(dai_editor_ui *p) {
    if (!p || !p->gizmo_grid) return;
    const dai_ui_style *st = dai_ui_style_of(p->ui);
    uint32_t minor = st->panel_border & 0xC0FFFFFFu;   // ~75% opacity
    uint32_t axis  = rgba(120, 170, 230, 160);
    // Segments, near-plane clipped: flying the camera low over the floor used
    // to swallow every line whose near end crossed the near plane - the grid
    // "disappearing" right where you stand. project_seg keeps the visible part.
    for (int g = -20; g <= 20; ++g) {
        if (g == 0) continue;
        float x1, y1, x2, y2;
        if (dai_editor_project_seg(p->ed, dai_vec3{ (float)g, 0, -20 },
                                   dai_vec3{ (float)g, 0,  20 }, &x1, &y1, &x2, &y2))
            dai_ui_line(p->ui, x1, y1, x2, y2, 1.0f, minor);
        if (dai_editor_project_seg(p->ed, dai_vec3{ -20, 0, (float)g },
                                   dai_vec3{  20, 0, (float)g }, &x1, &y1, &x2, &y2))
            dai_ui_line(p->ui, x1, y1, x2, y2, 1.0f, minor);
    }
    float x1, y1, x2, y2;
    if (dai_editor_project_seg(p->ed, dai_vec3{ -20, 0, 0 }, dai_vec3{ 20, 0, 0 },
                               &x1, &y1, &x2, &y2))
        dai_ui_line(p->ui, x1, y1, x2, y2, 1.5f, axis);
    if (dai_editor_project_seg(p->ed, dai_vec3{ 0, 0, -20 }, dai_vec3{ 0, 0, 20 },
                               &x1, &y1, &x2, &y2))
        dai_ui_line(p->ui, x1, y1, x2, y2, 1.5f, axis);
}

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
        // The Camera COMPONENT is the truth now; the old MainCamera tag still
        // counts so scenes made before components keep working.
        if (r.camera != 0 || std::strcmp(r.tag, CAMERA_TAG) == 0) return id;
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
    if (!dai_editor_live_transform(p->ed, cam, &wp, &wr, &ws) &&
        dai_doc_world_transform(d, cam, &wp, &wr, &ws) != DAI_OK) return 0;
    dai_vec3 dir = qrot_v(wr, dai_vec3{ 0, 0, -1 });
    if (eye) *eye = wp;
    if (look) *look = v_add(wp, dir);
    dai_node_desc cr{};
    dai_doc_get(d, cam, &cr);
    if (fov_deg) *fov_deg = cr.camera_fov > 0.0f ? cr.camera_fov : 60.0f;
    return 1;
}

int dai_editor_ui_camera_preview(const dai_editor_ui *p,
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

float dai_editor_ui_game_ortho(const dai_editor_ui *p) {
    if (!p) return 0.0f;
    dai_node cam = find_camera(p);
    if (cam == DAI_INVALID_NODE) return 0.0f;
    dai_node_desc cr{};
    if (dai_doc_get(dai_editor_doc(p->ed), cam, &cr) != DAI_OK) return 0.0f;
    if (cr.camera != 2) return 0.0f;
    return cr.camera_size > 0.0f ? cr.camera_size : 5.0f;
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
    r.camera = 1; r.camera_fov = 60.0f; r.camera_size = 5.0f;
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
        if (!dai_editor_live_transform(p->ed, id, &wp, &wr, &ws))
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

    // Two halves, and the split is the point (it is Unity's): what is above
    // describes THIS DESK and lives outside the project; what is below
    // describes the GAME and lives in the project, in version control, the
    // same for everyone on the team.
    {
        static const char *const TABS[] = { "Preferences", "Project", "Gizmos" };
        dai_ui_segmented(ui, TABS, 3, &p->settings_tab);
    }
    // The Gizmos tab: which overlays show in the scene view.
    if (p->settings_tab == 2) {
        dai_ui_label(ui, "Shown in the scene view:");
        { int g = p->gizmo_grid;      if (dai_ui_checkbox(p->ui, "Floor grid", &g))      p->gizmo_grid = g; }
        { int g = p->gizmo_fps;       if (dai_ui_checkbox(p->ui, "FPS", &g))             p->gizmo_fps = g; }
        { int g = p->gizmo_colliders; if (dai_ui_checkbox(p->ui, "Collider frames", &g)) p->gizmo_colliders = g; }
        { int g = p->gizmo_cameras;   if (dai_ui_checkbox(p->ui, "Camera frustums", &g)) p->gizmo_cameras = g; }
        dai_ui_separator(ui);
        dai_ui_label(ui, "Gizmo size and snapping live in Preferences.");
        return;
    }
    if (p->settings_tab == 1) {
        dai_ui_label(ui, "These belong to the project, not to you:");
        dai_ui_label(ui, "they are saved in settings/project.txt and");
        dai_ui_label(ui, "shared with everyone who opens it.");
        dai_ui_separator(ui);
        if (p->proj_settings_host) {
            p->proj_settings_host(p->proj_user);
        } else {
            dai_ui_label(ui, "no project open");
        }
        return;
    }

    dai_ui_section(ui, "Appearance");
    // Font size is the ONE thing the host owns (it made the font and the
    // texture), so the editor asks. Everything else it can do itself.
    int size_idx = 0;
    // Thresholds sit BETWEEN the offered sizes: 13.0 is "Normal", and a
    // boundary that eats it (13.5) made the option snap back to Klein and
    // look unselectable.
    if (p->settings_font_px >= 14.5f) size_idx = 2;
    else if (p->settings_font_px >= 12.5f) size_idx = 1;
    static const char *const SIZES[] = { "Klein (12)", "Normal (13)", "Gross (16)" };
    if (dai_ui_option(ui, "UI size", &size_idx, SIZES, 3)) {
        float px = size_idx == 0 ? 12.0f : size_idx == 1 ? 13.0f : 16.0f;
        p->settings_font_px = px;
        if (p->apply_font) p->apply_font(px, p->apply_user);
    }

    // The display scale. Auto is right almost everywhere; "almost" is why
    // this row exists - a laptop panel that reports the wrong physical size
    // makes the whole interface half or double the size it should be.
    {
        static const float SCALE_V[6] = { 0.0f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f };
        static const char *const SCALES[] = { "Auto (display)", "100%", "125%",
                                              "150%", "175%", "200%" };
        int si = 0;
        for (int i = 1; i < 6; ++i)
            if (p->settings_ui_scale > SCALE_V[i] - 0.06f &&
                p->settings_ui_scale < SCALE_V[i] + 0.06f) si = i;
        if (dai_ui_option(ui, "UI scale", &si, SCALES, 6)) {
            p->settings_ui_scale = SCALE_V[si];
            if (p->apply_scale) p->apply_scale(SCALE_V[si], p->apply_scale_user);
        }
        char now[64];
        std::snprintf(now, sizeof(now), "now: %.0f%%", dai_ui_scale_get(ui) * 100.0f);
        dai_ui_label(ui, now);
    }

    {
        static const char *const LANGS[] = { "English", "Deutsch" };
        int lang = dai_tr_lang_get();
        if (dai_ui_option(ui, "Language", &lang, LANGS, 2))
            dai_tr_lang(lang == 1 ? DAI_LANG_DE : DAI_LANG_EN);
    }

    static const char *const THEMES[] = { "Unity Dark", "Darker", "Slate" };
    {
        // Which editor a double click on a script opens. Both are real
        // answers: the built-in one is here and instant, the external one has
        // your extensions - and the project carries a .d.ts so it understands
        // the engine either way.
        static const char *const SCRIPT_ED[] = { "Built-in editor", "External (VS Code)" };
        if (dai_ui_option(ui, "Scripts open in", &p->script_external, SCRIPT_ED, 2))
            dai_editor_ui_toast(p, p->script_external ? "scripts open externally"
                                                      : "scripts open in the Script tab", 2.0f);
    }
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
    dai_ui_label(p->ui, "Values apply immediately.");
    // ---- About ------------------------------------------------------------
    // Version, where things are, and whether there is a newer one. Everything
    // a bug report needs, in the one place people already open when something
    // is wrong - and none of it is worth a panel of its own.
    dai_ui_separator(ui);
    dai_ui_label(ui, "About");
    dai_ui_label_fmt(ui, "Version   %s", dai_version());
    dai_ui_label_fmt(ui, "Projects  %s", p->about_projects[0] ? p->about_projects : "(unset)");
    dai_ui_label_fmt(ui, "Assets    %s", p->about_assets[0] ? p->about_assets : "(no project open)");
    if (p->about_status[0]) dai_ui_label_fmt(ui, "Update    %s", p->about_status);
    if (dai_ui_button_fit(ui, "Check for updates")) p->want_update_check = 1;
    if (dai_ui_button_fit(ui, "Copy this to the clipboard")) {
        char all[900];
        std::snprintf(all, sizeof(all),
                      "DAIDALOS %s\nprojects: %s\nassets: %s\nupdate: %s",
                      dai_version(), p->about_projects, p->about_assets,
                      p->about_status[0] ? p->about_status : "not checked");
        dai_editor_ui_clipboard_set(p, 0, all);
        dai_editor_ui_toast(p, "copied - paste it into a bug report", 2.0f);
    }
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
    // Nor may a press that is not IN the scene view at all. "Not over a
    // widget" is not the same thing as "in the 3D view": tab bars, the
    // toolbar and the splitters between panels are all neither, and a click
    // on any of them used to pick - which is why changing tabs deselected.
    if (p->view_w > 0.0f && p->view_h > 0.0f &&
        (mx < p->view_x || mx >= p->view_x + p->view_w ||
         my < p->view_y || my >= p->view_y + p->view_h))
        return 0;

    if (pressed) {
        int axis = p->collider_edit ? DAI_AXIS_NONE : dai_editor_gizmo_hit(p->ed, mx, my);
        if (axis != DAI_AXIS_NONE) {
            dai_editor_drag_begin(p->ed, axis, mx, my);
            p->viewport_dragging = dai_editor_dragging(p->ed) != 0;
            return 1;
        }
        // A UI element drawn over the scene is a thing you can AIM at. Before
        // this, the 3D pick ran first, found nothing behind the label - HUD
        // nodes have no body - and cleared the selection. That is exactly the
        // click you make to grab a panel and resize it, and it threw away the
        // selection instead.
        {
            dai_node hn = DAI_INVALID_NODE;
            if (dai_hud_pick(mx, my, &hn) && hn != DAI_INVALID_NODE) {
                // Already selected: hands off. The move/resize grips take this
                // same press, and re-selecting would cancel the drag.
                if (dai_editor_selection_count(p->ed) > 0 &&
                    dai_editor_selected(p->ed, 0) == hn)
                    return 1;
                dai_editor_select(p->ed, hn, 0);
                return 1;
            }
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
    p->last_ctrl_held = in->key_ctrl != 0;
    p->last_shift_held = in->key_shift != 0;

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
    // The right button in the scene view means "I am flying now": whatever
    // text field still had the keyboard gives it up, or W A S D go on being
    // typed into it and the camera only ever turns.
    if (ci.mouse_right && !p->prev_right_down && !over_ui) dai_ui_text_defocus(p->ui);
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

// The selected asset's path, or "" when the selection no longer names one.
// Never returns a pointer the caller has to test: the whole point is that
// there is exactly one place left where this can be got wrong.
static const char *asset_at(const dai_editor_ui *p, int index) {
    if (!p || index < 0 || index >= (int)p->assets.size()) return "";
    const char *a = p->assets[(size_t)index];
    return a ? a : "";
}

void dai_editor_ui_asset_list(dai_editor_ui *p, const char *const *paths, uint32_t count) {
    if (!p) return;
    p->assets.clear();
    if (paths && count) p->assets.assign(paths, paths + count);
    if (p->asset_sel >= (int)p->assets.size()) p->asset_sel = -1;
}

void dai_editor_ui_folder_list(dai_editor_ui *p, const char *const *paths, uint32_t count) {
    if (!p) return;
    p->folders_disk.clear();
    if (paths && count) p->folders_disk.assign(paths, paths + count);
}

int dai_editor_ui_asset_selected(const dai_editor_ui *p) {
    // Same rule as everywhere else: an index the list no longer has is no
    // selection at all.
    if (p && (p->asset_sel < 0 || p->asset_sel >= (int)p->assets.size())) return -1; return p ? p->asset_sel : -1; }

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
        const char *full = asset_at(p, (int)i);
        const char *slash = std::strrchr(full, '/');
        const char *label = slash ? slash + 1 : full;
        int selected = (int)i == p->asset_sel;
        char row[128];
        std::snprintf(row, sizeof(row), "%s%s", selected ? "> " : "  ", label);
        if (dai_ui_button(p->ui, row)) {
            if (selected && dai_ui_double_click(p->ui)) {
                // Double click opens it - a script in the external editor, the
                // way Unity hands the file to whatever edits it. The host
                // knows the machine; the editor only knows the path.
                if (p->open_asset) p->open_asset(nullptr, full, p->open_asset_user);
            }
            p->asset_sel = selected ? -1 : (int)i;
        }
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
    if (pick && is_behaviour_file(pick)) {
        if (dai_ui_button_fit(p->ui, "Assign to selection") &&
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
    if (dai_ui_button_fit(p->ui, "Place")) {
        if (out_path) *out_path = pick;
        if (out_as_tree) *out_as_tree = 0;
        placed = 1;
    }
    if (dai_ui_button_fit(p->ui, "As tree")) {
        if (out_path) *out_path = pick;
        if (out_as_tree) *out_as_tree = 1;
        placed = 1;
    }
    dai_ui_row_end(p->ui);
    return placed;
}

void dai_editor_ui_layout_dump(const dai_editor_ui *p, char *out, size_t n) {
    if (!p || !out || !n) return;
    // The dock tree in one line: "{ h 0.18 { leaf 0 "Hierarchy" } ... }". A
    // screenshot of a maximised editor across the room is not data; this is.
    dai_dock_dump(p->dock, out, n);
}

// The host owns the disk: it hands the editor a save/load pair so named
// layouts live in files the editor itself never has to know about.
void dai_editor_ui_layout_save_as(dai_editor_ui *p, const char *name) {
    if (!p || !name || !*name || !p->layout_save_host) return;
    char buf[4096];
    size_t n = dai_dock_to_text(p->dock, buf, sizeof(buf));
    if (n) p->layout_save_host(name, buf, n, p->layout_host_user);
}

void dai_editor_ui_layout_host(dai_editor_ui *p,
                                 void (*save)(const char *, const char *, size_t, void *),
                                 int  (*load)(const char *, char *, size_t, void *),
                                 void *user) {
    if (!p) return;
    p->layout_save_host = save;
    p->layout_load_host = load;
    p->layout_host_user = user;
}

int dai_editor_ui_layout_apply_name(dai_editor_ui *p, const char *name) {
    if (!p || !name || !*name || !p->layout_load_host) return 0;
    char buf[4096];
    int n = p->layout_load_host(name, buf, sizeof(buf), p->layout_host_user);
    if (n <= 0) return 0;
    if (dai_dock_from_text(p->dock, buf) == DAI_OK) { p->layout_ready = true; return 1; }
    return 0;
}

void dai_editor_ui_layout_reset(dai_editor_ui *p, float vw, float vh) {
    if (!p) return;
    // The layout every 3D editor opens with: hierarchy on the left, project
    // under it, inspector on the right, scene and game as two tabs of the
    // middle. Registering is idempotent, so this also runs on the first frame.
    dai_dock_reset(p->dock);
    dai_dock_add(p->dock, "Scene", DAI_DOCK_NONE, 0.0f);
    dai_dock_add_tab(p->dock, "Game", "Scene");
    dai_dock_add(p->dock, "Hierarchy", DAI_DOCK_LEFT, 0.18f);
    dai_dock_add(p->dock, "Inspector", DAI_DOCK_RIGHT, 0.20f);
    dai_dock_add(p->dock, "Project", DAI_DOCK_BOTTOM, 0.26f);
    // Registered so the Window menu can list them; they start as tabs of the
    // Project panel rather than stealing space from the scene view.
    // ONLY add_tab: a dai_dock_add() first would carve its own bottom strip
    // (0.26 of what is left, three times over) and then add_tab would see the
    // title already registered and return - which is how a fresh install ended
    // up with a 246px tall scene view in a 720px window.
    dai_dock_add_tab(p->dock, "Console", "Project");
    dai_dock_add_tab(p->dock, "Localisation", "Project");
    // The script editor starts beside the scene, where a code window belongs -
    // as a TAB of it, so it costs no space until something is opened in it.
    dai_dock_add_tab(p->dock, "Script", "Scene");
    dai_dock_add_tab(p->dock, "Audio", "Project");
    p->layout_ready = true;
    p->layout_w = vw; p->layout_h = vh;
}

int dai_editor_ui_game_view_rect(const dai_editor_ui *p, float *x, float *y, float *w, float *h) {
    if (!p || !p->has_game) return 0;
    if (x) *x = p->game_x;
    if (y) *y = p->game_y;
    if (w) *w = p->game_w;
    if (h) *h = p->game_h;
    return 1;
}

void dai_editor_ui_viewport_rect(const dai_editor_ui *p, float *x, float *y, float *w, float *h) {
    if (!p) return;
    if (x) *x = p->view_x;
    if (y) *y = p->view_y;
    if (w) *w = p->view_w;
    if (h) *h = p->view_h;
}

// (the asset-path and script-list helpers live right after the struct now -
// the inspector needs them long before this point)


// The path to a folder opens in the tree: every ancestor expanded.
static void project_expand_to(dai_editor_ui *p, const std::string &dir) {
    p->proj_folds.insert("");
    size_t pos = 0;
    while (pos < dir.size()) {
        size_t slash = dir.find('/', pos);
        p->proj_folds.insert(dir.substr(0, slash));
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
}

// The two right click menus of the hierarchy and the viewport. They are run
// LAST in the frame so they paint above every window, and they mutate through
// the document like every other edit.
// A component's values as one line of text - the same "clipboard is text"
// idea as node copy, so a copied Rigidbody can leave the editor too.
static std::string comp_to_text(int id, const dai_node_desc &r) {
    char b[640];
    switch (id) {
    case 0: std::snprintf(b, sizeof(b), "comp:0 px=%g py=%g pz=%g rx=%g ry=%g rz=%g rw=%g sx=%g sy=%g sz=%g",
                          (double)r.position.x, (double)r.position.y, (double)r.position.z,
                          (double)r.rotation.x, (double)r.rotation.y, (double)r.rotation.z, (double)r.rotation.w,
                          (double)r.scale.x, (double)r.scale.y, (double)r.scale.z); break;
    case 1: std::snprintf(b, sizeof(b), "comp:1 motion=%d density=%g friction=%g restitution=%g",
                          r.motion, (double)r.density, (double)r.friction, (double)r.restitution); break;
    case 2: std::snprintf(b, sizeof(b), "comp:2 shape=%d hx=%g hy=%g hz=%g cx=%g cy=%g cz=%g trigger=%d",
                          r.shape, (double)r.half_extent.x, (double)r.half_extent.y, (double)r.half_extent.z,
                          (double)r.collider_center.x, (double)r.collider_center.y, (double)r.collider_center.z,
                          r.trigger); break;
    case 3: std::snprintf(b, sizeof(b), "comp:3 camera=%d fov=%g size=%g",
                          r.camera, (double)r.camera_fov, (double)r.camera_size); break;
    case 4: std::snprintf(b, sizeof(b), "comp:4 light=%d lr=%g lg=%g lb=%g range=%g intensity=%g cone=%g",
                          r.light, (double)r.light_color.x, (double)r.light_color.y, (double)r.light_color.z,
                          (double)r.light_range, (double)r.light_intensity, (double)r.light_cone); break;
    case 5: std::snprintf(b, sizeof(b), "comp:5 sx=%g sy=%g sz=%g",
                          (double)r.sprite_size.x, (double)r.sprite_size.y, (double)r.sprite_size.z); break;
    case 6: std::snprintf(b, sizeof(b), "comp:6 event=%s bus=%d vol=%g loop=%d autoplay=%d",
                          r.audio_event, r.audio_bus, (double)r.audio_volume, r.audio_loop, r.audio_autoplay); break;
    case 7: std::snprintf(b, sizeof(b), "comp:7 size=%g tr=%g tg=%g tb=%g anchor=%d tx=%g ty=%g str=%s",
                          (double)r.text_size, (double)r.text_color.x, (double)r.text_color.y,
                          (double)r.text_color.z, r.text_anchor, (double)r.text_x, (double)r.text_y,
                          r.text); break;
    default: b[0] = 0; break;
    }
    return b;
}

// One named float/int out of the text line; missing keys keep their value.
static float kv_f(const char *t, const char *key, float def) {
    const char *at = std::strstr(t, key);
    if (!at) return def;
    return (float)std::atof(at + std::strlen(key));
}
static int kv_i(const char *t, const char *key, int def) {
    const char *at = std::strstr(t, key);
    if (!at) return def;
    return std::atoi(at + std::strlen(key));
}

static int comp_from_text(const char *t, int want_id, dai_node_desc *r) {
    char prefix[16];
    std::snprintf(prefix, sizeof(prefix), "comp:%d ", want_id);
    if (!t || std::strncmp(t, prefix, std::strlen(prefix)) != 0) {
        // "comp:0" with no field would never match - space is part of prefix.
        std::snprintf(prefix, sizeof(prefix), "comp:%d", want_id);
        size_t pl = std::strlen(prefix);
        if (!t || std::strncmp(t, prefix, pl) != 0 || (t[pl] && t[pl] != ' ')) return 0;
    }
    switch (want_id) {
    case 0:
        r->position = { kv_f(t,"px=",r->position.x), kv_f(t,"py=",r->position.y), kv_f(t,"pz=",r->position.z) };
        r->rotation = { kv_f(t,"rx=",r->rotation.x), kv_f(t,"ry=",r->rotation.y),
                        kv_f(t,"rz=",r->rotation.z), kv_f(t,"rw=",r->rotation.w) };
        r->scale    = { kv_f(t,"sx=",r->scale.x), kv_f(t,"sy=",r->scale.y), kv_f(t,"sz=",r->scale.z) };
        break;
    case 1:
        r->motion = kv_i(t,"motion=",r->motion);
        r->density = kv_f(t,"density=",r->density);
        r->friction = kv_f(t,"friction=",r->friction);
        r->restitution = kv_f(t,"restitution=",r->restitution);
        break;
    case 2:
        r->shape = kv_i(t,"shape=",r->shape);
        r->half_extent = { kv_f(t,"hx=",r->half_extent.x), kv_f(t,"hy=",r->half_extent.y), kv_f(t,"hz=",r->half_extent.z) };
        r->collider_center = { kv_f(t,"cx=",r->collider_center.x), kv_f(t,"cy=",r->collider_center.y), kv_f(t,"cz=",r->collider_center.z) };
        r->trigger = kv_i(t,"trigger=",r->trigger);
        break;
    case 3:
        r->camera = kv_i(t,"camera=",r->camera);
        r->camera_fov = kv_f(t,"fov=",r->camera_fov);
        r->camera_size = kv_f(t,"size=",r->camera_size);
        if (r->camera && r->tag[0] == 0) std::snprintf(r->tag, sizeof(r->tag), "%s", CAMERA_TAG);
        break;
    case 4:
        r->light = kv_i(t,"light=",r->light);
        r->light_color = { kv_f(t,"lr=",r->light_color.x), kv_f(t,"lg=",r->light_color.y), kv_f(t,"lb=",r->light_color.z) };
        r->light_range = kv_f(t,"range=",r->light_range);
        r->light_intensity = kv_f(t,"intensity=",r->light_intensity);
        r->light_cone = kv_f(t,"cone=",r->light_cone);
        break;
    case 5:
        r->sprite = 1;
        r->sprite_size = { kv_f(t,"sx=",1.0f), kv_f(t,"sy=",1.0f), kv_f(t,"sz=",1.0f) };
        break;
    case 7: {
        r->text_on = 1;
        r->text_size = kv_f(t,"size=",24.0f);
        r->text_color = { kv_f(t,"tr=",0.0f), kv_f(t,"tg=",0.0f), kv_f(t,"tb=",0.0f) };
        r->text_anchor = kv_i(t,"anchor=",0);
        r->text_x = kv_f(t,"tx=",0.0f);
        r->text_y = kv_f(t,"ty=",0.0f);
        // str= is LAST in the line on purpose: a label may contain spaces, so
        // it takes everything that is left rather than stopping at one.
        const char *st2 = std::strstr(t, "str=");
        if (st2) std::snprintf(r->text, sizeof(r->text), "%s", st2 + 4);
        break;
    }
    case 6: {
        const char *ev = std::strstr(t, "event=");
        if (ev) {
            ev += 6;
            size_t el = 0;
            while (ev[el] && ev[el] != ' ' && el < sizeof(r->audio_event) - 1) ++el;
            std::memcpy(r->audio_event, ev, el);
            r->audio_event[el] = 0;
        }
        r->audio_bus = kv_i(t,"bus=",r->audio_bus);
        r->audio_volume = kv_f(t,"vol=",r->audio_volume);
        r->audio_loop = kv_i(t,"loop=",r->audio_loop);
        r->audio_autoplay = kv_i(t,"autoplay=",r->audio_autoplay);
        break;
    }
    default: return 0;
    }
    return 1;
}

static void run_context_menus(dai_editor_ui *p) {
    dai_doc *d = dai_editor_doc(p->ed);

    // Named layouts: the current one saved under a new name, or one picked
    // from disk and applied.
    if (p->menu_layout.open) {
        const char *defs[] = { "Default", "2 by 3", "Tall", "Wide" };
        dai_ui_menu_item items[12];
        int n = 0;
        for (const char *df : defs) {
            items[n++] = { DAI_ICON_LAYOUT, df, nullptr };
        }
        items[n++] = { DAI_ICON_SAVE, "Save current as: (type name below)", nullptr };
        int pick = dai_ui_popup_menu(p->ui, &p->menu_layout, items, n);
        if (pick >= 0 && pick < 4) {
            if (dai_editor_ui_layout_apply_name(p, defs[pick])) {
                dai_editor_ui_toast(p, "layout loaded", 2.0f);
            } else {
                // No saved preset by that name yet: reset to the base layout.
                dai_editor_ui_layout_reset(p, p->layout_w, p->layout_h);
                dai_editor_ui_toast(p, "layout: default", 2.0f);
            }
        } else if (pick == 4) {
            // Name it after the focused panel if the user gave no name - that
            // is the layout they were actually shaping.
            std::string nm = p->layout_name_buf[0] ? p->layout_name_buf : "my layout";
            dai_editor_ui_layout_save_as(p, nm.c_str());
            char msg[96];
            std::snprintf(msg, sizeof(msg), "layout saved as '%s'", nm.c_str());
            dai_editor_ui_toast(p, msg, 2.0f);
            p->layout_name_buf[0] = 0;
        }
        (void)p;
    }

    // The Window menu, built from what the dock actually knows - so a panel
    // added later shows up here without anyone remembering to list it.
    if (p->menu_window.open) {
        // The dock's own register, plus the set this editor always has. A
        // layout loaded from disk restores the TREE, not the register - so
        // after the first restart the menu was empty and the button did
        // nothing at all, which is exactly what it looked like.
        static const char *const ALWAYS[] = { "Scene", "Game", "Hierarchy", "Inspector",
                                              "Project", "Console", "Localisation", "Audio", "Settings" };
        const char *names[16];
        uint32_t n = dai_dock_panels(p->dock, names, 16);
        if (n > 16) n = 16;
        for (uint32_t k = 0; k < 8 && n < 16; ++k) {
            bool have = false;
            for (uint32_t i = 0; i < n; ++i)
                if (std::strcmp(names[i], ALWAYS[k]) == 0) { have = true; break; }
            if (!have) names[n++] = ALWAYS[k];
        }
        dai_ui_menu_item items[16];
        char labels[16][64];
        for (uint32_t i = 0; i < n; ++i) {
            int open = dai_dock_is_open(p->dock, names[i]);
            std::snprintf(labels[i], sizeof(labels[i]), "%s%s", open ? "" : "+ ", names[i]);
            items[i] = { open ? DAI_ICON_CHECK : DAI_ICON_PLUS, labels[i], nullptr };
        }
        int pick = dai_ui_popup_menu(p->ui, &p->menu_window, items, (int)n);
        if (pick >= 0 && pick < (int)n) {
            dai_dock_open(p->dock, names[pick]);
            if (std::strcmp(names[pick], "Settings") == 0) p->settings_open = 1;
        }
    }

    // The mesh picker's list: every builtin plus the two "derive it" entries.
    // Opened by the object field in the inspector, applied to the node it was
    // opened for - not to the selection, which can change while it is open.
    if (p->menu_mesh.open) {
        std::vector<std::string> names;
        std::vector<uint32_t>    ids;
        names.push_back("From shape (auto)"); ids.push_back(0xFFFFFFFFu);
        names.push_back("From asset file");   ids.push_back(0xFFFFFFFEu);
        for (uint32_t i = 0; i < p->mesh_count && i < 64; ++i) {
            const char *nm = p->mesh_name ? p->mesh_name(i, p->mesh_user) : nullptr;
            char buf[64];
            if (!nm) { std::snprintf(buf, sizeof(buf), "mesh %u", i); nm = buf; }
            names.push_back(nm); ids.push_back(i);
        }
        std::vector<dai_ui_menu_item> items(names.size());
        for (size_t i = 0; i < names.size(); ++i) {
            items[i].icon = i < 2 ? DAI_ICON_RESET : DAI_ICON_CUBE;
            items[i].label = names[i].c_str();
            items[i].shortcut = nullptr;
        }
        int pick = dai_ui_popup_menu(p->ui, &p->menu_mesh, items.data(), (uint32_t)items.size());
        if (pick >= 0 && pick < (int)ids.size() && p->mesh_menu_node != DAI_INVALID_NODE) {
            dai_doc *md = dai_editor_doc(p->ed);
            dai_node_desc mr{};
            if (dai_doc_get(md, p->mesh_menu_node, &mr) == DAI_OK) {
                dai_doc_begin(md, "Mesh");
                mr.mesh = ids[(size_t)pick];
                dai_doc_set(md, p->mesh_menu_node, &mr);
                dai_doc_commit(md);
                dai_editor_resync(p->ed);
            }
            p->mesh_menu_node = DAI_INVALID_NODE;
        }
    }

    // Add Component, the Unity shape: a search field over everything you
    // could add - the built-in components AND every behaviour file in the
    // project. Flat lists stop working the day a project has forty scripts,
    // and forty scripts is a Tuesday.
    if (p->addcomp_list.open && p->addcomp_node != DAI_INVALID_NODE) {
        struct CompEntry { std::string label; std::string cat; int kind; std::string path; };
        // kind: 0 add rigidbody, 1 add collider, 2 add camera, 3 add light,
        //       4 add sprite, 5 add audio, 6 attach the behaviour in `path`.
        dai_node_desc ar2{};
        int have_node = dai_doc_get(d, p->addcomp_node, &ar2) == DAI_OK;
        std::vector<CompEntry> entries;
        if (have_node) {
            if (ar2.no_rigidbody) entries.push_back({ "Rigidbody", "Physics", 0, "" });
            if (ar2.no_collider)  entries.push_back({ "Collider", "Physics", 1, "" });
            if (!ar2.camera)      entries.push_back({ "Camera", "Rendering", 2, "" });
            if (!ar2.light)       entries.push_back({ "Light", "Rendering", 3, "" });
            // No "Sprite" entry any more. It set a flag nothing read - a
            // component that cannot be seen after it is added is worse than
            // one that is missing, because the second one you go and look
            // for. Image (UI) is what it was trying to be.
            if (!ar2.text_on)     entries.push_back({ "Text (UI)", "Rendering", 7, "" });
            if (!ar2.image_on)    entries.push_back({ "Image (UI)", "Rendering", 8, "" });
            if (!ar2.button_on)   entries.push_back({ "Button (UI)", "Rendering", 9, "" });
            if (!ar2.audio_event[0]) entries.push_back({ "Audio Source", "Audio", 5, "" });
            // NO "Remove X" entries. A menu called Add Component that offers
            // to remove things is a menu you have to read twice, and the
            // component is already removable where it lives: the header's
            // tick box switches it off, its context menu takes it away. A
            // component that is on the object simply does not appear here -
            // that IS the feedback, and it is the same one Unity gives.
            (void)0;
        }
        // Every script in the project is a component. Listed with its folder,
        // because two scripts called "player" in different folders are not
        // the same thing.
        for (const char *a : p->assets) {
            if (!a || !is_behaviour_file(a)) continue;
            bool on_node = false;
            if (have_node && ar2.script[0]) {
                std::string cur;
                for (const char *c = ar2.script; ; ++c) {
                    if (*c == ';' || !*c) {
                        if (cur == a) on_node = true;
                        cur.clear();
                        if (!*c) break;
                    } else cur += *c;
                }
            }
            if (on_node) continue;
            // Name without the extension, folder in front when it is not at
            // the top: two scripts called "player" in different folders are
            // not the same component, and "player.js" tells you nothing that
            // the icon has not already said.
            std::string b = base_of(a);
            size_t dot = b.find_last_of('.');
            if (dot != std::string::npos && dot > 0) b = b.substr(0, dot);
            std::string dir = parent_of(a);
            if (!dir.empty()) b = base_of(dir) + "/" + b;
            entries.push_back({ b, "Scripts", 6, a });
        }

        // Category headers, Unity's grey separators: they are items too, so
        // the keyboard can walk past them without treating them as picks.
        std::vector<std::string> flat;
        std::vector<int>         row_kind;   // -1 = header, else index into entries
        std::string last_cat;
        for (const CompEntry &e : entries) {
            if (e.cat != last_cat) { last_cat = e.cat; flat.push_back(e.cat); row_kind.push_back(-1); }
            flat.push_back(e.label); row_kind.push_back(1);
        }
        std::vector<dai_ui_menu_item> items(flat.size());
        for (size_t i = 0; i < flat.size(); ++i) {
            if (row_kind[i] < 0) items[i] = { nullptr, flat[i].c_str(), nullptr, 1 };
            else {
                const char *ic = DAI_ICON_SCRIPT;
                for (const CompEntry &e : entries)
                    if (e.label == flat[i]) {
                        if (e.cat == "Physics") ic = DAI_ICON_SETTINGS;
                        else if (e.cat == "Audio") ic = DAI_ICON_AUDIO;
                        else if (e.cat == "Rendering")
                            ic = e.kind == 2 ? DAI_ICON_CAMERA
                               : e.kind == 3 ? DAI_ICON_LIGHT
                               : e.kind == 7 ? DAI_ICON_C_TEXT
                               : e.kind == 8 ? DAI_ICON_IMAGE : DAI_ICON_SPRITE;
                        break;
                    }
                items[i] = { ic, flat[i].c_str(), nullptr, 0 };
            }
        }
        int pick = dai_ui_searchlist_draw(p->ui, &p->addcomp_list, items.data(), (uint32_t)items.size());
        if (pick >= 0 && pick < (int)flat.size() && row_kind[pick] >= 0 && have_node) {
            const std::string &lbl = flat[pick];
            const CompEntry *sel = nullptr;
            for (const CompEntry &e : entries) if (e.label == lbl) { sel = &e; break; }
            if (sel) {
                dai_doc_begin(d, "Add component");
                switch (sel->kind) {
                case 0: ar2.no_rigidbody = 0; ar2.no_body = 0;
                        ar2.friction = p->def_friction; ar2.restitution = p->def_restitution; break;
                case 1: ar2.no_collider = 0; ar2.no_body = 0; break;
                case 2: ar2.camera = 1; break;
                case 3: ar2.light = 1; break;
                case 9: ar2.button_on = 1;
                        // A button with nothing to show is a button you cannot
                        // find. Unity's Add > UI > Button does the same: it
                        // arrives with a background and a label already on it.
                        if (!ar2.image_on && !ar2.text_on) {
                            ar2.text_on = 1;
                            if (!ar2.text[0]) std::snprintf(ar2.text, sizeof(ar2.text), "Button");
                        }
                        break;
                case 8: ar2.image_on = 1;
                        if (ar2.image_w <= 0.0f) { ar2.image_w = 128.0f; ar2.image_h = 128.0f; }
                        break;
                case 7: ar2.text_on = 1;
                        // A label with nothing in it is invisible, and an
                        // invisible component reads as one that did not get
                        // added. It says its own name until told otherwise.
                        if (!ar2.text[0]) std::snprintf(ar2.text, sizeof(ar2.text), "Text");
                        if (ar2.text_size <= 0.0f) ar2.text_size = 24.0f;
                        break;
                case 5: std::snprintf(ar2.audio_event, sizeof(ar2.audio_event), "click"); break;
                case 6: {
                    std::vector<std::string> list = script_list(ar2.script);
                    list.push_back(sel->path);
                    script_join(ar2.script, sizeof(ar2.script), list);
                    break;
                }
                }   /* nothing removes from here any more - see above */
                dai_doc_set(d, p->addcomp_node, &ar2);
                dai_doc_commit(d);
                dai_editor_resync(p->ed);
            }
        }
        if (!p->addcomp_list.open) p->addcomp_node = DAI_INVALID_NODE;
    }

    // "Delete X?" - two words and two buttons, over the row it is about.
    if (p->menu_delete.open && !p->delete_ask.empty()) {
        std::string q = "Delete " + base_of(p->delete_ask) + "?";
        dai_ui_menu_item items[2] = {
            { DAI_ICON_TRASH, q.c_str(), nullptr, 1 },   // the question, not a choice
            { nullptr, "Delete permanently", nullptr, 0 },
        };
        int pick = dai_ui_popup_menu(p->ui, &p->menu_delete, items, 2);
        if (pick == 1) {
            if (p->asset_delete && p->asset_delete(p->delete_ask.c_str(), nullptr,
                                                   p->delete_user)) {
                char msg[192];
                std::snprintf(msg, sizeof(msg), "deleted %s",
                              base_of(p->delete_ask).c_str());
                dai_editor_ui_toast(p, msg, 2.0f);
                p->want_refresh = 1;
                if (p->proj_sel_folder == p->delete_ask) p->proj_sel_folder.clear();
                p->asset_sel = -1;
            } else {
                dai_editor_ui_toast(p, "could not delete it", 2.5f);
            }
            p->delete_ask.clear();
        } else if (!p->menu_delete.open) {
            p->delete_ask.clear();
        }
    }

    // The material picker's list. A material is a name today (the renderer
    // has no material files yet), so the list offers the ones the palette
    // already knows; typing in the search field filters them.
    if (p->mat_list.open && p->mat_menu_node != DAI_INVALID_NODE) {
        // Every .daimat in the project, and "Default" for none. The six
        // hard-coded names that used to be here were not materials - nothing
        // read them, and two objects called "Metal" shared a word, not a
        // surface.
        std::vector<std::string> paths;
        paths.push_back("Default");
        for (const char *a : p->assets)
            if (a && is_material_file(a)) paths.push_back(a);
        std::vector<std::string> labels;
        labels.reserve(paths.size());
        for (const std::string &pp : paths)
            labels.push_back(pp == "Default" ? pp : base_of(pp));
        std::vector<dai_ui_menu_item> items(paths.size());
        for (size_t i = 0; i < paths.size(); ++i)
            items[i] = { DAI_ICON_MATERIAL, labels[i].c_str(), nullptr, 0 };
        int pick = dai_ui_searchlist_draw(p->ui, &p->mat_list, items.data(),
                                          (uint32_t)items.size());
        if (pick >= 0 && pick < (int)paths.size()) {
            dai_doc *md = dai_editor_doc(p->ed);
            dai_node_desc mr{};
            if (dai_doc_get(md, p->mat_menu_node, &mr) == DAI_OK) {
                std::vector<std::string> mats = script_list(mr.materials);
                while ((int)mats.size() <= p->mat_menu_slot) mats.push_back("Default");
                mats[(size_t)p->mat_menu_slot] = paths[(size_t)pick];
                dai_doc_begin(md, "Material");
                script_join(mr.materials, sizeof(mr.materials), mats);
                dai_doc_set(md, p->mat_menu_node, &mr);
                dai_doc_commit(md);
                dai_editor_resync(p->ed);
                p->want_material_apply = 1;
            }
            p->mat_menu_node = DAI_INVALID_NODE;
        }
    }

    // The object picker: every node in the scene, filtered by the search box,
    // plus "None" at the top. It is the target button's half of the gesture -
    // drag and drop is the other half, and a field that only accepts a drag is
    // a field you cannot fill while the hierarchy is scrolled somewhere else.
    if (p->obj_list.open && p->obj_pick_node != DAI_INVALID_NODE) {
        dai_doc *od = dai_editor_doc(p->ed);
        std::vector<dai_node> all(dai_doc_count(od));
        uint32_t na = all.empty() ? 0 : dai_doc_nodes(od, all.data(), (uint32_t)all.size());
        // Only what the field's type accepts. A list that offers a light to a
        // Camera slot is a list that has to be read twice, and the second read
        // is the one that goes wrong.
        std::string want = p->obj_pick_type;
        const NodeTypeInfo *pnt = node_type_of(want);
        std::vector<std::string> names;
        names.push_back("None");
        for (uint32_t i = 0; i < na; ++i) {
            dai_node_desc od2{};
            if (dai_doc_get(od, all[i], &od2) != DAI_OK || !od2.name[0]) continue;
            if (!node_has_type(od2, want)) continue;
            names.push_back(od2.name);
        }
        std::vector<dai_ui_menu_item> items(names.size());
        for (size_t i = 0; i < names.size(); ++i)
            items[i] = { i == 0 ? nullptr : (pnt ? pnt->icon : DAI_ICON_C_TRANSFORM),
                         names[i].c_str(), nullptr, 0 };
        int pick = dai_ui_searchlist_draw(p->ui, &p->obj_list, items.data(),
                                          (uint32_t)items.size());
        if (pick >= 0 && pick < (int)names.size()) {
            dai_node_desc orr{};
            if (dai_doc_get(od, p->obj_pick_node, &orr) == DAI_OK) {
                std::vector<std::string> olist = script_list(orr.script);
                if (p->obj_pick_entry >= 0 && p->obj_pick_entry < (int)olist.size()) {
                    entry_set_param(olist[(size_t)p->obj_pick_entry], p->obj_pick_key,
                                    pick == 0 ? std::string() : names[(size_t)pick]);
                    dai_doc_begin(od, "Reference");
                    script_join(orr.script, sizeof(orr.script), olist);
                    dai_doc_set(od, p->obj_pick_node, &orr);
                    dai_doc_commit(od);
                    dai_editor_resync(p->ed);
                }
            }
            p->obj_pick_node = DAI_INVALID_NODE;
        }
    }

    // The texture picker for an Image component.
    if (p->image_list.open && p->image_pick_node != DAI_INVALID_NODE) {
        std::vector<std::string> imgs;
        imgs.push_back("None");
        for (const char *a : p->assets) {
            if (!a) continue;
            std::string f = a;
            size_t dot = f.find_last_of('.');
            if (dot == std::string::npos) continue;
            std::string e = f.substr(dot + 1);
            for (char &c : e) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (e == "png" || e == "jpg" || e == "jpeg" || e == "tga") imgs.push_back(f);
        }
        std::vector<dai_ui_menu_item> items(imgs.size());
        for (size_t i = 0; i < imgs.size(); ++i)
            items[i] = { i == 0 ? nullptr : DAI_ICON_IMAGE, imgs[i].c_str(), nullptr, 0 };
        int pick = dai_ui_searchlist_draw(p->ui, &p->image_list, items.data(), (uint32_t)items.size());
        if (pick >= 0 && pick < (int)imgs.size()) {
            dai_node_desc ir{};
            if (dai_doc_get(d, p->image_pick_node, &ir) == DAI_OK) {
                dai_doc_begin(d, "Image");
                if (pick == 0) ir.image[0] = 0;
                else std::snprintf(ir.image, sizeof(ir.image), "%s", imgs[(size_t)pick].c_str());
                dai_doc_set(d, p->image_pick_node, &ir);
                dai_doc_commit(d);
            }
            p->image_pick_node = DAI_INVALID_NODE;
        }
    }

    // The font picker: every .ttf/.otf in the project, plus "Default".
    if (p->font_list.open && p->font_pick_node != DAI_INVALID_NODE) {
        std::vector<std::string> fonts;
        fonts.push_back("Default (system UI)");
        for (const char *a : p->assets) {
            if (!a) continue;
            std::string f = a;
            size_t dot = f.find_last_of('.');
            if (dot == std::string::npos) continue;
            std::string e = f.substr(dot + 1);
            for (char &c : e) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (e == "ttf" || e == "otf" || e == "ttc") fonts.push_back(f);
        }
        std::vector<dai_ui_menu_item> items(fonts.size());
        for (size_t i = 0; i < fonts.size(); ++i)
            items[i] = { i == 0 ? nullptr : DAI_ICON_C_TEXT, fonts[i].c_str(), nullptr, 0 };
        int pick = dai_ui_searchlist_draw(p->ui, &p->font_list, items.data(), (uint32_t)items.size());
        if (pick >= 0 && pick < (int)fonts.size()) {
            dai_node_desc fr{};
            if (dai_doc_get(d, p->font_pick_node, &fr) == DAI_OK) {
                dai_doc_begin(d, "Font");
                if (pick == 0) fr.text_font[0] = 0;
                else std::snprintf(fr.text_font, sizeof(fr.text_font), "%s", fonts[(size_t)pick].c_str());
                dai_doc_set(d, p->font_pick_node, &fr);
                dai_doc_commit(d);
            }
            p->font_pick_node = DAI_INVALID_NODE;
        }
    }

    // The Add Component list. Entries flip between add and remove so one menu
    // covers both directions - a component that is already there offers to go.
    if (0 && p->menu_addcomp.open && dai_editor_selection_count(p->ed) > 0) {
        dai_node an = dai_editor_selected(p->ed, 0);
        dai_node_desc ar{};
        if (dai_doc_get(d, an, &ar) == DAI_OK) {
            int has_rb = !ar.no_rigidbody && !ar.no_body;
            int has_col = !ar.no_collider && !ar.no_body;
            int has_cam = ar.camera != 0;
            int has_light = ar.light != 0;
            int has_sprite = ar.sprite != 0;
            int has_audio = ar.audio_event[0] || ar.audio_autoplay || ar.audio_bus;
            // One list, entries flip between add and remove - the same menu
            // covers both directions, which is what "components" means here.
            char lbl[6][40];
            std::snprintf(lbl[0], 40, "%sRigidbody", has_rb ? "Remove " : "");
            std::snprintf(lbl[1], 40, "%sCollider", has_col ? "Remove " : "");
            std::snprintf(lbl[2], 40, "%sCamera", has_cam ? "Remove " : "");
            std::snprintf(lbl[3], 40, "%sLight", has_light ? "Remove " : "");
            std::snprintf(lbl[4], 40, "%sSprite (2D)", has_sprite ? "Remove " : "");
            std::snprintf(lbl[5], 40, "%sAudio Source", has_audio ? "Remove " : "");
            dai_ui_menu_item items[7];
            items[0] = { DAI_ICON_SETTINGS, lbl[0], nullptr };
            items[1] = { DAI_ICON_BOX, lbl[1], nullptr };
            items[2] = { DAI_ICON_CAMERA, lbl[2], nullptr };
            items[3] = { DAI_ICON_SUN, lbl[3], nullptr };
            items[4] = { DAI_ICON_IMAGE, lbl[4], nullptr };
            items[5] = { DAI_ICON_AUDIO, lbl[5], nullptr };
            items[6] = { DAI_ICON_SCRIPT, "Script (drag a .js or .cpp here)", nullptr };
            int pick = dai_ui_popup_menu(p->ui, &p->menu_addcomp, items, 7);
            if (pick >= 0 && pick <= 5) {
                dai_doc_begin(d, "Component");
                switch (pick) {
                case 0:
                    ar.no_rigidbody = has_rb ? 1 : 0;
                    if (!has_rb) {   // adding: start from the project's defaults
                        ar.friction = p->def_friction;
                        ar.restitution = p->def_restitution;
                    }
                    break;
                case 1: ar.no_collider = has_col ? 1 : 0; break;
                case 2:
                    if (has_cam) ar.camera = 0;
                    else { ar.camera = 1; ar.camera_fov = 60.0f; ar.camera_size = 5.0f;
                           std::snprintf(ar.tag, sizeof(ar.tag), "%s", CAMERA_TAG); }
                    break;
                case 3:
                    if (has_light) ar.light = 0;
                    else { ar.light = 1; ar.light_color = { 1, 1, 1 };
                           ar.light_range = 10.0f; ar.light_intensity = 1.0f; ar.light_cone = 30.0f; }
                    break;
                case 4:
                    if (has_sprite) ar.sprite = 0;
                    else { ar.sprite = 1; ar.sprite_size = { 1, 1, 1 }; }
                    break;
                case 5:
                    if (has_audio) { ar.audio_event[0] = 0; ar.audio_autoplay = 0; ar.audio_bus = 0; }
                    else { ar.audio_bus = 2; ar.audio_volume = 1.0f; ar.audio_autoplay = 1; }
                    p->audio_buf_node = DAI_INVALID_NODE;
                    break;
                default: break;
                }
                // no_body is the "neither" state: the node stops being physical
                // at all only once BOTH are gone, and comes back the moment
                // either returns. Getting this wrong made the mesh disappear.
                ar.no_body = (ar.no_rigidbody && ar.no_collider) ? 1 : 0;
                dai_doc_set(d, an, &ar);
                dai_doc_commit(d);
                dai_editor_resync(p->ed);
            }
        }
    } else if (p->menu_addcomp.open) {
        dai_ui_menu_item none[1] = { { DAI_ICON_FILE, "select an object first", nullptr } };
        dai_ui_popup_menu(p->ui, &p->menu_addcomp, none, 1);
    }

    // Right click on a component header: Unity's gear menu - copy the values,
    // paste them onto the same component of another object, or remove it.
    if (p->menu_comp.open) {
        static const dai_ui_menu_item COMP_MENU[3] = {
            { DAI_ICON_COPY,  "Copy Component",  nullptr },
            { DAI_ICON_FILE,  "Paste Component", nullptr },
            { DAI_ICON_TRASH, "Remove Component", nullptr },
        };
        int target = p->comp_menu_target;
        // Transform gets its own menu. Copying "the transform" is almost never
        // what is meant: you want this object to stand WHERE that one stands,
        // and to keep its own rotation - or the other way round. One entry for
        // all four values, three for the parts.
        if (target == 0) {
            static const dai_ui_menu_item TR_MENU[] = {
                { DAI_ICON_RESET, "Reset Transform", nullptr },
                { DAI_ICON_COPY,  "Copy All",        nullptr },
                { DAI_ICON_COPY,  "Copy Position",   nullptr },
                { DAI_ICON_COPY,  "Copy Rotation",   nullptr },
                { DAI_ICON_COPY,  "Copy Scale",      nullptr },
                { DAI_ICON_SAVE,  "Paste",           nullptr },
            };
            int tpick = dai_ui_popup_menu(p->ui, &p->menu_comp, TR_MENU, 6);
            if (tpick >= 0 && dai_editor_selection_count(p->ed) > 0) {
                dai_node tn = dai_editor_selected(p->ed, 0);
                dai_node_desc tr{};
                if (dai_doc_get(d, tn, &tr) == DAI_OK) {
                    char buf[256];
                    if (tpick == 0) {
                        dai_doc_begin(d, "Reset Transform");
                        tr.position = dai_vec3{ 0, 0, 0 };
                        tr.rotation = dai_quat{ 0, 0, 0, 1 };
                        tr.scale = dai_vec3{ 1, 1, 1 };
                        dai_doc_set(d, tn, &tr);
                        dai_doc_commit(d);
                        dai_editor_resync(p->ed);
                    } else if (tpick >= 1 && tpick <= 4) {
                        // The clipboard says WHICH parts it holds, so a paste
                        // knows what it may touch. A blob that always claims
                        // to be a whole transform is how "copy position" ends
                        // up rotating things.
                        const char *what = tpick == 1 ? "all" : tpick == 2 ? "pos"
                                         : tpick == 3 ? "rot" : "scale";
                        std::snprintf(buf, sizeof(buf),
                            "transform:%s p=%g,%g,%g r=%g,%g,%g,%g s=%g,%g,%g", what,
                            (double)tr.position.x, (double)tr.position.y, (double)tr.position.z,
                            (double)tr.rotation.x, (double)tr.rotation.y,
                            (double)tr.rotation.z, (double)tr.rotation.w,
                            (double)tr.scale.x, (double)tr.scale.y, (double)tr.scale.z);
                        dai_editor_ui_clipboard_set(p, 0, buf);
                        dai_editor_ui_toast(p,
                            tpick == 1 ? "transform copied" :
                            tpick == 2 ? "position copied" :
                            tpick == 3 ? "rotation copied" : "scale copied", 1.5f);
                    } else if (tpick == 5) {
                        const char *cb = dai_editor_ui_clipboard_get(p, nullptr);
                        if (!cb || std::strncmp(cb, "transform:", 10) != 0) {
                            dai_editor_ui_toast(p, "the clipboard holds no transform", 2.0f);
                        } else {
                            const char *what = cb + 10;
                            bool all = std::strncmp(what, "all", 3) == 0;
                            bool pos = all || std::strncmp(what, "pos", 3) == 0;
                            bool rot = all || std::strncmp(what, "rot", 3) == 0;
                            bool scl = all || std::strncmp(what, "scale", 5) == 0;
                            float v[10] = { 0 };
                            const char *pp = std::strstr(cb, "p=");
                            const char *rr = std::strstr(cb, "r=");
                            const char *ss = std::strstr(cb, "s=");
                            if (pp) std::sscanf(pp + 2, "%f,%f,%f", &v[0], &v[1], &v[2]);
                            if (rr) std::sscanf(rr + 2, "%f,%f,%f,%f", &v[3], &v[4], &v[5], &v[6]);
                            if (ss) std::sscanf(ss + 2, "%f,%f,%f", &v[7], &v[8], &v[9]);
                            dai_doc_begin(d, "Paste Transform");
                            // Onto the WHOLE selection: pasting one position
                            // onto twelve selected crates is a real thing to
                            // want, and it is the same rule the rest of the
                            // inspector follows.
                            for (uint32_t si = 0; si < dai_editor_selection_count(p->ed); ++si) {
                                dai_node on = dai_editor_selected(p->ed, si);
                                dai_node_desc orr{};
                                if (dai_doc_get(d, on, &orr) != DAI_OK) continue;
                                if (pos) orr.position = dai_vec3{ v[0], v[1], v[2] };
                                if (rot) orr.rotation = dai_quat{ v[3], v[4], v[5], v[6] };
                                if (scl) orr.scale = dai_vec3{ v[7], v[8], v[9] };
                                dai_doc_set(d, on, &orr);
                            }
                            dai_doc_commit(d);
                            dai_editor_resync(p->ed);
                            dai_editor_ui_toast(p, pos && rot && scl ? "transform pasted"
                                                : pos ? "position pasted"
                                                : rot ? "rotation pasted" : "scale pasted", 1.5f);
                        }
                    }
                }
            }
            if (!p->menu_comp.open) p->comp_menu_target = -1;
            // NOT a return: the project menu, the delete confirmation and the
            // window menu are all drawn further down in this same function,
            // and skipping them for a frame is how a popup starts flickering.
            goto after_component_menu;
        }
        {
        static const dai_ui_menu_item COMP_MENU_X[] = {
            { DAI_ICON_RESET, "Reset", nullptr },
            { DAI_ICON_COPY, "Copy Component", nullptr },
            { DAI_ICON_SAVE, "Paste Component Values", nullptr },
            { DAI_ICON_CLOSE, "Remove Component", nullptr },
        };
        int cpick = dai_ui_popup_menu(p->ui, &p->menu_comp, COMP_MENU_X, 4);
        if (cpick >= 0 && target >= 0 && dai_editor_selection_count(p->ed) > 0) {
            dai_node an = dai_editor_selected(p->ed, 0);
            dai_node_desc ar{};
            if (dai_doc_get(d, an, &ar) == DAI_OK) {
                if (cpick == 0) {
                    // Reset is what a grown-up editor answers when you clicked
                    // three values too far: the component goes back to the
                    // defaults, the rest of the object stays put.
                    dai_doc_begin(d, "Reset Component");
                    switch (target) {
                    case 1: ar.density = 0.0f; ar.friction = p->def_friction;
                            ar.restitution = p->def_restitution; ar.motion = DAI_DYNAMIC; break;
                    case 2: ar.trigger = 0; ar.collider_center = dai_vec3{ 0, 0, 0 }; break;
                    case 3: ar.camera_fov = 0.0f; ar.camera_size = 0.0f; break;
                    case 4: ar.light_color = dai_vec3{ 0, 0, 0 }; ar.light_range = 0.0f;
                            ar.light_intensity = 0.0f; ar.light_cone = 0.0f; break;
                    default: break;
                    }
                    dai_doc_set(d, an, &ar);
                    dai_doc_commit(d);
                    dai_editor_resync(p->ed);
                    dai_editor_ui_toast(p, "component reset", 1.5f);
                } else if (cpick == 1) {
                    dai_editor_ui_clipboard_set(p, 2, comp_to_text(target, ar).c_str());
                    dai_editor_ui_toast(p, "component copied", 1.5f);
                } else if (cpick == 2) {
                    int kind = -1;
                    const char *text = dai_editor_ui_clipboard_get(p, &kind);
                    if (text && kind == 2 && comp_from_text(text, target, &ar)) {
                        dai_doc_begin(d, "Paste Component");
                        ar.no_body = (ar.no_rigidbody && ar.no_collider) ? 1 : 0;
                        dai_doc_set(d, an, &ar);
                        dai_doc_commit(d);
                        dai_editor_resync(p->ed);
                        dai_editor_ui_toast(p, "component pasted", 1.5f);
                    } else {
                        dai_editor_ui_toast(p, "clipboard holds a different component", 2.0f);
                    }
                } else if (cpick == 3) {
                    dai_doc_begin(d, "Remove Component");
                    if (target == 1) ar.no_rigidbody = 1;
                    else if (target == 2) ar.no_collider = 1;
                    else if (target == 3) ar.camera = 0;
                    else if (target == 4) ar.light = 0;
                    else if (target == 5) ar.sprite = 0;
                    else if (target == 7) ar.text_on = 0;
                    else if (target == 8) ar.image_on = 0;
                    else if (target == 9) ar.button_on = 0;
                    else if (target == 6) { ar.audio_event[0] = 0; ar.audio_autoplay = 0; ar.audio_bus = 0; }
                    ar.no_body = (ar.no_rigidbody && ar.no_collider) ? 1 : 0;
                    dai_doc_set(d, an, &ar);
                    dai_doc_commit(d);
                    dai_editor_resync(p->ed);
                    dai_editor_ui_toast(p, "component removed", 1.5f);
                }
            }
        }
        if (!p->menu_comp.open) p->comp_menu_target = -1;
        }
    }
    after_component_menu:;

    // The project window's own Create menu. The editor owns the click, the
    // host owns the disk - the same split the asset browser already uses.
    // Rename and Delete only exist when the click landed ON something. A menu
    // that offers to delete when you right clicked the background is a menu
    // that will eventually delete the wrong thing.
    const bool on_row = !p->rename_click.empty();
    static const dai_ui_menu_item PROJ_ITEMS[] = {
        { DAI_ICON_SCRIPT, "Create: JS Script", nullptr },
        { DAI_ICON_FOLDER, "Create: Folder", nullptr },
        { DAI_ICON_PLUS, "Rename", "F2" },
        { DAI_ICON_SAVE, "Save scene", "Ctrl+S" },
        { DAI_ICON_SEARCH, "Refresh", nullptr },
        { DAI_ICON_SCRIPT, "Create: C++ Behaviour", nullptr },
        { DAI_ICON_TRASH, "Delete", "Del" },
        { DAI_ICON_MATERIAL, "Create: Material", nullptr },
        { DAI_ICON_C_SCRIPT, "Create: Player Controller (C++)", nullptr },
    };
    // The two row-only entries sit at 2 and 6; without a row the menu is the
    // other five, and the indices below are mapped back so nothing else moves.
    static const int WITH_ROW[9]    = { 0, 1, 7, 8, 2, 3, 4, 5, 6 };
    static const int WITHOUT_ROW[7] = { 0, 1, 7, 8, 3, 4, 5 };
    dai_ui_menu_item shown_items[9];
    const int *map = on_row ? WITH_ROW : WITHOUT_ROW;
    uint32_t shown_n = on_row ? 9u : 7u;
    for (uint32_t i = 0; i < shown_n; ++i) shown_items[i] = PROJ_ITEMS[map[i]];
    int raw = dai_ui_popup_menu(p->ui, &p->menu_project, shown_items, shown_n);
    int ppick = (raw >= 0 && raw < (int)shown_n) ? map[raw] : raw;
    if (ppick == 6) {
        // Whatever the right click landed on, or failing that the selection.
        std::string target = !p->rename_click.empty() ? p->rename_click
                           : !p->proj_sel_folder.empty() ? p->proj_sel_folder
                           : (p->asset_sel >= 0 && p->asset_sel < (int)p->assets.size() &&
                              p->assets[(size_t)p->asset_sel]
                              ? std::string(p->assets[(size_t)p->asset_sel])
                              : std::string());
        p->rename_click.clear();
        if (target.empty()) {
            dai_editor_ui_toast(p, "nothing selected to delete", 2.0f);
        } else if (!p->asset_delete) {
            dai_editor_ui_toast(p, "this build cannot delete files", 2.0f);
        } else {
            p->delete_ask = target;
            float dmx2 = 0, dmy2 = 0;
            dai_ui_mouse(p->ui, &dmx2, &dmy2, nullptr, nullptr);
            dai_ui_popup_open(&p->menu_delete, dmx2, dmy2);
        }
    }
    if (ppick == 2) {
        // Rename works on whatever the browser has selected - a file OR a
        // folder. std::rename does both, so there was never a reason for
        // folders to be the one thing you could create but not correct.
        std::string pick = p->rename_click.empty()
                         ? (p->asset_sel >= 0 && p->asset_sel < (int)p->assets.size()
                            ? std::string(p->assets[(size_t)p->asset_sel] ? p->assets[(size_t)p->asset_sel] : "")
                            : std::string())
                         : p->rename_click;
        if (!pick.empty()) {
            p->rename_asset = pick;
            // The rename field is drawn in the list of the CURRENT folder.
            // Renaming from the tree with another folder open meant the row
            // never existed, and "row not drawn" cancels the rename on the
            // spot - so open the parent first.
            p->proj_dir = parent_of(pick);
            project_expand_to(p, p->proj_dir);
            std::string base = base_of(pick);
            if (base.size() > 3 && base.compare(base.size() - 3, 3, ".js") == 0)
                base.resize(base.size() - 3);
            std::snprintf(p->rename_asset_buf, sizeof(p->rename_asset_buf), "%s", base.c_str());
            p->rename_seen_active = 0;
        }
        p->rename_click.clear();
    }
    else if (ppick > 2) ppick += 0;   // Save/Refresh keep their meaning below
    // "New C++ Script" is the same flow one entry down: the host writes the
    // file, the row goes straight into rename. The extension is what tells
    // the runner which engine to hand it to.
    if (ppick == 5 && p->script_create) {
        p->proj_tab = 0;
        std::string base5 = p->proj_dir.empty() ? std::string() : p->proj_dir + "/";
        for (int i = 0; i < 30; ++i) {
            char nm[64], rel[224];
            if (i == 0) std::snprintf(nm, sizeof(nm), "NewBehaviour.cpp");
            else        std::snprintf(nm, sizeof(nm), "NewBehaviour (%d).cpp", i);
            std::snprintf(rel, sizeof(rel), "%s%s", base5.c_str(), nm);
            if (p->script_create(rel, p->script_user)) {
                p->rename_asset = rel;
                std::string stem = nm;
                stem.resize(stem.size() - 4);
                std::snprintf(p->rename_asset_buf, sizeof(p->rename_asset_buf), "%s", stem.c_str());
                p->rename_seen_active = 0;
                project_expand_to(p, p->proj_dir);
                p->want_refresh = 1;
                break;
            }
        }
    }
    if (ppick == 8 && p->script_create) {
        // Asks the host for that exact file name; the host knows the template
        // that goes with it. One entry, one file, no second callback.
        p->proj_tab = 0;
        std::string base8 = p->proj_dir.empty() ? std::string() : p->proj_dir + "/";
        std::string rel8 = base8 + "PlayerController.cpp";
        if (p->script_create(rel8.c_str(), p->script_user)) {
            project_expand_to(p, p->proj_dir);
            p->want_refresh = 1;
            dai_editor_ui_toast(p, "PlayerController.cpp created - drop it on an object "
                                   "with a Rigidbody", 3.0f);
        } else {
            dai_editor_ui_toast(p, "PlayerController.cpp is already here", 2.0f);
        }
    }
    if (ppick == 0 && p->script_create) {
        // Unity's create flow: the file exists the moment the menu closes -
        // NewScript, NewScript2, ... - and its row in the list is in rename
        // right away. Enter commits the name, Escape keeps the default.
        p->proj_tab = 0;
        std::string base = p->proj_dir.empty() ? std::string() : p->proj_dir + "/";
        for (int i = 0; i < 30; ++i) {
            char nm[64], rel[224];
            if (i == 0) std::snprintf(nm, sizeof(nm), "NewScript");
            else        std::snprintf(nm, sizeof(nm), "NewScript (%d)", i);
            std::snprintf(rel, sizeof(rel), "%s%s", base.c_str(), nm);
            if (p->script_create(rel, p->script_user)) {
                p->rename_asset = std::string(rel) + ".js";
                std::snprintf(p->rename_asset_buf, sizeof(p->rename_asset_buf), "%s", nm);
                p->rename_seen_active = 0;
                project_expand_to(p, p->proj_dir);
                p->want_refresh = 1;
                break;
            }
        }
    }
    else if (ppick == 1 && p->folder_create) {
        // Like every file manager: New Folder, New Folder 2, ... - in the
        // folder the browser is showing, not blindly at the root.
        std::string base = p->proj_dir.empty() ? std::string() : p->proj_dir + "/";
        for (int i = 0; i < 10; ++i) {
            char name[128];
            if (i == 0) std::snprintf(name, sizeof(name), "%sNew Folder", base.c_str());
            else        std::snprintf(name, sizeof(name), "%sNew Folder %d", base.c_str(), i);
            if (p->folder_create(name, p->script_user)) {
                // Selected, not entered. Walking into a folder you have just
                // made hides the folder you were working in, and the next
                // thing anybody does is click Back - Unity selects it and
                // starts the rename right there instead.
                p->proj_sel_folder = name;
                p->last_pick = name;
                p->asset_sel = -1;
                p->want_refresh = 1;
                break;
            }
        }
    }
    else if (ppick == 7 && p->material_create) {
        // NewMaterial, NewMaterial 2, ... in the folder that is open - the
        // same walk "Create: Folder" does, and for the same reason.
        std::string base7 = p->proj_dir.empty() ? std::string() : p->proj_dir + "/";
        for (int i = 0; i < 30; ++i) {
            char rel7[224];
            if (i == 0) std::snprintf(rel7, sizeof(rel7), "%sNewMaterial.daimat", base7.c_str());
            else        std::snprintf(rel7, sizeof(rel7), "%sNewMaterial %d.daimat", base7.c_str(), i);
            if (p->material_create(rel7, p->material_user)) {
                p->proj_tab = 0;
                p->want_refresh = 1;
                p->rename_asset = rel7;          // straight into the rename
                std::string b7 = base_of(rel7);
                if (b7.size() > 7) b7.resize(b7.size() - 7);   // drop ".daimat"
                std::snprintf(p->rename_asset_buf, sizeof(p->rename_asset_buf), "%s", b7.c_str());
                p->rename_seen_active = 0;
                break;
            }
        }
    }
    else if (ppick == 3) p->want_save = 1;
    else if (ppick == 4) p->want_refresh = 1;

    static const dai_ui_menu_item NODE_ITEMS[] = {
        { DAI_ICON_PLUS, "Rename", "F2" },
        { DAI_ICON_COPY, "Duplicate", "Ctrl+D" },
        { DAI_ICON_TRASH, "Delete", "Del" },
        { DAI_ICON_SCENE, "Create Prefab", nullptr },
    };
    int pick = dai_ui_popup_menu(p->ui, &p->menu_node, NODE_ITEMS, 4);
    if (pick >= 0 && p->menu_target != DAI_INVALID_NODE) {
        if (pick == 0) {
            dai_editor_ui_rename(p, p->menu_target);
        } else if (pick == 1) {
            dai_editor_select(p->ed, p->menu_target, 0);
            dai_editor_duplicate_selection(p->ed);
        } else if (pick == 2) {
            dai_editor_select(p->ed, p->menu_target, 0);
            dai_editor_delete_selection(p->ed);
        } else if (pick == 3 && p->prefab_save) {
            // The object becomes a reusable file under prefabs/. The host owns
            // the disk, the editor owns the click - the same split the asset
            // browser already uses.
            dai_node_desc pr{};
            if (dai_doc_get(d, p->menu_target, &pr) == DAI_OK) {
                char rel[192];
                std::snprintf(rel, sizeof(rel), "prefabs/%s.daidalos",
                              pr.name[0] ? pr.name : "Prefab");
                char nm[64];
                std::snprintf(nm, sizeof(nm), "%u", (unsigned)p->menu_target);
                if (p->prefab_save(nm, rel, p->rename_user)) {
                    dai_editor_ui_toast(p, "prefab saved", 2.0f);
                    p->want_refresh = 1;
                }
            }
        }
        p->menu_target = DAI_INVALID_NODE;
    }

    // Everything "Create" can mean, in one place. An empty node is a group:
    // no body, nothing drawn - it exists to carry children and a transform.
    // Unity's GameObject menu, flattened: no submenus in this popup yet, so
    // the six things people actually make are listed instead of hidden one
    // level down.
    static const dai_ui_menu_item CANVAS_ITEMS[] = {
        { DAI_ICON_PLUS, "Create Empty", "Ctrl+Shift+N" },
        { DAI_ICON_BOX, "3D Object: Cube", nullptr },
        { DAI_ICON_SPHERE, "3D Object: Sphere", nullptr },
        { DAI_ICON_CAPSULE, "3D Object: Capsule", nullptr },
        { DAI_ICON_CAPSULE, "3D Object: Cylinder", nullptr },
        { DAI_ICON_GRID, "3D Object: Plane", nullptr },
        { DAI_ICON_CAMERA, "Camera", nullptr },
    };
    int cpick = dai_ui_popup_menu(p->ui, &p->menu_canvas, CANVAS_ITEMS, 7);
    if (cpick == 6) { dai_editor_ui_add_camera(p); return; }
    if (cpick == 5) {
        dai_node_desc r = dai_node_desc_default();
        std::snprintf(r.name, sizeof(r.name), "Plane");
        r.shape = DAI_SHAPE_BOX;
        r.motion = DAI_STATIC;
        r.half_extent = { 5.0f, 0.05f, 5.0f };
        r.position = { 0, 0, 0 };
        dai_doc_begin(d, "New plane");
        dai_node n = dai_doc_add(d, &r);
        dai_doc_commit(d);
        dai_editor_resync(p->ed);
        dai_editor_select(p->ed, n, 0);
        return;
    }
    if (cpick >= 0) {
        dai_node_desc r = dai_node_desc_default();
        (void)0;
        const char *undo = "New box";
        if (cpick == 0) {
            std::snprintf(r.name, sizeof(r.name), "GameObject");
            r.no_body = 1;
            r.hidden = 1;           // an empty has no mesh to draw
            undo = "Create empty";
        } else {
            const char *names[5] = { "", "Box", "Sphere", "Capsule", "Cylinder" };
            int shapes[5] = { 0, DAI_SHAPE_BOX, DAI_SHAPE_SPHERE, DAI_SHAPE_CAPSULE,
                              DAI_SHAPE_CYLINDER };
            std::snprintf(r.name, sizeof(r.name), "%s", names[cpick]);
            r.shape = shapes[cpick];
            r.motion = DAI_DYNAMIC;
            r.half_extent = { 0.5f, 0.5f, 0.5f };
            r.position = spawn_point(p, d, 0.5f);
            undo = cpick == 2 ? "New sphere" : cpick == 3 ? "New capsule"
                 : cpick == 4 ? "New cylinder" : "New box";
        }
        dai_doc_begin(d, undo);
        dai_node n = dai_doc_add(d, &r);
        dai_doc_commit(d);
        dai_editor_resync(p->ed);
        dai_editor_select(p->ed, n, 0);
    }
}

// Where a newly created object goes. Two rules, both of them Unity's: in
// FRONT of the camera, not at the origin behind you, and never inside
// something that is already there - two primitives at the same coordinates
// look exactly like two primitives that refuse to collide.
static dai_vec3 spawn_point(dai_editor_ui *p, dai_doc *d, float half_y) {
    dai_vec3 o{ 0, 0, 0 }, dir{ 0, 0, -1 };
    if (p->view_w > 1.0f && p->view_h > 1.0f)
        dai_editor_ray(p->ed, p->view_x + p->view_w * 0.5f,
                       p->view_y + p->view_h * 0.5f, &o, &dir);
    // The ground plane if the camera is looking down at it, eight metres out
    // otherwise - the same answer Unity gives when you drop a cube in.
    float t = 8.0f;
    if (dir.y < -0.05f) {
        float tg = (half_y - o.y) / dir.y;
        if (tg > 0.5f && tg < 60.0f) t = tg;
    }
    dai_vec3 pos{ o.x + dir.x * t, o.y + dir.y * t, o.z + dir.z * t };
    if (pos.y < half_y) pos.y = half_y;
    // Round to a tenth so the numbers in the inspector are readable.
    pos.x = (float)((int)(pos.x * 10.0f + (pos.x < 0 ? -0.5f : 0.5f))) * 0.1f;
    pos.z = (float)((int)(pos.z * 10.0f + (pos.z < 0 ? -0.5f : 0.5f))) * 0.1f;

    // Step aside until the spot is free. Sixteen tries, then take it anyway:
    // a create that silently does nothing would be worse than an overlap.
    uint32_t n = dai_doc_count(d);
    std::vector<dai_node> ids(n);
    if (n) dai_doc_nodes(d, ids.data(), n);
    for (int attempt = 0; attempt < 16; ++attempt) {
        bool clash = false;
        for (dai_node id : ids) {
            dai_node_desc e{};
            if (dai_doc_get(d, id, &e) != DAI_OK) continue;
            if (e.no_body && e.hidden) continue;         // empties do not block
            dai_vec3 wp{};
            dai_quat wr{ 0, 0, 0, 1 };
            dai_vec3 ws{ 1, 1, 1 };
            if (dai_doc_world_transform(d, id, &wp, &wr, &ws) != DAI_OK) wp = e.position;
            float dx = wp.x - pos.x, dy = wp.y - pos.y, dz = wp.z - pos.z;
            if (dx * dx + dy * dy + dz * dz < 1.1f * 1.1f) { clash = true; break; }
        }
        if (!clash) break;
        pos.x += 1.3f;
    }
    return pos;
}

// ---- the project window, Unity's two column browser -------------------------
// The folder tree on the left, the chosen folder's contents on the right, a
// breadcrumb and a search field along the top. The data is the host's flat
// list of asset paths; the tree is derived from it every frame.

// One clickable row of the browser: hover, selection, icon, label. Returns 1
// the frame it is left-clicked. An open popup menu eats every click.
static int browser_row(dai_editor_ui *p, float x, float y, float w, float h,
                       const char *icon, const char *label, int selected,
                       uint32_t icon_col) {
    dai_ui *ui = p->ui;
    const dai_ui_style *st = dai_ui_style_of(ui);
    float mx = 0, my = 0;
    int pressed = 0;
    dai_ui_mouse(ui, &mx, &my, nullptr, &pressed);
    bool over = mx >= x && mx < x + w && my >= y && my < y + h;
    if (selected)  dai_ui_rect(ui, x, y, w, h, st->accent);
    else if (over) dai_ui_rect(ui, x, y, w, h, st->button_hover);
    float tx = x + 4.0f;
    if (icon && dai_ui_has_icon(ui, icon)) {
        // On a selected row the fill is already the accent colour; a hue on
        // top of it fights the fill instead of naming the file, so the icon
        // goes plain white there and keeps its colour everywhere else.
        dai_ui_icon_at(ui, icon, tx, y + (h - 13.0f) * 0.5f, 13.0f,
                       selected ? 0xFFFFFFFFu : (icon_col ? icon_col : st->text_dim));
        tx += 19.0f;
    }
    dai_ui_text(ui, tx, y + (h - dai_ui_text_height(ui)) * 0.5f, label, st->text);
    return over && pressed && !dai_ui_popup_active(ui);
}

static int browser_button(dai_editor_ui *p, float x, float y, float w, float h,
                          const char *label) {
    dai_ui *ui = p->ui;
    const dai_ui_style *st = dai_ui_style_of(ui);
    float mx = 0, my = 0;
    int pressed = 0;
    dai_ui_mouse(ui, &mx, &my, nullptr, &pressed);
    bool over = mx >= x && mx < x + w && my >= y && my < y + h;
    dai_ui_rrect(ui, x, y, w, h, 4.0f, over ? st->button_hover : st->titlebar);
    float tw = dai_ui_text_width(ui, label);
    // Vertically centred against the REAL line height. The old constant 13
    // was the font size, not the line box, so descenders were shaved off at
    // every size - which is what "the console buttons are cut off" was.
    dai_ui_text(ui, x + (w - tw) * 0.5f, y + (h - dai_ui_text_height(ui)) * 0.5f, label, st->text);
    return over && pressed && !dai_ui_popup_active(ui);
}

// Drag a row onto a folder and it moves there. On disk this is a rename with
// a different parent, which is exactly what the host's rename callback does -
// so an explorer's most basic gesture needs no new plumbing, only the two
// guards that make it safe:
//
//   - a folder cannot move into itself or into its own descendant. Left to
//     std::rename that is EINVAL on Linux and a lost subtree on some others;
//     either way it is never what was meant.
//   - dropping something into the folder it already lives in does nothing,
//     silently. It is the commonest miss-drop there is.
static void project_move(dai_editor_ui *p, const std::string &src, const std::string &dir) {
    if (!p || !p->asset_rename || src.empty()) return;
    if (parent_of(src) == dir) return;
    if (dir == src) return;
    if (dir.size() > src.size() && dir.compare(0, src.size(), src) == 0 &&
        dir[src.size()] == '/') return;
    std::string b = base_of(src);
    if (b.empty()) return;
    std::string dst = dir.empty() ? b : dir + "/" + b;
    if (dst == src) return;
    if (p->asset_rename(src.c_str(), dst.c_str(), p->rename_user)) {
        p->want_refresh = 1;
        char msg[192];
        std::snprintf(msg, sizeof(msg), "moved %s to %s", b.c_str(),
                      dir.empty() ? "Assets" : dir.c_str());
        dai_editor_ui_toast(p, msg, 1.6f);
    } else {
        dai_editor_ui_toast(p, "could not move it - is something with that name already there?", 2.5f);
    }
}

// Does this folder hold anything at all - a file or another folder? The two
// folder icons differ by exactly this, and a hollow folder that turns out to
// be full is worse than no icon difference at all.
static bool folder_has_content(dai_editor_ui *p, const std::set<std::string> &folders,
                               const std::string &dir) {
    for (const auto &f : folders)
        if (parent_of(f) == dir) return true;
    for (const char *a : p->assets)
        if (a && parent_of(a) == dir) return true;
    return false;
}

// How many rows the tree WILL draw, before it draws them - the scroll offset
// has to be clamped against the real height, and the real height cannot be a
// number left over from last frame. Mirrors project_tree_rows exactly: this
// row, plus the children of an open folder.
static int project_tree_count(dai_editor_ui *p, const std::set<std::string> &folders,
                              const std::string &dir) {
    int n = 1;
    if (p->proj_folds.count(dir))
        for (const auto &f : folders)
            if (parent_of(f) == dir) n += project_tree_count(p, folders, f);
    return n;
}

// One folder row of the tree, then its visible children. `ry` walks down the
// column; the clip rect cuts whatever the panel cannot show.
static void project_tree_rows(dai_editor_ui *p, const std::set<std::string> &folders,
                              const std::string &dir, int depth, float px, float tree_w,
                              float cols_y, float cols_h, float &ry, int &rows) {
    dai_ui *ui = p->ui;
    const dai_ui_style *st = dai_ui_style_of(ui);
    const float ROW = 20.0f;
    float mx = 0, my = 0;
    int pressed = 0;
    dai_ui_mouse(ui, &mx, &my, nullptr, &pressed);
    int clicks_ok = !dai_ui_popup_active(ui);
    int right_pressed = dai_ui_right_pressed(ui);

    bool has_kids = false;
    for (const auto &f : folders)
        if (parent_of(f) == dir) { has_kids = true; break; }
    bool open = p->proj_folds.count(dir) != 0;
    ++rows;
    if (ry + ROW > cols_y && ry < cols_y + cols_h) {
        bool over = mx >= px && mx < px + tree_w && my >= ry && my < ry + ROW;
        int selected = p->proj_dir == dir;
        if (selected)  dai_ui_rect(ui, px, ry, tree_w, ROW, st->accent);
        else if (over) dai_ui_rect(ui, px, ry, tree_w, ROW, st->button_hover);
        float indent = 6.0f + 12.0f * (float)depth;
        if (has_kids)
            dai_ui_text(ui, px + indent, ry + 3.0f, open ? "v" : ">", st->text_dim);
        std::string label = dir.empty() ? "Assets" : base_of(dir);
        float text_x = px + indent + 14.0f;
        const char *fic = folder_has_content(p, folders, dir) ? DAI_ICON_FOLDER_FULL
                                                             : DAI_ICON_FOLDER;
        if (dai_ui_has_icon(ui, fic)) {
            dai_ui_icon_at(ui, fic, text_x, ry + 3.5f, 13.0f,
                           selected ? st->text : st->text_dim);
            text_x += 19.0f;
        }
        dai_ui_text(ui, text_x, ry + 3.0f, label.c_str(), st->text);
        // A row being dragged aims at this folder: light it up, and remember
        // it for the release. The tree is the only way to reach a folder that
        // is not in the current listing - "up one level", in other words.
        if (!p->drag_script.empty() && over && p->drag_script != dir) {
            p->proj_drop_dir = dir;
            p->proj_drop_ok = 1;
            dai_ui_rect_outline(ui, px, ry, tree_w, ROW, 1.0f, st->accent);
        }
        if (over && right_pressed && clicks_ok) {
            // Selects, does not navigate: opening the folder moves the listing
            // out from under the pointer before the menu has even appeared,
            // and then the menu is about a folder you are now inside.
            p->rename_click = dir;
            p->proj_sel_folder = dir;
            p->last_pick = dir;
            p->asset_sel = -1;
        }
        if (over && pressed && clicks_ok) {
            if (has_kids && mx < px + indent + 14.0f) {
                // The chevron folds, the name selects: hitting the triangle
                // must not lose the folder you were in.
                if (open) p->proj_folds.erase(dir);
                else      p->proj_folds.insert(dir);
            } else {
                p->proj_dir = dir;
                p->proj_list_scroll = 0.0f;
                if (has_kids) p->proj_folds.insert(dir);
            }
        }
    }
    ry += ROW;
    if (!open) return;
    for (const auto &f : folders)
        if (parent_of(f) == dir)
            project_tree_rows(p, folders, f, depth + 1, px, tree_w, cols_y, cols_h,
                              ry, rows);
}

// While the game runs, everything that is NOT the game goes behind glass.
// Unity does this for one reason and it is a good one: edits made in play mode
// are thrown away on Stop, and an editor that looks identical either way will
// eat an afternoon of work exactly once per user.
//
// Drawn INSIDE the panel's own layer (before dock_panel_end pops it), so it
// covers that panel and nothing else - the scene and game views never call it.
static void play_dim(dai_editor_ui *p, float px, float py, float pw, float ph) {
    if (dai_editor_state_get(p->ed) == DAI_EDITOR_EDIT) return;
    dai_ui_rect(p->ui, px, py, pw, ph, 0x66000000u);
}

// Where the pointer is pointing, on the ground. The scene view's ray against
// the y = 0 plane, which is the plane everything in an editor is placed on
// until it is moved. Falls back to a point a few metres in front of the camera
// when the ray runs parallel to the floor or points at the sky - "somewhere in
// front of you" beats "at the origin, behind you".
static bool viewport_ground_point(dai_editor_ui *p, float mx, float my, dai_vec3 *out) {
    if (!p || !p->ed || !out) return false;
    dai_vec3 o{}, d{};
    dai_editor_ray(p->ed, mx, my, &o, &d);
    if (d.y < -0.0001f) {
        float t = -o.y / d.y;
        if (t > 0.0f && t < 5000.0f) {
            *out = dai_vec3{ o.x + d.x * t, 0.0f, o.z + d.z * t };
            return true;
        }
    }
    const float FAR_ = 8.0f;
    *out = dai_vec3{ o.x + d.x * FAR_, o.y + d.y * FAR_, o.z + d.z * FAR_ };
    return true;
}

// A scene file - which is also what a prefab is. The two are the same format
// on purpose (a prefab is a scene with one root), so the only thing that can
// tell them apart is where they live: Scenes/ holds scenes, everything else
// holding a .daidalos holds a prefab.
// One console filter chip: the level's icon in its OWN colours, the count in
// the level's colour, on a plate that is never the level's colour.
//
// It used to be a browser_row, and a browser_row fills a SELECTED row with
// st->accent - which is blue. The info icon is also blue (COLOR_RULES paints
// it #4C8CCE) and a coloured icon refuses to be tinted, so switching the info
// filter on drew a blue glyph onto a blue plate and the chip went blank at
// exactly the moment it mattered. Warnings and errors only survived by luck
// of hue; nothing about that arrangement was deliberate.
//
// The fix is not a different blue. It is that state never rides on the fill:
// off is the chrome one step DARKER than the panel, on is one step lighter
// plus a rule underneath in the level's colour, which is the same "this tab
// is the live one" language the tab bars already speak. The glyph keeps its
// own colours in both states and only loses opacity when the filter is off -
// so the chip reads on all three of the dark themes, and would read on a
// light one, without the icon's hue entering into it at all.
static int console_chip(dai_editor_ui *p, float x, float y, float w, float h,
                        const char *icon, const char *label, uint32_t level_col,
                        int on) {
    dai_ui *ui = p->ui;
    const dai_ui_style *st = dai_ui_style_of(ui);
    float mx = 0, my = 0;
    int pressed = 0;
    dai_ui_mouse(ui, &mx, &my, nullptr, &pressed);
    bool over = mx >= x && mx < x + w && my >= y && my < y + h;
    uint32_t bg = on ? (over ? st->button_hover : st->button)
                     : (over ? st->button      : st->titlebar);
    dai_ui_rrect(ui, x, y, w, h, 4.0f, bg);
    dai_ui_rect_outline(ui, x, y, w, h, 1.0f, on ? level_col : st->panel_border);
    if (on) dai_ui_rect(ui, x + 3.0f, y + h - 3.0f, w - 6.0f, 2.0f, level_col);
    const float IS = 14.0f;
    float tx = x + 7.0f;
    if (icon && dai_ui_has_icon(ui, icon)) {
        dai_ui_icon_at(ui, icon, tx, y + (h - IS) * 0.5f, IS,
                       on ? 0xFFFFFFFFu : 0x7AFFFFFFu);
        tx += IS + 5.0f;
    }
    dai_ui_text(ui, tx, y + (h - dai_ui_text_height(ui)) * 0.5f, label,
                on ? level_col : st->text_dim);
    return over && pressed && !dai_ui_popup_active(ui);
}

// The project window's body, so it can live in a dock panel of any size.
// The console: engine messages and script print(), filterable by level.
static void console_body(dai_editor_ui *p, float px, float py, float pw, float ph) {
    dai_ui *ui = p->ui;
    const dai_ui_style *st = dai_ui_style_of(ui);
    // 30 still clipped the bottom border of the buttons against the divider
    // line drawn at py + BAR. Height of the row + the 3 px it starts at + a
    // pixel of air, measured from the font instead of guessed.
    const float BTN_H = dai_ui_text_height(ui) + 9.0f;
    const float BAR = BTN_H + 8.0f;
    static const char *LEVEL_NAME[3] = { "Info", "Warnings", "Errors" };
    const uint32_t LEVEL_COL[3] = { st->text_dim, rgba(230, 190, 90, 255), rgba(235, 105, 95, 255) };
    // What the chips use. Info is the icon's own blue instead of the body
    // text's grey: on a chip, "dim grey" is what OFF already means.
    const uint32_t CHIP_COL[3] = { rgba(0x76, 0xB4, 0xF0, 255), LEVEL_COL[1], LEVEL_COL[2] };

    uint32_t counts[3] = { 0, 0, 0 };
    for (const auto &l : p->log) counts[l.level] += l.count;

    float bx = px + 4.0f;
    if (browser_button(p, bx, py + 4.0f, 56.0f, BTN_H, "Clear")) {
        dai_editor_ui_log_clear(p);
        p->log_sel = -1;              // or the detail pane outlives its message
    }
    bx += 60.0f;
    // Copy: every visible line, one per row - for pasting an error into a
    // chat or a search. One line copies by clicking it, this is the batch.
    if (browser_button(p, bx, py + 4.0f, 56.0f, BTN_H, "Copy")) {
        std::string all;
        for (const auto &l : p->log) {
            if (!p->log_show[l.level]) continue;
            if (l.count > 1) { char pre[24]; std::snprintf(pre, sizeof(pre), "(%u) ", l.count); all += pre; }
            all += l.text;
            all += '\n';
        }
        if (!all.empty()) {
            dai_editor_ui_clipboard_set(p, 0, all.c_str());
            dai_editor_ui_toast(p, "console copied", 1.5f);
        }
    }
    bx += 60.0f;
    static const char *LEVEL_ICON[3] = { DAI_ICON_INFO, DAI_ICON_WARNING, DAI_ICON_ERROR };
    // Named while the panel is wide enough to say the words. A bare "0 0 0"
    // next to three small glyphs is a puzzle the first time you meet it, and
    // the console is usually docked across the whole bottom of the editor
    // where there is room for the answer.
    float need = 0.0f;
    for (int i = 0; i < 3; ++i) {
        char t[64];
        std::snprintf(t, sizeof(t), "%s  %u", LEVEL_NAME[i], counts[i]);
        need += dai_ui_text_width(ui, t) + 37.0f;
    }
    bool wide = bx + need < px + pw - 8.0f;
    for (int i = 0; i < 3; ++i) {
        char lbl[64];
        if (wide) std::snprintf(lbl, sizeof(lbl), "%s  %u", LEVEL_NAME[i], counts[i]);
        else      std::snprintf(lbl, sizeof(lbl), "%u", counts[i]);
        float w = dai_ui_text_width(ui, lbl) + 33.0f;
        if (console_chip(p, bx, py + 4.0f, w, BTN_H, LEVEL_ICON[i], lbl,
                         CHIP_COL[i], p->log_show[i]))
            p->log_show[i] = !p->log_show[i];
        bx += w + 4.0f;
    }
    dai_ui_rect(ui, px, py + BAR, pw, 1.0f, st->panel_border);

    // ---- the list, Unity's shape ------------------------------------------
    // A row is two text lines tall with the level's icon on the left and the
    // repeat count on the right: the first line is the message, the second is
    // where it came from. One click SELECTS (it used to copy, which meant the
    // clipboard changed every time you tried to read something), and the
    // selected message is written out in full underneath.
    const float TH = dai_ui_text_height(ui);
    const float ROW = TH * 2.0f + 9.0f;
    float mx = 0, my = 0;
    int ddown = 0, dpressed = 0;
    dai_ui_mouse(ui, &mx, &my, &ddown, &dpressed);
    bool inside = mx >= px && mx < px + pw && my >= py && my < py + ph;

    // The detail pane takes the bottom third, but never more than half and
    // never so much that fewer than two rows are left to pick from.
    float detail_h = 0.0f;
    if (p->log_sel >= 0 && p->log_sel < (int)p->log.size()) {
        detail_h = (ph - BAR) * 0.34f;
        float max_h = ph - BAR - ROW * 2.0f - 6.0f;
        if (detail_h > max_h) detail_h = max_h;
        if (detail_h < TH * 3.0f) detail_h = 0.0f;    // no room: list wins
    }
    p->log_detail = detail_h;
    const float LIST_H = ph - BAR - detail_h;

    if (inside && my < py + BAR + LIST_H) p->log_scroll -= dai_ui_wheel(ui) * 40.0f;
    float total = 0.0f;
    for (const auto &l : p->log) if (p->log_show[l.level]) total += ROW;
    float maxs = total - (LIST_H - 6.0f);
    if (maxs < 0.0f) maxs = 0.0f;
    if (p->log_scroll > maxs) p->log_scroll = maxs;
    if (p->log_scroll < 0.0f) p->log_scroll = 0.0f;

    dai_ui_clip_begin(ui, px, py + BAR + 1.0f, pw, LIST_H - 1.0f);
    float ry = py + BAR + 4.0f - p->log_scroll;
    if (p->log.empty())
        dai_ui_text(ui, px + 8.0f, ry + 2.0f,
                    "no messages - script print() and engine warnings land here", st->text_dim);
    static const char *ROW_ICON[3] = { DAI_ICON_INFO, DAI_ICON_WARNING, DAI_ICON_ERROR };
    int click_at = -1;
    for (size_t li = 0; li < p->log.size(); ++li) {
        const auto &l = p->log[li];
        if (!p->log_show[l.level]) continue;
        if (ry + ROW > py + BAR && ry < py + BAR + LIST_H) {
            bool sel = (int)li == p->log_sel;
            bool over = inside && mx < px + pw && my >= ry && my < ry + ROW &&
                        my < py + BAR + LIST_H;
            // Alternating bands, the way every console does it: the eye needs
            // something to follow across a wide panel.
            if (sel)        dai_ui_rect(ui, px, ry, pw, ROW, st->button_active);
            else if (over)  dai_ui_rect(ui, px, ry, pw, ROW, st->button);
            else if (li & 1) dai_ui_rect(ui, px, ry, pw, ROW, st->track);

            if (dai_ui_has_icon(ui, ROW_ICON[l.level]))
                dai_ui_icon_at(ui, ROW_ICON[l.level], px + 7.0f,
                               ry + (ROW - 16.0f) * 0.5f, 16.0f, 0xFFFFFFFFu);

            // The message is one line here however long it is; the rest of it
            // is what the pane below is for. Split on the first newline, so a
            // two part message shows its second part as the context line.
            std::string first = l.text, secondl;
            size_t nl = first.find('\n');
            if (nl != std::string::npos) { secondl = first.substr(nl + 1); first = first.substr(0, nl); }
            size_t nl2 = secondl.find('\n');
            if (nl2 != std::string::npos) secondl = secondl.substr(0, nl2);

            float tx = px + 29.0f;
            float tw_avail = pw - 29.0f - 44.0f;
            dai_ui_clip_begin(ui, tx, ry, tw_avail, ROW);
            dai_ui_text(ui, tx, ry + 4.0f, first.c_str(),
                        sel ? 0xFFFFFFFFu : LEVEL_COL[l.level]);
            if (!secondl.empty())
                dai_ui_text(ui, tx, ry + 4.0f + TH + 1.0f, secondl.c_str(), st->text_dim);
            dai_ui_clip_end(ui);

            // The repeat badge, right hand end - "this happened 47 times" is
            // the difference between a bug and a loop.
            if (l.count > 1) {
                char cb[24];
                std::snprintf(cb, sizeof(cb), "%u", l.count);
                float cw = dai_ui_text_width(ui, cb) + 14.0f;
                float cx = px + pw - cw - 8.0f;
                dai_ui_rrect(ui, cx, ry + (ROW - TH - 6.0f) * 0.5f, cw, TH + 6.0f, 7.0f, st->titlebar);
                dai_ui_text(ui, cx + 7.0f, ry + (ROW - TH) * 0.5f, cb, st->text_dim);
            }
            if (dpressed && over && !dai_ui_popup_active(ui)) click_at = (int)li;
        }
        ry += ROW;
    }
    dai_ui_clip_end(ui);
    if (click_at >= 0) {
        // Clicking the selected row again closes the pane: the same key opens
        // and shuts, which is the only arrangement nobody has to be told.
        p->log_sel = (p->log_sel == click_at) ? -1 : click_at;
        p->log_detail_scroll = 0.0f;
    }

    // ---- the detail pane ---------------------------------------------------
    if (detail_h > 0.0f && p->log_sel >= 0 && p->log_sel < (int)p->log.size()) {
        float dy = py + BAR + LIST_H;
        dai_ui_rect(ui, px, dy, pw, 1.0f, st->panel_border);
        dai_ui_rect(ui, px, dy + 1.0f, pw, detail_h - 1.0f, st->track);
        const auto &l = p->log[(size_t)p->log_sel];

        // Copy sits in the pane, not the toolbar: what you want is THIS
        // message, and you want it right where you are reading it.
        float bw = dai_ui_text_width(ui, "Copy") + 18.0f;
        if (browser_button(p, px + pw - bw - 8.0f, dy + 5.0f, bw, TH + 8.0f, "Copy")) {
            dai_editor_ui_clipboard_set(p, 0, l.text.c_str());
            dai_editor_ui_toast(p, "copied", 1.0f);
        }

        if (inside && my >= dy) p->log_detail_scroll -= dai_ui_wheel(ui) * 40.0f;
        // Wrap the text to the pane, so a long line is readable instead of
        // running off the right edge into nothing.
        dai_ui_clip_begin(ui, px, dy + 1.0f, pw - bw - 14.0f, detail_h - 2.0f);
        float wrap_w = pw - bw - 26.0f;
        float ty = dy + 6.0f - p->log_detail_scroll;
        float used = 0.0f;
        std::string rest = l.text;
        while (!rest.empty()) {
            std::string lineone;
            size_t nl = rest.find('\n');
            if (nl == std::string::npos) { lineone = rest; rest.clear(); }
            else { lineone = rest.substr(0, nl); rest = rest.substr(nl + 1); }
            // hard wrap on width, at a space where there is one
            for (;;) {
                if (dai_ui_text_width(ui, lineone.c_str()) <= wrap_w) {
                    dai_ui_text(ui, px + 8.0f, ty, lineone.c_str(), st->text);
                    ty += TH + 2.0f; used += TH + 2.0f;
                    break;
                }
                size_t cut = lineone.size();
                while (cut > 1 && dai_ui_text_width(ui, lineone.substr(0, cut).c_str()) > wrap_w) --cut;
                size_t sp = lineone.rfind(' ', cut);
                if (sp != std::string::npos && sp > cut / 2) cut = sp;
                dai_ui_text(ui, px + 8.0f, ty, lineone.substr(0, cut).c_str(), st->text);
                ty += TH + 2.0f; used += TH + 2.0f;
                lineone = lineone.substr(cut);
                while (!lineone.empty() && lineone[0] == ' ') lineone.erase(0, 1);
                if (lineone.empty()) break;
            }
        }
        dai_ui_clip_end(ui);
        float dmax = used - (detail_h - 12.0f);
        if (dmax < 0.0f) dmax = 0.0f;
        if (p->log_detail_scroll > dmax) p->log_detail_scroll = dmax;
        if (p->log_detail_scroll < 0.0f) p->log_detail_scroll = 0.0f;
    }
}

// The Localisation window: one row per key, one column per language.
//
// The alternative is what it replaces - two text files open side by side, and
// a new key typed into both at the same place. That is where localisation
// stops being done: not because translating is hard, but because ADDING a
// string is three steps in three files, and three steps get skipped.
//
// Missing entries are drawn as empty cells rather than as the key, on purpose.
// Everywhere else a missing key shows the key, because a player must never
// see a blank - but this is the one place whose job is to show what is NOT
// translated yet, and here a blank is the point.
static void loc_body(dai_editor_ui *p, float px, float py, float pw, float ph) {
    dai_ui *ui = p->ui;
    const dai_ui_style *st = dai_ui_style_of(ui);
    const float TH = dai_ui_text_height(ui);
    const float ROW = TH + 10.0f;
    const float BAR = ROW + 8.0f;

    if (!p->loc_loaded && p->loc_load) { p->loc_load(p->loc_user); p->loc_loaded = 1; }

    // ---- toolbar -----------------------------------------------------------
    float bx = px + 4.0f;
    if (browser_button(p, bx, py + 4.0f, 60.0f, ROW, "Reload")) {
        if (p->loc_load) p->loc_load(p->loc_user);
        p->loc_dirty = 0;
    }
    bx += 64.0f;
    {
        char lbl[32];
        std::snprintf(lbl, sizeof(lbl), p->loc_dirty ? "Save *" : "Save");
        if (browser_button(p, bx, py + 4.0f, 60.0f, ROW, lbl) && p->loc_save) {
            if (p->loc_save(p->loc_user)) {
                p->loc_dirty = 0;
                dai_editor_ui_toast(p, "string tables written", 1.5f);
            } else {
                dai_editor_ui_toast(p, "could not write the string tables", 2.5f);
            }
        }
    }
    bx += 68.0f;
    // New key. The field is where it is used - a dialog for one text box is a
    // dialog nobody opens twice.
    dai_ui_text(ui, bx, py + 4.0f + 5.0f, "New key", st->text_dim);
    bx += dai_ui_text_width(ui, "New key") + 6.0f;
    dai_ui_text_field(ui, "lockey", bx, py + 4.0f, 160.0f, ROW, p->loc_newkey,
                      sizeof(p->loc_newkey), nullptr);
    bx += 164.0f;
    if (browser_button(p, bx, py + 4.0f, 30.0f, ROW, "+") && p->loc_newkey[0]) {
        bool have = false;
        for (const auto &e : p->loc_rows) if (e.key == p->loc_newkey) have = true;
        if (have) dai_editor_ui_toast(p, "that key is already in the table", 2.0f);
        else {
            dai_editor_ui::LocEntry e;
            e.key = p->loc_newkey;
            e.vals.assign(p->loc_langs.size(), std::string());
            p->loc_rows.push_back(e);
            std::sort(p->loc_rows.begin(), p->loc_rows.end(),
                      [](const dai_editor_ui::LocEntry &a, const dai_editor_ui::LocEntry &b) {
                          return a.key < b.key;
                      });
            p->loc_newkey[0] = 0;
            p->loc_dirty = 1;
        }
    }
    bx += 36.0f;
    dai_ui_text(ui, bx, py + 4.0f + 5.0f, "New language", st->text_dim);
    bx += dai_ui_text_width(ui, "New language") + 6.0f;
    dai_ui_text_field(ui, "loclang", bx, py + 4.0f, 60.0f, ROW, p->loc_newlang,
                      sizeof(p->loc_newlang), nullptr);
    bx += 64.0f;
    if (browser_button(p, bx, py + 4.0f, 30.0f, ROW, "+") && p->loc_newlang[0]) {
        bool have = false;
        for (const auto &l : p->loc_langs) if (l == p->loc_newlang) have = true;
        if (!have) {
            p->loc_langs.push_back(p->loc_newlang);
            p->loc_names.push_back(p->loc_newlang);
            for (auto &e : p->loc_rows) e.vals.push_back(std::string());
            p->loc_dirty = 1;
        }
        p->loc_newlang[0] = 0;
    }
    dai_ui_rect(ui, px, py + BAR, pw, 1.0f, st->panel_border);

    if (p->loc_langs.empty()) {
        dai_ui_text(ui, px + 10.0f, py + BAR + 10.0f,
                    "no languages yet - type a code (de, fr, ja) and press +", st->text_dim);
        return;
    }

    // ---- the grid ----------------------------------------------------------
    const float KEYW = 220.0f;
    float colw = (pw - KEYW - 12.0f) / (float)p->loc_langs.size();
    if (colw < 90.0f) colw = 90.0f;

    // header
    float hy = py + BAR + 2.0f;
    dai_ui_rect(ui, px, hy, pw, ROW, st->titlebar);
    dai_ui_text(ui, px + 6.0f, hy + 4.0f, "Key", st->text);
    for (size_t c = 0; c < p->loc_langs.size(); ++c) {
        float cx = px + KEYW + colw * (float)c;
        char lbl[64];
        std::snprintf(lbl, sizeof(lbl), "%s  (%s)", p->loc_names[c].c_str(), p->loc_langs[c].c_str());
        dai_ui_clip_begin(ui, cx, hy, colw - 4.0f, ROW);
        dai_ui_text(ui, cx + 4.0f, hy + 4.0f, lbl, st->text);
        dai_ui_clip_end(ui);
    }

    float listy = hy + ROW + 1.0f, listh = ph - (listy - py);
    float mx = 0, my = 0;
    int pressed = 0;
    dai_ui_mouse(ui, &mx, &my, nullptr, &pressed);
    bool inside = mx >= px && mx < px + pw && my >= listy && my < listy + listh;
    if (inside) p->loc_scroll -= dai_ui_wheel(ui) * 40.0f;
    float total = ROW * (float)p->loc_rows.size();
    float maxs = total - listh + 4.0f;
    if (maxs < 0.0f) maxs = 0.0f;
    if (p->loc_scroll > maxs) p->loc_scroll = maxs;
    if (p->loc_scroll < 0.0f) p->loc_scroll = 0.0f;

    dai_ui_clip_begin(ui, px, listy, pw, listh);
    float ry = listy - p->loc_scroll;
    int delete_row = -1;
    for (size_t i = 0; i < p->loc_rows.size(); ++i) {
        auto &e = p->loc_rows[i];
        if (ry + ROW > listy && ry < listy + listh) {
            if (i & 1) dai_ui_rect(ui, px, ry, pw, ROW, st->track);
            dai_ui_clip_begin(ui, px + 4.0f, ry, KEYW - 30.0f, ROW);
            dai_ui_text(ui, px + 6.0f, ry + 4.0f, e.key.c_str(), st->text);
            dai_ui_clip_end(ui);
            // A key with an empty cell somewhere is the thing this window
            // exists to show, so it says so on the row rather than making you
            // scan the columns.
            bool gap = false;
            for (const std::string &v : e.vals) if (v.empty()) gap = true;
            if (gap) dai_ui_rect(ui, px, ry, 3.0f, ROW, rgba(230, 190, 90, 255));
            float dx2 = px + KEYW - 22.0f;
            if (browser_button(p, dx2, ry + 2.0f, 18.0f, ROW - 4.0f, "x")) delete_row = (int)i;

            for (size_t c = 0; c < p->loc_langs.size() && c < e.vals.size(); ++c) {
                float cx = px + KEYW + colw * (float)c;
                char fid[64];
                std::snprintf(fid, sizeof(fid), "loc%zu_%zu", i, c);
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%s", e.vals[c].c_str());
                if (dai_ui_text_field(ui, fid, cx, ry + 2.0f, colw - 6.0f, ROW - 4.0f,
                                      buf, sizeof(buf), nullptr)) {
                    if (e.vals[c] != buf) { e.vals[c] = buf; p->loc_dirty = 1; }
                }
            }
        }
        ry += ROW;
    }
    dai_ui_clip_end(ui);
    if (delete_row >= 0) {
        p->loc_rows.erase(p->loc_rows.begin() + delete_row);
        p->loc_dirty = 1;
    }
}

// The Script panel: the files you are editing, as tabs, with the code editor
// underneath and one line of status at the bottom.
//
// Why build one at all when VS Code exists: because the round trip matters.
// Changing a number in a behaviour and pressing Play should be two keys, not
// alt-tab, edit, save, alt-tab, play - and an editor that cannot show you the
// script you just attached is an editor you are always leaving. This one is
// deliberately small: no completion, no language server, no extensions. When
// you want those, the preference sends the file to the real thing and the
// project already carries the .d.ts that makes it understand the engine.
static void script_body(dai_editor_ui *p, float px, float py, float pw, float ph) {
    dai_ui *ui = p->ui;
    const dai_ui_style *st = dai_ui_style_of(ui);
    const float BAR = dai_ui_text_height(ui) + 12.0f;

    if (p->scripts_open.empty()) {
        dai_ui_text(ui, px + 10.0f, py + 10.0f,
                    "no file open - double click a script in the Project window",
                    st->text_dim);
        return;
    }
    if (p->script_tab >= (int)p->scripts_open.size()) p->script_tab = 0;
    if (p->script_tab < 0) p->script_tab = 0;

    // ---- the tab strip -----------------------------------------------------
    float tx = px + 2.0f;
    int close_at = -1;
    float mx = 0, my = 0;
    int pressed = 0;
    dai_ui_mouse(ui, &mx, &my, nullptr, &pressed);
    for (size_t i = 0; i < p->scripts_open.size(); ++i) {
        std::string lbl = base_of(p->scripts_open[i].path);
        if (p->scripts_open[i].dirty) lbl += " *";
        float tw = dai_ui_text_width(ui, lbl.c_str()) + 34.0f;
        if (tx + tw > px + pw - 4.0f) break;
        bool on = (int)i == p->script_tab;
        bool over = mx >= tx && mx < tx + tw && my >= py + 2.0f && my < py + BAR - 2.0f;
        dai_ui_rrect(ui, tx, py + 2.0f, tw, BAR - 4.0f, 4.0f,
                     on ? st->button : (over ? st->button_hover : st->titlebar));
        if (on) dai_ui_rect(ui, tx + 3.0f, py + BAR - 5.0f, tw - 6.0f, 2.0f, st->accent);
        dai_ui_text(ui, tx + 9.0f, py + 2.0f + (BAR - 4.0f - dai_ui_text_height(ui)) * 0.5f,
                    lbl.c_str(), on ? st->text : st->text_dim);
        // the close cross
        float cx = tx + tw - 15.0f, cy = py + BAR * 0.5f;
        bool over_x = mx >= cx - 6.0f && mx < cx + 6.0f && my >= cy - 6.0f && my < cy + 6.0f;
        uint32_t xc = over_x ? st->text : st->text_dim;
        dai_ui_line(ui, cx - 3.5f, cy - 3.5f, cx + 3.5f, cy + 3.5f, 1.4f, xc);
        dai_ui_line(ui, cx + 3.5f, cy - 3.5f, cx - 3.5f, cy + 3.5f, 1.4f, xc);
        if (pressed && over && !dai_ui_popup_active(ui)) {
            if (over_x) close_at = (int)i;
            else        p->script_tab = (int)i;
        }
        tx += tw + 3.0f;
    }
    dai_ui_rect(ui, px, py + BAR, pw, 1.0f, st->panel_border);

    // ---- the editor --------------------------------------------------------
    auto &cur = p->scripts_open[(size_t)p->script_tab];
    const float STATUS = dai_ui_text_height(ui) + 10.0f;
    float ey = py + BAR + 1.0f;
    float eh = ph - BAR - 1.0f - STATUS;
    if (eh > 20.0f) {
        // Only the comment/string/keyword sets differ, and they overlap
        // almost entirely - the extension picks which one leads.
        std::string ext = cur.path;
        size_t d = ext.find_last_of('.');
        ext = d == std::string::npos ? std::string() : ext.substr(d + 1);
        int lang = (ext == "cpp" || ext == "cc" || ext == "cxx" || ext == "h" ||
                    ext == "hpp") ? DAI_CODE_LANG_CPP : DAI_CODE_LANG_JS;
        char cid[64];
        std::snprintf(cid, sizeof(cid), "code%d", p->script_tab);
        if (dai_ui_code_edit(ui, cid, px + 2.0f, ey, pw - 4.0f, eh,
                             cur.buf.data(), cur.buf.size(), &cur.st, lang))
            cur.dirty = 1;
    }

    // ---- the status line ---------------------------------------------------
    float sy = py + ph - STATUS;
    dai_ui_rect(ui, px, sy, pw, 1.0f, st->panel_border);
    int line = 1, col = 1;
    dai_ui_code_caret_pos(cur.buf.data(), cur.st.caret, &line, &col);
    char info[256];
    std::snprintf(info, sizeof(info), "%s   Ln %d, Col %d%s",
                  cur.path.c_str(), line, col, cur.dirty ? "   (unsaved)" : "");
    dai_ui_text(ui, px + 8.0f, sy + 5.0f, info, st->text_dim);
    float bw = dai_ui_text_width(ui, "Save") + 18.0f;
    float ow = dai_ui_text_width(ui, "Open externally") + 18.0f;
    if (browser_button(p, px + pw - bw - 6.0f, sy + 3.0f, bw, STATUS - 6.0f, "Save"))
        dai_editor_ui_script_save(p);
    if (browser_button(p, px + pw - bw - ow - 12.0f, sy + 3.0f, ow, STATUS - 6.0f,
                       "Open externally") && p->open_asset)
        p->open_asset(nullptr, cur.path.c_str(), p->open_asset_user);

    if (close_at >= 0) {
        // A tab with unsaved work says so rather than dropping it.
        if (p->scripts_open[(size_t)close_at].dirty) {
            dai_editor_ui_toast(p, "unsaved - press Save first, then close", 2.5f);
        } else {
            p->scripts_open.erase(p->scripts_open.begin() + close_at);
            if (p->script_tab >= (int)p->scripts_open.size())
                p->script_tab = (int)p->scripts_open.size() - 1;
            if (p->script_tab < 0) p->script_tab = 0;
        }
    }
}

// The audio mixer: the four busses every game grows anyway.
static void audio_body(dai_editor_ui *p, float px, float py, float pw, float ph) {
    dai_ui *ui = p->ui;
    const dai_ui_style *st = dai_ui_style_of(ui);
    static const char *BUS[4] = { "Master", "Music", "SFX", "UI" };
    dai_ui_text(ui, px + 8.0f, py + 6.0f, "Mixer", st->text_dim);
    float y = py + 26.0f;
    for (int i = 0; i < 4; ++i) {
        dai_ui_text(ui, px + 8.0f, y + 4.0f, BUS[i], st->text);
        // A speaker with a cross through it needs no translation and no
        // column width; "muted"/"on" needed both.
        if (dai_ui_icon_button_at(ui, p->bus_mute[i] ? DAI_ICON_VOLUME_X : DAI_ICON_VOLUME,
                                  px + 70.0f, y, 24.0f, 20.0f, p->bus_mute[i]))
            p->bus_mute[i] = !p->bus_mute[i];
        // The slider is a bar you drag - the same widget the inspector uses
        // for a number, without pretending to be a knob.
        float tx = px + 124.0f, tw = pw - 200.0f;
        if (tw > 40.0f) {
            dai_ui_rect(ui, tx, y + 7.0f, tw, 6.0f, st->track);
            dai_ui_rect(ui, tx, y + 7.0f, tw * p->bus_gain[i], 6.0f, st->accent);
            float mx = 0, my = 0; int down = 0;
            dai_ui_mouse(ui, &mx, &my, &down, nullptr);
            if (down && mx >= tx && mx <= tx + tw && my >= y && my <= y + 20.0f) {
                p->bus_gain[i] = (mx - tx) / tw;
                if (p->bus_gain[i] < 0.0f) p->bus_gain[i] = 0.0f;
                if (p->bus_gain[i] > 1.0f) p->bus_gain[i] = 1.0f;
            }
            char v[24];
            std::snprintf(v, sizeof(v), "%3.0f%%", p->bus_gain[i] * 100.0f);
            dai_ui_text(ui, tx + tw + 10.0f, y + 4.0f, v, st->text_dim);
        }
        y += 26.0f;
    }
    (void)ph;
}

static void project_body(dai_editor_ui *p, float px, float py, float pw, float ph) {
    dai_ui *ui = p->ui;
    const dai_ui_style *st = dai_ui_style_of(ui);
    float mx = 0, my = 0;
    int down = 0, pressed = 0;
    dai_ui_mouse(ui, &mx, &my, &down, &pressed);
    float wheel = dai_ui_wheel(ui);
    int clicks_ok = !dai_ui_popup_active(ui);
    int right_pressed = dai_ui_right_pressed(ui);

    // ---- top bar: breadcrumb left, search and the mode switch right ---------
    const float BAR = 26.0f;
    float by = py + 3.0f;
    if (p->proj_tab == 0) {
        float bx = px + 4.0f;
        // The breadcrumb: Assets / models / props - every segment a button.
        std::vector<std::string> segs;
        segs.push_back(std::string());
        if (!p->proj_dir.empty()) {
            size_t pos = 0;
            while (pos < p->proj_dir.size()) {
                size_t slash = p->proj_dir.find('/', pos);
                segs.push_back(p->proj_dir.substr(pos, slash == std::string::npos
                                                          ? slash : slash - pos));
                if (slash == std::string::npos) break;
                pos = slash + 1;
            }
        }
        std::string acc;
        for (size_t i = 0; i < segs.size(); ++i) {
            if (i > 0) { if (i > 1) acc += "/"; acc += segs[i]; }
            std::string target = i == 0 ? std::string() : acc;
            const char *label = i == 0 ? "Assets" : segs[i].c_str();
            float tw = dai_ui_text_width(ui, label) + 14.0f;
            if (browser_row(p, bx, by, tw, BAR - 4.0f, nullptr, label,
                            (int)i == (int)segs.size() - 1, 0) && clicks_ok) {
                p->proj_dir = target;
                p->proj_list_scroll = 0.0f;
            }
            bx += tw;
            if (i + 1 < segs.size()) {
                dai_ui_text(ui, bx + 1.0f, by + 3.0f, "/", st->text_dim);
                bx += 10.0f;
            }
        }
        // The mode switch sits next to the breadcrumb (Assets | Projects) -
        // both views of the same window belong together. Search keeps right.
        float pw_btn = dai_ui_text_width(ui, "Projects") + 16.0f;
        if (browser_row(p, bx + 6.0f, by, pw_btn, BAR - 4.0f, nullptr,
                        "Projects", 0, 0) && clicks_ok) {
            p->proj_tab = 1;
            dai_editor_ui_projects_refresh(p);
        }
        float right = px + pw - 4.0f;
        float sw = right - 20.0f - (bx + 6.0f + pw_btn);
        if (sw > 170.0f) sw = 170.0f;
        if (sw >= 70.0f)
            dai_ui_text_field(ui, "projsearch", right - sw, by + 1.0f, sw, BAR - 6.0f,
                              p->proj_search, sizeof(p->proj_search), nullptr);
    } else {
        float bw = dai_ui_text_width(ui, "< Files") + 16.0f;
        if (browser_row(p, px + 4.0f, by, bw, BAR - 4.0f, nullptr, "< Files", 0, 0) &&
            clicks_ok)
            p->proj_tab = 0;
        dai_ui_text(ui, px + 4.0f + bw + 10.0f, by + 3.0f, "Projects", st->text);
    }
    dai_ui_rect(ui, px, py + BAR, pw, 1.0f, st->panel_border);

    // ---- the Projects list keeps the flow layout it always had --------------
    if (p->proj_tab == 1) {
        // Same two-column shape as the file browser: the list of projects on
        // the left, what the selected one contains on the right. Full width
        // buttons stacked down the window read as a settings page, not as a
        // list of things you pick from.
        const float pcols_y = py + BAR + 1.0f;
        const float pcols_h = ph - BAR - 1.0f;
        float ptree_w = p->proj_tree_w;
        if (ptree_w > pw * 0.5f) ptree_w = pw * 0.5f;
        if (ptree_w < 100.0f) ptree_w = pw > 200.0f ? 100.0f : pw * 0.5f;
        float pright_x = px + ptree_w + 5.0f;
        dai_ui_rect(ui, px + ptree_w, pcols_y, 1.0f, pcols_h, st->panel_border);

        const float PROW = 20.0f;
        float pry = pcols_y + 2.0f;
        dai_ui_clip_begin(ui, px, pcols_y, ptree_w, pcols_h);
        if (p->projects.empty())
            dai_ui_text(ui, px + 8.0f, pry + 4.0f,
                        p->proj_list ? "no projects yet" : "no project host", st->text_dim);
        for (const std::string &name : p->projects) {
            int is_open = name == p->proj_current;
            if (browser_row(p, px + 2.0f, pry, ptree_w - 4.0f, PROW,
                            DAI_ICON_FOLDER, name.c_str(), is_open,
                            rgba(0xD8, 0xB4, 0x6A, 255)) && clicks_ok &&
                p->proj_open) {
                if (p->proj_open(name.c_str(), p->proj_user)) {
                    p->proj_current = name;
                    p->proj_tab = 0;
                }
            }
            pry += PROW;
        }
        dai_ui_clip_end(ui);

        // Right column: what this project holds, and how to make a new one.
        dai_ui_clip_begin(ui, pright_x, pcols_y, px + pw - pright_x, pcols_h);
        float ry2 = pcols_y + 4.0f;
        if (p->proj_current.empty())
            dai_ui_text(ui, pright_x + 6.0f, ry2, "no project open - pick one on the left", st->text_dim);
        else
            dai_ui_text(ui, pright_x + 6.0f, ry2, p->proj_current.c_str(), st->text);
        ry2 += 22.0f;
        // Scenes are FILES of the open project, like Unity's .unity assets.
        if (p->scene_list) {
            dai_ui_text(ui, pright_x + 6.0f, ry2, "Scenes", st->text_dim);
            ry2 += 18.0f;
            for (uint32_t i = 0; ; ++i) {
                const char *nm = p->scene_list(i, p->scene_user);
                if (!nm) break;
                if (browser_row(p, pright_x + 6.0f, ry2, 220.0f, PROW, DAI_ICON_FILE, nm, 0,
                                rgba(0x6C, 0xB2, 0xF0, 255)) &&
                    clicks_ok && p->scene_open)
                    p->scene_open(nm, p->scene_user);
                ry2 += PROW;
            }
            ry2 += 6.0f;
            dai_ui_text_field(ui, "scenename", pright_x + 6.0f, ry2, 150.0f, 22.0f,
                              p->scene_name_buf, sizeof(p->scene_name_buf), nullptr);
            if (browser_button(p, pright_x + 162.0f, ry2, 110.0f, 22.0f, "Save scene as") &&
                p->scene_save_as && p->scene_name_buf[0]) {
                std::string nm = p->scene_name_buf;
                const std::string ext = ".daidalos";
                if (nm.size() <= ext.size() || nm.compare(nm.size() - ext.size(), ext.size(), ext) != 0)
                    nm += ext;
                if (p->scene_save_as(nm.c_str(), p->scene_user)) p->scene_name_buf[0] = 0;
            }
            ry2 += 30.0f;
        }
        dai_ui_text(ui, pright_x + 6.0f, ry2, "New project", st->text_dim);
        ry2 += 18.0f;
        dai_ui_text_field(ui, "projname", pright_x + 6.0f, ry2, 150.0f, 22.0f,
                          p->proj_name_buf, sizeof(p->proj_name_buf), nullptr);
        if (browser_button(p, pright_x + 162.0f, ry2, 110.0f, 22.0f, "Create + open") &&
            p->proj_create) {
            if (p->proj_name_buf[0] && p->proj_create(p->proj_name_buf, p->proj_user)) {
                p->proj_current = p->proj_name_buf;
                p->proj_name_buf[0] = 0;
                dai_editor_ui_projects_refresh(p);
                p->proj_tab = 0;
            }
        }
        dai_ui_clip_end(ui);
        return;
    }

    // ---- the folder tree, derived from the flat asset list -------------------
    std::set<std::string> folders;
    for (const char *a : p->assets) {
        std::string sa = a ? a : "";
        for (size_t slash = sa.find('/'); slash != std::string::npos;
             slash = sa.find('/', slash + 1))
            folders.insert(sa.substr(0, slash));
    }
    // Directories the host saw on disk: empty ones too. Without this a fresh
    // "New Folder" had no file to hide behind and simply never appeared.
    for (const char *a : p->folders_disk) {
        std::string sa = a ? a : "";
        if (sa.empty()) continue;
        folders.insert(sa);
        for (size_t slash = sa.find('/'); slash != std::string::npos;
             slash = sa.find('/', slash + 1))
            folders.insert(sa.substr(0, slash));
    }
    // The folder being shown exists even with nothing in it (a fresh one).
    if (!p->proj_dir.empty()) {
        folders.insert(p->proj_dir);
        for (std::string anc = parent_of(p->proj_dir); !anc.empty();
             anc = parent_of(anc))
            folders.insert(anc);
    }

    const float cols_y = py + BAR + 1.0f;
    const float cols_h = ph - BAR - 1.0f;
    float tree_w = p->proj_tree_w;
    float max_tree = pw * 0.5f;
    if (tree_w > max_tree) tree_w = max_tree;
    if (tree_w < 100.0f) tree_w = pw > 200.0f ? 100.0f : max_tree;
    float list_x = px + tree_w + 5.0f;
    float list_w = px + pw - list_x;

    // Whatever the last right click hit is a fact about THAT click. Cleared
    // before the rows redraw, so a press on empty space leaves it empty and
    // the menu below can tell the difference between "this file" and "here".
    if (right_pressed) p->rename_click.clear();

    // The divider between the columns drags, like every other split.
    bool over_div = mx >= px + tree_w && mx < px + tree_w + 5.0f &&
                    my >= cols_y && my < cols_y + cols_h;
    if (over_div || p->proj_tree_drag) dai_ui_cursor_set(ui, DAI_CURSOR_SIZE_WE);
    if (over_div && pressed && clicks_ok) { p->proj_tree_drag = 1; dai_ui_claim_mouse(ui); }
    if (p->proj_tree_drag && down) { p->proj_tree_w = mx - px; dai_ui_claim_mouse(ui); }
    else if (!down) p->proj_tree_drag = 0;
    dai_ui_rect(ui, px + tree_w + 2.0f, cols_y, 1.0f, cols_h, st->panel_border);

    // ---- the tree column ------------------------------------------------------
    {
        dai_ui_clip_begin(ui, px, cols_y, tree_w, cols_h);
        float tmax = (float)project_tree_count(p, folders, std::string()) * 20.0f
                   + 4.0f - cols_h;
        if (tmax < 0.0f) tmax = 0.0f;
        if (mx >= px && mx < px + tree_w && my >= cols_y && my < cols_y + cols_h)
            p->proj_tree_scroll -= wheel * 32.0f;
        if (p->proj_tree_scroll > tmax) p->proj_tree_scroll = tmax;
        if (p->proj_tree_scroll < 0.0f) p->proj_tree_scroll = 0.0f;
        float ry = cols_y + 2.0f - p->proj_tree_scroll;
        int rows = 0;
        project_tree_rows(p, folders, std::string(), 0, px, tree_w, cols_y, cols_h,
                          ry, rows);
        dai_ui_clip_end(ui);
    }

    // ---- the contents of the current folder -----------------------------------
    std::vector<std::string> subfolders;
    std::vector<int> files;
    bool searching = p->proj_search[0] != 0;
    if (!searching)
        for (const auto &f : folders)
            if (parent_of(f) == p->proj_dir) subfolders.push_back(base_of(f));
    std::string needle;
    for (const char *c = p->proj_search; *c; ++c)
        needle += (char)std::tolower((unsigned char)*c);
    for (size_t i = 0; i < p->assets.size(); ++i) {
        std::string sa = p->assets[i] ? p->assets[i] : "";
        if (searching) {
            std::string low;
            for (char c : sa) low += (char)std::tolower((unsigned char)c);
            if (low.find(needle) == std::string::npos) continue;
        } else if (parent_of(sa) != p->proj_dir) continue;
        files.push_back((int)i);
    }
    if (!searching)
        std::sort(files.begin(), files.end(), [&](int a, int b) {
            return std::strcmp(p->assets[(size_t)a], p->assets[(size_t)b]) < 0;
        });

    // Recomputed here and used here. It used to be decided at the top of the
    // function and trusted a hundred lines later, across every row the browser
    // draws - and a row can change the selection.
    if (p->asset_sel >= (int)p->assets.size()) p->asset_sel = -1;
    int sel_valid = p->asset_sel >= 0 && p->assets[(size_t)p->asset_sel] != nullptr;
    int folder_sel = !p->proj_sel_folder.empty();
    float strip_h = (sel_valid || folder_sel || p->script_focus) ? 28.0f : 0.0f;
    float list_h = cols_h - strip_h;
    const float ROW = 20.0f;
    {
        dai_ui_clip_begin(ui, list_x, cols_y, list_w, list_h);
        // The listing knows its own length before it draws a thing: the
        // folders and the files it already collected, or the one line that
        // says there are none.
        int want_rows = (int)subfolders.size() + (int)files.size();
        if (want_rows == 0) want_rows = 1;
        float lmax = (float)want_rows * ROW + 4.0f - list_h;
        if (lmax < 0.0f) lmax = 0.0f;
        if (mx >= list_x && mx < list_x + list_w && my >= cols_y && my < cols_y + list_h)
            p->proj_list_scroll -= wheel * 32.0f;
        if (p->proj_list_scroll > lmax) p->proj_list_scroll = lmax;
        if (p->proj_list_scroll < 0.0f) p->proj_list_scroll = 0.0f;
        float ry = cols_y + 2.0f - p->proj_list_scroll;
        int rows = 0;
        if (p->assets.empty()) {
            dai_ui_text(ui, list_x + 8.0f, ry + 4.0f,
                        "nothing mounted - open or create a project", st->text_dim);
            ++rows;
        } else if (subfolders.empty() && files.empty()) {
            dai_ui_text(ui, list_x + 8.0f, ry + 4.0f,
                        searching ? "no matches" : "empty folder - right click to create",
                        st->text_dim);
            ++rows;
        }
        for (const std::string &name : subfolders) {
            ++rows;
            if (ry + ROW > cols_y && ry < cols_y + list_h) {
                std::string ffull = p->proj_dir.empty() ? name : p->proj_dir + "/" + name;
                if (ffull == p->rename_asset) {
                    p->rename_drawn = 1;
                    int commit = 0;
                    dai_ui_text_focus_next(ui);
                    dai_ui_text_field(ui, "assetrename", list_x + 2.0f, ry, list_w - 4.0f,
                                      ROW, p->rename_asset_buf, sizeof(p->rename_asset_buf),
                                      &commit);
                    if (dai_ui_text_active(ui)) p->rename_seen_active = 1;
                    if (commit || (p->rename_seen_active && !dai_ui_text_active(ui))) {
                        std::string nn = p->rename_asset_buf;
                        if (script_name_ok(nn) && p->asset_rename) {
                            std::string dir = parent_of(p->rename_asset);
                            std::string np = dir.empty() ? nn : dir + "/" + nn;
                            if (np != p->rename_asset)
                                p->asset_rename(p->rename_asset.c_str(), np.c_str(), p->rename_user);
                        }
                        p->rename_asset.clear();
                        p->rename_seen_active = 0;
                        p->want_refresh = 1;
                    }
                } else {
                    int over_f = mx >= list_x + 2.0f && mx < list_x + list_w - 4.0f &&
                                 my >= ry && my < ry + ROW;
                    {
                        int fpress = 0;
                        dai_ui_mouse(ui, nullptr, nullptr, nullptr, &fpress);
                        if (over_f && fpress && clicks_ok) {
                            p->drag_pending = ffull;      // a folder drags too
                            p->drag_px = mx; p->drag_py = my;
                        }
                    }
                    const char *fic2 = folder_has_content(p, folders, ffull)
                                     ? DAI_ICON_FOLDER_FULL : DAI_ICON_FOLDER;
                    if (browser_row(p, list_x + 2.0f, ry, list_w - 4.0f, ROW,
                                    fic2, name.c_str(),
                                    ffull == p->proj_sel_folder ||
                                    (p->click_kind == 1 && p->click_path == ffull),
                                    rgba(0xD8, 0xB4, 0x6A, 255)) && clicks_ok) {
                        // One click SELECTS it. Two go in. Both wait for the
                        // button to come up: a press that dragged the folder
                        // somewhere else was never a click on it.
                        p->click_kind = 1;
                        p->click_path = ffull;
                        p->click_dbl = dai_ui_double_click(ui) ? 1 : 0;
                    }
                    // ...and it is where a dragged row lands.
                    if (!p->drag_script.empty() && over_f && p->drag_script != ffull) {
                        p->proj_drop_dir = ffull;
                        p->proj_drop_ok = 1;
                        dai_ui_rect_outline(ui, list_x + 2.0f, ry, list_w - 4.0f, ROW,
                                            1.0f, st->accent);
                    }
                    // A right click SELECTS the folder it landed on, the way
                    // a left click does. Acting on the old selection while the
                    // pointer sits on another row is how the wrong folder gets
                    // deleted - and it is the one mistake with no undo.
                    if (over_f && right_pressed && clicks_ok) {
                        p->rename_click = ffull;
                        p->proj_sel_folder = ffull;
                        p->last_pick = ffull;
                        p->asset_sel = -1;
                    }
                }
            }
            ry += ROW;
        }
        for (int fi : files) {
            ++rows;
            if (ry + ROW > cols_y && ry < cols_y + list_h) {
                std::string full = asset_at(p, fi);
                std::string label = searching ? full : base_of(full);
                int selected = fi == p->asset_sel;
                bool over = mx >= list_x + 2.0f && mx < list_x + list_w - 4.0f &&
                            my >= ry && my < ry + ROW;
                if (full == p->rename_asset) {
                    p->rename_drawn = 1;
                    // Fresh out of "Create: Script": the row IS the rename
                    // field - type the name, Enter commits, Escape keeps the
                    // default. Focus loss commits too, or it waits forever.
                    int commit = 0;
                    dai_ui_text_focus_next(ui);
                    dai_ui_text_field(ui, "assetrename", list_x + 2.0f, ry, list_w - 4.0f,
                                      ROW, p->rename_asset_buf, sizeof(p->rename_asset_buf),
                                      &commit);
                    if (dai_ui_text_active(ui)) p->rename_seen_active = 1;
                    if (commit || (p->rename_seen_active && !dai_ui_text_active(ui))) {
                        std::string nn = p->rename_asset_buf;
                        // Keep whatever extension the OLD path had - a folder
                        // has none, and forcing ".js" onto one turned "models"
                        // into "models.js" the moment it was renamed.
                        std::string ob = base_of(p->rename_asset), ext;
                        size_t dot = ob.find_last_of('.');
                        if (dot != std::string::npos) ext = ob.substr(dot);
                        if (!ext.empty() && nn.size() > ext.size() &&
                            nn.compare(nn.size() - ext.size(), ext.size(), ext) == 0)
                            nn.resize(nn.size() - ext.size());
                        if (script_name_ok(nn)) {
                            std::string dir = parent_of(p->rename_asset);
                            std::string np = dir.empty() ? nn + ext : dir + "/" + nn + ext;
                            if (np != p->rename_asset && p->asset_rename)
                                p->asset_rename(p->rename_asset.c_str(), np.c_str(), p->rename_user);
                        }
                        p->rename_asset.clear();
                        p->rename_seen_active = 0;
                        p->want_refresh = 1;
                    }
                } else {
                    if (browser_row(p, list_x + 2.0f, ry, list_w - 4.0f, ROW,
                                    icon_for_asset(full), label.c_str(),
                                    selected || (p->click_kind == 2 && p->click_path == full),
                                    icon_color_for_asset(full, 0))
                            && clicks_ok) {
                        // Armed only. What it means - select, open, place -
                        // is decided when the button comes up, because until
                        // then it may still turn out to have been a drag.
                        p->click_kind = 2;
                        p->click_ctrl = p->last_ctrl_held ? 1 : 0;
                        p->click_shift = p->last_shift_held ? 1 : 0;
                        p->click_path = full;
                        p->click_dbl = dai_ui_double_click(ui) ? 1 : 0;
                    }
                    // A right click selects what it landed on, then the menu opens.
                    if (over && right_pressed && clicks_ok) {
                        p->asset_sel = fi;
                        p->rename_click = full;
                        p->last_pick = full;
                        p->proj_sel_folder.clear();   // a file and a folder are not both it
                    }
                    // EVERY row arms a drag now, not just a .js: press, move
                    // 6 px, and it travels with the cursor. Where it lands
                    // decides what it meant - a folder moves it there, a
                    // hierarchy node still attaches a script. A browser in
                    // which only one file type can be picked up is not a file
                    // browser, it is a script list with icons.
                    int lpress = 0;
                    dai_ui_mouse(ui, nullptr, nullptr, nullptr, &lpress);
                    if (over && lpress && clicks_ok) {
                        p->drag_pending = full;
                        p->drag_px = mx; p->drag_py = my;
                    }
                }
            }
            ry += ROW;
        }
        (void)rows;   // the clamp happened before the draw, where it belongs
        dai_ui_clip_end(ui);
    }

    // ---- the bottom strip: what the selection can do ---------------------------
    if (strip_h > 0.0f) {
        float sy = py + ph - strip_h;
        dai_ui_rect(ui, list_x, sy - 1.0f, list_w, 1.0f, st->panel_border);
        if (p->script_focus) {
            dai_ui_text(ui, list_x + 4.0f, sy + 7.0f, "New script:", st->text_dim);
            float fx = list_x + 76.0f;
            float fw = list_x + list_w - fx - 148.0f;
            if (fw > 40.0f)
                dai_ui_text_field(ui, "newscript", fx, sy + 3.0f, fw, 22.0f,
                                  p->script_name_buf, sizeof(p->script_name_buf), nullptr);
            if (browser_button(p, list_x + list_w - 140.0f, sy + 3.0f, 66.0f, 22.0f,
                               "Create") && p->script_create && p->script_name_buf[0]) {
                // The script lands in the folder the browser shows.
                std::string name = p->proj_dir.empty()
                    ? p->script_name_buf
                    : p->proj_dir + "/" + p->script_name_buf;
                if (p->script_create(name.c_str(), p->script_user)) {
                    p->script_name_buf[0] = 0;
                    p->script_focus = 0;
                    p->want_refresh = 1;
                }
            }
            if (browser_button(p, list_x + list_w - 68.0f, sy + 3.0f, 64.0f, 22.0f,
                               "Cancel"))
                p->script_focus = 0;
        } else if (folder_sel && !sel_valid) {
            std::string b = base_of(p->proj_sel_folder);
            dai_ui_text(ui, list_x + 4.0f, sy + 7.0f, b.c_str(), st->text);
            float bw2 = dai_ui_text_width(ui, "Open") + 20.0f;
            if (browser_button(p, list_x + list_w - bw2 - 6.0f, sy + 3.0f, bw2, 22.0f, "Open")) {
                p->proj_dir = p->proj_sel_folder;
                project_expand_to(p, p->proj_dir);
                p->proj_list_scroll = 0.0f;
                p->proj_sel_folder.clear();
            }
        } else if (p->asset_sel >= 0 && p->asset_sel < (int)p->assets.size()) {
            const char *pick = asset_at(p, p->asset_sel);
            std::string base = base_of(pick);
            dai_ui_text(ui, list_x + 4.0f, sy + 7.0f, base.c_str(), st->text_dim);
            if (*pick && is_behaviour_file(pick)) {
                // No assign button on purpose: a script attaches by DRAG -
                // onto the object in the hierarchy or into the inspector.
                dai_ui_text(ui, list_x + list_w - 220.0f, sy + 7.0f,
                            "drag onto an object to attach", st->text_dim);
            } else {
                // Two buttons because the difference is physical, not cosmetic:
                // one body for the whole model, or one body per piece.
                if (browser_button(p, list_x + list_w - 148.0f, sy + 3.0f, 70.0f, 22.0f,
                                   "Place")) {
                    p->pending_asset = pick;
                    p->pending_as_tree = 0;
                }
                if (browser_button(p, list_x + list_w - 72.0f, sy + 3.0f, 68.0f, 22.0f,
                                   "As tree")) {
                    p->pending_asset = pick;
                    p->pending_as_tree = 1;
                }
            }
        }
    }

    // Right click anywhere in the panel: the Create menu, the way Unity's
    // project window does it - and a SECOND right click somewhere else moves
    // it there. Refusing while one was already open meant the first press was
    // swallowed and you had to press twice, which reads as the menu being
    // stuck to the spot it first appeared.
    if (right_pressed && dai_ui_root_hovered(ui, "Project") &&
        !p->menu_node.open && !p->menu_canvas.open) {
        p->menu_project.open = 0;          // forget where it was
        p->menu_project.placed = 0;        // ...including the frozen position
        dai_ui_popup_open(&p->menu_project, mx, my);
    }
}

void dai_editor_ui_frame(dai_editor_ui *p, float vw, float vh) {
    if (!p) return;
    dai_ui *ui = p->ui;
    const dai_ui_style *st = dai_ui_style_of(ui);
    const float TOP = 34.0f, BOTTOM = 24.0f;

    if (!p->layout_ready) dai_editor_ui_layout_reset(p, vw, vh);
    p->layout_w = vw; p->layout_h = vh;

    // The chrome: solid bars top and bottom, the dock tree in between. There
    // is no "free area" any more - every pixel between the bars belongs to
    // exactly one panel, which is what makes overlapping impossible.
    dai_ui_rect(ui, 0, 0, vw, TOP, st->chrome);
    dai_ui_rect(ui, 0, vh - BOTTOM, vw, BOTTOM, st->chrome);
    dai_ui_rect(ui, 0, vh - BOTTOM, vw, 1.0f, st->panel_border);

    dai_editor_ui_toolbar(p, 0.0f, 0.0f, vw);

    // Play switches to the Game tab and Stop switches back, once each - the
    // tabs are real tabs of one dock leaf now, so this is just a selection.
    int playing = dai_editor_state_get(p->ed) != DAI_EDITOR_EDIT;
    if (playing && !p->view_was_playing) dai_dock_focus(p->dock, "Game");
    if (!playing && p->view_was_playing) dai_dock_focus(p->dock, "Scene");
    p->view_was_playing = playing;
    if (p->settings_open && !dai_dock_is_open(p->dock, "Settings")) {
        // Settings is a panel like any other - it docks, it tabs, it closes.
        dai_dock_add(p->dock, "Settings", DAI_DOCK_RIGHT, 0.24f);
        dai_dock_focus(p->dock, "Settings");
    }

    // Panels this build knows about, every frame. Idempotent by design (see
    // dai_dock_add_tab): the first call registers, the rest return at once.
    // A closed panel stays closed - dai_dock_begin skips closed registrations.
    dai_dock_add_tab(p->dock, "Console", "Project");
    dai_dock_add_tab(p->dock, "Localisation", "Project");
    dai_dock_add_tab(p->dock, "Audio", "Project");
    dai_dock_add_tab(p->dock, "Script", "Scene");

    dai_dock_begin(p->dock, ui, 0.0f, TOP, vw, vh - TOP - BOTTOM);

    float px, py, pw, ph;
    if (dai_dock_panel(p->dock, "Hierarchy", &px, &py, &pw, &ph)) {
        dai_ui_panel_begin(ui, px, py, pw, ph, nullptr);
        hierarchy_body(p, ph - 8.0f);
        play_dim(p, px, py, pw, ph);
        // Right click on empty space in the hierarchy: the GameObject menu.
        float mx = 0, my = 0;
        dai_ui_mouse(ui, &mx, &my, nullptr, nullptr);
        if (dai_ui_right_pressed(ui) && dai_ui_root_hovered(ui, "Hierarchy") &&
            !p->menu_node.open && !p->menu_canvas.open)
            dai_ui_popup_open(&p->menu_canvas, mx, my);
        dai_ui_panel_end(ui);
        dai_dock_panel_end(p->dock);
    }
    if (dai_dock_panel(p->dock, "Project", &px, &py, &pw, &ph)) {
        dai_ui_panel_begin(ui, px, py, pw, ph, nullptr);
        project_body(p, px, py, pw, ph);
        play_dim(p, px, py, pw, ph);
        dai_ui_panel_end(ui);
        dai_dock_panel_end(p->dock);
    }
    (void)0;
    // Add Tab can put a second Inspector or Console next to the first - two
    // locks on two different objects is a real workflow, not a bug. So the
    // dock is asked for EVERY instance, not the first.
    for (int inst = 0; inst < DAI_MAX_PANEL_INSTANCES &&
                       dai_dock_panel(p->dock, "Localisation", &px, &py, &pw, &ph); ++inst) {
        dai_ui_panel_begin(ui, px, py, pw, ph, nullptr);
        loc_body(p, px, py, pw, ph);
        play_dim(p, px, py, pw, ph);
        dai_ui_panel_end(ui);
        dai_dock_panel_end(p->dock);
    }
    for (int inst = 0; inst < DAI_MAX_PANEL_INSTANCES &&
                       dai_dock_panel(p->dock, "Console", &px, &py, &pw, &ph); ++inst) {
        dai_ui_panel_begin(ui, px, py, pw, ph, nullptr);
        console_body(p, px, py, pw, ph);
        play_dim(p, px, py, pw, ph);
        dai_ui_panel_end(ui);
        dai_dock_panel_end(p->dock);
    }
    for (int inst = 0; inst < DAI_MAX_PANEL_INSTANCES &&
                       dai_dock_panel(p->dock, "Script", &px, &py, &pw, &ph); ++inst) {
        dai_ui_panel_begin(ui, px, py, pw, ph, nullptr);
        script_body(p, px, py, pw, ph);
        dai_ui_panel_end(ui);
        dai_dock_panel_end(p->dock);
    }
    for (int inst = 0; inst < DAI_MAX_PANEL_INSTANCES &&
                       dai_dock_panel(p->dock, "Audio", &px, &py, &pw, &ph); ++inst) {
        dai_ui_panel_begin(ui, px, py, pw, ph, nullptr);
        audio_body(p, px, py, pw, ph);
        play_dim(p, px, py, pw, ph);
        dai_ui_panel_end(ui);
        dai_dock_panel_end(p->dock);
    }
    for (int inst = 0; inst < DAI_MAX_PANEL_INSTANCES &&
                       dai_dock_panel(p->dock, "Inspector", &px, &py, &pw, &ph); ++inst) {
        dai_ui_panel_begin(ui, px, py, pw, ph, nullptr);
        char sid[24];
        std::snprintf(sid, sizeof(sid), "inspector%d", inst);
        dai_ui_scroll_begin(ui, sid, ph - 6.0f);
        inspector_body(p);
        dai_ui_scroll_end(ui);
        play_dim(p, px, py, pw, ph);
        dai_ui_panel_end(ui);
        dai_dock_panel_end(p->dock);
    }
    if (dai_dock_panel(p->dock, "Settings", &px, &py, &pw, &ph)) {
        dai_ui_panel_begin(ui, px, py, pw, ph, nullptr);
        settings_body(p);
        play_dim(p, px, py, pw, ph);
        dai_ui_panel_end(ui);
        dai_dock_panel_end(p->dock);
    } else if (p->settings_open && !dai_dock_visible(p->dock, "Settings") &&
               !dai_dock_is_open(p->dock, "Settings")) {
        p->settings_open = 0;
    }

    // The scene and the game are two tabs of one leaf, and their body is the
    // 3D view: no panel background is drawn, the host renders the world into
    // exactly this rectangle.
    // Scene and Game are independent views now: each panel keeps its own
    // rectangle, the host renders the editor camera into Scene and the game
    // camera into Game - side by side when both are docked open.
    int have_view = 0;
    p->has_game = 0;
    if (dai_dock_panel(p->dock, "Scene", &px, &py, &pw, &ph)) {
        p->view_x = px; p->view_y = py; p->view_w = pw; p->view_h = ph;
        have_view = 1;
        // In prefab mode the scene view belongs to ONE prefab, and it has to
        // say so - Unity puts a bar across the top with a way back, because
        // an editor that looks identical whether you are editing the world or
        // one object in isolation is an editor you save the wrong thing in.
        if (!p->prefab_mode.empty()) {
            const dai_ui_style *bs = dai_ui_style_of(ui);
            float bh = dai_ui_text_height(ui) + 12.0f;
            dai_ui_layer_push(ui, DAI_LAYER_WINDOW + 5);
            dai_ui_rect(ui, px, py, pw, bh, rgba(0x25, 0x3A, 0x52, 255));
            dai_ui_rect(ui, px, py + bh - 1.0f, pw, 1.0f, bs->accent);
            float bx2 = px + 8.0f;
            float bwid = dai_ui_text_width(ui, "< Scene") + 18.0f;
            if (browser_button(p, bx2, py + 3.0f, bwid, bh - 6.0f, "< Scene"))
                p->prefab_exit_want = 1;
            bx2 += bwid + 10.0f;
            if (dai_ui_has_icon(ui, DAI_ICON_C_PREFAB)) {
                dai_ui_icon_at(ui, DAI_ICON_C_PREFAB, bx2, py + (bh - 14.0f) * 0.5f, 14.0f,
                               bs->accent);
                bx2 += 19.0f;
            }
            char pb[200];
            std::snprintf(pb, sizeof(pb), "Prefab  %s", base_of(p->prefab_mode).c_str());
            dai_ui_text(ui, bx2, py + (bh - dai_ui_text_height(ui)) * 0.5f, pb, bs->text);
            const char *hint = "editing the prefab - changes reach every instance";
            float hw = dai_ui_text_width(ui, hint);
            if (px + pw - hw - 10.0f > bx2 + 200.0f)
                dai_ui_text(ui, px + pw - hw - 10.0f,
                            py + (bh - dai_ui_text_height(ui)) * 0.5f, hint, bs->text_dim);
            dai_ui_layer_pop(ui);
        }
        dai_dock_panel_end(p->dock);
    }
    if (p->gizmo_fps) {
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
        // The preview's chrome. Drawn AFTER the scene panel has reported its
        // rectangle and before the host renders into it - the world lands on
        // top of this plate and inside this border.
        float cx2, cy2, cw2, ch2;
        if (dai_editor_ui_camera_preview(p, &cx2, &cy2, &cw2, &ch2,
                                         nullptr, nullptr, nullptr, nullptr)) {
            const dai_ui_style *cs2 = dai_ui_style_of(ui);
            dai_ui_layer_push(ui, DAI_LAYER_WINDOW + 4);
            // A BORDER, drawn as four thin bars - not a filled rectangle with
            // a smaller one inside it. The interface is composited over the
            // world, so anything filled here is simply opaque.
            const float B2 = 1.0f;
            dai_ui_rect(ui, cx2 - B2, cy2 - B2, cw2 + B2 * 2, B2, cs2->panel_border);
            dai_ui_rect(ui, cx2 - B2, cy2 + ch2, cw2 + B2 * 2, B2, cs2->panel_border);
            dai_ui_rect(ui, cx2 - B2, cy2, B2, ch2, cs2->panel_border);
            dai_ui_rect(ui, cx2 + cw2, cy2, B2, ch2, cs2->panel_border);
            float lh2 = dai_ui_text_height(ui) + 6.0f;
            dai_ui_rect(ui, cx2, cy2 - lh2, cw2, lh2, (cs2->chrome & 0x00FFFFFFu) | 0xE6000000u);
            dai_ui_text(ui, cx2 + 6.0f, cy2 - lh2 + 3.0f, "Camera Preview", cs2->text_dim);
            dai_ui_layer_pop(ui);
        }
    }
    if (dai_dock_panel(p->dock, "Game", &px, &py, &pw, &ph)) {
        p->game_x = px; p->game_y = py; p->game_w = pw; p->game_h = ph;
        p->has_game = 1;
        if (!have_view) {   // Game alone: it is THE view, exactly as before
            p->view_x = px; p->view_y = py; p->view_w = pw; p->view_h = ph;
            have_view = 1;
        }
        dai_dock_panel_end(p->dock);
    }
    // Tabs in one leaf still switch: the selected tab is the visible one.
    p->view = dai_dock_visible(p->dock, "Scene") ? DAI_VIEW_SCENE : DAI_VIEW_GAME;
    if (!have_view) { p->view_w = 0; p->view_h = 0; }

    if (p->has_game && !dai_editor_ui_game_camera(p, nullptr, nullptr, nullptr)) {
        const char *msg = "No camera in the scene - right click the hierarchy, New Camera";
        float tw = dai_ui_text_width(ui, msg);
        dai_ui_text(ui, p->game_x + (p->game_w - tw) * 0.5f,
                    p->game_y + p->game_h * 0.5f, msg, st->text_dim);
    }

    dai_dock_end(p->dock);

    // A rename row whose file vanished (deleted, refreshed away, folder left)
    // would keep an invisible text field focused forever - and a focused field
    // eats Delete, W/E/R and Space, so the whole editor goes deaf. If the row
    // was not drawn this frame, the rename is over.
    if (!p->rename_asset.empty() && !p->rename_drawn) {
        p->rename_asset.clear();
        p->rename_seen_active = 0;
    }
    p->rename_drawn = 0;
    if (p->toast_left > 0.0f) p->toast_left -= 1.0f / 60.0f;

    dai_editor_ui_timeline(p, p->view_x + 8.0f, vh - BOTTOM - 50.0f, p->view_w - 16.0f);
    dai_editor_ui_status(p, 0.0f, vh - BOTTOM, vw, BOTTOM);
    if (p->view == DAI_VIEW_SCENE && have_view) {
        // Scene view toolbar, top right, the way Unity does it: a Gizmos
        // dropdown (what overlays the view) and a camera dropdown (how the
        // editor camera moves). It floats over the 3D, so it must not be
        // clipped by the viewport rect.
        {
            const float TBH = 22.0f, TBW = 30.0f, GAP = 4.0f;
            float bx = p->view_x + p->view_w - TBW - 6.0f;
            float by = p->view_y + 6.0f;
            dai_ui_layer_push(ui, 1 << 18);
            dai_ui_root_begin(ui, "scenetb", bx - TBW - GAP, by, TBW * 2 + GAP, TBH);
            // camera settings (speed) - the rightmost button
            if (dai_ui_icon_button_at(ui, DAI_ICON_CAMERA, bx, by, TBW, TBH, p->menu_scecam.open))
                dai_ui_popup_open(&p->menu_scecam, bx - 150.0f, by + TBH + 2.0f);
            bx -= TBW + GAP;
            if (dai_ui_icon_button_at(ui, DAI_ICON_EYE, bx, by, TBW, TBH, p->menu_gizmos.open))
                dai_ui_popup_open(&p->menu_gizmos, bx - 170.0f, by + TBH + 2.0f);
            dai_ui_root_end(ui);
            if (p->menu_gizmos.open) {
                dai_ui_popup_panel_begin(ui, &p->menu_gizmos, 180.0f, 0.0f);
                int g = p->gizmo_grid;      if (dai_ui_checkbox(ui, "Floor grid", &g))      p->gizmo_grid = g;
                int q = p->gizmo_fps;       if (dai_ui_checkbox(ui, "FPS", &q))             p->gizmo_fps = q;
                int c = p->gizmo_colliders; if (dai_ui_checkbox(ui, "Collider frames", &c)) p->gizmo_colliders = c;
                int f = p->gizmo_cameras;   if (dai_ui_checkbox(ui, "Camera frustums", &f)) p->gizmo_cameras = f;
                float gpx = p->settings_gizmo_px;
                if (dai_ui_num_field(ui, "Gizmo px", &gpx, 1.0f, 30.0f, 300.0f, "tbgizmo")) {
                    p->settings_gizmo_px = gpx;
                    dai_editor_gizmo_size(p->ed, gpx);
                }
                dai_ui_popup_panel_end(ui);
            }
            if (p->menu_scecam.open) {
                dai_ui_popup_panel_begin(ui, &p->menu_scecam, 190.0f, 0.0f);
                float speed = dai_editor_cam_speed_get(p->ed);
                if (dai_ui_num_field(ui, "Cam speed", &speed, 0.05f, 0.1f, 200.0f, "tbcamspd"))
                    dai_editor_cam_speed(p->ed, speed);
                float snap = p->settings_snap;
                if (dai_ui_num_field(ui, "Snap step", &snap, 0.01f, 0.0f, 100.0f, "tbsnap")) {
                    p->settings_snap = snap;
                    dai_editor_snap(p->ed, snap, 15.0f, 0.1f);
                }
                dai_ui_popup_panel_end(ui);
            }
            dai_ui_layer_pop(ui);
        }
        // The wireframes are UI lines over the 3D - clipped to the panel the
        // 3D lives in, or an object behind the inspector draws its gizmo on
        // the inspector.
        dai_ui_clip_begin(ui, p->view_x, p->view_y, p->view_w, p->view_h);
        // grid: world lines via dai_editor_ui_grid_lines, drawn by the host
        (void)0;
        if (p->gizmo_colliders) dai_editor_ui_colliders(p);
        if (p->gizmo_cameras) draw_cameras(p);
        dai_editor_ui_gizmo(p);
        dai_ui_clip_end(ui);
    }

    // Script drag & drop, frame level: the pill follows the cursor over every
    // panel, and the drop lands on the hierarchy row under it (that node) or
    // anywhere on the inspector (the selection) - the Unity gesture.
    {
        float dmx = 0, dmy = 0; int ddown = 0;
        dai_ui_mouse(ui, &dmx, &dmy, &ddown, nullptr);

        // ---- a press became a click: nothing was dragged, so it counts -----
        // This runs BEFORE the drag state below is cleared, because "was this
        // a drag" is exactly the question being asked.
        if (!ddown && p->click_kind) {
            bool dragged = !p->drag_script.empty() || p->drag_node != DAI_INVALID_NODE;
            if (!dragged) {
                if (p->click_kind == 3 && p->click_node != DAI_INVALID_NODE) {
                    if (p->click_shift && p->range_anchor_node != DAI_INVALID_NODE) {
                        // Shift takes everything BETWEEN, in the order the
                        // rows are on screen - which is the order the tree was
                        // walked, not the order the ids were made in. Windows,
                        // Unity and every file manager work this way, and a
                        // hierarchy that only does ctrl-click makes you click
                        // forty times to select forty rows.
                        select_range_nodes(p, p->range_anchor_node, p->click_node,
                                           p->click_ctrl);
                    } else {
                        dai_editor_select(p->ed, p->click_node, p->click_ctrl);
                        p->range_anchor_node = p->click_node;
                    }
                    // ONE selection, not two. The Project window keeps its own
                    // highlight otherwise, and then F2 renames a file while
                    // you are looking at a selected object - which is exactly
                    // what it used to do.
                    p->inspect_asset.clear();
                    p->asset_sel = -1;
                    p->asset_multi.clear();
                    p->proj_sel_folder.clear();
                    p->last_pick.clear();
                } else if (p->click_kind == 1) {
                    p->last_pick = p->click_path;  // F2 renames THIS folder
                    p->proj_sel_folder = p->click_path;
                    p->asset_sel = -1;             // a folder and a file cannot both be it
                    p->asset_multi.clear();
                    p->range_anchor_asset.clear();
                    p->inspect_asset = p->click_path;
                    dai_editor_deselect_all(p->ed);
                    if (p->click_dbl) {
                        p->proj_dir = p->click_path;
                        project_expand_to(p, p->proj_dir);
                        p->proj_list_scroll = 0.0f;
                        p->proj_sel_folder.clear();
                    }
                } else if (p->click_kind == 2) {
                    // The row index is looked up again: the listing is rebuilt
                    // on every disk change, and one can happen between the
                    // press and the release.
                    int fi2 = -1;
                    for (size_t ai = 0; ai < p->assets.size(); ++ai)
                        if (p->assets[ai] && p->click_path == p->assets[ai]) { fi2 = (int)ai; break; }
                    const std::string &full2 = p->click_path;
                    if (p->click_dbl) {
                        if (is_behaviour_file(full2) || is_text_file(full2)) {
                            if (!p->script_external && p->file_read)
                                dai_editor_ui_script_open(p, full2.c_str());
                            else if (p->open_asset)
                                p->open_asset(nullptr, full2.c_str(), p->open_asset_user);
                        } else if (is_scene_file(full2) && !is_scene_asset(full2)) {
                            p->prefab_open_want = full2;
                        } else if (fi2 >= 0) {
                            p->pending_asset = p->assets[(size_t)fi2];
                            p->pending_as_tree = 0;
                            p->pending_at_valid = 0;   // no drop point: it was a click
                        }
                    }
                    // Ctrl adds one, shift takes the run between the anchor
                    // and here, a plain click replaces - the file manager
                    // rules, because this IS a file manager.
                    if (p->click_shift && !p->range_anchor_asset.empty()) {
                        std::vector<std::string> rows = project_visible_files(p);
                        int ia = -1, ib = -1;
                        for (size_t i = 0; i < rows.size(); ++i) {
                            if (rows[i] == p->range_anchor_asset) ia = (int)i;
                            if (rows[i] == full2) ib = (int)i;
                        }
                        if (ia >= 0 && ib >= 0) {
                            if (ia > ib) { int t = ia; ia = ib; ib = t; }
                            if (!p->click_ctrl) p->asset_multi.clear();
                            for (int i = ia; i <= ib; ++i) {
                                bool have = false;
                                for (const std::string &e : p->asset_multi)
                                    if (e == rows[(size_t)i]) { have = true; break; }
                                if (!have) p->asset_multi.push_back(rows[(size_t)i]);
                            }
                        }
                    } else if (p->click_ctrl) {
                        bool had = false;
                        for (size_t i = 0; i < p->asset_multi.size(); ++i)
                            if (p->asset_multi[i] == full2) {
                                p->asset_multi.erase(p->asset_multi.begin() + (long)i);
                                had = true; break;
                            }
                        if (!had) p->asset_multi.push_back(full2);
                        p->range_anchor_asset = full2;
                    } else {
                        p->asset_multi.assign(1, full2);
                        p->range_anchor_asset = full2;
                    }
                    p->asset_sel = fi2;
                    p->last_pick = full2;
                    p->proj_sel_folder.clear();
                    p->inspect_asset = full2;      // the inspector follows
                    dai_editor_deselect_all(p->ed);
                }
            }
            p->click_kind = 0;
            p->click_node = DAI_INVALID_NODE;
            p->click_dbl = 0;
        }
        // An object field was clicked: it points AT something, so show it.
        if (p->ping_node != DAI_INVALID_NODE) {
            if (dai_doc_valid(dai_editor_doc(p->ed), p->ping_node)) {
                dai_editor_select(p->ed, p->ping_node, 0);
                p->inspect_asset.clear();
                p->reveal_row_wanted = 1;
            }
            p->ping_node = DAI_INVALID_NODE;
        }

        if (!p->drag_pending.empty() && p->drag_script.empty() && ddown) {
            float ddx = dmx - p->drag_px, ddy = dmy - p->drag_py;
            if (ddx * ddx + ddy * ddy > 36.0f) p->drag_script = p->drag_pending;
        }
        if (!ddown && !p->drag_pending.empty()) {
            if (!p->drag_script.empty()) {
                // A folder under the pointer wins: that gesture is a MOVE and
                // it is the only meaning it can have. Only then does the old
                // "drop a script on a node" reading get a look in - and only
                // for a file that actually is a script, or dropping a .png on
                // a crate would add a behaviour called crate.png.
                if (p->proj_drop_ok) {
                    project_move(p, p->drag_script, p->proj_drop_dir);
                } else if (is_scene_file(p->drag_script) && !is_scene_asset(p->drag_script) &&
                           (dai_ui_root_hovered(ui, "Scene") ||
                            dai_ui_root_hovered(ui, "Hierarchy"))) {
                    // A prefab dragged into the viewport (or onto the
                    // hierarchy) is placed. The host does the instantiating -
                    // it owns the assets root - so this hands over the same
                    // pending pick a double click produces, which already
                    // links the instance to the file it came from.
                    for (const char *a : p->assets) {
                        if (a && p->drag_script == a) { p->pending_asset = a; break; }
                    }
                    p->pending_as_tree = 0;
                    // ...and WHERE. A prefab dropped into the viewport belongs
                    // under the pointer; dropping it at the origin is how you
                    // end up with nine crates inside each other.
                    if (dai_ui_root_hovered(ui, "Scene")) {
                        dai_vec3 g{};
                        if (viewport_ground_point(p, dmx, dmy, &g)) {
                            p->pending_at = g;
                            p->pending_at_valid = 1;
                        }
                    }
                } else if (is_material_file(p->drag_script) &&
                           (p->hover_node != DAI_INVALID_NODE ||
                            dai_ui_root_hovered(ui, "Inspector") ||
                            dai_ui_root_hovered(ui, "Scene"))) {
                    // Dropped on an object: that object wears it. This is the
                    // gesture people try first, and it is the only one that
                    // does not require finding the slot in the inspector.
                    // In the viewport it is whatever is UNDER the pointer -
                    // the same pick a click makes. Falling back to "whatever
                    // was selected" there would paint an object you cannot
                    // see because the dragged pill is over it.
                    dai_node target = DAI_INVALID_NODE;
                    if (p->hover_node != DAI_INVALID_NODE) target = p->hover_node;
                    else if (dai_ui_root_hovered(ui, "Scene"))
                        target = dai_editor_pick(p->ed, dmx, dmy);
                    else if (dai_editor_selection_count(p->ed) > 0)
                        target = dai_editor_selected(p->ed, 0);
                    dai_doc *md2 = dai_editor_doc(p->ed);
                    dai_node_desc mr2{};
                    if (target != DAI_INVALID_NODE &&
                        dai_doc_get(md2, target, &mr2) == DAI_OK) {
                        std::vector<std::string> mats = script_list(mr2.materials);
                        if (mats.empty()) mats.push_back("Default");
                        mats[0] = p->drag_script;
                        dai_doc_begin(md2, "Material");
                        script_join(mr2.materials, sizeof(mr2.materials), mats);
                        dai_doc_set(md2, target, &mr2);
                        dai_doc_commit(md2);
                        dai_editor_resync(p->ed);
                        p->want_material_apply = 1;
                        char mm[160];
                        std::snprintf(mm, sizeof(mm), "%s applied",
                                      base_of(p->drag_script).c_str());
                        dai_editor_ui_toast(p, mm, 1.5f);
                    }
                } else if (is_behaviour_file(p->drag_script)) {
                    dai_node target = DAI_INVALID_NODE;
                    if (p->hover_node != DAI_INVALID_NODE) target = p->hover_node;
                    else if (dai_ui_root_hovered(ui, "Inspector") &&
                             dai_editor_selection_count(p->ed) > 0)
                        target = dai_editor_selected(p->ed, 0);
                    if (target != DAI_INVALID_NODE) script_attach(p, target, p->drag_script);
                }
            }
            p->drag_pending.clear();
            p->drag_script.clear();
        }
        if (!p->drag_script.empty()) {
            std::string lbl = base_of(p->drag_script);
            if (p->proj_drop_ok)
                lbl += "  ->  " + (p->proj_drop_dir.empty() ? std::string("Assets")
                                                            : p->proj_drop_dir);
            else if (is_behaviour_file(p->drag_script) && p->hover_node != DAI_INVALID_NODE)
                lbl += "  ->  attach";
            else if (is_scene_file(p->drag_script) &&
                     (dai_ui_root_hovered(ui, "Scene") || dai_ui_root_hovered(ui, "Hierarchy")))
                lbl += "  ->  place in scene";
            else if (is_material_file(p->drag_script) &&
                     (p->hover_node != DAI_INVALID_NODE ||
                      dai_ui_root_hovered(ui, "Inspector") ||
                      dai_ui_root_hovered(ui, "Scene")))
                lbl += "  ->  apply material";
            // A ghost where it would land: a footprint on the ground plus a
            // box standing on it, drawn in the accent colour. It is not the
            // mesh - the editor has no renderer of its own and will not gain
            // one for a drag - but it is the POSITION, the SIZE and the
            // ORIENTATION of what is about to appear, which is what the
            // question "where will this go" is actually asking.
            if (is_scene_file(p->drag_script) && !is_scene_asset(p->drag_script) &&
                dai_ui_root_hovered(ui, "Scene")) {
                dai_vec3 g{};
                if (viewport_ground_point(p, dmx, dmy, &g)) {
                    const float R = 0.5f;      // half a metre: a default cube
                    dai_vec3 c[8] = {
                        { g.x - R, g.y,       g.z - R }, { g.x + R, g.y,       g.z - R },
                        { g.x + R, g.y,       g.z + R }, { g.x - R, g.y,       g.z + R },
                        { g.x - R, g.y + 2*R, g.z - R }, { g.x + R, g.y + 2*R, g.z - R },
                        { g.x + R, g.y + 2*R, g.z + R }, { g.x - R, g.y + 2*R, g.z + R },
                    };
                    static const int E[12][2] = {
                        {0,1},{1,2},{2,3},{3,0}, {4,5},{5,6},{6,7},{7,4}, {0,4},{1,5},{2,6},{3,7}
                    };
                    uint32_t ghost = (st->accent & 0x00FFFFFFu) | 0xCC000000u;
                    dai_ui_layer_push(ui, DAI_LAYER_WINDOW + 99);
                    for (const auto &e : E) {
                        float ax, ay, bx, by;
                        if (dai_editor_project(p->ed, c[e[0]], &ax, &ay) &&
                            dai_editor_project(p->ed, c[e[1]], &bx, &by))
                            dai_ui_line(ui, ax, ay, bx, by, 1.5f, ghost);
                    }
                    // The footprint, so the height reads against the floor.
                    for (int k = 0; k < 4; ++k) {
                        float ax, ay, bx, by;
                        if (dai_editor_project(p->ed, c[k], &ax, &ay) &&
                            dai_editor_project(p->ed, c[(k + 1) % 4], &bx, &by))
                            dai_ui_line(ui, ax, ay, bx, by, 2.5f, ghost);
                    }
                    dai_ui_layer_pop(ui);
                }
            }
            float tw = dai_ui_text_width(ui, lbl.c_str()) + 18.0f;
            float th = dai_ui_text_height(ui) + 8.0f;
            // Drawn on the window layer so the pill survives leaving the
            // editor window - a drag you cannot see outside is a drag lost.
            dai_ui_layer_push(ui, DAI_LAYER_WINDOW + 100);
            dai_ui_rrect(ui, dmx + 12.0f, dmy + 10.0f, tw, th, 4.0f, st->accent);
            dai_ui_rect_outline(ui, dmx + 12.0f, dmy + 10.0f, tw, th, 1.0f, 0xFFFFFFFFu);
            dai_ui_text(ui, dmx + 21.0f, dmy + 14.0f, lbl.c_str(), st->text);
            dai_ui_layer_pop(ui);
            dai_ui_claim_mouse(ui);
        }
        // Set while the folders were drawn, spent above, gone by the next
        // frame - so a stale target cannot survive the Project window being
        // closed, or hidden behind another tab.
        p->proj_drop_ok = 0;
        p->proj_drop_dir.clear();

        // A hierarchy node in flight: arm, drag, drop on an assign field.
        if (p->drag_node_pending != DAI_INVALID_NODE && p->drag_node == DAI_INVALID_NODE && ddown) {
            float ddx = dmx - p->drag_nx, ddy = dmy - p->drag_ny;
            if (ddx * ddx + ddy * ddy > 36.0f) p->drag_node = p->drag_node_pending;
        }
        if (!ddown && p->drag_node_pending != DAI_INVALID_NODE) {
            // Dropped on the PROJECT window: the object becomes a prefab
            // file, exactly the Unity gesture. Asked by position, not by a
            // hover flag - the flag is rebuilt per panel per frame, and the
            // drop can land between rebuilds.
            float dmx2 = 0, dmy2 = 0;
            dai_ui_mouse(ui, &dmx2, &dmy2, nullptr, nullptr);
            bool over_project = false;
            {
                float qx, qy, qw, qh;
                for (int inst = 0; inst < 8 &&
                     dai_dock_panel_rect(p->dock, "Project", inst, &qx, &qy, &qw, &qh); ++inst) {
                    if (dmx2 >= qx && dmx2 < qx + qw && dmy2 >= qy && dmy2 < qy + qh)
                        { over_project = true; break; }
                }
            }
            if (p->drag_node != DAI_INVALID_NODE && over_project) {
                dai_doc *doc3 = dai_editor_doc(p->ed);
                dai_node_desc pr3{};
                if (p->prefab_save && dai_doc_get(doc3, p->drag_node, &pr3) == DAI_OK) {
                // The folder the pointer was actually over wins; otherwise
                // the folder the browser is showing - and "" IS a folder, it
                // is Assets. There is no third place to guess at: an object
                // dropped in Assets belongs in Assets.
                std::string dir = p->proj_drop_ok ? p->proj_drop_dir : p->proj_dir;
                char rel[224];
                if (dir.empty())
                    std::snprintf(rel, sizeof(rel), "%s.daidalos",
                                  pr3.name[0] ? pr3.name : "Prefab");
                else
                    std::snprintf(rel, sizeof(rel), "%s/%s.daidalos", dir.c_str(),
                                  pr3.name[0] ? pr3.name : "Prefab");
                char nm[32];
                std::snprintf(nm, sizeof(nm), "%u", (unsigned)p->drag_node);
                if (p->prefab_save(nm, rel, p->rename_user)) {
                    dai_editor_ui_toast(p, "prefab created", 2.0f);
                    p->want_refresh = 1;
                }
                }
            }
            // A node dropped on a hierarchy ROW re-parents; dropped on a
            // script's reference field it becomes that reference. The row wins
            // when both could apply - that is where the pointer actually is.
            else if (p->drag_node != DAI_INVALID_NODE && p->hover_node != DAI_INVALID_NODE &&
                p->hover_node != p->drag_node) {
                reparent_dragged(p, p->hover_node == DAI_SCENE_ROOT_NODE
                                        ? DAI_INVALID_NODE : p->hover_node);
            } else if (p->drag_node != DAI_INVALID_NODE &&
                       p->hover_node == DAI_INVALID_NODE &&
                       dai_ui_root_hovered(ui, "Hierarchy")) {
                // Let go over the empty part of the hierarchy: the object
                // leaves its parent and becomes a root. Without this there is
                // no gesture for "out" at all - only "into something else" -
                // and a child that was dragged into a group by mistake can
                // never be dragged back out of it.
                reparent_dragged(p, DAI_INVALID_NODE);
            } else if (p->drag_node != DAI_INVALID_NODE && p->param_hover_entry >= 0 &&
                p->drag_ref_target != DAI_INVALID_NODE) {
                dai_doc *d = dai_editor_doc(p->ed);
                dai_node_desc r{}, src{};
                if (dai_doc_get(d, p->drag_ref_target, &r) == DAI_OK &&
                    dai_doc_get(d, p->drag_node, &src) == DAI_OK && src.name[0]) {
                    std::vector<std::string> list = script_list(r.script);
                    if (p->param_hover_entry < (int)list.size()) {
                        entry_set_param(list[(size_t)p->param_hover_entry],
                                        p->param_hover_key, src.name);
                        dai_doc_begin(d, "Assign reference");
                        script_join(r.script, sizeof(r.script), list);
                        dai_doc_set(d, p->drag_ref_target, &r);
                        dai_doc_commit(d);
                    }
                }
            }
            p->drag_node_pending = DAI_INVALID_NODE;
            p->drag_node = DAI_INVALID_NODE;
        }
        if (p->drag_node != DAI_INVALID_NODE) {
            dai_node_desc dr{};
            const char *nm = "node";
            if (dai_doc_get(dai_editor_doc(p->ed), p->drag_node, &dr) == DAI_OK && dr.name[0])
                nm = dr.name;
            // What letting go would DO, not just what is being held. Three
            // gestures share this drag - re-parent, unparent, save a prefab -
            // and they are told apart by where the pointer is, which is
            // exactly the thing the pointer is covering up.
            char note[224];
            dai_node_desc tr2{};
            if (p->hover_node == DAI_SCENE_ROOT_NODE)
                std::snprintf(note, sizeof(note), "%s  ->  scene root", nm);
            else if (p->hover_node != DAI_INVALID_NODE && p->hover_node != p->drag_node &&
                     dai_doc_get(dai_editor_doc(p->ed), p->hover_node, &tr2) == DAI_OK &&
                     tr2.name[0])
                std::snprintf(note, sizeof(note), "%s  ->  child of %s", nm, tr2.name);
            else if (dai_ui_root_hovered(ui, "Project"))
                std::snprintf(note, sizeof(note), "%s  ->  prefab", nm);
            else
                std::snprintf(note, sizeof(note), "%s", nm);
            float tw = dai_ui_text_width(ui, note) + 18.0f;
            float th = dai_ui_text_height(ui) + 8.0f;
            // On the window layer, like the script drag: a pill clipped to
            // the panel it started in disappears the moment the drag leaves
            // the hierarchy, which is every drag that matters.
            dai_ui_layer_push(ui, DAI_LAYER_WINDOW + 100);
            dai_ui_rrect(ui, dmx + 12.0f, dmy + 10.0f, tw, th, 4.0f, st->accent);
            dai_ui_rect_outline(ui, dmx + 12.0f, dmy + 10.0f, tw, th, 1.0f, 0xFFFFFFFFu);
            dai_ui_text(ui, dmx + 21.0f, dmy + 14.0f, note, st->text);
            dai_ui_layer_pop(ui);
            dai_ui_claim_mouse(ui);
        }
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
    // A save that says nothing might as well not have happened - Ctrl+S in an
    // editor has to leave a mark somewhere.
    if (p->toast_left > 0.0f && p->toast[0]) {
        float tw2 = dai_ui_text_width(ui, p->toast);
        dai_ui_text(ui, x + (w - tw2) * 0.5f, y + 3.0f, p->toast, st->accent);
    }
    char right[96];
    {
        float sc = dai_ui_scale_get(p->ui);
        if (sc > 1.01f || sc < 0.99f)
            std::snprintf(right, sizeof(right), "viewport %.0fx%.0f  (%.0fx%.0f px, UI %.0f%%)",
                          p->view_w, p->view_h, p->view_w * sc, p->view_h * sc, sc * 100.0f);
        else
            std::snprintf(right, sizeof(right), "viewport %.0fx%.0f", p->view_w, p->view_h);
    }
    float rw = dai_ui_text_width(ui, right);
    dai_ui_text(ui, x + w - rw - 8.0f, y + 3.0f, right, st->text_dim);
}

void dai_editor_ui_prefab_mode(dai_editor_ui *p, const char *prefab_rel) {
    if (!p) return;
    p->prefab_mode = prefab_rel ? prefab_rel : "";
}
const char *dai_editor_ui_prefab_mode_get(const dai_editor_ui *p) {
    return p && !p->prefab_mode.empty() ? p->prefab_mode.c_str() : nullptr;
}
int dai_editor_ui_take_prefab_exit(dai_editor_ui *p) {
    if (!p || !p->prefab_exit_want) return 0;
    p->prefab_exit_want = 0;
    return 1;
}
int dai_editor_ui_take_prefab_open(dai_editor_ui *p, const char **out_rel) {
    if (!p || p->prefab_open_want.empty()) return 0;
    static std::string held;      // stays alive until the next call, like the
    held = p->prefab_open_want;   // asset pick above
    p->prefab_open_want.clear();
    if (out_rel) *out_rel = held.c_str();
    return 1;
}

int dai_editor_ui_take_asset_at(const dai_editor_ui *p, float *x, float *y, float *z) {
    if (!p || !p->pending_at_valid) return 0;
    if (x) *x = p->pending_at.x;
    if (y) *y = p->pending_at.y;
    if (z) *z = p->pending_at.z;
    return 1;
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
