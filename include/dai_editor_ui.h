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

/* Draws every Text component of `doc` into the rectangle given, through `ui`.
 *
 * It lives here rather than in the host because BOTH hosts need it and they
 * must agree: what the Game view shows and what the exported game shows have
 * to be the same picture, or the editor is a rehearsal for a different play.
 * The exported runtime calls exactly this function with its whole window.
 *
 * `resolve` turns what the component holds into what the player reads - see
 * dai_strings_resolve. Null means "the text is the text", which is right for
 * a project with no string tables.
 *
 * `scale` is the UI scale, so a HUD authored at 24 px is 24 px on a 4K
 * display too rather than a sixth of the size. */
typedef const char *(*dai_hud_resolve_fn)(const char *text, void *user);
/* Turns a project path into a texture the renderer already holds. Null means
 * "this host has no textures", and then Image components simply do not draw -
 * which is the right behaviour for a headless test, not an error. */
typedef uint32_t (*dai_hud_image_fn)(const char *path, float *out_w, float *out_h, void *user);
DAI_API void dai_hud_images(dai_hud_image_fn fn, void *user);

/* Where the last dai_hud_draw put a node's UI, in screen pixels. The editor
 * uses it to draw a frame around the selected element and to drag it; nothing
 * else can know, because the anchor maths lives inside the draw. Returns 0
 * when that node drew nothing this frame. */
DAI_API int dai_hud_rect_of(dai_node n, float *x, float *y, float *w, float *h);
DAI_API void dai_hud_draw(struct dai_ui *ui, struct dai_doc *doc,
                          float x, float y, float w, float h, float scale,
                          dai_hud_resolve_fn resolve, void *user);

/* The Localisation window's data lives in the panel; the FILES belong to the
 * host, which is the one that knows where Assets/Strings is. `load` fills the
 * grid, `save` writes it back - both return 1 on success.
 *
 * The grid is reached with the three calls below, which is deliberately a
 * poke-and-peek API rather than a shared struct: the panel owns its rows, and
 * a host that reached into them would have to be updated every time a column
 * is added. */
/* What a "@key" means to a player. The inspector shows it under the Text
 * field so a key can be checked without exporting. Same function the HUD uses. */
DAI_API void dai_editor_ui_tr_host(dai_editor_ui *p, dai_hud_resolve_fn fn, void *user);

typedef int (*dai_editor_ui_loc_fn)(void *user);
DAI_API void dai_editor_ui_loc_host(dai_editor_ui *p, dai_editor_ui_loc_fn load,
                                    dai_editor_ui_loc_fn save, void *user);
/* Replaces the whole grid. Languages first, then one call per key. */
DAI_API void dai_editor_ui_loc_begin(dai_editor_ui *p);
DAI_API void dai_editor_ui_loc_lang(dai_editor_ui *p, const char *code, const char *name);
DAI_API void dai_editor_ui_loc_set(dai_editor_ui *p, const char *key,
                                   const char *lang, const char *text);
/* Reading it back, for the host's writer. */
DAI_API uint32_t    dai_editor_ui_loc_lang_count(const dai_editor_ui *p);
DAI_API const char *dai_editor_ui_loc_lang_at(const dai_editor_ui *p, uint32_t i,
                                              const char **out_name);
DAI_API uint32_t    dai_editor_ui_loc_key_count(const dai_editor_ui *p);
DAI_API const char *dai_editor_ui_loc_key_at(const dai_editor_ui *p, uint32_t i);
DAI_API const char *dai_editor_ui_loc_get(const dai_editor_ui *p, uint32_t key_i,
                                          uint32_t lang_i);

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

/* The floor grid as world-space line segments (x,y,z pairs, consecutive).
 * Returns the point count written (2 per segment), 0 when the grid is off.
 * The host feeds these to dai_render_lines so the grid is depth tested
 * against the scene instead of shining through every object as a 2D overlay. */
DAI_API uint32_t dai_editor_ui_grid_lines(const dai_editor_ui *p, float *out_xyz, uint32_t max_points);

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

/* Where the last taken asset was dropped, in world space, and whether there
 * IS such a place (0 when it was picked by double click rather than dragged
 * into the viewport - then the host decides, as it always did).
 *
 * Read straight after dai_editor_ui_take_asset. */
