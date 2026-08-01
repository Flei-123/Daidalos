/*
 * Daidalos scene document - the editor's source of truth.
 *
 * The running world is a *view*. This is the thing being edited.
 *
 * Why this layer exists at all: an editor that mutates the live world directly
 * can only undo what it can put back, and a destroyed physics body cannot be
 * brought back under the same handle. So "undo delete" ends up either broken or
 * missing. Unity and Godot solve it the same way - edit a document of plain
 * records with stable ids, then reconcile the runtime against it. Undo becomes
 * a property of the document, and it covers *everything* for free: delete,
 * rename, reparent, swap a material, change a collision shape.
 *
 *   dai_doc          plain data, stable ids, undo/redo. No physics, no renderer.
 *      |  dai_doc_sync_apply()      incremental, only touches what changed
 *      v
 *   dai_scene + dai_world           the live view. Handles here are disposable.
 *
 * A node id is stable for the lifetime of the document and is never reused,
 * even across delete + undo. Entity and body handles behind it are not - they
 * are recreated whenever the shape changes, and that is fine, because nothing
 * outside the sync layer stores them.
 *
 * Not modelled in version 1, deliberately: compound bodies (they are a runtime
 * merge result, see dai_body_merge) and joints. Both are on the roadmap; saying
 * so beats a half-serialised scene.
 */
#ifndef DAI_DOC_H
#define DAI_DOC_H

#include "daidalos.h"
#include "dai_scene.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dai_doc      dai_doc;
typedef struct dai_doc_sync dai_doc_sync;

/* Stable, never reused. 0 means "none" and doubles as the root parent. */
typedef uint32_t dai_node;
#define DAI_INVALID_NODE ((dai_node)0)
#define DAI_NODE_NAME_MAX 64

/* One node, entirely by value: a snapshot is a memcpy, which is what makes the
 * undo stack generic instead of one command class per property. */
typedef struct dai_node_desc {
    char     name[DAI_NODE_NAME_MAX];
    char     tag[32];           /* free form, like Unity's tag - "Player", "Enemy" */
    dai_node parent;            /* 0 = root                                     */

    /* transform, local to the parent */
    dai_vec3 position;
    dai_quat rotation;
    dai_vec3 scale;             /* scales the collision shape too, not just the
                                   mesh - a box you scaled in the editor must
                                   collide the way it looks                     */

    /* physics */
    int      shape;             /* dai_shape, no compound in v1                 */
    int      motion;            /* dai_motion                                   */
    dai_vec3 half_extent;
    int      trigger;           /* 1 = collider reports overlaps, blocks nothing.
                                   This is what Unity's "Is Trigger" is: the
                                   collider exists, the collision does not.    */
    dai_vec3 collider_center;   /* offset of the shape from the transform       */
    float    density;           /* 0 -> engine default                          */
    float    friction;          /* 0 -> engine default                          */
    float    restitution;
    int      no_sleeping;
    int      no_body;           /* 1 = pure transform/graphics node, no rigid
                                   body. Groups and markers need this.          */

    /* graphics */
    uint32_t mesh;              /* 0xFFFFFFFF -> derive from shape              */
    /* Asset reference BY PATH - "models/crate.glb". The path is the identity
     * (see Mnemosyne), a bare index would point somewhere else on the next
     * run. When set it wins over `mesh`; how a path becomes a mesh is the
     * sync layer's resolver, so the document stays engine free. */
    char     asset[96];
    /* Prefab reference BY PATH, relative to the scene file. A node with one is
     * the ROOT of an instance: its children come from that file and are not
     * written into this scene, so a hundred crates cost a hundred lines and
     * fixing the crate fixes all hundred. Editing an instance's children is
     * not a thing yet - there is nowhere to record the override, so a reload
     * would silently discard it. */
    char     prefab[96];
    dai_vec3 color;             /* 0,0,0 -> stable colour from the id           */
    float    roughness;         /* 0 -> 1 (matte)                               */
    float    emissive;
    uint32_t render_flags;      /* dai_render_flags                             */
    int      hidden;

    uint32_t user_data;
} dai_node_desc;

