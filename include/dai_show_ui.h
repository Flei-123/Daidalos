/*
 * dai_show_ui.h - the drone show panels, in the dock that already exists.
 *
 * The pipeline in dai_show.h is arithmetic on points and time and knows
 * nothing about an interface; this header is the other half. The split is the
 * same one dai_editor / dai_editor_ui makes, and for the same reason: a show
 * has to be solvable from a test or a batch job on a machine with no display,
 * so nothing that computes may depend on anything that draws.
 *
 * Immediate mode, like the rest of this editor. Every panel is rebuilt from
 * the document each frame, which here is not a style preference: a formation
 * deleted in the storyboard must not leave a row in the validation list, and a
 * conflict list that outlives the plan it came from is the one bug in this
 * program that could hurt somebody.
 *
 * WHAT THE PANELS ARE
 *
 *   Storyboard   the figures in order, with hold and transition times, and the
 *                button that turns the selected mesh into a formation.
 *   Parameters   fleet size, minimum distance, v_max, a_max, sampling mode,
 *                assignment method - and Solve, which is the only button that
 *                spends real time.
 *   Validation   what is wrong, sorted, clickable: a click seeks the timeline
 *                to the moment and selects the drones involved.
 *   Viewport     the fleet as coloured points at the current instant, with the
 *                timeline under it. Conflicting drones are drawn RED with a
 *                line between them, because "somewhere in the third minute two
 *                of ten thousand drones are close" is not a usable answer.
 *
 * The viewport preview draws through dai_ui rather than through the renderer.
 * A show at this stage is points, lines and text - the same things every other
 * panel is made of - and routing them through the 2D canvas keeps the renderer,
 * the scene and the instance path untouched by a second project type. No new
 * pipeline, no new shader, no branch in dai_render.
 */
#ifndef DAI_SHOW_UI_H
#define DAI_SHOW_UI_H

#include "dai_show.h"
#include "dai_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

struct dai_dock;

typedef struct dai_show_ui dai_show_ui;

/* The panels do not own the document - the editor does, because it is what
 * loads and saves it. A panel set outlives no show it was pointed at. */
DAI_API dai_show_ui *dai_show_ui_create(dai_show *sh);
DAI_API void         dai_show_ui_destroy(dai_show_ui *u);
DAI_API dai_show    *dai_show_ui_doc(const dai_show_ui *u);

/* ---- playback ------------------------------------------------------------
 * The clock lives HERE, in the interface, and is handed in from outside as a
 * delta. The solver never sees it: what the viewport shows is a function of a
 * time value, and that value is a number in a struct like any other. */
DAI_API void  dai_show_ui_advance(dai_show_ui *u, float dt);
/* Solve whenever the document has changed and nothing is being dragged. The
 * plan is a cache that every edit throws away, so "is there a plan" is the
 * dirty flag; the panel keeps a snapshot so a solve that FAILED is not retried
 * against an unchanged document sixty times a second. */
DAI_API int   dai_show_ui_auto_solve(const dai_show_ui *u);
DAI_API void  dai_show_ui_set_auto_solve(dai_show_ui *u, int on);
DAI_API float dai_show_ui_time(const dai_show_ui *u);
DAI_API void  dai_show_ui_seek(dai_show_ui *u, float t);
DAI_API int   dai_show_ui_playing(const dai_show_ui *u);
DAI_API void  dai_show_ui_play(dai_show_ui *u, int on);

/* ---- what the host feeds in ----------------------------------------------
 * The mesh a formation is sampled from arrives as the same triangle soup the
 * pipeline takes, so this file needs no asset layer either. `source` is what
 * the storyboard shows as the figure's origin. Passing a NULL desc means "no
 * mesh is selected", and the storyboard says so rather than offering a button
 * that cannot work. */
DAI_API void dai_show_ui_mesh(dai_show_ui *u, const dai_show_sample_desc *desc,
                              const char *source);

/* Which drone the user last clicked (through the validation list or the
 * viewport), and the conflict that put it there. UINT32_MAX = nothing. */
DAI_API uint32_t dai_show_ui_selected_drone(const dai_show_ui *u);
DAI_API int      dai_show_ui_selected_conflict(const dai_show_ui *u);

