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

/* ---- one entry of the modifier stack ------------------------------------
 * The list of rules that turns a rough blockout into something worth looking
 * at. This is the shape the DOCUMENT holds an entry in - flat, fixed size and
 * without a pointer in it, because it lives inside dai_node_desc and the undo
 * stack is a memcpy of that struct.
 *
 * Every field is four bytes and there is no padding anywhere in here, which is
 * what keeps the whole-record memcmp in src/dai_doc.cpp honest: two nodes are
 * equal when their bytes are, and a hole full of stack rubbish would make two
 * identical stacks compare different.
 *
 * Zero means "the default" for every number, the same rule the rest of this
 * struct follows - the ONE reading of what a zero means is bevel_of() and its
 * four siblings in include/dai_modifier.h, and the inspector, the bridge, the
 * host and the tests all go through them.
 *
 * `smooth` and `relative` are the two flags, as their own ints rather than as
 * bits in a word: every other flag in dai_node_desc (hidden, trigger,
 * no_body) is an int, and a script says `modifier.0.smooth = 1` through the
 * same property machinery those use. DAI_MODF_* below are what they become on
 * the way into daimod::Mod, whose `flags` word is the geometry side's shape.
 */
#define DAI_MODIFIER_MAX 8
#define DAI_MODF_SMOOTH   0x1u
#define DAI_MODF_RELATIVE 0x2u
typedef struct dai_modifier {
    int      type;      /* dai_modifier_type, 0 = the slot is empty          */
    int      off;       /* 1 = switched off: the stack skips it entirely     */
    float    amount;    /* bevel width | solidify thickness | mirror weld, m */
    int      count;     /* bevel segments 1..4 | subdiv level 1..3 | copies  */
    float    angle;     /* bevel threshold deg | array degrees per copy      */
    int      axis;      /* mirror axis | array rotation axis, 0 X 1 Y 2 Z    */
    int      smooth;    /* subdivide: average the normals (DAI_MODF_SMOOTH)  */
    int      relative;  /* array: offset is a multiple of the bounding size
                           per axis rather than metres (DAI_MODF_RELATIVE)   */
    dai_vec3 offset;    /* array step                                        */
    float    param;     /* solidify shift -1..1                              */
} dai_modifier;         /* all fields 4 bytes: no padding, memcmp is honest  */

