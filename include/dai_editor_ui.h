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

/* Starts renaming a node in the hierarchy: the row turns into a text field,
 * focused, with the current name selected. What the toolbar's F2 and the
 * context menu's Rename both call. */
DAI_API void dai_editor_ui_rename(dai_editor_ui *p, dai_node n);

/* Whether a right click menu is currently open. The host needs this because
 * the right button is also the camera's look around: while a menu is up it
 * belongs to the menu. */
DAI_API int  dai_editor_ui_menu_open(const dai_editor_ui *p);

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
/* The Game panel docked next to the Scene panel is its own view, with its
 * own rectangle and the game camera. 0 when the Game panel is not visible. */
DAI_API int  dai_editor_ui_game_view_rect(const dai_editor_ui *p, float *x, float *y,
                                          float *w, float *h);
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

/* ---- scene view / game view --------------------------------------------
 *
 * Unity's two tabs, and they are two different questions: the scene view is
 * where you build (editor camera, gizmos, collider wireframes), the game view
 * is what the player sees (the camera in the scene, no editor furniture).
 * Pressing Play switches to Game and Stop switches back, which is what every
 * muscle memory expects.
 *
 * The host renders: ask which view is up, and where the game camera is. */
#define DAI_VIEW_SCENE 0
#define DAI_VIEW_GAME  1
DAI_API int  dai_editor_ui_view(const dai_editor_ui *p);
DAI_API void dai_editor_ui_view_set(dai_editor_ui *p, int view);
/* The camera the game view renders from: the node tagged "MainCamera", or the
 * first camera node in the scene. Returns 0 when the scene has none - the host
 * should then draw the "No cameras rendering" message Unity draws, rather than
 * quietly showing the editor camera and calling it the game. */
DAI_API int  dai_editor_ui_game_camera(const dai_editor_ui *p, dai_vec3 *eye,
                                       dai_vec3 *look, float *fov_deg);
/* Adds a camera node at the current editor camera - "Align with view", which
 * is the only sane way to place a camera. Returns the node. */
DAI_API dai_node dai_editor_ui_add_camera(dai_editor_ui *p);

/* ---- colliders ----------------------------------------------------------
 *
 * The green wireframe every 3D editor draws around the selection, plus the
 * face handles of Edit Collider mode. Separate from the gizmo because it is a
 * different thing: the gizmo moves the OBJECT, these resize what it can hit,
 * and confusing the two is how a collider ends up silently matching the mesh
 * forever. */
DAI_API void dai_editor_ui_colliders(dai_editor_ui *p);
DAI_API int  dai_editor_ui_collider_edit(const dai_editor_ui *p);
DAI_API void dai_editor_ui_collider_edit_set(dai_editor_ui *p, int on);

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

/* The project window needs two things from the host: where the projects
 * live, and what to do when the user makes or opens one. The editor owns
 * none of that - it cannot know where your disk is - but it does own the
 * two clicks. */
typedef const char *(*dai_editor_ui_project_list_fn)(uint32_t index, void *user);
typedef int (*dai_editor_ui_project_action_fn)(const char *name, void *user);

DAI_API void dai_editor_ui_project_host(dai_editor_ui *p,
                                        dai_editor_ui_project_list_fn list,
                                        dai_editor_ui_project_action_fn create,
                                        dai_editor_ui_project_action_fn open,
                                        void *user);
/* Reloads the list from the host - after a project was created on disk. */
DAI_API void dai_editor_ui_projects_refresh(dai_editor_ui *p);

/* "New Script" in the Project window: the host writes the file (it owns the
 * disk - the editor cannot know where projects live) and the list refresh
 * makes it show up. The name comes without an extension. */
/* Renames a file inside the project's assets dir (the inline rename of the
 * Project window). old_path/new_path are asset-relative. 1 = done. */
typedef int (*dai_editor_ui_rename_fn)(const char *old_path, const char *new_path, void *user);
DAI_API void dai_editor_ui_rename_host(dai_editor_ui *p, dai_editor_ui_rename_fn fn, void *user);

