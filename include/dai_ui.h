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
#include "dai_icons.h"

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
    int      right_down;      /* right button held - the context menu     */
    float    wheel;
    uint32_t text[8];         /* code points typed this frame, 0 terminated.
                                 IMPORTANT: dai_ui does NOT track held keys -
                                 feed a code point only when it was TYPED
                                 (edge triggered), or every frame of a held
                                 key repeats the letter. */
    int      key_backspace, key_enter, key_tab;
    /* The rest of what a real text field needs. All EDGE triggered (the frame
     * the key went down, OS repeat included) except the two modifiers, which
     * are "held" - a text field that only knows backspace is a toy: you cannot
     * fix a typo in the middle of a number without retyping it. */
    int      key_left, key_right, key_home, key_end, key_delete, key_escape;
    int      key_up_arrow, key_down_arrow;   /* dropdown / list navigation */
    int      key_shift, key_ctrl;    /* held */
    int      key_select_all;         /* Ctrl+A, edge triggered */
    /* Ctrl+C / X / V, edge triggered. Flags rather than characters because a
     * control code is not a character: the window backend filters those out
     * of the text stream before anything can read them, and it is right to. */
    int      key_copy, key_cut, key_paste;
    int      double_click;           /* the press this frame was a double click */
} dai_ui_input;

/* What the pointer should look like where it is. The UI knows which widget is
 * under it - the host owns the window and is the only one who can set a system
 * cursor - so the UI reports and the host applies:
 *
 *     dai_window_cursor(win, dai_ui_cursor(ui));
 *
 * A resize edge that does not change the pointer is an edge you have to
 * discover by trial. */
typedef enum dai_ui_cursor {
    DAI_CURSOR_ARROW = 0,
    DAI_CURSOR_TEXT,        /* I-beam: over a text or numeric field  */
    DAI_CURSOR_SIZE_WE,     /* vertical split / horizontal drag      */
    DAI_CURSOR_SIZE_NS,
    DAI_CURSOR_SIZE_NWSE,   /* bottom right / top left corner        */
    DAI_CURSOR_SIZE_NESW,
    DAI_CURSOR_HAND
} dai_ui_cursor_kind;

DAI_API int dai_ui_cursor(const dai_ui *ui);
/* Widgets that draw themselves (the editor's gizmo overlay, collider handles)
 * ask for a cursor too. Last writer of the frame wins, which is right: the
 * frontmost thing under the pointer is drawn last. */
DAI_API void dai_ui_cursor_set(dai_ui *ui, int cursor);

/* ---- draw layers --------------------------------------------------------
 *
 * Everything is drawn into one list and sorted by layer at the end of the
 * frame. That is what lets a dropdown opened inside a panel be drawn OVER the
 * window next to it: in an immediate mode UI the order things are submitted
 * in is the order they are laid out in, and that is almost never the order
 * they should be painted in.
 *
 * Windows occupy DAI_LAYER_WINDOW + their position in the z order, so there
 * is room for a few hundred of them before they reach the next layer. */
enum {
    DAI_LAYER_BACKGROUND = 0,
    DAI_LAYER_WINDOW     = 1,
    DAI_LAYER_DOCK_PREVIEW = 1 << 12,
    DAI_LAYER_POPUP      = 1 << 13,
    DAI_LAYER_DRAG       = 1 << 14,
    DAI_LAYER_TOOLTIP    = 1 << 15
};
DAI_API void dai_ui_layer_push(dai_ui *ui, int layer);
DAI_API void dai_ui_layer_pop(dai_ui *ui);

/* ---- hit testing --------------------------------------------------------
 *
 * "Is the pointer over THIS thing, and is this thing the frontmost thing under
 * the pointer?" Every widget asks that; a host drawing its own chrome (a dock
 * tab bar, a viewport) has to ask it too, or two overlapping things both react
 * to the same click.
 *
 * The frontmost root is decided at the END of a frame and used by the NEXT
 * one - one frame of lag, which is invisible and is what every immediate mode
 * UI does, because who is on top is not known until everyone has been laid
 * out. */
DAI_API void dai_ui_root_begin(dai_ui *ui, const char *id, float x, float y, float w, float h);
DAI_API void dai_ui_root_end(dai_ui *ui);
/* 1 when this root is the frontmost one under the pointer. */
DAI_API int  dai_ui_root_hovered(const dai_ui *ui, const char *id);

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
/* Swaps the font (and its texture) without rebuilding the UI - what changing
 * the UI size in Settings does. The widget geometry follows automatically:
 * everything is measured from the font. */
