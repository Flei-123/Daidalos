/*
 * dai_show.h - a drone light show, as a pure function of its inputs.
 *
 * Daidalos' one rule is state(n+1) = step(state(n), input(n)): the simulation
 * never reads the clock and never an unseeded random number. A drone show is
 * the same promise with the stakes raised - if the tool says "collision free",
 * that answer has to be reproducible on the operator's laptop, on the show
 * director's workstation and in the accident report. So everything below is
 * deterministic by construction: every random draw comes from a seed carried in
 * the input, every parallel section reduces in a fixed index order, and no
 * function here calls time(), rand() or a floating point sum whose order
 * depends on how many cores the machine has.
 *
 * THE PIPELINE, and why it is six stages rather than one button
 *
 *   1 SAMPLE     a mesh -> exactly N points, no two closer than min_distance.
 *                Poisson disk + Lloyd relaxation. The hard part is not the
 *                sampling, it is saying NO: N drones at min_distance need a
 *                certain amount of figure, and a tool that silently squeezes
 *                them together has produced a crash rather than a show.
 *                dai_show_sample_feasible answers that BEFORE any work.
 *
 *   2 ASSIGN     which drone flies to which point. Linear sum assignment on a
 *                distance cost. The classical Hungarian method is O(n^3) - at
 *                10,000 drones that is 10^12 operations, so it is not an
 *                implementation detail, it is the difference between a tool
 *                and a demo. Exact (Jonker-Volgenant) for small and medium N,
 *                a spatially decomposed auction for large N, and the stats say
 *                which one ran and what it cost in path length.
 *
 *   3 LAYER      optimal assignment still crosses. Paths are separated in
 *                height and in time until the crossings are gone, and whatever
 *                cannot be separated is REPORTED rather than hidden. Skybrush
 *                guarantees collision freedom only "if the formations are
 *                sparse enough"; here the guarantee is either held or itemised.
 *
 *   4 PROFILE    how the motion is shaped in time: linear, or a cubic Bezier
 *                that starts and ends at zero speed, or eased at one end only.
 *                Synchronised or staggered. v_max and a_max are not hopes -
 *                dai_show_plan_build refuses a duration that would break them
 *                and says what the shortest legal duration is.
 *
 *   5 VALIDATE   the whole timeline: pairwise distance, speed, acceleration,
 *                geofence, ground clearance. A uniform grid broadphase, because
 *                10,000 drones squared per tick is 10^8 distance tests per tick
 *                and there are tens of thousands of ticks. Streaming over time,
 *                because 10,000 x 30,000 x 12 bytes is 3.6 GB and nobody has
 *                that to spare - the plan stores keyframes and interpolates.
 *
 *   6 EXPORT     .skyc (Skybrush's container, implemented here from the format
 *                description - no GPL code was copied, Daidalos is MIT) so the
 *                show flies on proven ArduPilot firmware, plus CSV and JSON for
 *                anyone who wants to check the numbers themselves.
 *
 * MEMORY. A show is keyframes, never materialised ticks. dai_show_plan holds,
 * per drone, a small ordered list of (time, position, colour) with the segment
 * shape in between; dai_show_plan_sample reconstructs any instant. 10,000
 * drones through 20 formations is a few tens of megabytes, and the validator
 * walks time in windows rather than building an array.
 *
 * WHAT IS NOT HERE. No renderer, no Vulkan, no dai_ui. This header is
 * arithmetic on points and time, which is why tests/test_droneshow runs on a
 * machine with no GPU and no display. The panels live in dai_show_ui.h.
 */
#ifndef DAI_SHOW_H
#define DAI_SHOW_H

#include "daidalos.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the fixed vocabulary ----------------------------------------------- */

/* One drone at one instant. Position in show-local metres (+Y up, the engine's
 * convention), colour as the LED actually emits it: r/g/b plus the separate
 * white channel real show fixtures have, 0..255. Packed to 16 bytes so a
 * formation of 10,000 is 160 KB and fits in cache while the solver works. */
