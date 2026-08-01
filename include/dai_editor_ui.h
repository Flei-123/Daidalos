/*
 * The editor's panels: hierarchy, inspector, toolbar, timeline, gizmo overlay.
 *
 * This is the one place that knows about both dai_editor and dai_ui. The editor
 * core stays free of any UI dependency, so a different frontend (a web viewer,
 * a Qt shell) can drive the same core without dragging this file in.
 *
 * It is immediate mode like everything else: call the panel functions every
 * frame, they read the document and write edits straight back to it. The only
 * retained state is what genuinely cannot be derived - which tree rows are
 * folded, and whether a field is mid-drag.
 */
#ifndef DAI_EDITOR_UI_H
#define DAI_EDITOR_UI_H

#include "dai_editor.h"
#include "dai_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dai_editor_ui dai_editor_ui;

DAI_API dai_editor_ui *dai_editor_ui_create(dai_editor *editor, dai_ui *ui);
DAI_API void           dai_editor_ui_destroy(dai_editor_ui *p);

/* The whole editor for a surface of this size: a solid toolbar along the top, a
 * status bar along the bottom, and Hierarchy, Project and Inspector as windows
 * the user can move, resize, collapse and raise. What no window covers is the
 * scene view - ask dai_editor_ui_viewport_rect where that ended up. */
DAI_API void dai_editor_ui_frame(dai_editor_ui *p, float viewport_w, float viewport_h);

/* Puts the three windows back where they started. The layout is the user's, so
 * it survives every frame - which also means a window dragged somewhere useless
 * needs a way back. */
DAI_API void dai_editor_ui_layout_reset(dai_editor_ui *p, float viewport_w, float viewport_h);

/* The part of the surface the scene is visible in, after the bars and the
 * docked windows. */
DAI_API void dai_editor_ui_viewport_rect(const dai_editor_ui *p, float *x, float *y,
                                         float *w, float *h);

/* The bar along the bottom: mode, node count, selection, last undo step. */
DAI_API void dai_editor_ui_status(dai_editor_ui *p, float x, float y, float w, float h);

/* Did the user ask to place an asset in the Project window this frame? Same
 * meaning as dai_editor_ui_assets' return value, for the built in layout.
 * Clears itself when read. */
DAI_API int dai_editor_ui_take_asset(dai_editor_ui *p, const char **out_path, int *out_as_tree);

/* Or place the pieces yourself. */
DAI_API void dai_editor_ui_hierarchy(dai_editor_ui *p, float x, float y, float w, float h);
DAI_API void dai_editor_ui_inspector(dai_editor_ui *p, float x, float y, float w, float h);
DAI_API void dai_editor_ui_toolbar(dai_editor_ui *p, float x, float y, float w);
DAI_API void dai_editor_ui_timeline(dai_editor_ui *p, float x, float y, float w);
/* Projects the gizmo into screen space and draws it as UI lines, so it is
 * always on top of the scene instead of buried in it. */
DAI_API void dai_editor_ui_gizmo(dai_editor_ui *p);

/* Feeds a viewport click to the editor: gizmo handles win over objects, a drag
 * continues until release, an empty click clears the selection. Does nothing
 * while the pointer is over a panel. Returns 1 if it consumed the input. */
DAI_API int dai_editor_ui_viewport_input(dai_editor_ui *p, float mouse_x, float mouse_y,
                                         int mouse_down);

/* Everything the viewport does in one call: camera first (Unity bindings, see
 * dai_editor.h), then selection and gizmo with the left button. Returns 1 if
 * the viewport used the input. Prefer this over the two calls above. */
DAI_API int dai_editor_ui_viewport(dai_editor_ui *p, const dai_editor_cam_input *in);

/* Opens every component block in the inspector. For a screenshot, and for a
 * test that clicks its way down the panel and would otherwise fold the block it
 * is looking for. */
DAI_API void dai_editor_ui_expand_all(dai_editor_ui *p);

/* Number of rows the hierarchy currently shows - folded subtrees excluded. */
DAI_API uint32_t dai_editor_ui_visible_rows(const dai_editor_ui *p);

/* ---- asset browser ------------------------------------------------------ */

/*
 * What is on disk, and one click to put it in the scene.
 *
 * The panel does NOT know where the list comes from or how to load anything -
 * the same rule the resolver follows. The host fills it (dai_assets_list is
 * the obvious source) and the host does the placing, so the editor UI keeps
 * building without the asset layer and a project with its own idea of where
 * assets live can still use the panel.
 *
 *   char paths[64][96];
 *   uint32_t n = dai_assets_list(assets, paths[0], 64, 96);
 *   const char *ptrs[64];
 *   for (uint32_t i = 0; i < n && i < 64; ++i) ptrs[i] = paths[i];
 *   dai_editor_ui_asset_list(panel, ptrs, n);
 *   ...
 *   const char *pick; int as_tree;
 *   if (dai_editor_ui_assets(panel, x, y, w, h, &pick, &as_tree)) {
 *       if (as_tree) dai_assets_instantiate(assets, doc, pick, 0);
 *       else         add_a_node_with(pick);
 *   }
 *
 * The pointers must stay alive until the next call.
 */
DAI_API void dai_editor_ui_asset_list(dai_editor_ui *p, const char *const *paths, uint32_t count);

/* Draws the browser. Returns 1 on the frame the user asked to place something:
 * `out_path` is which, and `out_as_tree` says whether they hit "Place" (one
 * node, one rigid body) or "As tree" (one node per piece, one body each - the
 * crate whose lid opens). */
DAI_API int dai_editor_ui_assets(dai_editor_ui *p, float x, float y, float w, float h,
                                 const char **out_path, int *out_as_tree);

/* Which row is highlighted, or -1. Survives between frames so the panel can be
 * drawn from anywhere. */
DAI_API int dai_editor_ui_asset_selected(const dai_editor_ui *p);

#ifdef __cplusplus
}
#endif

#endif /* DAI_EDITOR_UI_H */