DAI_API void    dai_ui_font_set(dai_ui *ui, dai_font *font, dai_texture font_texture);
DAI_API void    dai_ui_destroy(dai_ui *ui);
DAI_API dai_ui_style *dai_ui_style_of(dai_ui *ui);

/* Frame lifecycle. Everything between begin and end describes this frame. */
DAI_API void dai_ui_begin(dai_ui *ui, float width, float height, const dai_ui_input *in);
/* Display scale. The UI keeps laying itself out in LOGICAL pixels - the host
 * feeds it a logical size and a logical pointer - and the vertex list that
 * comes out is multiplied by this on the way to the renderer. 1.25 on a 125%
 * desktop; the font and the icons must be rasterised at that scale too, or
 * the text is simply magnified. */
DAI_API void  dai_ui_scale_set(dai_ui *ui, float scale);
DAI_API float dai_ui_scale_get(const dai_ui *ui);
DAI_API void dai_ui_end(dai_ui *ui);
DAI_API uint32_t dai_ui_draws(dai_ui *ui, const dai_ui_draw **out);
/* True when the pointer is over UI, so the game can ignore that click. */
DAI_API int dai_ui_wants_mouse(const dai_ui *ui);
/* "That click was mine." For code that draws its own interactive chrome (the
 * Scene/Game tabs, collider handles) instead of using the widgets above -
 * without it the same click also lands in the 3D view behind it and clears the
 * selection. */
DAI_API void dai_ui_claim_mouse(dai_ui *ui);
/* The pointer state this frame, for code that draws its own interactive
 * widgets (the editor timeline) instead of using the ones above. */
DAI_API void dai_ui_mouse(const dai_ui *ui, float *x, float *y, int *down, int *pressed);
/* The wheel delta fed this frame. The scroll region handles its own; a list
 * drawn by hand (a file browser column) reads it here. */
DAI_API float dai_ui_wheel(const dai_ui *ui);
/* Was this frame's press a double click? Read by list rows that open what was
 * clicked twice - a single click selects, a double one opens, and telling
 * them apart from the host is a timer nobody needs. */
DAI_API int  dai_ui_double_click(const dai_ui *ui);
/* The right button, held this frame and pressed this frame. The context menu
 * is the only consumer of the right button in the whole UI, so these are
 * top level rather than buried in the input struct. */
DAI_API int  dai_ui_right_down(const dai_ui *ui);
DAI_API int  dai_ui_right_pressed(const dai_ui *ui);
/* Whether any numeric field is being typed into right now. An editor that
 * batches document edits into undo steps needs it: a drag ends when the
 * button comes up, but typing ends when the field LOSES FOCUS - and one undo
 * step per keystroke is not undo, it is a replay. */
DAI_API int  dai_ui_num_editing(const dai_ui *ui);

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
    /* A viewport window draws its title bar and border but NO body: whatever
     * is behind it (the 3D view) shows through. The host reads the body rect
     * back and treats exactly that as the viewport. This is what makes the
     * scene view a real window - dockable, resizable, tear-off - instead of
     * "the hole the panels leave". */
    int   viewport;
    /* Set by the window system when the ⋮ button was pressed; the host opens
     * its own menu and clears it. A window menu belongs to the host - only it
     * knows what "close" or "dock left" mean for this particular panel. */
    int   menu_wanted;
    /* Set by the host, not by the window system: a viewport window that has
     * been dragged off its fill position floats; one that has not covers the
     * free area. */
    int   floated;
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
/* Draw layer of a window (its position in the z order + 1), for chrome the
 * host draws on TOP of a window after window_end - the tabs in a viewport
 * window's title bar, for instance. */
DAI_API int  dai_ui_window_layer(const dai_ui *ui, const char *title);
DAI_API void dai_ui_layer_set(dai_ui *ui, int layer);
/* The largest rectangle no window covers - where a 3D viewport belongs.
 * Approximated by starting from the full surface and cutting away the docked
 * edges, which is what an editor layout actually looks like. */
DAI_API void dai_ui_free_area(const dai_ui *ui, float *x, float *y, float *w, float *h);

/* Opens a panel at x,y; widgets after it stack downwards inside it. */
DAI_API void dai_ui_panel_begin(dai_ui *ui, float x, float y, float w, float h, const char *title);
DAI_API void dai_ui_panel_end(dai_ui *ui);
DAI_API void dai_ui_row(dai_ui *ui, float height);      /* next widgets go side by side */
/* Back to stacking downwards. A row that never ends is why a panel with two
 * buttons in it used to draw everything after them on the same line, off the
 * right hand edge - starting a new row ends the previous one too. */
