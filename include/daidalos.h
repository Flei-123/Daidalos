/*
 * Daidalos - a deterministic C++ game engine core.
 *
 *   Jolt Physics  ->  rigid body simulation        (vendored, not written here)
 *   Aulos         ->  event driven game audio      (vendored, not written here)
 *   Daidalos      ->  deterministic tick, world state, snapshots, rollback,
 *                     input queue, audio decoupling, renderer interface
 *   your host     ->  Unity / SDL / your own renderer. The engine does not
 *                     care and never calls back into it.
 *
 * The one rule the whole design follows:
 *
 *     state(n+1) = step(state(n), input(n))
 *
 * The simulation is a pure function of state and input. It never reads the
 * clock, never reads the audio system, never reads the renderer, never uses
 * an unseeded random number. That is what makes rollback netcode possible,
 * and it is the thing that cannot be retrofitted later (ask Wube).
 *
 * Everything that mutates the world goes through a command stamped with a
 * tick number, so a rollback can undo and replay it deterministically.
 *
 * License: MIT.
 */
#ifndef DAIDALOS_H
#define DAIDALOS_H

#include <stdint.h>
#include <stddef.h>

#if defined(_WIN32) && defined(DAI_BUILD_SHARED)
#  define DAI_API __declspec(dllexport)
#elif defined(_WIN32) && defined(DAI_USE_SHARED)
#  define DAI_API __declspec(dllimport)
#else
#  define DAI_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dai_world dai_world;

/* Handle = (slot + 1) | (generation << 24). Stale handles are inert, never
 * a crash - same contract as Aulos instances. */
typedef uint32_t dai_body;
#define DAI_INVALID_BODY ((dai_body)0)

typedef uint64_t dai_tick;

typedef enum dai_result {
    DAI_OK               =  0,
    DAI_ERR_INVALID_ARG  = -1,
    DAI_ERR_OUT_OF_MEMORY= -2,
    DAI_ERR_FILE         = -3,
    DAI_ERR_STATE        = -4,
    DAI_ERR_FULL         = -5,
    DAI_ERR_NOT_FOUND    = -6,
    DAI_ERR_TOO_OLD      = -7   /* rollback target is outside the snapshot ring */
} dai_result;

typedef struct dai_vec2 { float x, y; } dai_vec2;
typedef struct dai_vec3 { float x, y, z; } dai_vec3;
typedef struct dai_quat { float x, y, z, w; } dai_quat;

/* ---- configuration ----------------------------------------------------- */

/* Which physics backend the world runs on. DAI_PHYSICS_NULL exists to prove
 * the abstraction holds: it has gravity and a floor, nothing else. If the
 * engine ever stops building or running against it, Jolt has leaked out of
 * physics_jolt.cpp. */
typedef enum dai_physics_backend {
    DAI_PHYSICS_JOLT = 0,
    DAI_PHYSICS_NULL = 1,
    /* Talos (github.com/Flei-123/talos) through its C API. Same contract as
     * Jolt: bodies, joints, contacts, raycasts, save/restore. A saved backend
     * state is NOT portable between backends - that has always been true and
     * is why dai_save writes body descriptions, not a physics blob. */
    DAI_PHYSICS_TALOS = 2
} dai_physics_backend;

typedef struct dai_config {
    int      backend;           /* dai_physics_backend, default JOLT */
    uint32_t tick_hz;           /* 0 -> 60. The sim only ever advances by 1/tick_hz.  */
    uint32_t max_bodies;        /* 0 -> 8192                                          */
    uint32_t physics_threads;   /* 0 -> hardware_concurrency-1, 1 -> single threaded  */
    uint32_t snapshot_ring;     /* 0 -> 64. How many ticks back a rollback can reach. */
    uint64_t seed;              /* seeds the deterministic RNG                        */
    uint32_t velocity_steps;    /* 0 -> 10 (Jolt default)                             */
    uint32_t position_steps;    /* 0 -> 2  (Jolt default)                             */
    /* audio - optional, pass audio_bank = NULL to run the engine silent      */
    const char *asset_root;
    const char *audio_bank;
    int         enable_audio_device; /* 0 = offline (tests/servers), 1 = open a device */
} dai_config;

