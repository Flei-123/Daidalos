/*
 * Editor core - frontend agnostic.
 *
 * Selection, picking and gizmo maths live here, in C, so the native editor and
 * any other frontend share one implementation instead of two that drift apart.
 * Nothing in this header draws anything: it turns a mouse position into a ray,
 * a ray into a selection, and a drag into an edit on the scene document.
 *
 * Undo is deliberately NOT implemented here. It belongs to dai_doc, which owns
 * the data being edited - that is what makes "undo delete" work at all (see the
 * header comment in dai_doc.h). The functions below just forward, so a frontend
 * has one place to call.
 */
#ifndef DAI_EDITOR_H
#define DAI_EDITOR_H

#include "daidalos.h"
#include "dai_doc.h"

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

/* The editor edits the document and picks against the live world, so it needs
 * the sync layer that connects the two. */
DAI_API dai_editor *dai_editor_create(dai_doc *doc, dai_doc_sync *sync);
DAI_API void        dai_editor_destroy(dai_editor *e);
DAI_API dai_doc    *dai_editor_doc(const dai_editor *e);

/* ---- camera, so picking knows where the screen is ---------------------- */

DAI_API void dai_editor_camera(dai_editor *e, dai_vec3 eye, dai_vec3 target, dai_vec3 up,
                               float fov_deg, float znear, float zfar,
                               float viewport_w, float viewport_h);
/* Changes only the viewport size - for a window that was resized. Picking and
 * the gizmo work in pixels, so they need the new size; the camera's position
 * and angles must not move, which is why this is not dai_editor_camera with
 * the same eye and target. */
DAI_API void dai_editor_camera_viewport(dai_editor *e, float viewport_w, float viewport_h);
/* The viewport is a rectangle ON the surface, not the surface: the scene view
 * lives inside its window now. x/y are where its body starts, w/h its size.
 * Picking, the gizmo and the world clip all speak surface pixels, so every
 * side of this has to agree. */
DAI_API void dai_editor_camera_viewport_rect(dai_editor *e, float x, float y,
                                             float w, float h);

/* Builds the world space ray under a pixel. Also used by gameplay code that
 * wants to click on things. */
DAI_API void dai_editor_ray(const dai_editor *e, float mouse_x, float mouse_y,
                            dai_vec3 *origin, dai_vec3 *direction);
/* Projects a world point to pixels. Returns 0 when it is behind the camera,
 * in which case *out is not written. */
DAI_API int  dai_editor_project(const dai_editor *e, dai_vec3 world,
                                float *out_x, float *out_y);

/* ---- viewport camera (Unity bindings) ---------------------------------- */

/*
 * Deliberately Unity's scheme, not Blender's:
 *
 *   right mouse held      look around in place (flythrough), and while it is
 *                         held WASD moves, Q/E go down/up, shift is the boost,
 *                         and the wheel changes the move SPEED rather than the
 *                         position - that last one is the detail people notice
 *                         is missing.
 *   wheel (nothing held)  dolly along the view direction
 *   middle mouse held     pan in the screen plane
 *   alt + left            orbit the pivot
 *   alt + right           dolly by dragging
 *   F                     frame the selection and put the pivot on it
 *
 * The host fills this every frame from its own window backend, so the bindings
 * live in one place instead of being scattered through the frontend.
 */
typedef struct dai_editor_cam_input {
    float mouse_x, mouse_y;
    int   mouse_left, mouse_right, mouse_middle;
    float wheel;                 /* notches, positive = away from the user */
    int   key_w, key_a, key_s, key_d, key_q, key_e;
    int   key_shift, key_alt;
    int   key_focus;             /* F - edge triggered, held does not repeat */
    float dt;                    /* seconds since the last call */
} dai_editor_cam_input;

/* Returns 1 while the camera is using the pointer, so the caller knows not to
 * pick or drag a gizmo with the same click. */