typedef struct dai_show_point {
    float   x, y, z;
    uint8_t r, g, b, w;
} dai_show_point;

/* The project's droneshow settings, mirrored out of settings/project.txt so
 * the solver never has to know what a project is. dai_show_settings_default
 * gives the values a fresh droneshow project is created with. */
typedef struct dai_show_settings {
    float    min_distance_m;      /* pairwise floor, the safety number         */
    float    v_max_ms;            /* horizontal+vertical speed limit           */
    float    a_max_ms2;           /* acceleration limit                        */
    uint32_t drone_count;         /* the fleet: every formation has exactly N  */
    double   show_origin_lat;     /* WGS84, degrees - where local (0,0,0) is   */
    double   show_origin_lon;
    float    show_origin_amsl;    /* metres above mean sea level               */
    float    show_orientation_deg;/* local +Z rotated off true north           */
    float    takeoff_alt_m;       /* the height the fleet forms up at          */
    int      fps;                 /* export/validation sampling rate           */
    /* The box the show may not leave. Half extents around the origin in X/Z,
     * ceiling in Y; ground clearance is min_ground_m. A zero geofence means
     * "not fenced" and the geofence check is then skipped rather than passed. */
    float    fence_half_x, fence_half_z, fence_top_m, min_ground_m;
    uint64_t seed;                /* every random draw in the pipeline         */
} dai_show_settings;

DAI_API dai_show_settings dai_show_settings_default(void);

/* Where the points come from off a mesh, and why there are three answers.
 *
 * SURFACE  - the skin of the model. What a figure normally wants: a dragon
 *            made of drones is its outline in 3D, not a solid block.
 * VOLUME   - the inside as well. For blobs, clouds, hearts - shapes whose
 *            reading depends on being filled.
 * SILHOUETTE - points pushed onto the outline as seen from the audience. A
 *            show is watched from one direction, and 3,000 drones spread over
 *            a surface read as mush where 3,000 drones on the contour read as
 *            a crisp shape. This is the mode that sells the figure. */
typedef enum dai_show_sample_mode {
    DAI_SHOW_SAMPLE_SURFACE    = 0,
    DAI_SHOW_SAMPLE_VOLUME     = 1,
    DAI_SHOW_SAMPLE_SILHOUETTE = 2
} dai_show_sample_mode;

/* A triangle soup plus how to colour it. The caller supplies plain arrays -
 * dai_gltf_read_geometry hands exactly these back - so sampling needs no
 * renderer and no asset layer.
 *
 * Colour is looked up in this order at every sample point: vertex colours if
 * `vertex_rgba` is given (barycentric blend of the triangle's three), else the
 * texture if `tex_rgba` is given (bilinear at the interpolated uv), else
 * `base_rgba`. That order is the glTF material's own, so a figure looks in the
 * viewport like the model looked in Blender. */
typedef struct dai_show_sample_desc {
    const float    *positions;    /* 3 floats per vertex, mesh space           */
    const float    *normals;      /* 3 per vertex, may be NULL                 */
    const float    *uvs;          /* 2 per vertex, may be NULL                 */
    const uint32_t *vertex_rgba;  /* 0xAABBGGRR per vertex, may be NULL        */
    uint32_t        vertex_count;
    const uint32_t *indices;      /* 3 per triangle                            */
    uint32_t        index_count;

    const uint8_t  *tex_rgba;     /* RGBA8, may be NULL                        */
    uint32_t        tex_w, tex_h;
    uint32_t        base_rgba;    /* the fallback colour, 0xAABBGGRR           */

    int      mode;                /* dai_show_sample_mode                      */
    uint32_t count;               /* EXACTLY this many points come out         */
    float    min_distance_m;      /* the hard pairwise floor                   */
    float    scale;               /* mesh units -> metres                      */
    dai_vec3 centre;              /* where the figure's centre lands, metres   */
    dai_vec3 view_dir;            /* audience -> figure, for SILHOUETTE        */
    uint32_t relax_iterations;    /* Lloyd passes; 0 = none, 8 is a good day   */
    uint64_t seed;
} dai_show_sample_desc;