/* The scene shown as the hierarchy's root row - which scene is open, the way
 * Unity puts the .unity file above everything. Dropping a node on it makes
 * that node a root again. */
DAI_API void dai_editor_ui_scene_label(dai_editor_ui *p, const char *name);
/* The hierarchy's root row reports itself as this node when a drag hovers it. */
#define DAI_SCENE_ROOT_NODE ((dai_node)0xFFFFFFFEu)
/* Scenes are files of their own (<project>/scenes/<name>.daidalos), like
 * Unity's .unity assets: the Projects tab lists them, clicking opens, and
 * "Save scene as" writes the current scene under a new name. The host owns
 * the disk; scene_open hands over a file name and the loop loads it. */
/* Fills buf with the comma separated parameter names a script declares with
 * "// @param name" lines. The inspector shows the assigned references, and a
 * hierarchy node dragged onto the field becomes the value. */
typedef void (*dai_editor_ui_params_fn)(const char *script_path, char *buf, size_t buf_size, void *user);
DAI_API void dai_editor_ui_params_host(dai_editor_ui *p, dai_editor_ui_params_fn fn, void *user);
DAI_API void dai_editor_ui_scene_host(dai_editor_ui *p,
                                      dai_editor_ui_project_list_fn list,
                                      dai_editor_ui_project_action_fn open,
                                      dai_editor_ui_project_action_fn save_as,
                                      void *user);
DAI_API void dai_editor_ui_script_host(dai_editor_ui *p,
                                       int (*create)(const char *name, void *user),
                                       void *user);
/* Same split for folders, and for the two commands the project window's menu
 * can only ask for: the host owns the disk. Read and clear each frame. */
DAI_API void dai_editor_ui_folder_host(dai_editor_ui *p,
                                       int (*create)(const char *name, void *user),
                                       void *user);
/* The Project Settings half of the Settings panel. The editor does not know
 * what a project setting IS - gravity and tick rate belong to the host's
 * project layer - so the host draws that half with plain dai_ui widgets. */
DAI_API void dai_editor_ui_project_settings_host(dai_editor_ui *p,
                                                 void (*draw)(void *user), void *user);
DAI_API int  dai_editor_ui_take_save(dai_editor_ui *p);
DAI_API int  dai_editor_ui_take_refresh(dai_editor_ui *p);

/* ---- the dock layout ----------------------------------------------------
 * Panels tile in a tree (see dai_dock.h). The host needs these two to keep a
 * layout across restarts, and the Window menu to reopen a closed panel. */
DAI_API size_t     dai_editor_ui_layout_save(const dai_editor_ui *p, char *buf, size_t n);
DAI_API dai_result dai_editor_ui_layout_load(dai_editor_ui *p, const char *text);
DAI_API void       dai_editor_ui_panel_open(dai_editor_ui *p, const char *title);
/* Which project is open ("" when none) - the host shows it in the title bar
 * and knows which folder "save" means. */
DAI_API const char *dai_editor_ui_project(const dai_editor_ui *p);

/* The mesh picker: the host hands over its renderer's inventory, the Project
 * window shows it, and the renderer component edits the selection's mesh.
 * Names come from a host function so the engine needs no renderer include. */
typedef const char *(*dai_editor_ui_mesh_name_fn)(uint32_t mesh, void *user);
DAI_API void dai_editor_ui_mesh_host(dai_editor_ui *p,
                                     dai_editor_ui_mesh_name_fn name,
                                     uint32_t mesh_count, void *user);

/* The Settings window can change the font size, and the font is the host's
 * (it loaded it, it owns the texture). When the user picks a size the host
 * gets the pixel value and should reload the font and call dai_ui_font_set. */
DAI_API void dai_editor_ui_settings_host(dai_editor_ui *p,
                                         void (*apply_font)(float px, void *user),
                                         float current_px, void *user);

/* Where every window is, one line: "Hierarchy dock=1 slot=1 0,34 230x528 | ...".
 * For the field report "the layout looks wrong" - a screenshot of a maximised
 * window on a monitor across the room is not data. */
DAI_API void dai_editor_ui_layout_dump(const dai_editor_ui *p, char *out, size_t n);

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