/* ---- bodies ------------------------------------------------------------ */

typedef enum dai_shape {
    DAI_SHAPE_BOX = 0,
    DAI_SHAPE_SPHERE,
    DAI_SHAPE_CAPSULE,
    DAI_SHAPE_COMPOUND   /* built from dai_compound_part, see dai_body_desc */
} dai_shape;

typedef enum dai_motion {
    DAI_STATIC = 0,
    DAI_KINEMATIC,
    DAI_DYNAMIC
} dai_motion;

#define DAI_MAX_PARTS 4096

typedef struct dai_compound_part {
    int      shape;          /* dai_shape, no compound nesting                 */
    dai_vec3 half_extent;    /* box: half size; sphere: x = r; capsule: x = r, y = half height */
    dai_vec3 offset;
    dai_quat rotation;
} dai_compound_part;

typedef struct dai_body_desc {
    int        shape;            /* dai_shape                                */
    int        motion;           /* dai_motion                               */
    dai_vec3   half_extent;
    dai_vec3   position;
    dai_quat   rotation;
    dai_vec3   linear_velocity;
    dai_vec3   angular_velocity;
    float      density;          /* 0 -> 1000                                */
    float      friction_static;  /* 0 -> 0.6                                 */
    float      friction_kinetic; /* 0 -> same as static (no stiction)        */
    float      restitution;
    float      linear_damping;   /* NOTE: Jolt defaults to 0.05, we default to 0 */
    float      angular_damping;
    int        no_sleeping;      /* 0 = may sleep (default), 1 = never sleeps */
    uint32_t   user_data;        /* free for the host                        */
    /* only for DAI_SHAPE_COMPOUND */
    const dai_compound_part *parts;
    uint32_t                 part_count;
} dai_body_desc;

/* ---- joints ------------------------------------------------------------ */

/* Bearings and pistons, i.e. the two things a construction game is made of.
 * Joints are tick stamped commands like everything else, so they survive a
 * rollback: created, destroyed and motor changes are all replayed. */
typedef uint32_t dai_joint;
#define DAI_INVALID_JOINT ((dai_joint)0)

typedef enum dai_joint_type {
    DAI_JOINT_FIXED = 0,   /* weld. Prefer merging into one compound body when
                              you can - a weld still costs solver time. */
    DAI_JOINT_HINGE,       /* bearing: 1 rotational DOF, optional limits + motor */
    DAI_JOINT_SLIDER,      /* piston: 1 translational DOF, optional limits + motor */
    DAI_JOINT_POINT,       /* ball joint: 3 rotational DOF */
    DAI_JOINT_DISTANCE     /* rope / spring between two points */
} dai_joint_type;

typedef enum dai_motor_state {
    DAI_MOTOR_OFF = 0,
    DAI_MOTOR_VELOCITY,    /* target is rad/s (hinge) or m/s (slider) */
    DAI_MOTOR_POSITION     /* target is rad (hinge) or m (slider)     */
} dai_motor_state;

typedef struct dai_joint_desc {
    int      type;             /* dai_joint_type                              */
    dai_body a;                /* required                                    */
    dai_body b;                /* DAI_INVALID_BODY -> anchored to the world   */
    dai_vec3 anchor;           /* world space pivot                           */
    dai_vec3 axis;             /* hinge/slider axis, world space              */
    dai_vec3 normal_axis;      /* reference axis perpendicular to `axis`      */
    int      enable_limits;
    float    limit_min;        /* hinge: rad in [-pi,0]; slider: metres <= 0  */
    float    limit_max;        /* hinge: rad in [0,pi];  slider: metres >= 0  */
    float    max_motor_force;  /* hinge: N*m, slider: N. 0 -> no motor        */
    float    max_friction;     /* joint friction, same units as above         */
    float    min_distance;     /* DAI_JOINT_DISTANCE, negative -> current     */
    float    max_distance;
    uint32_t user_data;
} dai_joint_desc;