DAI_API void dai_ui_row_end(dai_ui *ui);
/* Width of the panel or window the layout is currently inside, in pixels. A
 * panel that wants to size its own label column needs it - the label column is
 * a FRACTION of the panel in every real inspector, not a fixed number that is
 * right at one width and wrong at every other. */
DAI_API float dai_ui_panel_width(const dai_ui *ui);
DAI_API void dai_ui_spacing(dai_ui *ui, float pixels);
/* Where the next widget WOULD be drawn, without moving the cursor. Chrome
 * that draws one composite row by hand (the inspector's object header: check,
 * icon, name field on one line) has to place its pieces, and only the layout
 * knows where the cursor is. */
DAI_API void dai_ui_cursor_pos(const dai_ui *ui, float *x, float *y);
/* The rectangle the widget that was just drawn occupies. What drag & drop
 * needs to mark a row as the drop target: the caller knows it IS the target,
 * the layout knows where it is. Valid until the next widget. */
DAI_API void dai_ui_last_rect(const dai_ui *ui, float *x, float *y,
                              float *w, float *h);
/* Advances the layout as if a widget of this size had been drawn. */
DAI_API void dai_ui_advance(dai_ui *ui, float w, float h);
/* Clip everything drawn until dai_ui_clip_end to a rectangle. The editor's
 * wireframes (gizmo, colliders, camera frustums) are UI lines drawn over the
 * 3D view - without this they spill over the panels when the object sits
 * behind one. */
DAI_API void dai_ui_clip_begin(dai_ui *ui, float x, float y, float w, float h);
DAI_API void dai_ui_clip_end(dai_ui *ui);

/* ---- widgets ----------------------------------------------------------- */

DAI_API void dai_ui_label(dai_ui *ui, const char *utf8);
/* Switch translation of widget text on/off. OFF by default: an engine header
 * has no business rewriting the strings a game feeds it. The EDITOR turns it
 * on, and then every label/button/option it draws goes through the table in
 * dai_tr - the key being the English text, which is also the fallback. */
DAI_API void dai_ui_translate(dai_ui *ui, int on);
DAI_API void dai_ui_label_fmt(dai_ui *ui, const char *fmt, ...);
DAI_API int  dai_ui_button(dai_ui *ui, const char *utf8);
/* The same button, only as wide as its own label. A column of full width
 * buttons reads as a stack of banners; "Edit" is a word, not a banner. */
DAI_API int  dai_ui_button_fit(dai_ui *ui, const char *utf8);
DAI_API int  dai_ui_checkbox(dai_ui *ui, const char *utf8, int *value);
/* A full width button that can stay ON - "Edit Collider", "Wireframe". The
 * icon button next to a label did not read as a button at all, and a checkbox
 * is the wrong shape for "you are now in a mode". */
DAI_API int  dai_ui_toggle_button(dai_ui *ui, const char *utf8, int active);
DAI_API int  dai_ui_slider(dai_ui *ui, const char *utf8, float *value, float min, float max);
DAI_API void dai_ui_progress(dai_ui *ui, float fraction, const char *utf8);
DAI_API void dai_ui_separator(dai_ui *ui);

/* A fold INSIDE a component: small triangle, dim label, no header bar. Unity
 * spells "Constraints" and "Info" this way, and for the reason it matters -
 * a full header makes a sub-section read as another component. */
DAI_API int  dai_ui_subheader(dai_ui *ui, const char *title, int *open);

/* A multi-line text field, `rows` lines tall. Enter puts in a line break
 * instead of committing. Returns 1 on any frame the buffer changed. */
DAI_API int  dai_ui_input_multiline(dai_ui *ui, const char *label, char *buf,
                                    size_t buf_size, int rows);
/* One track, N cells, one of them lit: the control every settings page has
 * and the reason none of them writes [Preferences] to mark the current one.
 * Returns 1 the frame the choice changes. */
DAI_API int  dai_ui_segmented(dai_ui *ui, const char *const *labels, int count, int *value);
/* A heading with a rule running off to the right. */
DAI_API void dai_ui_section(dai_ui *ui, const char *title);
/* Drop the keyboard focus. The scene view calls this when a camera gesture
 * starts: a text field that still owns W A S D is a camera that only turns. */
DAI_API void dai_ui_text_defocus(dai_ui *ui);

/* A collapsible section header - the bar a Unity style inspector groups its
 * components under. `open` is the caller's fold state. Pass `enabled` for a
 * checkbox on the right (NULL for none); it is the component's on/off switch.
 * Returns 1 when the header was clicked, 2 when the checkbox was. */