/* ---- the figure being edited, and the tools that edit it -----------------
 *
 * The storyboard, the figure list and the inspector all talk about ONE
 * selected figure, and the viewport draws its gizmo. A host that wants a
 * keyboard shortcut or a menu item for any of that needs to be able to say so
 * from outside - and a test that wants to grab a handle the way a user does
 * needs to know where the handle was drawn.
 *
 * dai_show_ui_gizmo_handle reports where the three axis handles ENDED UP on
 * screen in the last frame the viewport drew, in window pixels, or 0 when that
 * axis was not on screen. It is a read of what was drawn, never a second
 * computation of it: a test that projected the pivot itself would be checking
 * its own arithmetic rather than the gizmo the user grabs. */
DAI_API void     dai_show_ui_select_formation(dai_show_ui *u, uint32_t i);
DAI_API uint32_t dai_show_ui_selected_formation(const dai_show_ui *u);
DAI_API int      dai_show_ui_gizmo_handle(const dai_show_ui *u, int axis,
                                          float *sx, float *sy);
/* The same reading for the handle on ONE selected drone's point. A drone is a
 * selection of its own: picking one lets the figure go, so only one of these
 * two ever answers at a time. */
DAI_API int      dai_show_ui_point_handle(const dai_show_ui *u, int axis,
                                          float *sx, float *sy);
DAI_API int      dai_show_ui_selected_is_drone(const dai_show_ui *u);

/* The point brush. `pick_on` makes a click in the preview select a point of the
 * selected figure; `paint_on` makes a drag colour every point under the brush.
 * Colour is set on the POINT, never on the drone number - which drone stands
 * there is the assignment's answer and it changes every time the show is
 * solved. */
DAI_API void     dai_show_ui_pick_mode(dai_show_ui *u, int pick_on, int paint_on);
DAI_API void     dai_show_ui_brush(dai_show_ui *u, float radius_px,
                                   uint8_t r, uint8_t g, uint8_t b);
DAI_API uint32_t dai_show_ui_picked_point(const dai_show_ui *u);

/* ---- the export row ------------------------------------------------------
 * Writing the file is the host's job - it is the host that knows the project's
 * folder. The panel knows WHEN, and what the last click asked for: 0 nothing,
 * 1 Skybrush .skyc, 2 DSX .dsx, 3 CSV, 4 the JSON round trip. The host calls
 * dai_show_ui_take_export once a frame, gets the pending format (the flag
 * clears itself), writes the file, and hands the outcome back through
 * dai_show_ui_note so the status line can say it. A panel that wrote the file
 * itself would need to know where the project lives, and a show document does
 * not. */
DAI_API int  dai_show_ui_take_export(dai_show_ui *u);
DAI_API void dai_show_ui_note(dai_show_ui *u, int bad, const char *text);

/* ---- the panels ----------------------------------------------------------
 * Each fills the rectangle the dock handed out. Called directly by a host that
 * lays out its own windows; dai_show_ui_panels does the ordinary thing of
 * registering all four in an existing dock and drawing whichever are visible. */
DAI_API void dai_show_ui_parameters(dai_show_ui *u, dai_ui *ui, float x, float y, float w, float h);
DAI_API void dai_show_ui_validation(dai_show_ui *u, dai_ui *ui, float x, float y, float w, float h);
/* The two panels the game layout already has, given the thing a show puts in
 * them: the figures in order - click one and the preview goes to where it
 * stands - with the number of the drone everything else is about; and the
 * detail view of that one drone at the instant on the timeline (position, LED
 * colour, keyframe count, nearest neighbour), under the conflict the
 * validation list currently has open.
 *
 * They exist because the alternative was two empty rectangles in the default
 * layout. A show has no scene graph, but it does have a hierarchy - the
 * storyboard is one - and it does have exactly one thing worth inspecting, and
 * a panel that says nothing is a panel a director stops opening. */
DAI_API void dai_show_ui_figures(dai_show_ui *u, dai_ui *ui, float x, float y, float w, float h);
DAI_API void dai_show_ui_inspector(dai_show_ui *u, dai_ui *ui, float x, float y, float w, float h);
/* The preview and the timeline under it, in one rectangle - they scrub each
 * other, so splitting them across two panels would only make it possible to
 * close half of a control. */
DAI_API void dai_show_ui_viewport(dai_show_ui *u, dai_ui *ui, float x, float y, float w, float h);
/* Drone count, tightest gap, fastest drone, and what the last solve cost. The
 * numbers a show director reads before signing anything. */
DAI_API void dai_show_ui_status(dai_show_ui *u, dai_ui *ui, float x, float y, float w, float h);
/* The verdict the status line puts at its right hand end, as text, without
 * drawing anything: "no conflicts", "1 conflict", "7 conflicts", "2 formation
 * faults". It exists so a test can read the sentence the operator reads -
 * counting words is the only way to catch "1 conflicts", which is not a
 * cosmetic defect but the line every debrief quotes. Returns the length
 * written; `buf` is always terminated. */