DAI_API dai_node_desc dai_node_desc_default(void);

DAI_API dai_doc *dai_doc_create(void);
DAI_API void     dai_doc_destroy(dai_doc *d);
DAI_API void     dai_doc_clear(dai_doc *d);   /* also drops undo history */

/* ---- nodes ------------------------------------------------------------- */

DAI_API dai_node   dai_doc_add(dai_doc *d, const dai_node_desc *desc);
/* Removes the node and every descendant. Undoable, ids come back unchanged. */
DAI_API dai_result dai_doc_remove(dai_doc *d, dai_node n);
DAI_API dai_result dai_doc_get(const dai_doc *d, dai_node n, dai_node_desc *out);
DAI_API dai_result dai_doc_set(dai_doc *d, dai_node n, const dai_node_desc *desc);
DAI_API int        dai_doc_valid(const dai_doc *d, dai_node n);
DAI_API uint32_t   dai_doc_count(const dai_doc *d);
/* Every live node, parents always before their children. */
DAI_API uint32_t   dai_doc_nodes(const dai_doc *d, dai_node *out, uint32_t max);
DAI_API uint32_t   dai_doc_children(const dai_doc *d, dai_node parent, dai_node *out, uint32_t max);
DAI_API dai_node   dai_doc_find(const dai_doc *d, const char *name);
/* Rejects cycles (a node cannot become its own descendant). */
DAI_API dai_result dai_doc_set_parent(dai_doc *d, dai_node n, dai_node parent);

/* Accumulated through the parent chain. Non uniform parent scale combined with
 * child rotation is applied component wise, without a shear correction - the
 * same approximation every engine of this shape makes. */
DAI_API dai_result dai_doc_world_transform(const dai_doc *d, dai_node n,
                                           dai_vec3 *pos, dai_quat *rot, dai_vec3 *scale);
/* Sets the local transform so the node ends up at this world transform. */
DAI_API dai_result dai_doc_set_world_position(dai_doc *d, dai_node n, dai_vec3 world_pos);
DAI_API dai_result dai_doc_set_world_rotation(dai_doc *d, dai_node n, dai_quat world_rot);

/* ---- undo -------------------------------------------------------------- */

/* Everything between begin and commit is one undo step, however many nodes it
 * touches. Nesting is counted, so helpers can bracket safely. A commit that
 * changed nothing does not push a step. */
DAI_API void dai_doc_begin(dai_doc *d, const char *name);
DAI_API void dai_doc_commit(dai_doc *d);
DAI_API void dai_doc_abort(dai_doc *d);       /* rolls back the open transaction */

DAI_API int         dai_doc_undo(dai_doc *d);
DAI_API int         dai_doc_redo(dai_doc *d);
DAI_API uint32_t    dai_doc_undo_depth(const dai_doc *d);
DAI_API uint32_t    dai_doc_redo_depth(const dai_doc *d);
DAI_API const char *dai_doc_undo_name(const dai_doc *d);
DAI_API const char *dai_doc_redo_name(const dai_doc *d);
/* Bumps on every committed change - cheap "is this unsaved" check. */
DAI_API uint64_t    dai_doc_revision(const dai_doc *d);

/* ---- text format ------------------------------------------------------- */

/* Line based and diff friendly on purpose: a scene belongs in version control,
 * and a merge conflict in JSON braces helps nobody. Only fields that differ
 * from the default are written, so files stay readable and adding a field
 * later does not rewrite every scene. */
DAI_API dai_result dai_doc_save(const dai_doc *d, const char *path);
/* Replaces the contents and clears undo history. On a parse error nothing is
 * changed and `err` (if given) holds the line number and reason. */