DAI_API int dai_ui_header(dai_ui *ui, const char *title, int *open, int *enabled);
/* The same with the component's icon in front of its name. `icon` is a name
 * from the icon set; an unknown one simply draws nothing, so a host that never
 * called dai_ui_set_icons still gets a working header. Returns 1 on header
 * click, 2 on checkbox click, 3 on right click (context menu). */
/* The same header with the icon in the component's own colour. Unity tells
 * its components apart by hue before anything is read; one accent-blue glyph
 * per row is a list you have to spell out to yourself. */
DAI_API int dai_ui_header_icon_col(dai_ui *ui, const char *icon, uint32_t tint,
                                   const char *title, int *open, int *enabled);
DAI_API int dai_ui_header_icon(dai_ui *ui, const char *icon, const char *title,
                               int *open, int *enabled);

/* ---- icons --------------------------------------------------------------
 *
 * Vector icons, rasterised once at the size the interface uses and packed into
 * one atlas (see dai_icons.h). They are coverage only and are TINTED here, so
 * the same icon serves the dim, hover and accent states.
 */
DAI_API void dai_ui_set_icons(dai_ui *ui, dai_icons *icons, dai_texture tex);
DAI_API int  dai_ui_has_icon(const dai_ui *ui, const char *name);
/* Absolute placement - for toolbars and gizmo overlays that do their own
 * layout. `size` <= 0 uses the atlas's native size, which is the crisp one. */
DAI_API void dai_ui_icon_at(dai_ui *ui, const char *name, float x, float y,
                            float size, uint32_t color);
/* Laid out like any other widget. */
DAI_API void dai_ui_icon(dai_ui *ui, const char *name, float size, uint32_t color);
/* A square button with an icon instead of a label. `tooltip` is what the icon
 * means, shown on hover - an icon only toolbar without one is a memory test.
 * `active` draws it in the accent colour, for a mode that is currently on. */
DAI_API int dai_ui_icon_button(dai_ui *ui, const char *name, const char *tooltip, int active);
/* A gap between groups of toolbar buttons. */
DAI_API void dai_ui_toolbar_gap(dai_ui *ui, float w);

/* ---- the popup menu ------------------------------------------------------
 *
 * A floating list of actions, opened with the right button. The state lives in
 * the caller (dai_ui_popup), like the window rectangles, so a menu can survive
 * the frames between opening it and clicking an entry.
 *
 *   static dai_ui_popup menu = { 0 };
 *   if (right_pressed) dai_ui_popup_open(&menu, mx, my);
 *   int pick = dai_ui_popup_menu(ui, &menu, ITEMS, COUNT);
 *
 * Returns -2 while it is open and nothing happened yet, -1 the frame it
 * closed (anywhere else clicked), or the item's index. */
typedef struct dai_ui_popup {
    int      placed;     /* 0 until the first draw has put it on screen */
    float x, y;
    int   open;
} dai_ui_popup;

typedef struct dai_ui_menu_item {
    const char *icon;        /* dai_icons name, or NULL  */
    const char *label;
    const char *shortcut;    /* shown right aligned, or NULL */
    /* A caption row: drawn dim and small, never highlighted, never returned as
     * a pick, skipped by the arrow keys. Zero for a normal item, so every
     * existing three-element initialiser keeps meaning exactly what it did.
     *
     * It exists because a searchlist that groups its rows has to draw the
     * group name SOMEWHERE, and drawing it as an ordinary row makes a
     * category look like a component you can add - which is a thing you then
     * click, and nothing happens, and you conclude the editor is broken. */
    int         header;
} dai_ui_menu_item;

DAI_API void dai_ui_popup_open(dai_ui_popup *m, float x, float y);
/* An icon button at explicit coordinates (toolbars that are not laid out by
 * panels). Returns 1 on click. */
DAI_API int  dai_ui_icon_button_at(dai_ui *ui, const char *name, float x, float y,
                                   float w, float h, int active);
/* A popup that hosts arbitrary widgets instead of menu items: begin draws the
 * panel and sets up layer/clip, end restores. Widgets in between behave like
 * panel widgets but float above everything. */
DAI_API void dai_ui_popup_panel_begin(dai_ui *ui, dai_ui_popup *m, float w, float min_h);
DAI_API void dai_ui_popup_panel_end(dai_ui *ui);
DAI_API void dai_ui_popup_close(dai_ui_popup *m);
DAI_API int  dai_ui_popup_menu(dai_ui *ui, dai_ui_popup *m,
                               const dai_ui_menu_item *items, uint32_t count);

