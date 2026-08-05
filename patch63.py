#!/usr/bin/env python3
# patch63 - a prefab you can see before you let go of it, landing where the
# pointer is.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p63'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# ================================================================== header
s = rd('include/dai_editor_ui.h')
s = sub1(s,
"""DAI_API int dai_editor_ui_take_asset(dai_editor_ui *p, const char **out_path, int *out_as_tree);""",
"""DAI_API int dai_editor_ui_take_asset(dai_editor_ui *p, const char **out_path, int *out_as_tree);

/* Where the last taken asset was dropped, in world space, and whether there
 * IS such a place (0 when it was picked by double click rather than dragged
 * into the viewport - then the host decides, as it always did).
 *
 * Read straight after dai_editor_ui_take_asset. */
DAI_API int dai_editor_ui_take_asset_at(const dai_editor_ui *p, float *x, float *y, float *z);""",
    'take_asset_at decl')
wr('include/dai_editor_ui.h', s)

s = rd('src/dai_editor_ui.cpp')

# ---------------------------------------------------------------- the state
s = sub1(s,
"""    const char *pending_asset = nullptr;   // clicked in the Project window""",
"""    const char *pending_asset = nullptr;   // clicked in the Project window
    // Where a dragged asset was let go, in the world. A double click has no
    // such place, which is what the flag is for.
    int      pending_at_valid = 0;
    dai_vec3 pending_at{ 0, 0, 0 };""",
    'pending_at state')

# ----------------------------------------------- the point under the pointer
s = sub1(s,
"""// A scene file - which is also what a prefab is.""",
"""// Where the pointer is pointing, on the ground. The scene view's ray against
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

// A scene file - which is also what a prefab is.""",
    'ground point helper')

# ------------------------------------------------------------- the preview
s = sub1(s,
"""            else if (is_scene_file(p->drag_script) &&
                     (dai_ui_root_hovered(ui, "Scene") || dai_ui_root_hovered(ui, "Hierarchy")))
                lbl += "  ->  place in scene";""",
"""            else if (is_scene_file(p->drag_script) &&
                     (dai_ui_root_hovered(ui, "Scene") || dai_ui_root_hovered(ui, "Hierarchy")))
                lbl += "  ->  place in scene";
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
            }""",
    'prefab ghost preview')

# ------------------------------------------------ remember where it landed
s = sub1(s,
"""                    for (const char *a : p->assets) {
                        if (a && p->drag_script == a) { p->pending_asset = a; break; }
                    }
                    p->pending_as_tree = 0;""",
"""                    for (const char *a : p->assets) {
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
                    }""",
    'remember drop point')

s = sub1(s,
"""int dai_editor_ui_take_asset(dai_editor_ui *p, const char **out_path, int *out_as_tree) {
    if (!p || !p->pending_asset) return 0;""",
"""int dai_editor_ui_take_asset_at(const dai_editor_ui *p, float *x, float *y, float *z) {
    if (!p || !p->pending_at_valid) return 0;
    if (x) *x = p->pending_at.x;
    if (y) *y = p->pending_at.y;
    if (z) *z = p->pending_at.z;
    return 1;
}

int dai_editor_ui_take_asset(dai_editor_ui *p, const char **out_path, int *out_as_tree) {
    if (!p || !p->pending_asset) return 0;""",
    'take_asset_at impl')

# A double click has no drop point; make sure the flag does not survive one.
s = sub1(s,
"""                                p->pending_asset = fi >= 0 && fi < (int)p->assets.size()
                                                 ? p->assets[(size_t)fi] : nullptr;
                                p->pending_as_tree = 0;""",
"""                                p->pending_asset = fi >= 0 && fi < (int)p->assets.size()
                                                 ? p->assets[(size_t)fi] : nullptr;
                                p->pending_as_tree = 0;
                                p->pending_at_valid = 0;   // no drop point: it was a click""",
    'double click clears drop point')
wr('src/dai_editor_ui.cpp', s)

# ============================================== the host places it there
s = rd('examples/editor_demo.cpp')
s = sub1(s,
"""                    dai_node made = dai_doc_prefab_instantiate(doc, pick, DAI_INVALID_NODE,
                                                               g_assets_dir, perr, sizeof(perr));
                    if (made) {
                        dai_doc_sync_apply(sync);
                        dai_editor_select(ed, made, 0);
                        dai_editor_ui_toast(panels, "prefab placed", 1.5f);""",
"""                    dai_node made = dai_doc_prefab_instantiate(doc, pick, DAI_INVALID_NODE,
                                                               g_assets_dir, perr, sizeof(perr));
                    if (made) {
                        // Dropped into the viewport? Then it goes where it was
                        // dropped. The prefab file stores its root at its own
                        // origin, so this is the whole placement.
                        float dx = 0, dy = 0, dz = 0;
                        if (dai_editor_ui_take_asset_at(panels, &dx, &dy, &dz)) {
                            dai_node_desc pr4{};
                            if (dai_doc_get(doc, made, &pr4) == DAI_OK) {
                                dai_doc_begin(doc, "Place prefab");
                                pr4.position = dai_vec3{ dx, dy + pr4.half_extent.y, dz };
                                dai_doc_set(doc, made, &pr4);
                                dai_doc_commit(doc);
                            }
                        }
                        dai_doc_sync_apply(sync);
                        dai_editor_select(ed, made, 0);
                        dai_editor_ui_toast(panels, "prefab placed", 1.5f);""",
    'place at drop point')
wr('examples/editor_demo.cpp', s)
print('patch63 ok')
