/*
 * The extension seam - how a feature that the editor host has to drive gets
 * into the editor without four people editing one 5000 line file.
 *
 * Why this exists at all, and why it is a header of INCLUDES rather than a
 * plugin system: `build.sh` and `build_win.sh` are frozen. They name every
 * translation unit they compile, one by one, so a new .cpp under src/ is a file
 * that never reaches an archive and never reaches the editor. The way in is
 * therefore the way the codebase already uses for `dai_gltf_common.hpp` and
 * `dai_doc_internal.hpp`: a header, included by the one translation unit that
 * owns the call site.
 *
 * Two shapes are used, and the difference matters:
 *
 *   FILE SCOPE seam    `#include "dai_blockout_host.inl"` next to the other
 *                      helpers. The file defines `static` functions; the host
 *                      calls them. Use this whenever a signature can be
 *                      written down.
 *
 *   STATEMENT seam     `#include "dai_editor_ui_blockout_inspector.inl"`
 *                      INSIDE a function body. The file is a block of
 *                      statements that runs with the host's locals in scope
 *                      (`p`, `d`, `n`, `r`). It exists for the inspector and
 *                      the Add Component list, where the interesting state is
 *                      six locals deep and passing it out would mean inventing
 *                      a second copy of the panel's own structure.
 *
 * Every seam file names its owning module in its first line. A module edits
 * ITS files and nothing else - that is the whole point, and it is what lets
 * the four modules of this round be built at the same time.
 *
 * The struct below is the context a host hands to a file scope seam. It is
 * everything a feature can need and nothing a feature can break: no window,
 * no input, no panels - a seam that wants to draw draws through the editor UI
 * seams instead.
 */
#ifndef DAI_EXT_H
#define DAI_EXT_H

#include "dai_doc.h"
#include "dai_render.h"
#include "dai_scene.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dai_ext_host {
    dai_doc      *doc;        /* the document being edited                     */
    dai_doc_sync *sync;       /* doc -> live scene, already applied this frame  */
    dai_scene    *scene;      /* the live scene                                 */
    dai_renderer *renderer;   /* where meshes and textures are created          */
    /* The open project's Assets folder, without a trailing slash. Empty when
     * no project is open - a seam that needs files must then do nothing, not
     * guess a path. Every asset reference in the document is relative to it. */
    const char   *assets_dir;
    /* Bumped by the host whenever assets were (re)loaded, so a seam can cache
     * derived data and rebuild only when this moves. */
    uint32_t      asset_revision;
} dai_ext_host;

#ifdef __cplusplus
}
#endif

#endif /* DAI_EXT_H */