/* A field whose value is typed, not dragged. Parses on Enter or when the
 * pointer leaves. `min`/`max` with min < max clamps the typed value. Returns 1
 * the frame a value was committed - which is the frame an editor should save
 * and resync, not before. */
DAI_API int dai_ui_num_field(dai_ui *ui, const char *label, float *value,
                             float step, float min, float max, const char *id);
/* Three of them in one row, with the axis letter in front of each. Hovering
 * the LETTER and holding the left button drags the value - that is how Unity's
 * inspector works, and typing still works by clicking the number. */
DAI_API int dai_ui_num_vec3(dai_ui *ui, const char *label, float *xyz, float step);

/* Sprites: any texture, any sub rectangle of it. This is how an atlas is used
 * for icons - one texture, many uv rects, one draw batch. */
DAI_API void dai_ui_image(dai_ui *ui, dai_texture tex, float w, float h,
                          float u0, float v0, float u1, float v1, uint32_t tint);
/* At an explicit position, for anything that is not a row in a panel. */
DAI_API void dai_ui_image_at(dai_ui *ui, dai_texture tex, float x, float y, float w, float h,
                             float u0, float v0, float u1, float v1, uint32_t tint);
/* A sprite that reacts to clicks - icon buttons, inventory slots. */
DAI_API int  dai_ui_image_button(dai_ui *ui, dai_texture tex, float w, float h,
                                 float u0, float v0, float u1, float v1);

/* Cycles through `items` on click - a real dropdown needs an overlay layer and
 * this is an editor field with four options, not a font picker. Returns 1 when
 * the value changed. */
/* A REAL dropdown: clicking opens a list over everything else, the list stays
 * open until an entry is clicked, Escape or a click outside cancels, and the
 * arrow keys move a highlight that only commits on Enter.
 *
 * The old one was a button that added one to the index per click. That is not
 * a dropdown, it is a counter: picking the third of five values took three
 * clicks, and there was no way to see what the other values even were. */
/* An asset reference field: what is assigned now, plus the button that opens
 * the list of what could be. Returns 1 on click - the CALLER opens the picker,
 * because only it knows what kind of thing goes in there.
 *
 * Two arrows that cycle a value are not a picker: they hide the list, and
 * choosing the fifth of six is five clicks and a memory test. */
/* Unity's object field. Returns 0 for nothing, 1 when the plate was clicked
 * (show what it points at) and 2 for the target button on the right, which is
 * where a picker belongs. While the pointer is over it the field's label is
 * reported by dai_ui_hot_label - that is how a dragged hierarchy node knows
 * which field it would land in. */
DAI_API int  dai_ui_object_field(dai_ui *ui, const char *label, const char *value,
                                 const char *icon);

DAI_API int  dai_ui_option(dai_ui *ui, const char *label, int *value,
                           const char *const *items, int count);
/* Same, laid out by the caller. */
DAI_API int  dai_ui_option_at(dai_ui *ui, const char *id, float x, float y, float w, float h,
                              int *value, const char *const *items, int count);
/* Is any popup (dropdown, context menu, window menu) open? The host needs it
 * for the same reason it needs dai_ui_text_active: while a menu is up, the
 * keyboard and the right mouse button belong to the menu. */
DAI_API int  dai_ui_popup_active(const dai_ui *ui);

/* Drag to change, the way every 3D editor does it: no keyboard, no modal state,
 * and it works on a laptop trackpad. `step` is world units per pixel. */
DAI_API int  dai_ui_drag_float(dai_ui *ui, const char *label, float *value, float step);
/* Three of the above on one line, labelled X/Y/Z. */
DAI_API int  dai_ui_drag_vec3(dai_ui *ui, const char *label, float *xyz, float step);
/* Editable text. Returns 1 on every change. Uses dai_ui_input::text and
 * key_backspace, which the host fills from its window backend. */
DAI_API int  dai_ui_input_text(dai_ui *ui, const char *label, char *buf, size_t buf_size);
/* A real text field, laid out by the caller.
 *
 * "Real" is not a mood: one click selects the whole value (so typing replaces
 * it, which is what an inspector field is for), a second click puts the caret
 * where you clicked, dragging selects a range, double click selects all again,
 * and Left/Right/Home/End/Shift+arrows/Ctrl+A/Delete all do what they do in
 * every other text box on the machine. Enter and Tab commit, Escape cancels.
 *
 * Returns 1 the frame the buffer changed. `commit` (optional) is set on the
 * frame the user finished - Enter, Tab, or clicking away. */
