/*
 * JavaScript scripting, for the parts of a game that should not need a
 * compiler: UI layout, menu logic, tuning, tools.
 *
 * The engine embeds QuickJS (the only vendored dependency besides Jolt and
 * Aulos). TypeScript is not a separate feature - TS compiles to JS, and
 * build/web/daidalos.d.ts already describes the API, so `tsc` gives you type
 * checked UI scripts that this runtime executes.
 *
 * The point is HOT RELOAD: dai_script_reload re-runs a file without restarting
 * the game. Nothing in the simulation is scriptable on purpose - gameplay that
 * must survive a rollback belongs in the tick callback, in C, where it is
 * deterministic. Scripts drive presentation.
 */
#ifndef DAI_SCRIPT_H
#define DAI_SCRIPT_H

#include "daidalos.h"
#include "dai_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dai_script dai_script;

DAI_API dai_script *dai_script_create(char *err, size_t err_len);
DAI_API void        dai_script_destroy(dai_script *s);

/* Makes a UI available to scripts as the global `ui` object. */
DAI_API void dai_script_bind_ui(dai_script *s, dai_ui *ui);

/* The scene a script may touch, as the globals `scene` and `node`:
 *   scene.find("Crate")          -> node id as a number (or -1)
 *   node.getPos(id)              -> [x, y, z]
 *   node.setPos(id, x, y, z)     / node.getRot(id) -> [x,y,z,w] / node.setRot(id, x,y,z,w)
 * Ids travel as doubles - JS has one number type and a dai_node fits easily. */
/* `set_text` writes a node's Text component - the whole point of having one,
 * since a score that cannot change is a decoration. */
typedef struct dai_script_node_host {
    double   (*find)(const char *name, void *user);
    int      (*get_pos)(double id, double *xyz, void *user);
    void     (*set_pos)(double id, const double *xyz, void *user);
    int      (*get_rot)(double id, double *xyzw, void *user);
    void     (*set_rot)(double id, const double *xyzw, void *user);
    /* The node's Text component - the whole point of having one, since a
     * score that cannot change is a decoration. */
    void     (*set_text)(double id, const char *str, void *user);
    void    *user;
} dai_script_node_host;
DAI_API void dai_script_bind_nodes(dai_script *s, const dai_script_node_host *host);

/* The animation player, as the global `anim` object. Same shape as the node
 * binding above: a struct of function pointers the host fills in, so this file
 * stays free of dai_anim.h and a game that has no animation simply does not
 * call bind.
 *
 *   anim.play("Run", 0.2)        fade in over 0.2 s; already playing = no-op
 *   anim.crossfade("Idle", 0.3)  the same call under the name you are thinking
 *   anim.restart("Punch", 0.05)  from the top, even if it is already current
 *   anim.stop(0.2)
 *   anim.speed = 1.5             a property, not a setter
 *   anim.time / anim.normalizedTime / anim.parameter
 *   anim.isPlaying("Run")        -> bool
 *   anim.current()               -> "Run"
 *   anim.weight("Idle")          -> 0..1
 *   anim.finished()              -> bool, for one-shot clips
 *   anim.onEvent("footstep", fn) fn(event, clip, time); one name may have many
 *   anim.onEvent(fn)             every event, whatever its name
 *
 * The host raises events with dai_script_anim_event - the C player calls back
 * on the C side, the host forwards, and QuickJS never sits inside the player's
 * update loop. */
typedef struct dai_script_anim_host {
    int    (*play)(const char *clip, double fade, void *user);
    int    (*restart)(const char *clip, double fade, double start, void *user);
    void   (*stop)(double fade, void *user);
    int    (*is_playing)(const char *clip, void *user);
    const char *(*current)(void *user);
    double (*weight)(const char *clip, void *user);
    int    (*finished)(void *user);
    double (*get_speed)(void *user);
    void   (*set_speed)(double v, void *user);
    double (*get_time)(void *user);
    void   (*set_time)(double v, void *user);
    double (*get_normalized)(void *user);
    double (*get_parameter)(void *user);
    void   (*set_parameter)(double v, void *user);
    void  *user;
} dai_script_anim_host;
DAI_API void dai_script_bind_anim(dai_script *s, const dai_script_anim_host *host);

/* Dispatches one animation event to whatever anim.onEvent registered. Safe to
 * call when nothing is bound and safe when a handler throws: the error is
 * counted like any other, and the frame carries on. */