DAI_API int dai_editor_ui_take_asset_at(const dai_editor_ui *p, float *x, float *y, float *z);

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
DAI_API int  dai_editor_ui_take_prefab_open(dai_editor_ui *p, const char **out_rel);

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
/* > 0 when the game camera is orthographic: half the visible height in world
 * units. 0 means perspective. This is how the host learns it is a 2D game. */
DAI_API float dai_editor_ui_game_ortho(const dai_editor_ui *p);
DAI_API int  dai_editor_ui_game_camera(const dai_editor_ui *p, dai_vec3 *eye,
                                       dai_vec3 *look, float *fov_deg);

/* ---- the camera preview -------------------------------------------------
 *
 * Unity's small window in the corner of the scene view: select a camera, and
 * what that camera sees appears while you move it. Framing a shot by flying
 * the editor camera to where the game camera is, looking, and flying back is
 * the alternative, and it is why nobody frames shots.
 *
 * Returns 1 when there is something to draw: `p` has a camera selected and the
 * Scene view is on screen. The rectangle is in UI (logical) pixels, already
 * inside the scene panel and already drawn as a frame by the editor - the host
 * only has to render the world into it from the camera below. */
DAI_API int dai_editor_ui_camera_preview(const dai_editor_ui *p,
                                         float *x, float *y, float *w, float *h,
                                         dai_vec3 *eye, dai_vec3 *look, float *fov_deg,
                                         float *ortho_size);
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
/* "Create Prefab" in the hierarchy menu. Gets the node id as a decimal string
 * and the asset-relative target path; the host does the actual save. Shares
 * the rename host's user pointer. */
DAI_API void dai_editor_ui_prefab_host(dai_editor_ui *p, dai_editor_ui_rename_fn fn);

/* The host opens a file in the machine's editor (VS Code when it is there).
 * The editor hands over the asset-relative path on a double click. */
DAI_API void dai_editor_ui_open_asset_host(dai_editor_ui *p, dai_editor_ui_rename_fn fn, void *user);

/* Importing: copy something from anywhere on disk INTO the project's assets.
 * Same signature as the rename host and the same division of labour - the
 * editor knows which folder is open and what was dropped on it, the host owns
 * the disk. `src` is an absolute OS path (a file or a whole folder), `dest`
 * is asset-relative ("models/crate.glb"). Returns 1 when it landed. */
DAI_API void dai_editor_ui_import_host(dai_editor_ui *p, dai_editor_ui_rename_fn fn, void *user);

/* Deleting an asset. `new_path` is NULL - the same callback shape as rename
 * and import, because the host's answer to all three is "it owns the disk".
 * Return 1 when the file (or folder, recursively) is gone. */
DAI_API void dai_editor_ui_delete_host(dai_editor_ui *p, dai_editor_ui_rename_fn fn, void *user);

/* Delete pressed while the Project window is under the pointer: asks about
 * whatever is selected there and returns 1 when it took the key, so the host
 * knows not to also delete the scene selection. */
DAI_API int  dai_editor_ui_delete_project_pick(dai_editor_ui *p);

/* ---- the built-in script editor -----------------------------------------
 *
 * Reading and writing a text asset. The editor owns the tabs, the caret and
 * the undo; the host owns the disk, as it does for every other file operation
 * in this header.
 *   read  - fills `out` with at most `max` bytes, returns how many (0 = could
 *           not read). Must NUL terminate.
 *   write - the whole file, NUL terminated text. 1 on success. */
typedef uint32_t (*dai_editor_ui_read_fn)(const char *rel, char *out, uint32_t max, void *user);
typedef int      (*dai_editor_ui_write_fn)(const char *rel, const char *text, void *user);
DAI_API void dai_editor_ui_file_host(dai_editor_ui *p, dai_editor_ui_read_fn rd,
                                     dai_editor_ui_write_fn wr, void *user);

/* Opens an asset in the Script panel (and shows the panel). 1 when it did. */
DAI_API int  dai_editor_ui_script_open(dai_editor_ui *p, const char *rel_path);
/* Saves whatever the Script panel is showing. What Ctrl+S means while the
 * script editor has the keyboard - the host asks this FIRST and only saves the
 * scene when the answer is 0. */
