/* dai_project.h - a project on disk, its settings, and this machine's taste.
 *
 * Three things live here, and the split between them is the entire point:
 *
 *   THE PROJECT      A directory with a known shape. The editor cannot open a
 *                    scene without one, the same way Unity cannot start without
 *                    a project: "where do assets come from", "where do settings
 *                    live", "which scene opens first" all need an answer before
 *                    anything else can happen. A folder is a project when it
 *                    has assets/, scenes/ and settings/ - not because someone
 *                    said so, but because those are the questions.
 *
 *   PROJECT SETTINGS <project>/settings/project.txt. Inside the project, and
 *                    therefore inside version control, and therefore the same
 *                    for everyone: gravity, tick rate, the physics backend,
 *                    tags and layers. Two people who disagree about the tick
 *                    rate are not playing the same game - determinism is a
 *                    property of the project, not of a machine.
 *
 *   PREFERENCES      ~/.config/daidalos/prefs.txt, %APPDATA%\daidalos on
 *                    Windows. OUTSIDE the project, because they describe this
 *                    human at this desk: interface scale, theme, how fast the
 *                    scene camera flies, snapping, autosave. Committing them
 *                    would mean one person's 4K scaling following the whole
 *                    team around, and that is why Unity keeps them in the
 *                    registry rather than in the repository.
 *
 * The layout dai_project_create writes:
 *
 *   <project>/assets/               content: models, textures, scripts
 *   <project>/scenes/main.daidalos  the startup scene
 *   <project>/settings/project.txt  shared settings, versionable, diff friendly
 *   <project>/cache/                derived data, DELETABLE AT ANY TIME
 *   <project>/project.daidalos      the marker: name, engine, when it was made
 *   <project>/.gitignore            because cache/ must never be committed
 *
 * cache/ is Unity's Library/ and carries the same promise: nothing in it is
 * authored, so deleting it costs a rebuild and never costs work.
 * dai_project_open recreates it, which is what turns "close the editor, delete
 * cache/, reopen" into a supported repair instead of a broken project.
 *
 * Nothing in here draws anything, owns a world, or knows what a renderer is.
 * It cannot: the project picker is on screen before any of that exists.
 * src/dai_project.cpp links on its own and build.sh proves it by compiling the
 * test against that one object file.
 *
 * "last_project" in dai_prefs is the small reason the editor feels finished -
 * it reopens what you were working on instead of asking every morning.
 */
#ifndef DAI_PROJECT_H
#define DAI_PROJECT_H

#include "daidalos.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An open project. Opaque: it is a handful of strings plus the settings lines
 * a newer build wrote and this one has to hand back untouched. */
typedef struct dai_project dai_project;

#define DAI_PROJECT_NAME_MAX  64    /* fits dai_project_settings::app_name    */
#define DAI_PROJECT_TAGS      16    /* tag and layer slots, index IS the id   */
#define DAI_PROJECT_TAG_MAX   32
#define DAI_PROJECT_MARKER    "project.daidalos"

/* ---- opening and making projects ---------------------------------------- */

/* Creates <root_dir>/<name> with the layout above and opens it.
 *
 * `name` is validated (see dai_project_name_valid) before a single directory
 * is made: it becomes a path component, and a "project" called "../../etc" is
 * how a file dialog turns into a security hole.
 *
 * Refuses to overwrite: if <root_dir>/<name>/project.daidalos already exists,
 * this fails and says so rather than merging into someone's work. If creation
 * fails halfway - a full disk, a read only parent - the directories it made
 * are removed again with rmdir, which can only delete what is still empty and
 * therefore can never destroy anything that was already there.
 *
 * Returns NULL on failure with the reason in `err`. */
DAI_API dai_project *dai_project_create(const char *root_dir, const char *name,
                                        char *err, size_t err_len);

/* Opens an existing project directory. Recreates cache/ if it is missing,
 * because cache/ is allowed to be missing - that is what makes it a cache.
 * Returns NULL with a reason in `err` when `path` is not a project. */
DAI_API dai_project *dai_project_open(const char *path, char *err, size_t err_len);

/* 1 when `path` is a directory holding assets/, scenes/ and settings/.
 *
 * Note what is NOT required: cache/ (disposable by definition) and
 * project.daidalos (a label, not a licence - a project whose marker was lost
 * to a bad merge should still open). This is the same test the project picker
 * runs over a folder full of candidates, so it stays cheap: three stats. */
DAI_API int dai_project_is_valid(const char *path);

DAI_API void dai_project_close(dai_project *p);

/* Path as opened, no trailing separator. NULL only for a NULL project. */
DAI_API const char *dai_project_path(const dai_project *p);

/* Display name, from project.daidalos; the directory name when the marker has
 * none. Never NULL for a live project. */
DAI_API const char *dai_project_name(const dai_project *p);

/* Absolute path of the startup scene: settings' default_scene resolved against
 * the project, falling back to scenes/main.daidalos. The editor opens this at
 * startup and never has to know the rule. Refreshed by settings_save. */