/* What `type` holds. The numbers are the file format, so they never move. */
typedef enum dai_modifier_type {
    DAI_MOD_NONE = 0,
    DAI_MOD_BEVEL,
    DAI_MOD_SUBDIVIDE,
    DAI_MOD_SOLIDIFY,
    DAI_MOD_ARRAY,
    DAI_MOD_MIRROR,
    DAI_MOD_TYPE_COUNT
} dai_modifier_type;

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
    /* Unity's Constraints, as a dai_freeze mask. A frozen axis is one the
     * SOLVER may not change - a script that sets the transform still moves
     * the object. 0 = nothing frozen, which is what every scene written
     * before this field existed means. */
    uint32_t freeze;
    int      no_body;           /* 1 = pure transform/graphics node, no rigid
                                   body. Groups and markers need this. Such a
                                   node still RENDERS - rendering is not part
                                   of physics.                                   */
    int      no_collider;       /* 1 = nothing can hit this (the shape stays as
                                   the rigidbody's volume, as a sensor).        */
    int      no_rigidbody;      /* 1 = a collider with nothing driving it: the
                                   body is static whatever `motion` says.       */
    char     script[512];       /* JS behaviours, ';' separated - several stack
                                   the way Unity stacks script components. Was
                                   96 bytes, which two scripts plus their
                                   assigned references already overflowed.     */

    /* ---- optional components ------------------------------------------
     * Each is off at its zero value, so an old scene file that never heard
     * of them loads unchanged - the text format only writes what differs
     * from the default. */
    int      camera;            /* 0 = none, 1 = perspective, 2 = orthographic.
                                   THIS is the 2D/3D switch: an orthographic
                                   camera with the world laid out on XY is a
                                   2D game, and nothing else about the engine
                                   has to change.                              */
    float    camera_fov;        /* perspective: degrees, 0 -> 60                */
    float    camera_size;       /* orthographic: half the visible height in
                                   world units, 0 -> 5                         */
    int      light;             /* 0 = none, 1 = point, 2 = spot, 3 = sun       */
    dai_vec3 light_color;       /* 0,0,0 -> white                               */
    float    light_range;       /* metres, 0 -> 10                              */
    float    light_intensity;   /* 0 -> 1                                       */
    float    light_cone;        /* spot: half angle in degrees, 0 -> 30         */
    int      sprite;            /* 1 = draw as a flat quad facing +Z (2D), the
                                   texture comes from `asset`                   */
    dai_vec3 sprite_size;       /* world units, 0 -> 1x1                        */
    /* ---- Text: the game's own UI ---------------------------------------
     * On screen, not in the world. A node with one draws a label in the Game
     * view and in the exported game, anchored to a corner of the picture
     * rather than to a place in the scene - which is what a score, a timer or
     * a "press any key" is.
     *
     * `text` holds either the words or a KEY: a leading '@' means look it up
     * in the project's string table (see dai_strings.h). Both spellings work
     * everywhere, so a label starts as "Score" and becomes "@hud.score" the
     * day a second language exists, without anything else changing. */
    int      text_on;           /* 1 = this node draws a label                  */
    char     text[192];         /* the words, or "@key"                         */
    float    text_size;         /* pixels, 0 -> 24                              */
    dai_vec3 text_color;        /* 0,0,0 -> white, the same "unset" rule as the
                                   light colour                                */
    int      text_anchor;       /* 0..8, reading order: 0 top left, 4 centre,
                                   8 bottom right. An anchor rather than a
                                   position because the picture changes size
                                   and a score pinned to 1920 is off screen on
                                   a 1280 window.                              */
    float    text_x, text_y;    /* pixels from the anchor, + is right and down  */
    /* The typeface, as a project path to a .ttf. Empty means the editor's own
     * UI font - a label has to draw on the day it is added, before anyone has
     * gone looking for a font file. */
    char     text_font[96];
    /* The box the text lives in, in pixels. 0,0 means "as wide as the words",
     * which is what a score wants; a real width is what a subtitle wants,
     * because a line that runs off the screen is not a subtitle.
     *
     * With a box, the text WRAPS at its width. With `text_autosize` on, it
     * also shrinks until it fits the height - the thing every UI toolkit
     * eventually grows, because a translated string is never the length the
     * layout was drawn for. German is famously a third longer than English,
     * and the box does not get bigger when it is. */
    float    text_w, text_h;
    int      text_autosize;     /* 1 = shrink to fit the box                 */
    /* How the LINES sit inside the block, which is not the same question as
     * where the block sits on screen. A subtitle is anchored bottom centre
     * and its lines are centred; a stat list is anchored top right and its
     * lines are left aligned. One field could not say both.
     * 0 follow the anchor (what it always did), 1 left, 2 centre, 3 right. */
    int      text_align;

    /* ---- Image: the game's UI, in pictures --------------------------------
     * A screen space sprite, anchored exactly like Text. `image` is a project
     * path to a .png; `image_w/h` is its size in pixels (0 = the file's own).
     *
     * This is what the old `sprite` flag was supposed to be. It set a bit that
     * NOTHING read - not the sync layer, not the scene, not the renderer -
     * so a Sprite component was a checkbox with no effect for as long as it
     * has existed. */
    int      image_on;
    char     image[96];
    float    image_w, image_h;
    dai_vec3 image_color;       /* tint, 0,0,0 -> white                      */
    int      image_anchor;      /* same 3x3 grid as the text                 */
    float    image_x, image_y;

    /* ---- Button: the Image/Text that answers the pointer ------------------
     * A Button IS the Image (its background) and the Text (its label) on the
     * same node, plus the one thing they lack: a reaction. It brightens under
     * the pointer, darkens while held, and calls the node's script when it is
     * let go INSIDE - let go outside is a cancelled click, which is what every
     * toolkit does and what people do without thinking about it.
     *
     * `button_action` names the function to call; empty means `onClick`. The
     * rectangle is the Image's when there is one, otherwise the text block's -
     * a label with no background is still a button, it just has no frame. */
    /* Components that are THERE but switched OFF. Removing one and turning
     * one off are two different things, and the document only ever had the
     * first: no_collider meant both, so the tick box in the header and the
     * Remove entry in its menu were two names for one bit - and turning a
     * collider off made its whole section vanish.
     *
     * Bit 0 the collider, bit 1 the rigidbody. */
    /* The hierarchy row is collapsed. Editor state, kept in the SCENE file -
     * Godot does the same, and for the same reason: it belongs to this scene,
     * not to the machine. Held in memory only, it was gone on restart and a
     * tidy tree had to be tidied again every morning. */
    int      ui_folded;

    uint32_t disabled_comps;