typedef struct dai_joint_state {
    float position;   /* hinge: current angle in rad, slider: metres */
    float speed;      /* rad/s or m/s                                */
} dai_joint_state;

DAI_API dai_joint  dai_joint_create(dai_world *w, const dai_joint_desc *desc);
DAI_API dai_result dai_joint_destroy(dai_world *w, dai_joint j);
DAI_API dai_result dai_joint_set_motor(dai_world *w, dai_joint j, int motor_state, float target);
DAI_API dai_result dai_joint_get(dai_world *w, dai_joint j, dai_joint_state *out);
DAI_API int        dai_joint_valid(dai_world *w, dai_joint j);
DAI_API uint32_t   dai_joint_count(dai_world *w);

/* ---- input ------------------------------------------------------------- */

/* One fixed size input record per player per tick. Fixed size on purpose:
 * a rollback has to reconstruct any tick without allocating. */
#define DAI_MAX_PLAYERS 8
#define DAI_INPUT_AXES  8

typedef struct dai_input {
    float    axis[DAI_INPUT_AXES];
    uint32_t buttons;   /* bitfield */
} dai_input;

/* ---- audio ------------------------------------------------------------- */

/* The simulation never calls Aulos. It emits events; the presentation layer
 * drains them after the tick. That is why sound cannot desync the sim, and
 * why a rollback can cancel sounds that never actually happened. */
typedef struct dai_audio_event {
    dai_tick tick;
    char     name[32];
    dai_vec3 position;
    float    volume;
    float    pitch;
    int      is_3d;
} dai_audio_event;

/* ---- rendering feed ---------------------------------------------------- */

typedef struct dai_transform {
    dai_body  body;
    dai_vec3  position;
    dai_quat  rotation;
    uint32_t  user_data;
} dai_transform;

/* ---- lifetime ---------------------------------------------------------- */

DAI_API dai_result   dai_create(const dai_config *cfg, dai_world **out_world);
DAI_API void         dai_destroy(dai_world *w);
DAI_API const char  *dai_last_error(dai_world *w);
DAI_API const char  *dai_version(void);

/* ---- world mutation (all of it is tick stamped and rollback safe) ------- */

DAI_API dai_body   dai_body_create(dai_world *w, const dai_body_desc *desc);
DAI_API dai_result dai_body_destroy(dai_world *w, dai_body b);
DAI_API dai_result dai_body_add_impulse(dai_world *w, dai_body b, dai_vec3 impulse);
DAI_API dai_result dai_body_set_velocity(dai_world *w, dai_body b, dai_vec3 linear, dai_vec3 angular);
/* Teleport. Not tick stamped: call it from the tick callback if it has to
 * survive a rollback, exactly like every other gameplay mutation. */
DAI_API dai_result dai_body_set_transform(dai_world *w, dai_body b, dai_vec3 position, dai_quat rotation);
DAI_API int        dai_body_valid(dai_world *w, dai_body b);
DAI_API dai_result dai_body_get(dai_world *w, dai_body b, dai_transform *out);
DAI_API dai_result dai_set_gravity(dai_world *w, dai_vec3 g);

/* ---- merging: turning many bodies into one, and back ------------------- */

/* Welds several bodies into a single compound body and destroys the originals.
 * This is the operation a construction game lives on: a hundred welded blocks
 * cost a hundred bodies, a hundred broad phase entries and a solver island the
 * size of the machine, while ONE compound body with a hundred shapes costs one
 * of each. Measured on this engine: 12x to 200x cheaper per tick, depending on
 * how much of the structure is touching.
 *
 * Already merged bodies can be merged again - their parts are folded in flat,
 * so there is no nesting and no accumulating cost.
 *
 * The new body inherits: motion type STATIC if any input was static, otherwise
 * DYNAMIC; the material of the first body; the momentum of the group (the mass
 * weighted average velocity, so a merge does not create or destroy energy).
 *
 * Both merge and split are made of ordinary create/destroy commands, so they
 * are tick stamped and survive a rollback like everything else. */