DAI_API void dai_script_anim_event(dai_script *s, const char *event, const char *clip, double time);
/* What a behaviour needs that a UI script does not: the keyboard, and a body
 * to push. Bound separately from the node host so a game that runs scripts
 * for its menus never has to answer these.
 *
 *   input.key("w") / ("space") / ("left") / ("shift")   -> true while held
 *   body.getVel(id)              -> [x, y, z]
 *   body.setVel(id, x, y, z)
 *   body.impulse(id, x, y, z)
 *   body.grounded(id)            -> true when something is under it
 *
 * Key names are lower case, one per physical key: the letters and digits by
 * their character, and "space", "shift", "ctrl", "alt", "enter", "escape",
 * "tab", "left", "right", "up", "down". A name the host does not know is
 * false, never an error - a typo in a script must not stop the game.
 *
 * The frame length arrives as `state.dt`, which the host sets each frame with
 * dai_script_set_number; there is nothing to bind for that. */
typedef struct dai_script_play_host {
    int    (*key)(const char *name, void *user);
    int    (*get_vel)(double id, double *xyz, void *user);
    void   (*set_vel)(double id, const double *xyz, void *user);
    void   (*impulse)(double id, const double *xyz, void *user);
    int    (*grounded)(double id, void *user);
    /* How far the pointer moved THIS frame, in pixels, and which buttons are
     * down as a mask: 1 left, 2 right, 4 middle. A delta rather than a
     * position because that is what a look control wants, and because a
     * position would make every script do the same subtraction. */
    void   (*mouse)(double *dx, double *dy, int *buttons, void *user);
    void  *user;
} dai_script_play_host;
DAI_API void dai_script_bind_play(dai_script *s, const dai_script_play_host *host);

/* Immediate mode UI for the GAME, as the global `gui`.
 *
 *   function frame() {
 *       gui.text(20, 20, "Score: " + score, 28, 0xFFFFFFFF);
 *       if (gui.button(20, 60, 160, 34, "Restart")) restart();
 *   }
 *
 * Coordinates are pixels from the top left of the view. This is the same
 * model dai_ui uses and the same one the editor is written in: a frame says
 * what should be on the screen, and nothing has to be deleted afterwards. A
 * retained tree would need creation, destruction and an owner for every
 * label - and the first bug it produces is a menu from the last game still
 * hanging there after a restart.
 *
 * The Text and Image COMPONENTS are the other half: they are for what is
 * always there, they are placed with a mouse, and they survive without a
 * script running. Use those for a HUD, this for anything conditional. */
typedef struct dai_script_gui_host {
    void (*text)(double x, double y, const char *utf8, double size, double rgba, void *user);
    void (*rect)(double x, double y, double w, double h, double rgba, void *user);
    void (*image)(double x, double y, double w, double h, const char *path, double rgba, void *user);
    /* Returns 1 on the frame the button is released over itself. */
    int  (*button)(double x, double y, double w, double h, const char *label, void *user);
    /* The view's size in pixels, so a script can lay out against the middle
     * or the right edge without being told how big the window is. */
    void (*size)(double *w, double *h, void *user);
    void *user;
} dai_script_gui_host;
DAI_API void dai_script_bind_gui(dai_script *s, const dai_script_gui_host *host);

/* Any host value scripts can read through `state.<name>`. */
DAI_API void dai_script_set_number(dai_script *s, const char *name, double value);
DAI_API void dai_script_set_string(dai_script *s, const char *name, const char *value);
DAI_API double dai_script_get_number(dai_script *s, const char *name, double fallback);

DAI_API dai_result dai_script_eval(dai_script *s, const char *code, const char *name,
                                   char *err, size_t err_len);
DAI_API dai_result dai_script_load(dai_script *s, const char *path, char *err, size_t err_len);
/* Re-runs the last loaded file. Returns DAI_OK even if the file is broken -
 * check `err`: a UI script with a typo must not take the game down. */
DAI_API dai_result dai_script_reload(dai_script *s, char *err, size_t err_len);
/* Calls a global function taking no arguments, e.g. the per frame draw(). */
DAI_API dai_result dai_script_call(dai_script *s, const char *fn, char *err, size_t err_len);

/* How many times a script error has been swallowed - a HUD can show it. */
DAI_API uint32_t dai_script_error_count(const dai_script *s);

#ifdef __cplusplus
}
#endif

#endif /* DAI_SCRIPT_H */