#define DAI_COMP_COLLIDER   0x1u
#define DAI_COMP_RIGIDBODY  0x2u

    int      button_on;
    dai_vec3 button_hover;      /* tint under the pointer, 0,0,0 -> automatic */
    dai_vec3 button_press;      /* tint while held,        0,0,0 -> automatic */
    char     button_action[64];

    char     audio_event[64];   /* AudioSource: event name in the sound bank    */
    int      audio_bus;         /* 0 master, 1 music, 2 sfx, 3 ui               */
    float    audio_volume;      /* 0 -> 1                                       */
    int      audio_loop;
    int      audio_autoplay;    /* 1 = starts with the scene                    */

    /* graphics */
    uint32_t mesh;              /* 0xFFFFFFFF -> derive from shape              */
    /* The material STACK, Unity's "Materials" array on a MeshRenderer: one
     * row per slot, first slot is the object's base colour + texture. Stored
     * ';'-separated like the scripts. */
    char     materials[256];
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
    /* Half size of the DRAWN mesh, in the node's own units. {0,0,0} means
     * "the same as the collider", which is what every scene written before
     * this field existed means, and what a freshly made box means.
     *
     * It exists because a collider is not a model. Resizing a Box Collider in
     * Unity does not resize the cube - it resizes what the cube can hit - and
     * an editor where the two are the same value has no way to say "this crate
     * is 2 m wide and its collision box is 1.9 m so it slides through doors". */
    dai_vec3 render_extent;
    dai_vec3 color;             /* 0,0,0 -> stable colour from the id           */
    float    roughness;         /* 0 -> 1 (matte)                               */
    float    emissive;
    uint32_t render_flags;      /* dai_render_flags                             */
    int      hidden;            /* the RENDERER is off: the object still
                                   exists, still collides, still runs its
                                   scripts. This is Unity's MeshRenderer
                                   checkbox, not the object's.                */
    int      disabled;          /* the OBJECT is off: nothing is drawn, no
                                   body, no scripts. Unity's checkbox next to
                                   the name. Sharing one flag with `hidden` is
                                   what made "activate the camera" switch the
                                   mesh renderer on.                          */

    /* ---- Blockout: the shapes a room is built out of ----------------------
     * A blockout node is not a mesh in the project folder - it is the RECIPE
     * for one. The fields below are what a wall, a stair or a doorway is made
     * of; the mesh is derived from them, rebuilt whenever one of them moves,
     * and never written back into the scene file. That is the whole point:
     * "this wall is 30 cm thick" survives in a form somebody can still edit
     * six months later, and a mesh baked into the scene does not.
     *
     * Sizes are FULL sizes in metres, not half extents - a door is 0.9 m wide,
     * and an artist made to type 0.45 will type 0.9 anyway. Zero means "the
     * default", the same rule every other optional component here follows, so
     * a scene written before these fields existed loads unchanged.
     *
     * What the numbers mean is in include/dai_blockout.h and docs/BLOCKOUT.md;
     * the property names a behaviour or the Jarvis bridge uses are in
     * include/dai_blockout_props.inl. */
    int      blockout;          /* dai_blockout_kind, 0 = not a blockout node */
    dai_vec3 blockout_size;     /* full size in metres, 0,0,0 -> 1,1,1        */
    int      blockout_segments; /* round shapes: sides around, 0 -> 16        */
    int      blockout_steps;    /* stairs: how many, 0 -> 8                   */
    float    blockout_thickness;/* arch: ring thickness in metres, 0 -> a
                                   fifth of the smaller of rise and span      */
    dai_vec3 blockout_pivot;    /* -1..1 per axis: where the node origin sits
                                   inside the shape's own box. 0 = the centre,
                                   -1 = the min face. A wall is built from the
                                   floor up, so its pivot Y is -1.            */
    /* The CSG node. Its blockout CHILDREN are combined in hierarchy order,
     * first child first, and the result is drawn on THIS node - which is why
     * the children stop drawing themselves the moment the parent has an
     * operation. A door hole is a Box child under a subtract node, and moving
     * that child with the ordinary gizmo moves the hole. */
    int      csg;               /* dai_csg_op, 0 = not a CSG node             */
    /* ---- The modifier stack ----------------------------------------------
     * Detail on a blockout does not come from placed vertices, it comes from
     * a short list of RULES that are re-run whenever the rough shape moves:
     * break the edges, divide the faces, give the plane a thickness, repeat
     * it, mirror it. That list is DATA on the node - so it is in the scene
     * file, it rides the same undo step as every other field, and the mesh it
     * produces stays derived and is never written back.
     *
     * `modifier_count` is how many of the eight slots are in use; the entries
     * are run in order, first entry first, and the ORDER is part of the
     * result: an array of a bevelled step is nine bevelled steps, a bevel of
     * an arrayed step is one long bevel down the joins.
     *
     * What each field means is include/dai_modifier.h; the names a script or
     * the bridge uses are include/dai_blockout_props.inl; the scene lines are
     * `modcount` and `mod` in src/dai_doc_text.cpp. */
    int          modifier_count;              /* 0..DAI_MODIFIER_MAX          */
    dai_modifier modifiers[DAI_MODIFIER_MAX];
    /* ---- DoorSocket: where the next room may be joined on -----------------
     * A marked opening: where it is, which way it faces, and how big it is.
     * The room generator that comes later docks against these, and until then
     * it is the thing that makes a doorway visible in the viewport as more
     * than a hole - see dai_blockout_draw_sockets in the editor UI. */
    int      door_socket;       /* 1 = this node carries one                  */
    dai_vec3 door_offset;       /* where it sits, in the node's own space     */
    dai_vec3 door_normal;       /* which way the door faces, 0,0,0 -> +Z      */
    float    door_width;        /* metres, 0 -> 0.9                           */
    float    door_height;       /* metres, 0 -> 2.0                           */

    uint32_t user_data;
} dai_node_desc;