DAI_API dai_body dai_body_merge(dai_world *w, const dai_body *bodies, uint32_t count, uint32_t user_data);

/* Explodes a compound body back into one body per part, giving each the
 * velocity it had as part of the rigid body (v + omega x r). Returns how many
 * were written to `out`. The compound is destroyed. */
DAI_API uint32_t dai_body_split(dai_world *w, dai_body body, dai_body *out, uint32_t max);

/* How many shapes a body is made of (1 for a simple shape). */
DAI_API uint32_t dai_body_part_count(dai_world *w, dai_body b);
DAI_API dai_result dai_body_get_velocity(dai_world *w, dai_body b, dai_vec3 *linear, dai_vec3 *angular);


/* ---- saving and loading ------------------------------------------------ */

/* Writes the whole world to a file: every body's description and its current
 * transform and velocity, every joint, the RNG state and the tick number.
 *
 * This is NOT the snapshot ring. Snapshots are the rollback mechanism - they
 * live in memory, hold the backend's opaque state blob and are only valid for
 * the process that made them. A save file is portable, versioned and readable
 * by a build that was compiled later. Mixing the two up is how engines end up
 * with save files that break every time the physics library is updated.
 *
 * Returns DAI_OK, or DAI_ERR_FILE if the file cannot be written. */
DAI_API dai_result dai_world_save(dai_world *w, const char *path);

/* Loads a world written by dai_world_save. The world is recreated from the
 * given config (backend, tick rate, thread count are the host's choice, not
 * the file's) and then filled from the file. Body handles are stable: a body
 * that was handle X when saved is handle X after loading. */
DAI_API dai_result dai_world_load(const dai_config *cfg, const char *path, dai_world **out_world);

/* Version of the save format this build writes and can read. */
DAI_API uint32_t dai_save_version(void);

/* ---- gameplay hook ----------------------------------------------------- */

/* The ONE place gameplay code is allowed to mutate the world. It is called at
 * the start of every tick, including every re-simulated tick during a
 * rollback - which is exactly why gameplay must live here and not in your
 * frame loop. Commands issued from inside the callback are not logged: the
 * callback re-issues them by itself when a tick is replayed. */
typedef void (*dai_tick_fn)(dai_world *w, dai_tick tick, void *user);
DAI_API void dai_set_tick_callback(dai_world *w, dai_tick_fn fn, void *user);

/* Deterministic RNG. Part of the simulation state, saved and restored with
 * every snapshot. Never call rand() in gameplay code. */
DAI_API uint32_t dai_random(dai_world *w);
DAI_API float    dai_random_float(dai_world *w);

/* ---- simulation -------------------------------------------------------- */

DAI_API dai_result dai_set_input(dai_world *w, uint32_t player, dai_tick tick, const dai_input *in);
DAI_API dai_result dai_get_input(dai_world *w, uint32_t player, dai_tick tick, dai_input *out);

/* Advances exactly one tick. Never takes a delta time - that is the point. */
DAI_API dai_result dai_step(dai_world *w);

/* Convenience for a real time host: accumulates wall clock time and calls
 * dai_step the right number of times. Returns how many ticks ran and writes
 * the interpolation alpha in [0,1) for the renderer. */
DAI_API uint32_t   dai_advance(dai_world *w, double real_seconds, float *out_alpha);

DAI_API dai_tick   dai_current_tick(const dai_world *w);
DAI_API double     dai_tick_seconds(const dai_world *w);

/* Order independent hash of the full simulation state. Two peers that ran
 * the same inputs must produce the same number, every tick. */
DAI_API uint64_t   dai_checksum(dai_world *w);

/* ---- rollback ---------------------------------------------------------- */

/* Rewinds to the state at the beginning of `tick` and re-simulates up to the
 * tick we were on, replaying stored inputs and recorded world commands.
 * Returns DAI_ERR_TOO_OLD if the tick fell out of the snapshot ring. */
DAI_API dai_result dai_rollback_to(dai_world *w, dai_tick tick);