DAI_API int  dai_ui_text_field(dai_ui *ui, const char *id, float x, float y, float w, float h,
                               char *buf, size_t buf_size, int *commit);
/* Is any field being typed into? A host that also reads the keyboard for
 * shortcuts needs it, or Delete both removes the selected object and a
 * character from the name being typed. */
/* Arms the NEXT text field drawn to take focus immediately, text selected -
 * the "create file, rename it right there" flow. One shot. */
DAI_API void dai_ui_text_focus_next(dai_ui *ui);
/* The label of the widget under the pointer this frame, nullptr = none.
 * Lets host code turn any button into a drop target. */
DAI_API const char *dai_ui_hot_label(const dai_ui *ui);
DAI_API int  dai_ui_text_active(const dai_ui *ui);
/* "Is the keyboard somebody's?" - a text field OR the code editor. What a host
 * asks before treating a key as a shortcut. dai_ui_text_active is the narrower
 * question (a FIELD is being edited) and stays that way, because widgets use
 * it to decide whether THEY lost focus - and a code editor two panels away is
 * not a reason for a rename to give up. */
DAI_API int  dai_ui_typing(const dai_ui *ui);
/* Only the code editor, not text fields. Ctrl+S asks this: it means the
 * SCRIPT when the caret is in a script, and the scene in every other case. */
DAI_API int  dai_ui_code_focused(const dai_ui *ui);

/* The clipboard, as a hand-off rather than an ownership.
 *   _feed  - the host pushes the OS clipboard in, once a frame. Copied.
 *   _taken - what a widget cut or copied this frame, or NULL. Read once; the
 *            host puts it on the OS clipboard and the UI forgets it. */
DAI_API void        dai_ui_clipboard_feed(dai_ui *ui, const char *utf8);
DAI_API const char *dai_ui_clipboard_taken(dai_ui *ui);

/* ---- the code editor ----------------------------------------------------
 *
 * A multi-line text widget that edits the caller's buffer in place. It is not
 * a text field with newlines: a code editor has to keep a caret across lines,
 * a selection that spans them, a viewport that follows the caret, and colour
 * that comes from what the text MEANS - and a field that does none of those
 * is the reason people alt-tab to a real editor and never come back.
 *
 * The state is the caller's so several editors can be open at once (one per
 * file, which is what a tab bar is).
 *
 *   static dai_ui_code_state st = {0};
 *   dai_ui_code_edit(ui, "script", x, y, w, h, buf, sizeof(buf), &st,
 *                    DAI_CODE_LANG_JS);
 *
 * Returns 1 on any frame the buffer changed. */
typedef struct dai_ui_code_state {
    int   caret;        /* byte offset of the caret                          */
    int   anchor;       /* the other end of the selection; == caret when none */
    float scroll_x, scroll_y;
    int   focused;
    int   want_focus;   /* set to 1 to take the keyboard on the next draw    */
    float blink;
    float prefer_x;     /* remembered column, so up/down do not drift left   */
    int   dragging;
    int   last_caret;   /* what the caret was last frame - see the widget    */
    int   have_last;
    int   follow_caret; /* one shot: bring the caret into view on this draw  */
    /* Autocomplete. `ac_open` is the list being shown, `ac_sel` the row the
     * arrows are on, `ac_start` where the word being completed begins. Kept
     * in the state rather than the widget so two open scripts each keep their
     * own popup - and so closing a tab closes its list with it. */
    int   ac_open;
    int   ac_sel;
    int   ac_start;
    /* 1 = a plain multi-line field, not code: no gutter, no line numbers and
     * no completion. Set by dai_ui_input_multiline; leave it 0 for scripts. */
    int   plain;
    /* Escape was pressed: no list until the word being typed changes. */
    int   ac_off;
    int   ac_off_len;
} dai_ui_code_state;

enum {
    DAI_CODE_LANG_NONE = 0,
    DAI_CODE_LANG_JS   = 1,
    DAI_CODE_LANG_CPP  = 2
};

DAI_API int dai_ui_code_edit(dai_ui *ui, const char *id, float x, float y,
                             float w, float h, char *buf, size_t buf_size,
                             dai_ui_code_state *st, int lang);
/* Which line the caret is on (1 based) and its column, for a status bar. */
DAI_API void dai_ui_code_caret_pos(const char *buf, int caret, int *line, int *col);

/* One row of a hierarchy. `depth` indents, `open` is the caller's fold state
 * (pass NULL for a leaf). Returns 1 when the row itself was clicked. */
DAI_API int  dai_ui_tree_item(dai_ui *ui, const char *label, int depth,
                              int has_children, int *open, int selected);
