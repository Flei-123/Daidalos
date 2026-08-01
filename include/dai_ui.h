/*
 * Immediate mode UI.
 *
 * There is no widget tree, no retained state, no callbacks. Every frame the
 * host describes the interface it wants and gets back the interactions that
 * happened. A button is an `if`:
 *
 *     if (dai_ui_button(ui, "Start")) start_game();
 *
 * Why immediate mode for an engine like this: the simulation is already a pure
 * function of state and input, and a retained UI tree would be a second,
 * separate source of truth that has to be kept in sync with it - exactly the
 * class of bug that makes menus show stale values after a rollback. Here the
 * UI is rebuilt from the state every frame, so it cannot disagree with it.
 *
 * The UI produces vertices, not draw calls: dai_ui_draws() hands back batches
 * of triangles with a texture id each, which the renderer draws in one pass.
 * That keeps the UI backend agnostic - the same code feeds a Vulkan, a WebGPU
 * or a software renderer.
 */
#ifndef DAI_UI_H
#define DAI_UI_H

#include "dai_render.h"
#include "dai_font.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dai_ui dai_ui;

typedef struct dai_ui_vertex {
    float    x, y;       /* pixels, origin top left */
    float    u, v;
    uint32_t color;      /* 0xAABBGGRR */
} dai_ui_vertex;

/* One batch of triangles sharing a texture. texture 0 = the font atlas. */
typedef struct dai_ui_draw {
    const dai_ui_vertex *vertices;
    uint32_t             count;
    dai_texture          texture;
    float                clip[4];   /* x0, y0, x1, y1 in pixels */
} dai_ui_draw;

/* Input the host feeds in once per frame. */
typedef struct dai_ui_input {
    float    mouse_x, mouse_y;
    int      mouse_down;      /* left button held                       */
    float    wheel;
    uint32_t text[8];         /* code points typed this frame, 0 terminated */
    int      key_backspace, key_enter, key_tab;
} dai_ui_input;

typedef struct dai_ui_style {
    uint32_t panel, panel_border, text, text_dim;
    uint32_t button, button_hover, button_active;
    uint32_t accent, track;
    float    padding, spacing, rounding, border;
    /* windows and the editor chrome behind them */
    uint32_t titlebar, titlebar_focused, chrome, shadow;
    float    label_w;      /* width of the label column in field widgets   */
    float    row_pad;      /* extra height a widget adds over the text     */
} dai_ui_style;

DAI_API dai_ui_style dai_ui_style_default(void);

DAI_API dai_ui *dai_ui_create(dai_font *font, dai_texture font_texture);
DAI_API void    dai_ui_destroy(dai_ui *ui);
DAI_API dai_ui_style *dai_ui_style_of(dai_ui *ui);

/* Frame lifecycle. Everything between begin and end describes this frame. */
DAI_API void dai_ui_begin(dai_ui *ui, float width, float height, const dai_ui_input *in);
DAI_API void dai_ui_end(dai_ui *ui);
DAI_API uint32_t dai_ui_draws(dai_ui *ui, const dai_ui_draw **out);
/* True when the pointer is over UI, so the game can ignore that click. */
DAI_API int dai_ui_wants_mouse(const dai_ui *ui);
/* The pointer state this frame, for code that draws its own interactive
 * widgets (the editor timeline) instead of using the ones above. */
DAI_API void dai_ui_mouse(const dai_ui *ui, float *x, float *y, int *down, int *pressed);

/* ---- layout ------------------------------------------------------------ */

/* ---- windows ------------------------------------------------------------
 *
 * A panel the user can move, resize, collapse and raise - the arrangement
 * every 3D editor has. The state lives in the CALLER's struct, not in a
 * registry inside the UI, for the same reason the rest of this file is
 * immediate mode: a layout is something the host wants to save, reset and
 * compute defaults for, and a hidden table it cannot reach makes all three
 * awkward.
 *
 *   static dai_ui_window inspector = dai_ui_window_make(1200, 60, 300, 600);
 *   if (dai_ui_window_begin(ui, "Inspector", &inspector)) {
 *       ...widgets...
 *   }
 *   dai_ui_window_end(ui);          // always, even when begin returned 0
 *
 * Windows are drawn in their own layers, so a window raised by a click ends up
 * in front no matter what order the host calls them in, and a click that lands
 * on an overlapping window only reaches the topmost one.
 */
/* Where a window is parked. A docked window keeps its own width (or height, on
 * the top and bottom edges) and takes the rest of the edge from the dock area,
 * so dragging the split is just resizing the window. */
typedef enum dai_ui_dock {
    DAI_DOCK_NONE = 0,
    DAI_DOCK_LEFT,
    DAI_DOCK_RIGHT,
    DAI_DOCK_TOP,
    DAI_DOCK_BOTTOM
} dai_ui_dock;

typedef struct dai_ui_window {
    float x, y, w, h;
    int   collapsed;     /* only the title bar is drawn                    */
    int   open;          /* 0 = not drawn at all; begin() returns 0        */
    float min_w, min_h;
    int   dock;          /* dai_ui_dock                                    */
    /* Which part of that edge: 0 = all of it, 1 = first half, 2 = second
     * half. Two windows can share an edge - hierarchy over project, which is
     * the layout every 3D editor ships with. */
    int   dock_slot;
} dai_ui_window;