/* Moves to `tick` and STAYS there - backwards restores a snapshot, forwards
 * replays the recorded commands. This is the difference between netcode and a
 * timeline: dai_rollback_to always ends up back on the tick you were on, which
 * makes it useless for scrubbing. Same ring limits apply. */
DAI_API dai_result dai_seek_to(dai_world *w, dai_tick tick);

/* Late input from a remote peer. If it differs from what was predicted for an
 * already simulated tick, this rolls back and re-simulates by itself.
 * Returns the number of ticks that had to be re-simulated (0 = nothing to do). */
DAI_API int        dai_apply_remote_input(dai_world *w, uint32_t player, dai_tick tick, const dai_input *in);

DAI_API dai_tick   dai_oldest_snapshot(const dai_world *w);

/* ---- queries ----------------------------------------------------------- */

typedef struct dai_ray_hit {
    dai_body body;              /* DAI_INVALID_BODY when nothing was hit */
    float    distance;
    dai_vec3 point;
    dai_vec3 normal;
} dai_ray_hit;

DAI_API int dai_raycast(dai_world *w, dai_vec3 from, dai_vec3 dir, float max_distance, dai_ray_hit *out);

/* Contacts of the last simulated tick, already in a deterministic order.
 * (The backend sorts them - Jolt reports contacts from several threads and
 * handing that raw order to gameplay would be a desync waiting to happen.) */
typedef struct dai_contact {
    dai_body a, b;              /* b == DAI_INVALID_BODY means "the world" */
    dai_vec3 point;
    dai_vec3 normal;            /* points from a towards b                  */
    /* Newton seconds of the IMPACT, computed from the closing speed at the
     * contact point and the two inverse masses. This is what "how hard did
     * that hit" means, and what a collision sound or a damage number scales
     * with. It is NOT the resting load: a crate sitting on the floor reports
     * about zero, because nothing is being stopped. Two static bodies also
     * report zero. Rotational inertia is left out, so an off centre hit reads
     * slightly high rather than slightly low. */
    float    impulse;
} dai_contact;

DAI_API uint32_t dai_poll_contacts(dai_world *w, dai_contact *out, uint32_t max);

DAI_API const char *dai_backend_name(dai_world *w);

/* ---- presentation ------------------------------------------------------ */

/* Interpolated transforms for rendering. alpha comes from dai_advance. */
DAI_API uint32_t   dai_get_transforms(dai_world *w, dai_transform *out, uint32_t max, float alpha);

/* Drain the audio events the sim emitted. */
DAI_API uint32_t   dai_poll_audio(dai_world *w, dai_audio_event *out, uint32_t max);

/* Emit a sound from gameplay code. Safe during a rollback: events from ticks
 * that get re-simulated are dropped before anything is heard. */
DAI_API dai_result dai_play(dai_world *w, const char *event, dai_vec3 pos, int is_3d);

/* Listener follows the camera, not the sim. Pure presentation. */
DAI_API void       dai_set_listener(dai_world *w, dai_vec3 pos, dai_vec3 fwd, dai_vec3 up, dai_vec3 vel);

/* Call once per rendered frame: pumps Aulos and plays the pending events. */
DAI_API void       dai_present(dai_world *w);

/* Offline audio render - only valid when the world was created with
 * enable_audio_device = 0. Interleaved stereo float32. Lets a server or a
 * test prove that sound is produced without ever opening a device. */
DAI_API uint32_t   dai_render_audio(dai_world *w, float *out_stereo, uint32_t frames);

/* ---- statistics -------------------------------------------------------- */

typedef struct dai_stats {
    uint32_t bodies;
    uint32_t active_bodies;
    uint64_t ticks_simulated;
    uint64_t ticks_resimulated;   /* how much work rollback has cost so far */
    uint32_t rollbacks;
    double   last_step_ms;
    double   avg_step_ms;
    uint32_t audio_voices;
} dai_stats;

DAI_API void dai_get_stats(dai_world *w, dai_stats *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DAIDALOS_H */
