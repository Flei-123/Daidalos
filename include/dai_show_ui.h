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

/* ---- the panels ----------------------------------------------------------
 * Each fills the rectangle the dock handed out. Called directly by a host that
 * lays out its own windows; dai_show_ui_panels does the ordinary thing of
 * registering all four in an existing dock and drawing whichever are visible. */
DAI_API void dai_show_ui_storyboard(dai_show_ui *u, dai_ui *ui, float x, float y, float w, float h);
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

DAI_API void dai_show_ui_panels(dai_show_ui *u, dai_ui *ui, struct dai_dock *dock);

#ifdef __cplusplus
}
#endif
#endif /* DAI_SHOW_UI_H */