DAI_API dai_ui_window dai_ui_window_make(float x, float y, float w, float h);
/* Docks a window without dragging it there - for a default layout. */
DAI_API dai_ui_window dai_ui_window_docked(int dock, int slot, float size);
/* The rectangle docked windows divide up: the surface minus the host's own
 * bars. Set it once per frame before the windows. Defaults to the whole
 * surface. */
DAI_API void dai_ui_dock_area(dai_ui *ui, float x, float y, float w, float h);
/* Returns 1 when the body is visible and widgets should be emitted. Call
 * dai_ui_window_end() either way. */
DAI_API int  dai_ui_window_begin(dai_ui *ui, const char *title, dai_ui_window *win);
DAI_API void dai_ui_window_end(dai_ui *ui);
/* Which window is in front, by title. "" when the pointer is over none. */
DAI_API const char *dai_ui_window_front(const dai_ui *ui);
/* The largest rectangle no window covers - where a 3D viewport belongs.
 * Approximated by starting from the full surface and cutting away the docked
 * edges, which is what an editor layout actually looks like. */
DAI_API void dai_ui_free_area(const dai_ui *ui, float *x, float *y, float *w, float *h);

/* Opens a panel at x,y; widgets after it stack downwards inside it. */
DAI_API void dai_ui_panel_begin(dai_ui *ui, float x, float y, float w, float h, const char *title);
DAI_API void dai_ui_panel_end(dai_ui *ui);
DAI_API void dai_ui_row(dai_ui *ui, float height);      /* next widgets go side by side */
DAI_API void dai_ui_spacing(dai_ui *ui, float pixels);

/* ---- widgets ----------------------------------------------------------- */

DAI_API void dai_ui_label(dai_ui *ui, const char *utf8);
DAI_API void dai_ui_label_fmt(dai_ui *ui, const char *fmt, ...);
DAI_API int  dai_ui_button(dai_ui *ui, const char *utf8);
DAI_API int  dai_ui_checkbox(dai_ui *ui, const char *utf8, int *value);
DAI_API int  dai_ui_slider(dai_ui *ui, const char *utf8, float *value, float min, float max);
DAI_API void dai_ui_progress(dai_ui *ui, float fraction, const char *utf8);
DAI_API void dai_ui_separator(dai_ui *ui);

/* A collapsible section header - the bar a Unity style inspector groups its
 * components under. `open` is the caller's fold state. Pass `enabled` for a
 * checkbox on the right (NULL for none); it is the component's on/off switch.
 * Returns 1 when the header was clicked, 2 when the checkbox was. */
DAI_API int dai_ui_header(dai_ui *ui, const char *title, int *open, int *enabled);

/* Sprites: any texture, any sub rectangle of it. This is how an atlas is used
 * for icons - one texture, many uv rects, one draw batch. */
DAI_API void dai_ui_image(dai_ui *ui, dai_texture tex, float w, float h,
                          float u0, float v0, float u1, float v1, uint32_t tint);
/* A sprite that reacts to clicks - icon buttons, inventory slots. */
DAI_API int  dai_ui_image_button(dai_ui *ui, dai_texture tex, float w, float h,
                                 float u0, float v0, float u1, float v1);

/* Cycles through `items` on click - a real dropdown needs an overlay layer and
 * this is an editor field with four options, not a font picker. Returns 1 when
 * the value changed. */
DAI_API int  dai_ui_option(dai_ui *ui, const char *label, int *value,
                           const char *const *items, int count);

/* Drag to change, the way every 3D editor does it: no keyboard, no modal state,
 * and it works on a laptop trackpad. `step` is world units per pixel. */
DAI_API int  dai_ui_drag_float(dai_ui *ui, const char *label, float *value, float step);
/* Three of the above on one line, labelled X/Y/Z. */
DAI_API int  dai_ui_drag_vec3(dai_ui *ui, const char *label, float *xyz, float step);
/* Editable text. Returns 1 on every change. Uses dai_ui_input::text and
 * key_backspace, which the host fills from its window backend. */
DAI_API int  dai_ui_input_text(dai_ui *ui, const char *label, char *buf, size_t buf_size);

/* One row of a hierarchy. `depth` indents, `open` is the caller's fold state
 * (pass NULL for a leaf). Returns 1 when the row itself was clicked. */
DAI_API int  dai_ui_tree_item(dai_ui *ui, const char *label, int depth,
                              int has_children, int *open, int selected);

/* A clipped, scrollable region inside a panel. Everything drawn between the
 * two calls is cut to the region and moves with the wheel. */
DAI_API void dai_ui_scroll_begin(dai_ui *ui, const char *id, float height);
DAI_API void dai_ui_scroll_end(dai_ui *ui);

/* ---- direct drawing, for HUDs that are not widgets --------------------- */

DAI_API void dai_ui_rect(dai_ui *ui, float x, float y, float w, float h, uint32_t color);
DAI_API void dai_ui_rect_outline(dai_ui *ui, float x, float y, float w, float h, float thickness, uint32_t color);
/* Any angle, given thickness. The gizmo overlay is built from these. */
DAI_API void dai_ui_line(dai_ui *ui, float x0, float y0, float x1, float y1,
                         float thickness, uint32_t color);
DAI_API void dai_ui_text(dai_ui *ui, float x, float y, const char *utf8, uint32_t color);
DAI_API float dai_ui_text_width(dai_ui *ui, const char *utf8);

#ifdef __cplusplus
}
#endif

#endif /* DAI_UI_H */