DAI_API int  dai_editor_ui_script_save(dai_editor_ui *p);
/* The list of files the Script panel has open, as text - one asset-relative
 * path per line, the active one first. Saved next to the layout, because that
 * is what it is: part of how the editor was left, not part of the project. */
DAI_API size_t dai_editor_ui_scripts_open_save(const dai_editor_ui *p, char *buf, size_t n);
DAI_API void   dai_editor_ui_scripts_open_load(dai_editor_ui *p, const char *text);
/* 0 = open scripts in the built-in editor, 1 = hand them to the external one.
 * The host stores it with the other preferences. */
DAI_API void dai_editor_ui_script_editor_pref(dai_editor_ui *p, int external);
DAI_API int  dai_editor_ui_script_editor_pref_get(const dai_editor_ui *p);

/* What the window system says was dropped on the editor: absolute paths, one
 * per line (dai_window_dropped_files hands over exactly this), plus where the
 * pointer was in UI coordinates. The Project window takes the drop when the
 * pointer is over it and imports into the folder it is showing; everything
 * else ignores it. Returns 1 when something was imported.
 *
 * The host does not have to work out which panel was hit - it cannot, the
 * layout is the editor's - it just forwards every drop. */
DAI_API int  dai_editor_ui_drop_files(dai_editor_ui *p, const char *paths_nl,
                                      float x, float y);

/* The scene shown as the hierarchy's root row - which scene is open, the way
 * Unity puts the .unity file above everything. Dropping a node on it makes
 * that node a root again. */
DAI_API void dai_editor_ui_scene_label(dai_editor_ui *p, const char *name);
/* Whether the open scene differs from the file on disk. The host compares the
 * document's revision against the one it last wrote - only it knows when a
 * save happened - and the hierarchy puts an asterisk on the scene row. */
DAI_API void dai_editor_ui_scene_dirty(dai_editor_ui *p, int dirty);
DAI_API int  dai_editor_ui_scene_dirty_get(const dai_editor_ui *p);
/* A short message in the status bar - "saved main.daidalos". Fades by itself. */
DAI_API void dai_editor_ui_toast(dai_editor_ui *p, const char *text, float seconds);

/* One clipboard for everything: objects from the hierarchy, the inspector's
 * fields, console lines, and free text. kind: 0 = free text, 1 = node. */
DAI_API void dai_editor_ui_clipboard_set(dai_editor_ui *p, int kind, const char *text);
DAI_API const char *dai_editor_ui_clipboard_get(const dai_editor_ui *p, int *kind);
DAI_API int  dai_editor_ui_clipboard_has(const dai_editor_ui *p);
/* Bumped on every clipboard_set - the host watches this to mirror copies into
 * the OS clipboard (dai_window_clipboard_set), so a copied node, component or
 * console error can be pasted into a chat or a search bar. */
DAI_API unsigned dai_editor_ui_clipboard_rev(const dai_editor_ui *p);
/* Named layouts: the host owns the files, the editor the dock text. */
DAI_API void dai_editor_ui_layout_host(dai_editor_ui *p,
                                       void (*save)(const char *, const char *, size_t, void *),
                                       int  (*load)(const char *, char *, size_t, void *),
                                       void *user);
DAI_API int  dai_editor_ui_layout_apply_name(dai_editor_ui *p, const char *name);
/* The Console panel. level: 0 info, 1 warning, 2 error. Repeats collapse. */
DAI_API void dai_editor_ui_log(dai_editor_ui *p, int level, const char *text);
DAI_API void dai_editor_ui_log_clear(dai_editor_ui *p);
/* The last lines the console holds, newest LAST, as plain text. What a crash
 * report wants: the console is the only running narrative the editor keeps,
 * and after the window is gone it is the only one that survives. Returns the
 * number of bytes written (never more than buf_size - 1, always NUL
 * terminated). Safe to call from a crash handler: it only reads and copies. */
DAI_API uint32_t dai_editor_ui_log_tail(const dai_editor_ui *p, char *buf, uint32_t buf_size);
/* The Audio panel's mixer. bus: 0 master, 1 music, 2 sfx, 3 ui. Muted = 0. */
DAI_API float dai_editor_ui_bus_gain(const dai_editor_ui *p, int bus);
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
/* "Create: Material" - the host writes a default .daimat at that
 * asset-relative path. Same split as every other create: the editor knows
 * which folder is open, the host owns the disk and the format. */