/* The answer to "can this figure hold this many drones at this spacing", given
 * BEFORE anything is sampled.
 *
 * The bound is geometric, not a guess: N points at pairwise distance d need
 * area (surface/silhouette) or volume (volume mode) at least N times the area
 * or volume of one packing cell, at the densest packing that exists in that
 * dimension (hexagonal in 2D, face centred cubic in 3D). `required_scale` is
 * the factor the model must be grown by for the request to become possible,
 * and `max_points` is how many fit at the size it is now. Saying this up front
 * is the difference between a tool and a machine that quietly builds a crash. */
typedef struct dai_show_feasibility {
    int      ok;                  /* 1 = the request is geometrically possible */
    uint32_t max_points;          /* how many fit at min_distance_m, as is     */
    float    required_scale;      /* multiply the model by this to fit `count` */
    float    required_size_m;     /* ...expressed as the figure's longest side */
    float    achievable_spacing_m;/* the best pairwise distance for `count`    */
} dai_show_feasibility;

/* Cheap: it measures area/volume and divides. No sampling, no allocation, safe
 * to call from a UI every frame while a number is being typed. */
DAI_API int dai_show_sample_feasible(const dai_show_sample_desc *d,
                                     dai_show_feasibility *out);

/* Fills `out` with exactly `d->count` points (never fewer - it either meets the
 * request or fails), returns how many were written, 0 on failure with the
 * reason in `err`. The failure that matters is the feasibility one, and its
 * message names the size the figure needs to be.
 *
 * GUARANTEE: every pair in the result is at least d->min_distance_m apart.
 * tests/droneshow_cases_sample.cpp asserts it by measuring, not by trusting. */
DAI_API uint32_t dai_show_sample(const dai_show_sample_desc *d,
                                 dai_show_point *out, uint32_t max,
                                 char *err, size_t err_len);

/* ---- stage 2: assignment ------------------------------------------------- */

/* AUTO picks: exact below DAI_SHOW_EXACT_MAX, clustered auction above. The
 * other three are there because a show director who has been bitten wants to
 * force one, and because a test that cannot ask for the exact answer cannot
 * measure what the fast one lost. */
typedef enum dai_show_assign_method {
    DAI_SHOW_ASSIGN_AUTO    = 0,
    DAI_SHOW_ASSIGN_EXACT   = 1,  /* Jonker-Volgenant, O(n^3) worst case       */
    DAI_SHOW_ASSIGN_AUCTION = 2,  /* auction with epsilon scaling              */
    DAI_SHOW_ASSIGN_CLUSTER = 3   /* spatial split, exact inside each cluster  */
} dai_show_assign_method;

/* Above this many drones AUTO stops asking for the exact answer. Chosen from
 * the measured table in tests/test_droneshow: JV is milliseconds at 1,000 and
 * minutes at 10,000. */
#define DAI_SHOW_EXACT_MAX 2000u

typedef struct dai_show_assign_stats {
    int      method_used;         /* dai_show_assign_method, never AUTO        */
    double   total_cost_m;        /* sum of |from - to| over the assignment    */
    double   exact_cost_m;        /* the optimum, or < 0 when not computed     */
    float    gap_percent;         /* 100*(total-exact)/exact, or < 0           */
    double   solve_ms;            /* measured, and never fed back into a result*/
    uint32_t clusters;            /* 1 for the exact paths                     */
    uint32_t iterations;
} dai_show_assign_stats;

/* out_perm[i] = the index in `to` that drone i flies to. A permutation, always
 * - a partial assignment is a drone with nowhere to go.
 *
 * Cost is squared distance internally (it orders the same as distance, has no
 * square root, and keeps the arithmetic exact for longer) but the stats report
 * real metres because that is what a director budgets in. Deterministic: ties
 * break on the lower index, and the clustered path splits space on a fixed
 * rule rather than on thread arrival order. */
