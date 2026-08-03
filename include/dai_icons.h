/*
 * The editor's icon set: SVG sources in, one greyscale atlas out.
 *
 * The icons are rasterised at startup at the size the interface actually uses,
 * from vector sources, into a single texture next to the font atlas. That is
 * the whole reason they are SVG and not PNGs: an editor that opens on a 4K
 * display asks for 20 px icons instead of 14 and gets them sharp, from the
 * same 700 bytes of path data.
 *
 * They are stored as COVERAGE, not colour, and tinted when drawn - so one
 * icon serves the dim state, the hover state and the accent colour without
 * three copies of it.
 */
#ifndef DAI_ICONS_H
#define DAI_ICONS_H

#include "daidalos.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dai_icons dai_icons;

/* Names of the built-in set. Plain strings rather than an enum so a game can
 * add its own icons and address them the same way. */
#define DAI_ICON_MOVE       "move"
#define DAI_ICON_ROTATE     "rotate"
#define DAI_ICON_SCALE      "scale"
#define DAI_ICON_PLAY       "play"
#define DAI_ICON_PAUSE      "pause"
#define DAI_ICON_STOP       "stop"
#define DAI_ICON_UNDO       "undo"
#define DAI_ICON_REDO       "redo"
#define DAI_ICON_COPY       "copy"
#define DAI_ICON_TRASH      "trash"
#define DAI_ICON_LAYOUT     "layout"
#define DAI_ICON_CHEVRON_R  "chevron-right"
#define DAI_ICON_CHEVRON_D  "chevron-down"
#define DAI_ICON_BOX        "box"
#define DAI_ICON_EYE        "eye"
#define DAI_ICON_EYE_OFF    "eye-off"
#define DAI_ICON_FOLDER     "folder"
#define DAI_ICON_FILE       "file"
#define DAI_ICON_PLUS       "plus"
#define DAI_ICON_CHECK      "check"
#define DAI_ICON_CLOSE      "close"
#define DAI_ICON_SAVE       "save"
#define DAI_ICON_SEARCH     "search"
#define DAI_ICON_SUN        "sun"
#define DAI_ICON_CAMERA     "camera"
#define DAI_ICON_SPHERE     "sphere"
#define DAI_ICON_CAPSULE    "capsule"
#define DAI_ICON_GRID       "grid"
#define DAI_ICON_SETTINGS   "settings"
#define DAI_ICON_LAYERS     "layers"
/* asset kinds - the project browser picks one per file extension */
#define DAI_ICON_SCRIPT     "script"
#define DAI_ICON_MODEL      "model"
#define DAI_ICON_AUDIO      "audio"
#define DAI_ICON_IMAGE      "image"
#define DAI_ICON_SCENE      "scene"
#define DAI_ICON_CONSOLE    "console"

/* Rasterises the built-in set at `pixel_size` and packs it. */
DAI_API dai_icons *dai_icons_create(float pixel_size);
DAI_API void       dai_icons_free(dai_icons *ic);

/* Adds one icon from SVG source. Rebuilds the atlas, so add before uploading
 * the texture. Returns 1 on success. */
DAI_API int dai_icons_add(dai_icons *ic, const char *name, const char *svg_text);
/* Same, from a file on disk - how a project ships its own icons. */
DAI_API int dai_icons_add_file(dai_icons *ic, const char *name, const char *path);

/* 8 bit coverage, tightly packed. Upload as a texture. */
DAI_API const uint8_t *dai_icons_atlas(const dai_icons *ic, uint32_t *w, uint32_t *h);
/* The same expanded to white RGBA with coverage in alpha. */
DAI_API const uint8_t *dai_icons_atlas_rgba(dai_icons *ic, uint32_t *w, uint32_t *h);

/* Where an icon sits in the atlas, 0..1. Returns 0 for an unknown name, which
 * is how a widget decides to fall back to text. */
DAI_API int dai_icons_uv(const dai_icons *ic, const char *name,
                         float *u0, float *v0, float *u1, float *v1);
DAI_API float    dai_icons_size(const dai_icons *ic);
DAI_API uint32_t dai_icons_count(const dai_icons *ic);
DAI_API const char *dai_icons_name(const dai_icons *ic, uint32_t index);

#ifdef __cplusplus
}
#endif

#endif /* DAI_ICONS_H */
