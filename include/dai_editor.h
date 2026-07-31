/*
 * Editor core - frontend agnostic.
 *
 * Selection, picking, transform edits and undo/redo live here, in C, so the
 * native editor and a future web frontend share one implementation instead of
 * two that drift apart. Nothing in this header draws anything: it turns a
 * mouse position into a ray, a ray into a selection, and an edit into a
 * command that can be undone.
 *
 * The command stack is the whole reason this is a separate layer. An editor
 * without undo is a toy, and undo bolted on later never covers everything.
 */
#ifndef DAI_EDITOR_H
#define DAI_EDITOR_H

#include "daidalos.h"
#include "dai_scene.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dai_editor dai_editor;

typedef enum dai_gizmo_mode {
    DAI_GIZMO_TRANSLATE = 0,
    DAI_GIZMO_ROTATE,
    DAI_GIZMO_SCALE
} dai_gizmo_mode;

typedef enum dai_gizmo_axis {
    DAI_AXIS_NONE = 0,
    DAI_AXIS_X, DAI_AXIS_Y, DAI_AXIS_Z,
    DAI_AXIS_XY, DAI_AXIS_XZ, DAI_AXIS_YZ,
    DAI_AXIS_ALL
} dai_gizmo_axis;

DAI_API dai_editor *dai_editor_create(dai_scene *scene);
DAI_API void        dai_editor_destroy(dai_editor *e);

/* ---- camera, so picking knows where the screen is ---------------------- */

DAI_API void dai_editor_camera(dai_editor *e, dai_vec3 eye, dai_vec3 target, dai_vec3 up,
                               float fov_deg, float znear, float zfar,
                               float viewport_w, float viewport_h);
/* Builds the world space ray under a pixel. Also used by gameplay code that
 * wants to click on things. */
DAI_API void dai_editor_ray(const dai_editor *e, float mouse_x, float mouse_y,
                            dai_vec3 *origin, dai_vec3 *direction);

/* ---- selection --------------------------------------------------------- */

/* Returns the entity under the pixel, or DAI_INVALID_ENTITY. */
DAI_API dai_entity dai_editor_pick(dai_editor *e, float mouse_x, float mouse_y);
DAI_API void       dai_editor_select(dai_editor *e, dai_entity ent, int additive);
DAI_API void       dai_editor_deselect_all(dai_editor *e);
DAI_API uint32_t   dai_editor_selection_count(const dai_editor *e);
DAI_API dai_entity dai_editor_selected(const dai_editor *e, uint32_t index);
DAI_API int        dai_editor_is_selected(const dai_editor *e, dai_entity ent);
/* Centre of the selection - where the gizmo goes. */
DAI_API dai_vec3   dai_editor_selection_center(const dai_editor *e);

/* ---- editing ----------------------------------------------------------- */

DAI_API void dai_editor_gizmo_mode(dai_editor *e, int mode);
DAI_API int  dai_editor_gizmo_mode_get(const dai_editor *e);
DAI_API void dai_editor_snap(dai_editor *e, float translate_step, float rotate_step_deg);

/* A drag is one undo step no matter how many mouse move events it takes:
 * begin, any number of updates, then end. */
DAI_API void dai_editor_drag_begin(dai_editor *e, int axis, float mouse_x, float mouse_y);
DAI_API void dai_editor_drag_update(dai_editor *e, float mouse_x, float mouse_y);
DAI_API void dai_editor_drag_end(dai_editor *e);
DAI_API int  dai_editor_dragging(const dai_editor *e);

/* Direct edits, each one undoable on its own. */
DAI_API void dai_editor_move_selection(dai_editor *e, dai_vec3 delta);
DAI_API dai_result dai_editor_delete_selection(dai_editor *e);

/* ---- undo -------------------------------------------------------------- */

DAI_API int  dai_editor_undo(dai_editor *e);
DAI_API int  dai_editor_redo(dai_editor *e);
DAI_API uint32_t dai_editor_undo_depth(const dai_editor *e);
DAI_API uint32_t dai_editor_redo_depth(const dai_editor *e);
/* Name of the next undo step, for the menu. Empty when there is nothing. */
DAI_API const char *dai_editor_undo_name(const dai_editor *e);

#ifdef __cplusplus
}
#endif

#endif /* DAI_EDITOR_H */