DAI_API dai_result dai_show_assign(const dai_show_point *from,
                                   const dai_show_point *to, uint32_t n,
                                   int method, uint32_t *out_perm,
                                   dai_show_assign_stats *stats);

/* The optimum by brute force, for n <= 8. Exists so the test can prove the
 * solver rather than agree with it. Returns the cost and fills the permutation. */
DAI_API double dai_show_assign_brute_force(const dai_show_point *from,
                                           const dai_show_point *to, uint32_t n,
                                           uint32_t *out_perm);

/* ---- stage 3 and 4: the transition -------------------------------------- */

typedef enum dai_show_profile {
    DAI_SHOW_PROFILE_LINEAR       = 0, /* constant speed, hard corners in v    */
    DAI_SHOW_PROFILE_SMOOTH       = 1, /* cubic Bezier, v = 0 at both ends     */
    DAI_SHOW_PROFILE_SMOOTH_LEFT  = 2, /* eased out of the start only          */
    DAI_SHOW_PROFILE_SMOOTH_RIGHT = 3  /* eased into the end only              */
} dai_show_profile;

typedef enum dai_show_timing {
    DAI_SHOW_TIMING_SYNC     = 0,  /* everyone leaves and lands together       */
    DAI_SHOW_TIMING_STAGGERED= 1   /* a per drone offset, deterministic order  */
} dai_show_timing;

typedef struct dai_show_transition {
    float duration_s;             /* how long the move takes                   */
    int   profile;                /* dai_show_profile                          */
    int   timing;                 /* dai_show_timing                           */
    float stagger_s;              /* total spread when timing is STAGGERED     */
    int   assign_method;          /* dai_show_assign_method                    */
} dai_show_transition;

DAI_API dai_show_transition dai_show_transition_default(void);

/* One drone's motion during one transition, after layering. The detour is the
 * whole of stage 3: a drone that would cross another is lifted to `layer_y`
 * for the middle of its leg, or held back by `delay_s`, or both. Everything
 * else about the leg follows from the profile.
 *
 * This is a value, not an object: 32 bytes per drone per transition, which is
 * why a 10,000 drone, 20 formation show is megabytes rather than gigabytes. */
typedef struct dai_show_leg {
    float t_start;                /* show time the drone starts moving         */
    float t_end;                  /* ...and arrives                            */
    float layer_y;                /* cruise height of the detour, absolute m   */
    float rise_frac;              /* 0 = no detour; else fraction spent rising */
    int   profile;
    int   pad;
} dai_show_leg;

/* ---- the plan: keyframes, and the only thing that stores the show -------- */

typedef struct dai_show_plan dai_show_plan;

/* Layering result. `resolved` is what the guarantee cost - how many legs had to
 * be lifted or delayed - and `unresolved` is the honest remainder: pairs the
 * separator could not untangle inside the duration allowed. A show with a
 * non-zero unresolved count is not signed off, and dai_show_layer says so
 * rather than returning DAI_OK. */
typedef struct dai_show_layer_stats {
    uint32_t crossings_found;
    uint32_t resolved_by_height;
    uint32_t resolved_by_delay;
    uint32_t unresolved;
    uint32_t layers_used;
    float    max_extra_height_m;
    double   solve_ms;
} dai_show_layer_stats;

/* Turns an assignment into legs that do not collide with each other.
 *
 * `from`/`to` are the two formations, `perm` the assignment, `tr` the shape and
 * duration asked for. Writes n legs. Returns DAI_OK when the result is provably
 * free of drone-drone conflict inside the transition, DAI_ERR_STATE when some
 * pairs are left over - and in that case the legs are still written (a director
 * needs to see the near miss to fix it) and stats->unresolved says how many.
 *
 * The separation is deterministic: crossing pairs are found through the same
 * uniform grid the validator uses, sorted by (lower index, higher index), and
 * assigned to layers in that order, so the same input yields the same heights
 * on every machine and in every thread count. */