DAI_API uint32_t dai_show_ui_verdict(const dai_show_ui *u, char *buf, size_t cap);


/* Scene-view navigation for the preview, the convention the game editor's
 * viewport and Unity share: HOLD the right button to look around, W A S D to
 * move, Q E down and up, shift to hurry; the middle button pans; F frames the
 * whole show again. It is a struct the HOST fills because dai_ui never sees a
 * held key - its input carries typed characters, not walking - and an orbit
 * you can only turn with the left drag is a camera you inspect, not one you
 * stand inside. The left-drag orbit and the wheel zoom stay in the panel
 * itself; this call is what adds flying. */
typedef struct dai_show_nav_input {
    float mouse_x, mouse_y;           /* logical pixels, dai_ui's space */
    int   mouse_right, mouse_middle;  /* held */
    int   key_w, key_a, key_s, key_d, key_q, key_e;
    int   key_shift;
    int   key_focus;                  /* F, held - the call edge-triggers it */
    int   key_alt;                    /* Alt held: right drag dollies instead
                                       * of looking, the scene view's zoom */
    float dt;                         /* seconds since the last call */
    int   can_fly;                    /* no text field owns the keyboard; when the
                                       * pointer is also over the preview and no
                                       * text field owns the keyboard: W A S D
                                       * then walk WITHOUT the right button
                                       * being held. A right button that never
                                       * arrives (a touchpad, a shell that ate
                                       * the click) must not be the only way
                                       * to move through a show. */
} dai_show_nav_input;
DAI_API void dai_show_ui_nav(dai_show_ui *u, const dai_show_nav_input *in);

/* Where the show's output goes. There is no Validation panel any more: a
 * conflict, a solve time, a figure that would not sample - all of it is the
 * program talking back, and the program has one place for that, the Console.
 * The host hands over a sink and gets every line the show would have printed
 * into a panel. level 0 is news, 1 is a fault. */
typedef void (*dai_show_ui_log_fn)(void *user, int level, const char *text);
DAI_API void dai_show_ui_log_sink(dai_show_ui *u, dai_show_ui_log_fn fn, void *user);

/* ---- modifiers -----------------------------------------------------------
 * dai_ui reports the pointer, never a held key, so the host feeds the three
 * that change what a drag MEANS. Alt+drag orbits the camera; a plain left drag
 * belongs to the selection, exactly as in the game editor's scene view. */
DAI_API void dai_show_ui_modifiers(dai_show_ui *u, int alt, int ctrl, int shift);

/* ---- undo ----------------------------------------------------------------
 * The panels own the stack because they are what knows where a gesture BEGAN:
 * a gizmo drag is one step however many frames it took. dai_show_snapshot does
 * the copying. `depth` is how many steps can still be taken back, so a host
 * can grey out a menu item honestly. */
DAI_API int  dai_show_ui_undo(dai_show_ui *u);
DAI_API int  dai_show_ui_redo(dai_show_ui *u);
DAI_API uint32_t dai_show_ui_undo_depth(const dai_show_ui *u);
DAI_API uint32_t dai_show_ui_redo_depth(const dai_show_ui *u);
/* One undo step, taken before the caller changes something. Used by the host
 * for edits it makes itself. */
DAI_API void dai_show_ui_begin_edit(dai_show_ui *u, const char *what);

/* Delete: the selected figure, or - when a drone's point is selected and the
 * figure is not - nothing, because a fleet drone is not a thing you can
 * remove. Returns 1 when something went. */
DAI_API int dai_show_ui_delete_selected(dai_show_ui *u);

/* ---- the file the panel cannot open --------------------------------------
 * A panel has no window handle, so it cannot raise the system's file dialog.
 * It says it WANTS one: the host calls take_browse once a frame, gets 1 when
 * the user pressed Browse, opens the dialog itself and hands the path back.
 * The same split dai_show_ui_take_export already uses. */
DAI_API int  dai_show_ui_take_browse(dai_show_ui *u);
DAI_API void dai_show_ui_set_image_path(dai_show_ui *u, const char *path);

DAI_API void dai_show_ui_panels(dai_show_ui *u, dai_ui *ui, struct dai_dock *dock);


#ifdef __cplusplus
}
#endif
#endif /* DAI_SHOW_UI_H */