DAI_API dai_result dai_doc_load(dai_doc *d, const char *path, char *err, size_t err_size);

/* ---- prefabs ----------------------------------------------------------- */

/* Writes the subtree rooted at `n` as a scene file of its own - the original
 * that instances point at. The root is written without a parent, so it can be
 * dropped anywhere. */
DAI_API dai_result dai_doc_prefab_save(const dai_doc *d, dai_node n, const char *path);

/* Loads that file in as a subtree under `parent` and marks the new root as an
 * instance of it. Everything it adds is one undo step. `path` is stored as
 * given, so pass it relative to the scene file if the scene is meant to be
 * portable. Returns the new root, or 0 with `err` filled. */
DAI_API dai_node dai_doc_prefab_instantiate(dai_doc *d, const char *path, dai_node parent,
                                            const char *base_dir, char *err, size_t err_size);

/* Throws away the children of every prefab instance and expands them again
 * from disk - what "I just edited the prefab" needs. Returns how many
 * instances were rebuilt. */
DAI_API uint32_t dai_doc_prefab_reload(dai_doc *d, const char *base_dir);
DAI_API dai_result dai_doc_from_text(dai_doc *d, const char *text, size_t len,
                                     char *err, size_t err_size);
/* Writes into `buf`; returns the number of bytes the text needs (excluding the
 * terminator), so a too small buffer is a size query, not a failure. */
DAI_API size_t     dai_doc_to_text(const dai_doc *d, char *buf, size_t buf_size);

/* ---- runtime sync ------------------------------------------------------ */

/* Reconciles a live scene against the document. Incremental: a node whose
 * revision has not moved is left alone, so physics keeps running underneath
 * instead of being reset to the document every frame. */
DAI_API dai_doc_sync *dai_doc_sync_create(dai_doc *d, dai_scene *scene);

/* Turns an asset path into render data. Fills up to `max` pieces and returns
 * how many the asset HAS - which may be more than max, so a caller that got a
 * full buffer can ask again with a bigger one. 0 means unresolved: missing
 * file, still loading, or a selector that names nothing. The node then falls
 * back to its shape mesh, visibly, instead of disappearing.
 *
 * More than one piece is the normal case, not an exception: a Blender file is
 * usually several objects, and forcing the user to make one scene node per
 * object would be an engine limitation leaking into their scene. Runs on the
 * sync thread. */
typedef uint32_t (*dai_asset_resolve_fn)(const char *path, dai_render_part *out,
                                         uint32_t max, void *user);
DAI_API void dai_doc_sync_resolver(dai_doc_sync *s, dai_asset_resolve_fn fn, void *user);
DAI_API void          dai_doc_sync_destroy(dai_doc_sync *s);
/* Returns how many nodes were created, updated or destroyed. 0 = nothing to do. */
DAI_API uint32_t      dai_doc_sync_apply(dai_doc_sync *s);
/* Pushes live physics transforms back into the document - what "stop play mode
 * and keep the result" needs. Returns the number of nodes written. */
DAI_API uint32_t      dai_doc_sync_pull(dai_doc_sync *s, const char *undo_name);

/* Forget what the live world currently looks like and write the document over
 * all of it on the next apply, velocities zeroed. This is what "stop play mode"
 * needs: the simulation has moved everything, and no revision changed. */
DAI_API void dai_doc_sync_reset(dai_doc_sync *s);

DAI_API dai_entity dai_doc_sync_entity(const dai_doc_sync *s, dai_node n);
DAI_API dai_node   dai_doc_sync_node(const dai_doc_sync *s, dai_entity e);
DAI_API dai_node   dai_doc_sync_node_of_body(const dai_doc_sync *s, dai_body b);
DAI_API dai_scene *dai_doc_sync_scene(const dai_doc_sync *s);
DAI_API dai_doc   *dai_doc_sync_doc(const dai_doc_sync *s);

#ifdef __cplusplus
}
#endif

#endif /* DAI_DOC_H */