DAI_API dai_result dai_show_layer(const dai_show_point *from,
                                  const dai_show_point *to, uint32_t n,
                                  const uint32_t *perm,
                                  const dai_show_transition *tr,
                                  const dai_show_settings *s,
                                  float t_start,
                                  dai_show_leg *out_legs,
                                  dai_show_layer_stats *stats);

/* The shortest duration a leg of this length can legally take under v_max and
 * a_max with this profile. What the parameter panel shows next to a duration
 * that is too short, and what dai_show_plan_build refuses below. */
DAI_API float dai_show_min_duration(float distance_m, int profile,
                                    const dai_show_settings *s);

/* Where a drone is at time t, and what colour it is. The ONLY way to read a
 * plan - there is deliberately no array of ticks to walk, because that array is
 * the 3.6 GB this design exists to avoid. O(log k) in the drone's keyframes.
 * Outside [0, duration] it clamps to the first/last formation, which is what a
 * viewport scrubbed past the end should show. */
DAI_API void dai_show_plan_sample(const dai_show_plan *p, uint32_t drone,
                                  float t, dai_show_point *out);
/* The whole fleet at time t, in drone order. `out` must hold drone_count. */
DAI_API void dai_show_plan_sample_all(const dai_show_plan *p, float t,
                                      dai_show_point *out);

DAI_API uint32_t dai_show_plan_drone_count(const dai_show_plan *p);
DAI_API float    dai_show_plan_duration(const dai_show_plan *p);
DAI_API uint32_t dai_show_plan_keyframe_count(const dai_show_plan *p, uint32_t drone);
DAI_API size_t   dai_show_plan_bytes(const dai_show_plan *p);
DAI_API void     dai_show_plan_destroy(dai_show_plan *p);

/* Reads a keyframe out, for the exporters and the round trip test. */
typedef struct dai_show_key {
    float          t;
    dai_show_point p;
    int            profile;       /* how to reach this key from the previous   */
} dai_show_key;
DAI_API int dai_show_plan_key_at(const dai_show_plan *p, uint32_t drone,
                                 uint32_t index, dai_show_key *out);

/* Builds a plan from keyframes directly - what the exporters' reimport and the
 * tests use, and what makes a deliberately broken plan constructible. Keys are
 * given per drone, `counts[i]` of them, concatenated in `keys`. */
DAI_API dai_show_plan *dai_show_plan_from_keys(uint32_t drone_count,
                                               const uint32_t *counts,
                                               const dai_show_key *keys);

/* ---- stage 5: validation ------------------------------------------------- */

typedef enum dai_show_conflict_kind {
    DAI_SHOW_CONFLICT_DISTANCE = 0,  /* two drones closer than min_distance    */
    DAI_SHOW_CONFLICT_VMAX     = 1,
    DAI_SHOW_CONFLICT_AMAX     = 2,
    DAI_SHOW_CONFLICT_FENCE    = 3,
    DAI_SHOW_CONFLICT_GROUND   = 4
} dai_show_conflict_kind;

/* One thing that is wrong, with everything needed to jump to it: the validation
 * panel seeks the timeline to `time_s` and selects `a` (and `b`), and the
 * viewport draws them red with a line between. `margin_m` is how far the limit
 * was missed by - negative is the amount of violation, which is the number a
 * director argues about. */
typedef struct dai_show_conflict {
    float    time_s;
    uint32_t a, b;                /* b == a for the single-drone kinds         */
    int      kind;
    float    value;               /* the measured distance / speed / height    */
    float    limit;               /* what it had to stay inside                */
} dai_show_conflict;

typedef struct dai_show_validate_stats {
    uint32_t ticks_checked;
    uint32_t pairs_tested;        /* the broadphase's whole point: << n^2      */
    float    min_distance_m;      /* the tightest moment in the show           */
    float    max_speed_ms;
    float    max_accel_ms2;
    uint32_t conflicts;           /* total, even when `out` was too small      */
    double   solve_ms;
    size_t   peak_bytes;          /* what the check itself needed              */
} dai_show_validate_stats;