/* Same row, more mouse: 1 on a left click, 2 on a right click - the bit
 * positions mean "both" never has to be a special value. dai_ui_tree_item is
 * the left click half of this. */
DAI_API int  dai_ui_tree_item_ex(dai_ui *ui, const char *label, int depth,
                                 int has_children, int *open, int selected);
/* The same row with the object's KIND drawn in front of its name. A hierarchy
 * that has to spell "MainCamera" into the name to say "this is a camera" has
 * given up its only readable column; a glyph says it in 16 pixels and leaves
 * the name to be the name. */
DAI_API int  dai_ui_tree_item_icon(dai_ui *ui, const char *icon, const char *label,
                                   int depth, int has_children, int *open, int selected);
/* Colours the label of the NEXT tree row, then forgets. What a hierarchy needs
 * to say "this one is a prefab instance" the way Unity does - in blue, on the
 * name, without a second column. Pass 0 to go back to the normal colour. */
DAI_API void dai_ui_tree_label_color(dai_ui *ui, uint32_t rgba);
/* The row as a text field, for renaming. 0 while editing, 1 to commit (Enter
 * or a click elsewhere). A rename that needs a special key to keep is a
 * rename you will lose. */
DAI_API int  dai_ui_tree_rename(dai_ui *ui, char *buf, size_t buf_size, int depth,
                                int has_children, int *open);

/* A clipped, scrollable region inside a panel. Everything drawn between the
 * two calls is cut to the region and moves with the wheel. */
/* ---- arrays -------------------------------------------------------------
 *
 * Unity's array, which is a shape and not a widget: a foldout carrying the
 * element count on the right, one indented row per element with a drag
 * handle, and the +/- pair under the list. Written once here because the
 * inspector has more than one array in it and drawing that layout by hand
 * twice is how two lists start looking different.
 *
 *     if (dai_ui_array_begin(ui, "Materials", &n, &open, 1, 8)) {
 *         for (int i = 0; i < n; ++i)
 *             if (dai_ui_array_object_row(ui, i, names[i], DAI_ICON_MATERIAL))
 *                 open_picker(i);
 *         n += dai_ui_array_end(ui, &n, 1, 8);
 *     }
 */
DAI_API int  dai_ui_array_begin(dai_ui *ui, const char *label, int *count,
                                int *open, int min_n, int max_n);
/* One element: handle, "Element n", and an object field. 1 when clicked. */
DAI_API int  dai_ui_array_object_row(dai_ui *ui, int index, const char *value,
                                     const char *icon);
/* The +/- pair. Returns +1, -1 or 0; the caller owns the list. */
DAI_API int  dai_ui_array_end(dai_ui *ui, int count, int min_n, int max_n);

/* ---- arrays -------------------------------------------------------------
 *
 * Unity's array, which is a shape and not a widget: a foldout carrying the
 * element count on the right, one indented row per element with a drag
 * handle, and the +/- pair under the list. Written once here because the
 * inspector has more than one array in it and drawing that layout by hand
 * twice is how two lists start looking different.
 *
 *     if (dai_ui_array_begin(ui, "Materials", &n, &open, 1, 8)) {
 *         for (int i = 0; i < n; ++i)
 *             if (dai_ui_array_object_row(ui, i, names[i], DAI_ICON_MATERIAL))
 *                 open_picker(i);
 *         n += dai_ui_array_end(ui, &n, 1, 8);
 *     }
 */
DAI_API int  dai_ui_array_begin(dai_ui *ui, const char *label, int *count,
                                int *open, int min_n, int max_n);
/* One element: handle, "Element n", and an object field. 1 when clicked. */
DAI_API int  dai_ui_array_object_row(dai_ui *ui, int index, const char *value,
                                     const char *icon);
/* The +/- pair. Returns +1, -1 or 0; the caller owns the list. */
DAI_API int  dai_ui_array_end(dai_ui *ui, int count, int min_n, int max_n);

DAI_API void dai_ui_scroll_begin(dai_ui *ui, const char *id, float height);
DAI_API void dai_ui_scroll_end(dai_ui *ui);
/* Bring a rectangle inside the current scroll region into view. What "Focus
 * selection" needs: an object selected in the viewport is no use if its row
 * in the hierarchy is forty rows below the fold. */
DAI_API void dai_ui_scroll_reveal(dai_ui *ui, float y, float h);

