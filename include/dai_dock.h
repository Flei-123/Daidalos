/*
 * Docked panels: a tree of splits with tabbed leaves.
 *
 * This is Unity's model, and it is the right one for a reason that is easy to
 * miss until it goes wrong: **docked panels tile, they never overlap.** The
 * layout is a binary tree - inner nodes split a rectangle horizontally or
 * vertically, leaves hold a list of tabs and show one of them. Every rectangle
 * in the tree is disjoint from every other, so no panel can ever cover another
 * one, and no click can ever be taken by two panels at once.
 *
 * The previous attempt docked windows to the EDGES of a free area and let them
 * float over whatever was left. That model has no answer to "the scene view
 * lies on top of the inspector and eats its clicks", because in that model
 * overlapping is not a bug, it is the default. A tree has no such state.
 *
 *   dai_dock (the tree)
 *    ├─ node  split, axis + ratio, two children
 *    └─ node  leaf, N tab titles + which one is selected
 *   plus zero or more FLOATING roots: a tree of its own with a screen
 *   rectangle, which is what a tab dragged out of the layout becomes.
 *
 * Usage per frame:
 *
 *     dai_dock_begin(dock, ui, x, y, w, h);        // lays out and draws tabs
 *     if (dai_dock_panel(dock, "Inspector", &px, &py, &pw, &ph)) {
 *         ... draw into that rectangle ...
 *         dai_dock_panel_end(dock);
 *     }
 *     dai_dock_end(dock);                          // drag preview, on top
 *
 * A panel that is not the selected tab of its leaf returns 0 and draws
 * nothing. That is the entire visibility rule.
 */
#ifndef DAI_DOCK_H
#define DAI_DOCK_H

#include "daidalos.h"
#include "dai_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dai_dock dai_dock;

/* Where a panel goes when it is first registered. DAI_DOCK_NONE means "the
 * middle" - the space everything else was docked around, which is where a
 * scene view belongs. */
DAI_API dai_dock *dai_dock_create(void);
DAI_API void      dai_dock_destroy(dai_dock *d);

/* Registers a panel. Called every frame is fine - it only does something the
 * first time, so a panel keeps wherever the user dragged it. `edge` is a
 * dai_ui_dock value; `fraction` is its share of the surface (0.22 = 22%). */
DAI_API void dai_dock_add(dai_dock *d, const char *title, int edge, float fraction);
/* Registers a panel as another TAB of an existing one - Scene and Game. */
DAI_API void dai_dock_add_tab(dai_dock *d, const char *title, const char *next_to);

/* Lays the tree out into this rectangle, draws the tab bars and handles all
 * dragging: splitters between neighbours, tabs within a bar, tabs out of the
 * layout into a floating window, and back in again. */
DAI_API void dai_dock_begin(dai_dock *d, dai_ui *ui, float x, float y, float w, float h);
/* The body rectangle of a panel. Returns 0 when the panel is not visible (not
 * the selected tab, or closed) - then do NOT call dai_dock_panel_end. */
DAI_API int  dai_dock_panel(dai_dock *d, const char *title, float *x, float *y,
                            float *w, float *h);
DAI_API void dai_dock_panel_end(dai_dock *d);
DAI_API void dai_dock_end(dai_dock *d);

/* Is this panel the visible one in its leaf? For a host that has to decide
 * whether to render a 3D view at all. */
/* The screen rectangle of the Nth instance of a panel, including its tab
 * bar. 0 when there is no such instance. Drop targets and hover checks ask
 * the dock, not the caller's last frame - the caller's frame is finished. */
DAI_API int  dai_dock_panel_rect(const dai_dock *d, const char *title, int index,
                                 float *x, float *y, float *w, float *h);

DAI_API int  dai_dock_visible(const dai_dock *d, const char *title);
/* Makes it the selected tab (and raises its floating window). */
DAI_API void dai_dock_focus(dai_dock *d, const char *title);
/* Closes / reopens a panel. A closed panel keeps its place for when it comes
 * back, the way Unity's "Close Tab" and the Window menu behave. */
DAI_API void dai_dock_close(dai_dock *d, const char *title);
/* Brings a closed panel back - the Window menu's job. Registering it again
 * with dai_dock_add does the same thing for a panel that is already known. */