/* Walks the whole timeline at s->fps and reports everything that breaks a rule.
 *
 * Not O(n^2) per tick: positions go into a uniform grid whose cell is the
 * minimum distance, so only the 27 neighbouring cells are ever tested, and the
 * fleet is never materialised for more than one tick at a time. Conflicts come
 * out sorted by (time, a, b) so two runs produce byte identical lists.
 *
 * Returns how many conflicts were written (<= max); stats->conflicts is the
 * true total. A show is clean when that total is zero, and nothing else. */
DAI_API uint32_t dai_show_validate(const dai_show_plan *p,
                                   const dai_show_settings *s,
                                   dai_show_conflict *out, uint32_t max,
                                   dai_show_validate_stats *stats);

/* ---- stage 6: export ----------------------------------------------------- */

/* The Skybrush container: a ZIP holding show.json plus one compressed
 * trajectory and light program per drone. Implemented here from the published
 * format description - Daidalos is MIT and copying GPL code into it would be
 * both illegal and pointless, since the format is a few hundred lines.
 *
 * Written through a temporary file and renamed, like every other writer in this
 * engine: an interrupted export must not leave something a ground station will
 * happily load half of. */
DAI_API dai_result dai_show_export_skyc(const dai_show_plan *p,
                                        const dai_show_settings *s,
                                        const char *path, char *err, size_t err_len);

/* time_s, drone, x, y, z, r, g, b, w - one line per drone per frame at s->fps.
 * The format for someone who wants to check the numbers in a spreadsheet, and
 * the reason it exists is that a proprietary container nobody can read is how
 * a show becomes unauditable. */
DAI_API dai_result dai_show_export_csv(const dai_show_plan *p,
                                       const dai_show_settings *s,
                                       const char *path, char *err, size_t err_len);

/* The same show as JSON, keyframes rather than samples - and this one round
 * trips: dai_show_import_json(dai_show_export_json(p)) is the same plan, key
 * for key and bit for bit. Floats are written at the shortest precision that
 * reads back identical, the way the scene format does it. */
DAI_API dai_result dai_show_export_json(const dai_show_plan *p,
                                        const dai_show_settings *s,
                                        const char *path, char *err, size_t err_len);
DAI_API dai_show_plan *dai_show_import_json(const char *path,
                                            dai_show_settings *out_settings,
                                            char *err, size_t err_len);
/* Reads back a .skyc this library wrote, so the export can be proven rather
 * than admired. Not a general Skybrush importer. */
DAI_API dai_show_plan *dai_show_import_skyc(const char *path,
                                            dai_show_settings *out_settings,
                                            char *err, size_t err_len);

/* ---- the document: formations, storyboard, and one solve ---------------- */

/* What the editor edits. A list of formations, the transitions between them,
 * the settings, and the last solved plan with its timings. Opaque, and the same
 * shape as the rest of this engine's documents: plain records, stable ids, a
 * line based text file that diffs per property. */
typedef struct dai_show dai_show;

#define DAI_SHOW_NAME_MAX 64

typedef struct dai_show_formation_info {
    char     name[DAI_SHOW_NAME_MAX];
    char     source[128];         /* the asset it was sampled from, or ""      */
    uint32_t point_count;
    float    hold_s;              /* how long the figure stands still          */
    int      sample_mode;
    float    t_start;             /* derived: where it lands on the timeline   */
} dai_show_formation_info;

DAI_API dai_show *dai_show_create(const dai_show_settings *s);
DAI_API void      dai_show_destroy(dai_show *sh);

DAI_API dai_show_settings dai_show_get_settings(const dai_show *sh);
/* Changing the settings invalidates the plan - a new min_distance with the old
 * "no conflicts" badge next to it is the worst thing this tool could show. */