DAI_API void dai_editor_ui_material_host(dai_editor_ui *p,
                                         int (*create)(const char *name, void *user),
                                         void *user);
/* The Project Settings half of the Settings panel. The editor does not know
 * what a project setting IS - gravity and tick rate belong to the host's
 * project layer - so the host draws that half with plain dai_ui widgets. */
DAI_API void dai_editor_ui_project_settings_host(dai_editor_ui *p,
                                                 void (*draw)(void *user), void *user);
DAI_API int  dai_editor_ui_take_save(dai_editor_ui *p);
DAI_API int  dai_editor_ui_take_refresh(dai_editor_ui *p);

/* What the About block in Settings shows. Pushed by the host each frame (it is
 * three strings) because the editor knows neither where the projects live nor
 * whether an update exists. `update_status` is free text - "up to date",
 * "2026.08.05-9 available", "could not reach the server". */
DAI_API void dai_editor_ui_about(dai_editor_ui *p, const char *projects_dir,
                                 const char *assets_dir, const char *update_status);
/* 1 once, when the user pressed "Check for updates". */
DAI_API int  dai_editor_ui_take_update_check(dai_editor_ui *p);
/* 1 once when a material was assigned (or the assets were refreshed): the host
 * should re-read every .daimat a node points at and copy its numbers onto that
 * node. The editor does not open files; the host does. */
DAI_API int  dai_editor_ui_take_material_apply(dai_editor_ui *p);

/* ---- the dock layout ----------------------------------------------------
 * Panels tile in a tree (see dai_dock.h). The host needs these two to keep a
 * layout across restarts, and the Window menu to reopen a closed panel. */
DAI_API size_t     dai_editor_ui_layout_save(const dai_editor_ui *p, char *buf, size_t n);
DAI_API dai_result dai_editor_ui_layout_load(dai_editor_ui *p, const char *text);
DAI_API void       dai_editor_ui_panel_open(dai_editor_ui *p, const char *title);

/* F2 in the Project window: rename whatever was last clicked there - a file
 * OR a folder. Returns 1 when it started one, so the host can fall through to
 * renaming the scene selection when the answer is no. */
DAI_API int        dai_editor_ui_rename_project_pick(dai_editor_ui *p);
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
/* Project-wide physics defaults, copied into every NEW rigidbody. The host
 * owns the project settings, so it pushes them here each frame (cheap) and
 * Add Component > Rigidbody starts from them instead of zeroes. */
DAI_API void dai_editor_ui_physics_defaults(dai_editor_ui *p, float friction, float restitution);

DAI_API void dai_editor_ui_settings_host(dai_editor_ui *p,
                                         void (*apply_font)(float px, void *user),
                                         float current_px, void *user);

/* The display scale. Auto (0) asks the window system; anything else is the
 * user overruling it, which a laptop whose EDID lies about its size needs.
 * The host owns it because it owns the font atlas and the icon atlas. */
DAI_API void dai_editor_ui_scale_host(dai_editor_ui *p,
                                      void (*apply_scale)(float scale, void *user),
                                      float current, void *user);

/* Where every window is, one line: "Hierarchy dock=1 slot=1 0,34 230x528 | ...".
 * For the field report "the layout looks wrong" - a screenshot of a maximised
 * window on a monitor across the room is not data. */
DAI_API void dai_editor_ui_layout_dump(const dai_editor_ui *p, char *out, size_t n);

/* Number of rows the hierarchy currently shows - folded subtrees excluded. */
DAI_API uint32_t dai_editor_ui_visible_rows(const dai_editor_ui *p);

/* The frame rate, for the readout in the corner of the views. The host owns
 * the clock - it is the only thing that knows when a frame began and ended -
 * and should hand over an already smoothed value; the editor draws digits, it
 * does not average. Switched off in the view options. */
DAI_API void dai_editor_ui_fps(dai_editor_ui *p, float fps);

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
/* Same feed, but for DIRECTORIES on disk (relative to the mounted assets
 * root, '/'-separated). The browser shows them even when empty - otherwise
 * a newly created folder is invisible until it holds a file. */
DAI_API void dai_editor_ui_folder_list(dai_editor_ui *p, const char *const *paths, uint32_t count);

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