/* Pulls a panel out of the layout into a floating window of its own - the
 * "Drop Window" entry of the tab menu, and Unity's gesture of dragging a tab
 * off the bar without having to be accurate about it. The rectangle is a
 * wish: it is clamped into the dock area, and w/h below a sane minimum are
 * replaced by a default. Returns 0 and changes nothing when the panel is not
 * in the tree, when it is already a window on its own, or when it is the only
 * thing left in the layout - a dock with nothing docked in it has no home to
 * drop the window back into. */
DAI_API int  dai_dock_undock(dai_dock *d, const char *title,
                             float x, float y, float w, float h);
/* ---- a panel that is its own window of the OPERATING SYSTEM --------------
 *
 * A floating panel is an overlay drawn inside the editor, and an overlay stops
 * at the editor's edge. That is fine until someone with two monitors wants the
 * Console on the second one - which is the entire reason to tear a panel off
 * in the first place.
 *
 * The trick that makes this cheap: the panel is still drawn into the SAME
 * frame, in a strip UNDER the dock area that the editor's own window never
 * shows, and the OS window blits that strip (dai_window_source_rect). One
 * rendered frame, several windows. No second renderer, no second render pass,
 * and the panel cannot be on screen twice because only one window shows each
 * part of the frame.
 *
 * The host's side of the deal, once per frame:
 *   1. make the offscreen frame dai_dock_spill_height() taller than the window
 *   2. dai_dock_native_windows() -> open/close/move real windows to match
 *   3. point each window at frame_x/frame_y with dai_window_source_rect
 *   4. hand back where the user dragged it with dai_dock_native_moved()
 */
typedef struct dai_dock_native {
    const char *title;      /* the first tab, which names the window          */
    float x, y, w, h;       /* on the DESKTOP, in screen pixels               */
    float frame_x, frame_y; /* where in the offscreen frame it was drawn      */
} dai_dock_native;

/* The host says whether it can open OS windows at all. Until it does, the tab
 * menu does not offer the entry - a menu item that makes a panel disappear
 * because nobody is opening the window it went into is worse than no menu
 * item. dai_dock_set_native still works, so a host can drive it directly. */
DAI_API void  dai_dock_native_supported(dai_dock *d, int yes);

/* Turns a FLOATING panel into an OS window and back. Returns 0 when the panel
 * is not floating - making a docked one a window is dai_dock_undock's job, and
 * one call that means two gestures is a call nobody can predict. */
DAI_API int   dai_dock_set_native(dai_dock *d, const char *title, int on);
DAI_API int   dai_dock_is_native(const dai_dock *d, const char *title);
/* How much taller the offscreen frame must be than the editor's window. 0 when
 * nothing is torn off, which is the normal case and costs nothing. */
DAI_API float dai_dock_spill_height(const dai_dock *d);
/* Every panel that wants a window. Call with out=NULL to ask how many. */
DAI_API uint32_t dai_dock_native_windows(const dai_dock *d, dai_dock_native *out, uint32_t max);
/* Where the user dragged (or resized) it, so the layout remembers. */
DAI_API void  dai_dock_native_moved(dai_dock *d, const char *title,
                                    float x, float y, float w, float h);

DAI_API void dai_dock_open(dai_dock *d, const char *title);
DAI_API int  dai_dock_is_open(const dai_dock *d, const char *title);
/* Every registered panel, for a Window menu. */
DAI_API uint32_t dai_dock_panels(const dai_dock *d, const char **out, uint32_t max);

/* Two tabs that can never be separated into different leaves: dragging one
 * takes the other with it, however it lands. Scene and Game are the case this
 * exists for - the host renders one world per frame, so the two views of it
 * must share a leaf. Survives dai_dock_reset. */
DAI_API void dai_dock_lock_pair(dai_dock *d, const char *a, const char *b);

/* Throws the layout away and re-applies the registration order. */
DAI_API void dai_dock_reset(dai_dock *d);

/* The layout as text, so it survives a restart. Same shape as the rest of this
 * engine's formats: line based, diff friendly, only what differs. */
DAI_API size_t     dai_dock_to_text(const dai_dock *d, char *buf, size_t buf_size);
DAI_API dai_result dai_dock_from_text(dai_dock *d, const char *text);

/* Diagnostics: one line describing the whole tree, for a bug report that is
 * data instead of a photograph of a monitor. */
DAI_API void dai_dock_dump(const dai_dock *d, char *out, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* DAI_DOCK_H */
