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
typedef struct dai_script_node_host {
    double   (*find)(const char *name, void *user);
    int      (*get_pos)(double id, double *xyz, void *user);
    void     (*set_pos)(double id, const double *xyz, void *user);
    int      (*get_rot)(double id, double *xyzw, void *user);
    void     (*set_rot)(double id, const double *xyzw, void *user);
    void    *user;
} dai_script_node_host;
DAI_API void dai_script_bind_nodes(dai_script *s, const dai_script_node_host *host);
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