/* What `blockout` holds. The numbers are the file format, so they never move -
 * a scene written today opens in a build that has learned a sixth shape. */
typedef enum dai_blockout_kind {
    DAI_BLOCKOUT_NONE = 0,
    DAI_BLOCKOUT_BOX,
    DAI_BLOCKOUT_CYLINDER,
    DAI_BLOCKOUT_STAIRS,
    DAI_BLOCKOUT_ARCH,
    DAI_BLOCKOUT_WEDGE,
    DAI_BLOCKOUT_KIND_COUNT
} dai_blockout_kind;

/* What `csg` holds. */
typedef enum dai_csg_op {
    DAI_CSG_NONE = 0,
    DAI_CSG_UNION,
    DAI_CSG_SUBTRACT,
    DAI_CSG_INTERSECT,
    DAI_CSG_OP_COUNT
} dai_csg_op;

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

/* ---- whole document snapshots ------------------------------------------
 *
 * A copy of every node, outside the undo stack. Play mode is what needs it:
 * pressing Play must be free, and pressing Stop must put the scene back the
 * way it was *including* everything the user dragged around while it ran -
 * Unity throws those away, and it is right to, because a change you made to
 * watch the physics is not a change to the scene.
 *
 * Restoring does NOT push an undo step: play mode is not an edit. */
typedef struct dai_doc_state dai_doc_state;
DAI_API dai_doc_state *dai_doc_state_capture(const dai_doc *d);
DAI_API void           dai_doc_state_free(dai_doc_state *s);
/* Returns the number of nodes that had to be changed back. */
DAI_API uint32_t       dai_doc_state_restore(dai_doc *d, const dai_doc_state *s);

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