DAI_API const char *dai_project_scene_path(const dai_project *p);

/* <project>/assets - what the asset browser mounts. */
DAI_API const char *dai_project_asset_dir(const dai_project *p);

/* <project>/cache - what a "clear cache" menu item deletes. */
DAI_API const char *dai_project_cache_dir(const dai_project *p);

/* Lists projects directly inside `root_dir`: one level down, no recursion,
 * because a project inside a project is a mistake and hunting for one would
 * only make it look supported.
 *
 * Writes each full path into `out` at `stride` byte intervals, NUL terminated
 * and truncated to fit, sorted by name so the picker does not reshuffle itself
 * between runs. Returns how many were written; pass out = NULL or max = 0 to
 * count first. */
DAI_API uint32_t dai_project_list(const char *root_dir, char *out,
                                  uint32_t max, uint32_t stride);

/* 1 when `name` is safe to use as a directory name: letters, digits, '-', '_'
 * and inner spaces only, 1..DAI_PROJECT_NAME_MAX-1 bytes, no leading or
 * trailing space, and not a Windows device name (CON, NUL, COM1...). Exposed
 * so a New Project dialog can grey the button out while it is being typed,
 * instead of failing after the user commits. */
DAI_API int dai_project_name_valid(const char *name);

/* ---- project settings: shared, versionable ------------------------------ */

/* The knobs that belong to the project rather than to a person. Defaults match
 * the engine's own (dai_config, dai_body_desc) so a project that never touches
 * this file behaves exactly like one that was never created. */
typedef struct dai_project_settings {
    float gravity[3];
    int   tick_hz;               /* the simulation only advances by 1/tick_hz */
    int   max_bodies;
    int   physics_backend;       /* dai_physics_backend                       */
    float default_friction;
    float default_restitution;
    char  app_name[DAI_PROJECT_NAME_MAX];        /* Unity's "Product Name"    */
    char  default_scene[128];                    /* relative to the project   */
    char  tags[DAI_PROJECT_TAGS][DAI_PROJECT_TAG_MAX];
    char  layers[DAI_PROJECT_TAGS][DAI_PROJECT_TAG_MAX];
} dai_project_settings;

DAI_API dai_project_settings dai_project_settings_default(void);

/* Reads <project>/settings/project.txt.
 *
 * A missing file is NOT an error: it means "nothing was changed yet", so `out`
 * gets the defaults and the result is DAI_OK. A project that will not open
 * because nobody has edited its settings would be absurd.
 *
 * Unknown keys are skipped, not rejected - the opposite of the scene format,
 * on purpose. A scene is authored data where a silently swallowed typo is data
 * loss. This file is passed between people on different builds, and refusing to
 * open a project because a colleague's newer editor wrote one more line would
 * make the strictness the bug. The lines that were not understood are kept on
 * `p` and written back by settings_save, so round tripping a project through an
 * older editor does not erode it. That is why this takes the project and not
 * just a path. */
DAI_API dai_result dai_project_settings_load(dai_project *p, dai_project_settings *out);

/* Writes the file, creating settings/ if it went missing. Only values that
 * differ from the defaults are written, so the file diffs per property and a
 * three way merge of two people changing different settings resolves. Written
 * to a temporary file and moved into place: a crash mid save must not leave the
 * project unopenable. */
DAI_API dai_result dai_project_settings_save(dai_project *p, const dai_project_settings *s);

/* ---- preferences: this machine, this human ------------------------------ */

typedef struct dai_prefs {
    float ui_scale;          /* 1.0 = 96 dpi                                  */
    int   theme;             /* 0 dark, 1 light - the editor names them       */
    float cam_speed;         /* scene view fly speed, metres per second       */
    float gizmo_px;          /* gizmo size in pixels, so it stays grabbable   */
    float snap_translate;    /* metres, 0 = off                               */
    float snap_rotate_deg;   /* degrees, 0 = off                              */
    int   autosave_seconds;  /* 0 = never                                     */
    char  last_project[256]; /* reopened at startup                           */
} dai_prefs;

DAI_API dai_prefs dai_prefs_default(void);

/* A missing file yields the defaults and DAI_OK - the first start of a fresh
 * install is the normal case, not a failure. */
DAI_API dai_result dai_prefs_load(dai_prefs *out);

/* Creates the config directory if needed, then writes atomically. */
DAI_API dai_result dai_prefs_save(const dai_prefs *p);

/* Where that file is, so a failure can name it instead of saying "could not
 * save preferences". Order: $DAI_PREFS_DIR (portable installs and tests, which
 * must not scribble on the developer's real config), then %APPDATA%\daidalos on
 * Windows, then $XDG_CONFIG_HOME/daidalos, then ~/.config/daidalos.
 *
 * Points at a static buffer, rebuilt on every call: not thread safe, same as
 * dai_version(). */
DAI_API const char *dai_prefs_path(void);

#ifdef __cplusplus
}
#endif
#endif /* DAI_PROJECT_H */