DAI_API int  dai_editor_cam_update(dai_editor *e, const dai_editor_cam_input *in);
DAI_API int  dai_editor_cam_active(const dai_editor *e);
/* Base metres per second; shift multiplies it. Default 6. */
DAI_API void dai_editor_cam_speed(dai_editor *e, float units_per_second);
DAI_API float dai_editor_cam_speed_get(const dai_editor *e);
/* Frames the selection (or the whole scene when nothing is selected) and moves
 * the orbit pivot onto it - what F does in Unity. */
DAI_API void dai_editor_cam_focus(dai_editor *e);
DAI_API dai_vec3 dai_editor_cam_pivot(const dai_editor *e);

/* ---- selection --------------------------------------------------------- */

/* The node under the pixel, or DAI_INVALID_NODE. */
DAI_API dai_node dai_editor_pick(dai_editor *e, float mouse_x, float mouse_y);
DAI_API void     dai_editor_select(dai_editor *e, dai_node n, int additive);
DAI_API void     dai_editor_deselect_all(dai_editor *e);
DAI_API uint32_t dai_editor_selection_count(const dai_editor *e);
DAI_API dai_node dai_editor_selected(const dai_editor *e, uint32_t index);
DAI_API int      dai_editor_is_selected(const dai_editor *e, dai_node n);
/* Centre of the selection - where the gizmo goes. */
DAI_API dai_vec3 dai_editor_selection_center(const dai_editor *e);

/* ---- gizmo ------------------------------------------------------------- */

DAI_API void dai_editor_gizmo_mode(dai_editor *e, int mode);
DAI_API int  dai_editor_gizmo_mode_get(const dai_editor *e);
/* 0 disables that kind of snapping. Snapping applies to the drag delta, not to
 * the absolute value: an object that already sat off grid must not jump onto
 * the grid just because you nudged it. */
DAI_API void dai_editor_snap(dai_editor *e, float translate_step, float rotate_step_deg,
                             float scale_step);

/* Screen constant size: the gizmo is drawn `pixels` tall no matter how far the
 * camera is. Default 90. */
DAI_API void  dai_editor_gizmo_size(dai_editor *e, float pixels);
DAI_API float dai_editor_gizmo_scale(const dai_editor *e);   /* world units */

/* One line of the gizmo, in world space, for the frontend to draw. */
typedef struct dai_gizmo_line {
    dai_vec3 a, b;
    dai_vec3 color;
    int      axis;        /* dai_gizmo_axis this line belongs to */
    int      highlighted; /* hovered or being dragged */
} dai_gizmo_line;

/* Fills `out` with the current gizmo for the current mode. Returns the number
 * of lines, 0 when nothing is selected. */
DAI_API uint32_t dai_editor_gizmo_lines(const dai_editor *e, dai_gizmo_line *out, uint32_t max);
/* Which handle is under the pixel. Call before picking objects: the gizmo sits
 * on top. */
DAI_API int  dai_editor_gizmo_hit(const dai_editor *e, float mouse_x, float mouse_y);
DAI_API void dai_editor_gizmo_hover(dai_editor *e, float mouse_x, float mouse_y);
DAI_API int  dai_editor_gizmo_hovered(const dai_editor *e);

/* ---- editing ----------------------------------------------------------- */

/* A drag is one undo step no matter how many mouse move events it takes:
 * begin, any number of updates, then end. */
DAI_API void dai_editor_drag_begin(dai_editor *e, int axis, float mouse_x, float mouse_y);
DAI_API void dai_editor_drag_update(dai_editor *e, float mouse_x, float mouse_y);
DAI_API void dai_editor_drag_end(dai_editor *e);
DAI_API void dai_editor_drag_cancel(dai_editor *e);   /* escape: back to the start */
DAI_API int  dai_editor_dragging(const dai_editor *e);

