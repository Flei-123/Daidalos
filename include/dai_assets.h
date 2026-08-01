/*
 * Assets: a path in the scene file becomes a mesh on screen.
 *
 * The scene document deliberately knows nothing about the renderer - a node
 * stores `asset models/crate.glb` and a callback turns that into render
 * handles. This is that callback, wired to two things that already existed
 * separately and did not talk to each other:
 *
 *   Mnemosyne   where the bytes come from (folders, pack files, mod
 *               priority), loading them once, caching them, noticing when a
 *               file changed on disk
 *   dai_gltf    bytes become meshes, materials and textures in the renderer
 *
 * The split matters: Mnemosyne never learns what a mesh is, the importer never
 * learns where files live, and the document stays engine free. This file is
 * the only place that knows all three.
 *
 *   dai_assets *a = dai_assets_create(renderer, 1);   // 1 = watch for edits
 *   dai_assets_mount_dir(a, "assets", 0);
 *   dai_assets_mount_pack(a, "game.mnp", -10);   // shipped content loses to a mod
 *   dai_assets_bind(a, sync);
 *
 *   // per frame
 *   if (dai_assets_poll(a)) dai_assets_bind(a, sync);   // something (re)loaded
 *   dai_doc_sync_apply(sync);
 *
 * Loading is asynchronous: reading and parsing happen on worker threads, and
 * the GPU upload happens inside dai_assets_poll() on the calling thread, which
 * must be the one that owns the Vulkan context. A node whose asset has not
 * arrived yet keeps its collision shape and draws as that shape - visibly
 * wrong, never invisible - and picks up the real mesh on a later frame.
 *
 * A file with several objects becomes several drawable pieces on ONE scene
 * node - a crate with a lid is one rigid body and two meshes. The selector
 * "models/scene.glb#Crate" narrows it to a single object, which is how one
 * Blender export can serve as a library of separate props.
 */
#ifndef DAI_ASSETS_H
#define DAI_ASSETS_H

#include "dai_doc.h"
#include "dai_gltf.h"
#include "dai_render.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dai_assets dai_assets;

/* The renderer has to outlive the asset registry: meshes and textures are
 * created inside it and released back to it.
 *
 * `watch_files` makes every poll compare mtimes and reload what changed. An
 * editor wants that; a shipped game does not, because it costs a stat per
 * tracked file per frame and nothing is ever going to change. */
DAI_API dai_assets *dai_assets_create(dai_renderer *r, int watch_files);
DAI_API void        dai_assets_destroy(dai_assets *a);

/* Sources are searched by priority, HIGHEST first - mount the shipped pack low
 * and a mod folder high and the mod wins, without anything else changing. */
DAI_API dai_result dai_assets_mount_dir(dai_assets *a, const char *dir, int priority);
DAI_API dai_result dai_assets_mount_pack(dai_assets *a, const char *pack_path, int priority);

/* Runs finalisers (the GPU uploads), applies hot reloads. Call once per frame
 * from the thread that owns the GPU context. Returns how many assets changed
 * state - non zero means "re-resolve, the handles moved". */
DAI_API uint32_t dai_assets_poll(dai_assets *a);

/* Kicks off a load and returns immediately; NULL until it is ready. */
DAI_API dai_model *dai_assets_model(dai_assets *a, const char *path);
/* Blocks until the model is loaded or has failed. Main thread only - it runs
 * the finalisers itself. Use it for a loading screen, not per frame. */
DAI_API dai_model *dai_assets_model_blocking(dai_assets *a, const char *path);

/* Matches dai_asset_resolve_fn exactly; `user` is the dai_assets *. Fills up to
 * `max` pieces and returns how many the asset has - so `out = NULL, max = 0`
 * asks for the count. Without a selector that is every node in the file; with
 * one ("path#Crate") it is exactly that node, and 1. Returns 0 for a missing
 * file, a file that is still loading, or an unknown node name - all three
 * leave the caller on its fallback shape. */
DAI_API uint32_t dai_assets_resolve(const char *path, dai_render_part *out,
                                    uint32_t max, void *user);

/* dai_doc_sync_resolver(sync, dai_assets_resolve, a) - and it also marks every
 * already built node as needing a rebuild, so calling it again after a reload
 * is how new handles reach the live scene. */
DAI_API void dai_assets_bind(dai_assets *a, dai_doc_sync *sync);

/* Diagnostics. `tracked` counts assets the cache knows about, ready ones
 * included; `failed` counts the ones that will never resolve. */
DAI_API uint32_t    dai_assets_tracked(dai_assets *a);
DAI_API uint32_t    dai_assets_ready(dai_assets *a);
DAI_API uint32_t    dai_assets_failed(dai_assets *a);
/* How often anything finished loading or reloaded. A host that caches derived
 * data compares this instead of re-resolving every frame. */
DAI_API uint32_t    dai_assets_revision(dai_assets *a);
DAI_API const char *dai_assets_last_error(dai_assets *a);
/* Why one specific asset is not there. Empty when it is fine. */
DAI_API const char *dai_assets_error_of(dai_assets *a, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* DAI_ASSETS_H */