DAI_API void dai_show_set_settings(dai_show *sh, const dai_show_settings *s);

/* Adds a formation from points already sampled. Returns its index, or
 * UINT32_MAX when the count does not match the fleet: every formation has
 * exactly drone_count points, because a drone cannot be left in the air. */
DAI_API uint32_t dai_show_formation_add(dai_show *sh, const char *name,
                                        const char *source,
                                        const dai_show_point *pts, uint32_t n,
                                        float hold_s);
/* Samples `desc` and adds the result in one step - what "Formation from
 * selected mesh" in the storyboard panel calls. Fails, with the required size
 * in `err`, when the figure is too small for the fleet. */
DAI_API uint32_t dai_show_formation_from_mesh(dai_show *sh, const char *name,
                                              const char *source,
                                              const dai_show_sample_desc *desc,
                                              char *err, size_t err_len);
DAI_API uint32_t dai_show_formation_count(const dai_show *sh);
DAI_API int      dai_show_formation_get(const dai_show *sh, uint32_t i,
                                        dai_show_formation_info *out);
DAI_API const dai_show_point *dai_show_formation_points(const dai_show *sh, uint32_t i);
DAI_API int      dai_show_formation_remove(dai_show *sh, uint32_t i);
DAI_API int      dai_show_formation_move(dai_show *sh, uint32_t i, int delta);
DAI_API int      dai_show_formation_rename(dai_show *sh, uint32_t i, const char *name);
DAI_API int      dai_show_formation_set_hold(dai_show *sh, uint32_t i, float hold_s);

/* The transition INTO formation i (i >= 1). Formation 0 is the take-off grid
 * and has none. */
DAI_API int dai_show_transition_get(const dai_show *sh, uint32_t i, dai_show_transition *out);
DAI_API int dai_show_transition_set(dai_show *sh, uint32_t i, const dai_show_transition *tr);

/* Runs stages 2, 3 and 4 over the whole storyboard and leaves a plan behind.
 * Returns DAI_OK when every transition came out provably clean, DAI_ERR_STATE
 * when some did not - the plan is built either way, because a show that cannot
 * be looked at cannot be fixed. Timings land in dai_show_timings. */
DAI_API dai_result dai_show_solve(dai_show *sh, char *err, size_t err_len);
DAI_API const dai_show_plan *dai_show_get_plan(const dai_show *sh);

/* What the last solve cost, per stage, in milliseconds. Measured with a
 * monotonic clock OUTSIDE the solver - no function that produces a result ever
 * reads a clock, which is the determinism rule this whole engine is built on. */
typedef struct dai_show_timings {
    double sample_ms, assign_ms, layer_ms, profile_ms, validate_ms;
    uint32_t drones, formations;
    dai_show_assign_stats   last_assign;
    dai_show_layer_stats    last_layer;
    dai_show_validate_stats last_validate;
} dai_show_timings;
DAI_API dai_show_timings dai_show_get_timings(const dai_show *sh);

/* The conflict list from the last dai_show_validate_show. Kept on the document
 * so the panel can be redrawn without solving again. */
DAI_API dai_result dai_show_validate_show(dai_show *sh);
DAI_API uint32_t   dai_show_conflict_count(const dai_show *sh);
DAI_API int        dai_show_conflict_at(const dai_show *sh, uint32_t i,
                                        dai_show_conflict *out);

/* The show as text, saved next to the scenes as <project>/scenes/<name>.dshow.
 * Line based, one property per line, only what differs from the default - the
 * same rules dai_doc_text follows, and for the same reason: a show file lives
 * in version control and has to merge per property. Formation points are
 * written as a fixed width block, because 10,000 of them per formation is the
 * one place where per-line prose would be silly. */
DAI_API dai_result dai_show_save(const dai_show *sh, const char *path,
                                 char *err, size_t err_len);
DAI_API dai_show  *dai_show_load(const char *path, char *err, size_t err_len);

#ifdef __cplusplus
}
#endif
#endif /* DAI_SHOW_H */