/* Direct edits, each one undoable on its own. */
DAI_API void       dai_editor_move_selection(dai_editor *e, dai_vec3 delta);
DAI_API dai_result dai_editor_delete_selection(dai_editor *e);
/* Copies the selection (and its children) and selects the copies. */
DAI_API uint32_t   dai_editor_duplicate_selection(dai_editor *e);

/* ---- play mode --------------------------------------------------------- */

/* Edit -> Play -> Stop returns the world exactly as it was, because the
 * document never changed while the simulation ran. Keeping a result is an
 * explicit step (dai_editor_apply_sim), not the default - losing a carefully
 * placed scene because you pressed play is the classic editor disaster. */
typedef enum dai_editor_state {
    DAI_EDITOR_EDIT = 0,
    DAI_EDITOR_PLAY,
    DAI_EDITOR_PAUSED
} dai_editor_state;

DAI_API void dai_editor_play(dai_editor *e);
DAI_API void dai_editor_pause(dai_editor *e);
DAI_API void dai_editor_stop(dai_editor *e);
DAI_API int  dai_editor_state_get(const dai_editor *e);
/* Advances the world when playing. Returns the number of ticks stepped. Does
 * nothing while editing or paused, so a frontend can call it unconditionally. */
/* The colour the node is actually drawn in, palette pick included. Returns 0
 * when the node has no live entity. */
DAI_API int dai_editor_node_color(dai_editor *e, dai_node n, dai_vec3 *out);

/* The transform the SELECTION is actually at. While editing this is the
 * document. While playing or paused the document still holds the pre-play
 * pose - that is what makes Stop exact - and this reads the live body
 * instead, so the inspector and the gizmo show the object where it IS, not
 * where it was when play was pressed. Returns 0 when the node has no live
 * body (a group node), and the caller should then show the document. */
DAI_API int dai_editor_live_position(const dai_editor *e, dai_node n, dai_vec3 *out);

/* The name used for picking paths and for status lines: "Player" is better
 * than "node 7", and a Unity user looks for the tag too. Writes "tag name",
 * "name", or the id, into buf. */
DAI_API void dai_editor_node_label(const dai_editor *e, dai_node n, char *buf, size_t len);

/* Pushes document changes into the live scene. Incremental (only nodes whose
 * revision changed) and idempotent, so calling it after an edit and again from
 * dai_editor_advance costs nothing. A frontend that writes to the document
 * itself needs this, or its edits are visible in the gizmo and nowhere else. */
DAI_API uint32_t dai_editor_resync(dai_editor *e);

DAI_API uint32_t dai_editor_advance(dai_editor *e, double real_seconds, float *out_alpha);
/* Writes the simulated transforms back into the document as one undo step. */
DAI_API uint32_t dai_editor_apply_sim(dai_editor *e);

/* ---- timeline ---------------------------------------------------------- */

/* Scrubbing rides the engine's snapshot ring: going back is a rollback, going
 * forward is a deterministic replay of the recorded inputs. Both directions
 * land on bit identical state, which is the whole point of the fixed tick. */
DAI_API dai_tick dai_editor_timeline_first(const dai_editor *e);
DAI_API dai_tick dai_editor_timeline_last(const dai_editor *e);
DAI_API dai_tick dai_editor_timeline_tick(const dai_editor *e);
/* Returns 0 when the tick is outside the ring - say so rather than silently
 * landing somewhere else. */
DAI_API int      dai_editor_scrub(dai_editor *e, dai_tick tick);

/* ---- undo (forwarded to the document) ---------------------------------- */

DAI_API int         dai_editor_undo(dai_editor *e);
DAI_API int         dai_editor_redo(dai_editor *e);
DAI_API uint32_t    dai_editor_undo_depth(const dai_editor *e);
DAI_API uint32_t    dai_editor_redo_depth(const dai_editor *e);
DAI_API const char *dai_editor_undo_name(const dai_editor *e);

#ifdef __cplusplus
}
#endif

#endif /* DAI_EDITOR_H */