/* ---- the searchable list -------------------------------------------------
 *
 * Unity's "Add Component" menu: a search field at the top, what you can pick
 * below it, typing filters by name. A flat list of seven buttons is fine for
 * seven things and unusable for forty - and scripts are things, so the list
 * grows whether the UI is ready for it or not.
 *
 * State lives in the caller like every other popup, because typing has to
 * survive the frames between opening the list and picking a row:
 *
 *   static dai_ui_searchlist sl = { 0 };
 *   if (open_now) { dai_ui_searchlist_open(&sl, x, y); sl.wants_focus = 1; }
 *   int pick = dai_ui_searchlist(ui, &sl, items, count);   // >= 0 = chosen
 *
 * The list stays open while the result is DAI_SEARCHLIST_OPEN (-2) and closes
 * with DAI_SEARCHLIST_CLOSED (-1): a click outside, Escape, or a pick. */
enum {
    DAI_SEARCHLIST_CLOSED = -1,
    DAI_SEARCHLIST_OPEN   = -2
};

typedef struct dai_ui_searchlist {
    float x, y;            /* where the popup opens                          */
    int   open;
    int   wants_focus;     /* set when opening: the search field takes keys  */
    int   highlight;       /* the row Enter would pick                       */
    float scroll;
    char  query[64];
    float w, h;            /* read by the host: where the panel ended up     */
    char  hint[40];        /* placeholder in the search box; empty = generic */
} dai_ui_searchlist;

DAI_API void dai_ui_searchlist_open(dai_ui_searchlist *s, float x, float y);
DAI_API int  dai_ui_searchlist_draw(dai_ui *ui, dai_ui_searchlist *s,
                                    const dai_ui_menu_item *items, uint32_t count);

/* ---- direct drawing, for HUDs that are not widgets --------------------- */

DAI_API void dai_ui_rect(dai_ui *ui, float x, float y, float w, float h, uint32_t color);
/* Filled rectangle with rounded corners (style.rounding is the usual radius).
 * Drawn as a centre cross plus four corner fans; falls back to dai_ui_rect
 * when the radius is too small to see. */
DAI_API void dai_ui_rrect(dai_ui *ui, float x, float y, float w, float h, float radius, uint32_t color);
/* Same, with a corner mask: bit 0 = top left, 1 = top right, 2 = bottom left,
 * 3 = bottom right. Tabs want 0x3 (top only), panels want 0xF. */
DAI_API void dai_ui_rrect_mask(dai_ui *ui, float x, float y, float w, float h,
                               float radius, uint32_t color, int corners);
DAI_API void dai_ui_rect_outline(dai_ui *ui, float x, float y, float w, float h, float thickness, uint32_t color);
/* Any angle, given thickness. The gizmo overlay is built from these. */
DAI_API void dai_ui_line(dai_ui *ui, float x0, float y0, float x1, float y1,
                         float thickness, uint32_t color);
DAI_API void dai_ui_text(dai_ui *ui, float x, float y, const char *utf8, uint32_t color);
/* The same, magnified. For game HUD text, whose size the author picks and the
 * atlas cannot know. See the note at the definition for what it costs. */
DAI_API void dai_ui_text_scaled(dai_ui *ui, float x, float y, const char *utf8,
                                uint32_t color, float scale);
DAI_API float dai_ui_text_width(dai_ui *ui, const char *utf8);
/* Line height of the current font. Host chrome that lays itself out (the dock
 * tab bar, the console button row) needs it to centre a label - a hardcoded
 * offset is right at one font size and clips descenders at every other. */
DAI_API float dai_ui_text_height(dai_ui *ui);
/* Attaches an explanation to the field drawn immediately before this call; it
 * is shown while the pointer rests on that row. Units belong here: "Friction"
 * with no tooltip is a number whose meaning you have to already know. */
/* A colour, as a colour. `rgb` is three floats 0..1, edited in place; returns
 * 1 on any frame it changed.
 *
 * It exists because "Color X 0.82 Y 0.24 Z 0.2" is not a colour, it is three
 * numbers wearing a colour's name - you can read all three and still not know
 * what it looks like. The row shows a SWATCH; clicking it opens the wheel,
 * the value bar, and RGB/HSV/Hex, which is the arrangement Blender, Krita and
 * every paint program settled on independently.
 *
 * `id` has to be unique per field, like every other stateful widget here. */
DAI_API int  dai_ui_color(dai_ui *ui, const char *label, float *rgb, const char *id);

DAI_API void dai_ui_help(dai_ui *ui, const char *text);
/* A tooltip for a hand drawn widget, which has no row to hang off. */
DAI_API void dai_ui_tooltip_at(dai_ui *ui, float x, float y, float w, float h,
                               const char *text);

#ifdef __cplusplus
}
#endif

#endif /* DAI_UI_H */
