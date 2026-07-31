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

/* ---- layout ------------------------------------------------------------ */

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

/* Sprites: any texture, any sub rectangle of it. This is how an atlas is used
 * for icons - one texture, many uv rects, one draw batch. */
DAI_API void dai_ui_image(dai_ui *ui, dai_texture tex, float w, float h,
                          float u0, float v0, float u1, float v1, uint32_t tint);
/* A sprite that reacts to clicks - icon buttons, inventory slots. */
DAI_API int  dai_ui_image_button(dai_ui *ui, dai_texture tex, float w, float h,
                                 float u0, float v0, float u1, float v1);

/* ---- direct drawing, for HUDs that are not widgets --------------------- */

DAI_API void dai_ui_rect(dai_ui *ui, float x, float y, float w, float h, uint32_t color);
DAI_API void dai_ui_rect_outline(dai_ui *ui, float x, float y, float w, float h, float thickness, uint32_t color);
DAI_API void dai_ui_text(dai_ui *ui, float x, float y, const char *utf8, uint32_t color);
DAI_API float dai_ui_text_width(dai_ui *ui, const char *utf8);

#ifdef __cplusplus
}
#endif

#endif /* DAI_UI_H */
