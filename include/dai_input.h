/*
 * Input actions: the layer between "the O key is down" and "the player wants
 * to jump".
 *
 * Games that read key codes directly cannot be rebound, cannot support a
 * gamepad without touching gameplay code, and cannot be tested without
 * synthesising key events. An action map fixes all three: gameplay asks for
 * actions, the map decides which physical inputs produce them, and the
 * bindings are data that can be saved, loaded and edited.
 *
 * Sources are deliberately opaque integers. Keyboard scan codes, X11 keysyms,
 * Win32 virtual keys, gamepad buttons - the map does not care, it only needs
 * the host to be consistent. Mouse buttons and gamepad buttons live in their
 * own ranges so they cannot collide with keys.
 */
#ifndef DAI_INPUT_H
#define DAI_INPUT_H

#include "daidalos.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dai_actions dai_actions;
typedef uint32_t dai_action;
typedef uint32_t dai_axis;
#define DAI_INVALID_ACTION ((uint32_t)0xFFFFFFFFu)

/* Source ranges, so a mouse button can never be mistaken for a key. */
#define DAI_SRC_KEY(code)   ((uint32_t)(code) & 0x0000FFFFu)
#define DAI_SRC_MOUSE(btn)  (0x00010000u | (uint32_t)(btn))
#define DAI_SRC_PAD(btn)    (0x00020000u | (uint32_t)(btn))

DAI_API dai_actions *dai_actions_create(void);
DAI_API void         dai_actions_destroy(dai_actions *a);

/* Defining. Names are copied; defining the same name twice returns the same
 * handle, so a config file can be loaded over defaults without duplicating. */
DAI_API dai_action dai_action_define(dai_actions *a, const char *name);
DAI_API dai_axis   dai_axis_define(dai_actions *a, const char *name);
DAI_API dai_action dai_action_find(const dai_actions *a, const char *name);
DAI_API dai_axis   dai_axis_find(const dai_actions *a, const char *name);
DAI_API uint32_t   dai_action_count(const dai_actions *a);
DAI_API uint32_t   dai_axis_count(const dai_actions *a);
DAI_API const char *dai_action_name(const dai_actions *a, dai_action act);

/* Binding. An action can have several sources; binding the same source twice
 * is a no-op. dai_action_clear removes all of them, which is what a rebinding
 * screen does before it records a new key. */
DAI_API dai_result dai_action_bind(dai_actions *a, dai_action act, uint32_t source);
DAI_API dai_result dai_action_clear(dai_actions *a, dai_action act);
/* An axis is two sources pulling in opposite directions (WASD, D-pad), and
 * optionally an analogue source fed with dai_actions_analog. */
DAI_API dai_result dai_axis_bind(dai_actions *a, dai_axis ax, uint32_t negative, uint32_t positive);
DAI_API dai_result dai_axis_bind_analog(dai_actions *a, dai_axis ax, uint32_t source, float scale);

/* ---- per frame --------------------------------------------------------- */

/* Feed raw input, in any order, then call dai_actions_update once. */
DAI_API void dai_actions_source(dai_actions *a, uint32_t source, int down);
DAI_API void dai_actions_analog(dai_actions *a, uint32_t source, float value);
DAI_API void dai_actions_update(dai_actions *a);

DAI_API int   dai_action_held(const dai_actions *a, dai_action act);
DAI_API int   dai_action_pressed(const dai_actions *a, dai_action act);   /* this frame only */
DAI_API int   dai_action_released(const dai_actions *a, dai_action act);
DAI_API float dai_axis_value(const dai_actions *a, dai_axis ax);

/* ---- persistence ------------------------------------------------------- */

/* Writes the bindings as text: one "action name = source, source" per line.
 * Text on purpose - a player can edit it, a diff shows what changed, and it
 * does not break when the engine adds a field. */
DAI_API dai_result dai_actions_save(const dai_actions *a, const char *path);
DAI_API dai_result dai_actions_load(dai_actions *a, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* DAI_INPUT_H */
