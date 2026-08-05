#!/usr/bin/env python3
# patch64 - Prefab Mode: double clicking a prefab opens the prefab, not a copy
# of it, and a bar across the top says so and takes you back.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p64'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# =================================================================== header
s = rd('include/dai_editor_ui.h')
s = sub1(s,
"""DAI_API int dai_editor_ui_take_asset_at(const dai_editor_ui *p, float *x, float *y, float *z);""",
"""DAI_API int dai_editor_ui_take_asset_at(const dai_editor_ui *p, float *x, float *y, float *z);

/* ---- prefab mode --------------------------------------------------------
 *
 * Unity's answer to "I want to change the prefab, not this copy of it": the
 * prefab file is opened AS the scene, on its own, and a bar across the top
 * says which one and how to get back. Editing an instance and hoping the
 * change reaches the original is the thing that has no answer.
 *
 * The host owns the loading (it owns the disk and the document); the editor
 * owns the bar and the button. Pass NULL to leave the mode. */
DAI_API void dai_editor_ui_prefab_mode(dai_editor_ui *p, const char *prefab_rel);
DAI_API const char *dai_editor_ui_prefab_mode_get(const dai_editor_ui *p);
/* 1 once, on the frame the user asked to go back. */
DAI_API int  dai_editor_ui_take_prefab_exit(dai_editor_ui *p);
/* Set when a prefab should be OPENED rather than placed - a double click in
 * the Project window. Reads and clears, like take_asset. */
DAI_API int  dai_editor_ui_take_prefab_open(dai_editor_ui *p, const char **out_rel);""",
    'prefab mode decl')
wr('include/dai_editor_ui.h', s)

s = rd('src/dai_editor_ui.cpp')
s = sub1(s,
"""    int      pending_at_valid = 0;
    dai_vec3 pending_at{ 0, 0, 0 };""",
"""    int      pending_at_valid = 0;
    dai_vec3 pending_at{ 0, 0, 0 };
    // Prefab mode: the name shown in the bar, and the two one-shot flags the
    // host reads.
    std::string prefab_mode;
    std::string prefab_open_want;
    int         prefab_exit_want = 0;""",
    'prefab mode state')

s = sub1(s,
"""int dai_editor_ui_take_asset_at(const dai_editor_ui *p, float *x, float *y, float *z) {""",
"""void dai_editor_ui_prefab_mode(dai_editor_ui *p, const char *prefab_rel) {
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

int dai_editor_ui_take_asset_at(const dai_editor_ui *p, float *x, float *y, float *z) {""",
    'prefab mode impl')

# A double click on a prefab OPENS it. Placing one is what dragging is for -
# which is the same split Unity has, and the reason a double click there has
# never dropped a copy into your scene.
s = sub1(s,
"""                            } else {
                                // The PENDING pick is handed to the host after
                                // the frame, so it must be the host's own
                                // pointer and not a temporary - asset_at
                                // returns "" for a bad index, which the host
                                // then ignores.
                                p->pending_asset = fi >= 0 && fi < (int)p->assets.size()
                                                 ? p->assets[(size_t)fi] : nullptr;
                                p->pending_as_tree = 0;
                                p->pending_at_valid = 0;   // no drop point: it was a click
                            }""",
"""                            } else if (is_scene_file(full) && !is_scene_asset(full)) {
                                // A prefab OPENS. Dropping a copy into the
                                // scene is what dragging it does; a double
                                // click that silently added an object was
                                // never anybody's intention.
                                p->prefab_open_want = full;
                            } else {
                                // The PENDING pick is handed to the host after
                                // the frame, so it must be the host's own
                                // pointer and not a temporary - asset_at
                                // returns "" for a bad index, which the host
                                // then ignores.
                                p->pending_asset = fi >= 0 && fi < (int)p->assets.size()
                                                 ? p->assets[(size_t)fi] : nullptr;
                                p->pending_as_tree = 0;
                                p->pending_at_valid = 0;   // no drop point: it was a click
                            }""",
    'double click opens prefab')

# ------------------------------------------------------------------ the bar
s = sub1(s,
"""    if (dai_dock_panel(p->dock, "Scene", &px, &py, &pw, &ph)) {
        p->view_x = px; p->view_y = py; p->view_w = pw; p->view_h = ph;
        have_view = 1;
        dai_dock_panel_end(p->dock);
    }""",
"""    if (dai_dock_panel(p->dock, "Scene", &px, &py, &pw, &ph)) {
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
    }""",
    'prefab mode bar')
wr('src/dai_editor_ui.cpp', s)

# ================================================================== the host
s = rd('examples/editor_demo.cpp')
s = sub1(s,
"""static dai_doc *g_scene_doc = nullptr;""",
"""static dai_doc *g_scene_doc = nullptr;
// Prefab mode: the scene to come back to. Empty means we are in the world.
static char g_prefab_return[512] = { 0 };""",
    'prefab return path')

s = sub1(s,
"""            const char *pick = nullptr;
            int as_tree = 0;""",
"""            // ---- prefab mode: open a prefab as the scene, and come back --
            {
                const char *popen = nullptr;
                if (dai_editor_ui_take_prefab_open(panels, &popen) && popen && *popen) {
                    // Save what is open first: leaving a scene without writing
                    // it is how an afternoon disappears.
                    if (g_scene_path[0]) dai_doc_save(doc, g_scene_path);
                    if (!g_prefab_return[0])
                        std::snprintf(g_prefab_return, sizeof(g_prefab_return), "%s", g_scene_path);
                    char full2[700];
                    std::snprintf(full2, sizeof(full2), "%s/%s", g_assets_dir, popen);
                    std::snprintf(g_scene_path, sizeof(g_scene_path), "%s", full2);
                    dai_editor_ui_prefab_mode(panels, popen);
                }
                if (dai_editor_ui_take_prefab_exit(panels)) {
                    // Back to the world, saving the prefab on the way out -
                    // which is what makes "edit the prefab" mean anything.
                    if (g_scene_path[0]) dai_doc_save(doc, g_scene_path);
                    if (g_prefab_return[0]) {
                        std::snprintf(g_scene_path, sizeof(g_scene_path), "%s", g_prefab_return);
                        g_prefab_return[0] = 0;
                    }
                    dai_editor_ui_prefab_mode(panels, nullptr);
                    // Instances in the scene are expanded from the file, so
                    // the edit shows up everywhere the moment it is reloaded.
                    dai_doc_prefab_reload(doc, g_assets_dir);
                }
            }
            const char *pick = nullptr;
            int as_tree = 0;""",
    'prefab mode host')
wr('examples/editor_demo.cpp', s)
print('patch64 ok')
