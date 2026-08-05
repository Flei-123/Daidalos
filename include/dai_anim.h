/*
 * Animation: clips, a player, crossfades and events.
 *
 * Skinning already works - the renderer takes joint matrices (dai_render_joints)
 * and the glTF importer reads skins. What was missing is the layer above: WHICH
 * pose, and how one pose becomes the next. That is this file.
 *
 * There is no state machine and no graph editor here, and that is a decision,
 * not an omission. A graph is a second programming language with worse tooling
 * than the one the engine already has: every transition is a click, every
 * condition is a variable you cannot grep, and the moment gameplay needs to say
 * "play the hurt clip unless we are already dying" the graph is in the way.
 * Code says it in one line:
 *
 *     anim.play("Run", 0.2);          // fade in over 0.2 s
 *     anim.crossfade("Idle", 0.3);
 *     anim.speed = 1.5;
 *     if (anim.isPlaying("Run")) ...
 *     anim.onEvent("footstep", playSound);
 *
 * The C API below is that API; src/dai_script.cpp binds it to the global `anim`
 * object for QuickJS. What a state machine gives you on top of this - "these
 * are the only legal transitions" - is a property of your own code, where you
 * can actually read it.
 *
 * ---- the pieces -----------------------------------------------------------
 *
 *   clip     channels of keyframes. A channel drives one TARGET (a joint, or
 *            any node in the rig) and one property (translation, rotation as a
 *            quaternion, scale). Plus a name, a duration, a loop flag and a
 *            list of events.
 *
 *   pose     an array of dai_anim_trs, indexed BY TARGET. Local transforms,
 *            not matrices: blending happens here, before the hierarchy is
 *            walked, because you cannot slerp a matrix.
 *
 *   player   holds the clips by name, one current state and any number of
 *            states fading out, and turns dt into a pose. Feed that pose to
 *            dai_anim_apply() and the joint matrices land in the renderer.
 *
 * The player knows nothing about models or the renderer - it is arithmetic on
 * arrays, which is why tests/test_anim.cpp runs without a GPU. The bridge to
 * glTF and to the skinning pipeline is at the bottom of this header and lives
 * in its own translation unit (src/dai_anim_gltf.cpp), so linking the player
 * does not drag the importer in.
 */
#ifndef DAI_ANIM_H
#define DAI_ANIM_H

#include "daidalos.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DAI_MODEL_FWD
#define DAI_MODEL_FWD
typedef struct dai_model dai_model;
#endif

/* ---- clips -------------------------------------------------------------- */

/* What a channel drives on its target. */
typedef enum dai_anim_path {
    DAI_ANIM_TRANSLATION = 0,   /* 3 floats per key                            */
    DAI_ANIM_ROTATION    = 1,   /* 4 floats per key: quaternion x,y,z,w        */
    DAI_ANIM_SCALE       = 2    /* 3 floats per key                            */
} dai_anim_path;

typedef enum dai_anim_interp {
    DAI_ANIM_LINEAR = 0,        /* lerp for T/S, shortest arc slerp for R      */
    DAI_ANIM_STEP   = 1         /* hold the left key until the next one        */
} dai_anim_interp;

/* One target's LOCAL transform. A pose is an array of these.
 *
 * The field order is tx ty tz | rx ry rz rw | sx sy sz - ten floats, no
 * padding - and dai_model_pose_local() reads exactly that layout. */
typedef struct dai_anim_trs {
    dai_vec3 t;
    dai_quat r;
    dai_vec3 s;
} dai_anim_trs;

DAI_API dai_anim_trs dai_anim_trs_identity(void);

typedef struct dai_anim_clip dai_anim_clip;

DAI_API dai_anim_clip *dai_anim_clip_create(const char *name);
DAI_API void           dai_anim_clip_destroy(dai_anim_clip *c);

/* Adds a channel and copies the keys in.
 *
 * `times` is key_count timestamps in seconds and MUST be non decreasing - the
 * lookup is a binary search, and a binary search over unsorted data is not slow,
 * it is wrong. Unsorted input is rejected with -1 rather than sampled to
 * nonsense. `values` is key_count * (path == DAI_ANIM_ROTATION ? 4 : 3) floats.
 *
 * Returns the channel index, or -1 if the arguments do not add up. The clip's
 * duration grows to the last timestamp of the longest channel.
 *
 * Rotation keys are normalised on the way in: a quaternion off the unit sphere
 * turns into a bone that grows or shrinks, and finding that later costs an
 * afternoon. */
DAI_API int dai_anim_clip_add_channel(dai_anim_clip *c, uint32_t target, dai_anim_path path,
                                      dai_anim_interp interp, const float *times,
                                      const float *values, uint32_t key_count);

/* A named moment. The player calls back when playback CROSSES it - see
 * dai_anim_on_event for exactly when that is. Events may be added in any order;
 * they are kept sorted by time. */
DAI_API void dai_anim_clip_add_event(dai_anim_clip *c, float time, const char *name);

DAI_API void        dai_anim_clip_set_loop(dai_anim_clip *c, int loop);   /* default: 1 */
DAI_API int         dai_anim_clip_loop(const dai_anim_clip *c);
DAI_API float       dai_anim_clip_duration(const dai_anim_clip *c);
/* Overrides the duration the keys imply - for a clip whose last key sits before
 * the end of the motion (a 2 s cycle whose last pose is at 1.8 s). */
DAI_API void        dai_anim_clip_set_duration(dai_anim_clip *c, float seconds);
DAI_API const char *dai_anim_clip_name(const dai_anim_clip *c);
DAI_API uint32_t    dai_anim_clip_channel_count(const dai_anim_clip *c);
DAI_API uint32_t    dai_anim_clip_event_count(const dai_anim_clip *c);
/* Highest target index the clip drives, plus one - the pose size it needs. */
DAI_API uint32_t    dai_anim_clip_target_count(const dai_anim_clip *c);
/* Reads an event back. Returns its name and writes the time, or NULL. */
DAI_API const char *dai_anim_clip_event_at(const dai_anim_clip *c, uint32_t index, float *out_time);

/* Samples the clip at `time` (seconds, clamped to [0, duration] - the PLAYER
 * does the looping) into `pose`. Only the targets the clip actually drives are
 * written, so `pose` must already hold the rest pose for everything else. */
DAI_API void dai_anim_clip_sample(const dai_anim_clip *c, float time,
                                  dai_anim_trs *pose, uint32_t count);

/* One channel on its own, into 3 or 4 floats. This is what a test pokes at:
 * the pose above is the same arithmetic with a hierarchy in the way. Returns
 * how many components were written, 0 if the channel does not exist. */
DAI_API uint32_t dai_anim_clip_sample_channel(const dai_anim_clip *c, uint32_t channel,
                                              float time, float *out);

/* ---- blending two poses ------------------------------------------------- */

/* dst = dst*(1-w) + src*w, per target. Translation and scale are linear,
 * rotation is a shortest arc slerp: a linear mix of quaternions shortens the
 * bone, which reads as a limb that shrinks mid stride. */
DAI_API void dai_anim_pose_blend(dai_anim_trs *dst, const dai_anim_trs *src,
                                 uint32_t count, float w);

/* ---- the player --------------------------------------------------------- */

typedef struct dai_anim dai_anim;

/* Fired when playback crosses an event. `clip` is the clip that owns it. */
typedef void (*dai_anim_event_fn)(const char *event, const char *clip, float time, void *user);

/* `targets` is the pose size - for a glTF model, its node count. The player
 * grows this by itself when a clip needs more, so an estimate is fine. */
DAI_API dai_anim *dai_anim_create(uint32_t targets);
DAI_API void      dai_anim_destroy(dai_anim *a);

/* Registers a clip under its own name. `own` = 1 hands ownership over, so
 * dai_anim_destroy frees it. Returns the clip index or -1 (a duplicate name
 * replaces the old entry - reloading an asset must not leak a state). */
DAI_API int             dai_anim_add(dai_anim *a, dai_anim_clip *clip, int own);
DAI_API dai_anim_clip  *dai_anim_find(const dai_anim *a, const char *name);
DAI_API uint32_t        dai_anim_count(const dai_anim *a);
DAI_API dai_anim_clip  *dai_anim_at(const dai_anim *a, uint32_t index);

/* The pose everything starts from: the rig's bind/rest transforms. Without it
 * every target a clip does not mention collapses to the identity, which is a
 * character folded into a point at the origin. dai_anim_from_model fills it. */
DAI_API void dai_anim_set_rest(dai_anim *a, const dai_anim_trs *rest, uint32_t count);
DAI_API uint32_t dai_anim_targets(const dai_anim *a);

/* Plays `clip`, fading it in over `fade` seconds while everything else fades
 * out. fade <= 0 snaps.
 *
 * Playing the clip that is ALREADY current does nothing - it keeps playing
 * rather than jumping back to frame 0. That is what makes it safe to call
 * every frame from gameplay code:
 *
 *     anim.play(speed > 0.1 ? "Run" : "Idle", 0.2);
 *
 * Use dai_anim_restart() when you mean "from the top" (a punch that must
 * replay while the previous punch is still finishing).
 *
 * Returns 1, or 0 if there is no clip by that name. */
DAI_API int dai_anim_play(dai_anim *a, const char *clip, float fade);
/* Same thing under the name the caller is thinking in. */
DAI_API int dai_anim_crossfade(dai_anim *a, const char *clip, float fade);
/* Always starts at `start` seconds, even if the clip is already current. */
DAI_API int dai_anim_restart(dai_anim *a, const char *clip, float fade, float start);
/* Everything fades out; the pose settles back to rest. */
DAI_API void dai_anim_stop(dai_anim *a, float fade);

/* Advances time, moves the fade weights, fires events, rebuilds the pose. */
DAI_API void dai_anim_update(dai_anim *a, float dt);

/* The pose as of the last update. Valid until the next one. */
DAI_API const dai_anim_trs *dai_anim_pose(const dai_anim *a, uint32_t *out_count);

DAI_API void  dai_anim_set_speed(dai_anim *a, float speed);   /* 1 = normal, negative = backwards */
DAI_API float dai_anim_speed(const dai_anim *a);
/* Playing at all - fading out counts, because the clip is still on screen. */
DAI_API int   dai_anim_is_playing(const dai_anim *a, const char *clip);
/* The clip being faded IN, or "" when nothing is playing. */
DAI_API const char *dai_anim_current(const dai_anim *a);
/* Its normalised blend weight, 0..1. The weights of everything playing sum to
 * 1 whenever anything is playing at all. */
DAI_API float dai_anim_weight(const dai_anim *a, const char *clip);
/* Current clip's playback head in seconds, and in 0..1 of its duration. */
DAI_API float dai_anim_time(const dai_anim *a);
DAI_API float dai_anim_normalized_time(const dai_anim *a);
/* Seeks. A seek does NOT fire the events it skipped over - it is a jump, not
 * playback, and a scrubbing timeline that machine guns footstep sounds is the
 * bug this avoids. Time that passes through dai_anim_update always fires. */
DAI_API void  dai_anim_set_time(dai_anim *a, float seconds);
/* Non looping clips report 1 once they reach the end and stay there. */
DAI_API int   dai_anim_finished(const dai_anim *a);

/* Events fire on the half open interval (previous, now]: an event is crossed
 * exactly once per pass, including the pass that wraps a looping clip, and a
 * dt that covers three loops fires each event three times. An event at t = 0
 * fires on the first update after the clip starts. Backwards playback fires
 * them too.
 *
 * Every playing state fires its own events, fading ones included - a foot that
 * is still on screen is still stepping. The cap is 64 firings per event per
 * update, so a dt measured in minutes cannot wedge the frame.
 *
 * A looping clip's timeline is half open, [0, duration): an event at exactly
 * the duration is the SAME instant as one at 0, and both fire when the clip
 * wraps. Put loop-boundary events at 0. */
DAI_API void dai_anim_on_event(dai_anim *a, dai_anim_event_fn fn, void *user);

/* ---- 1D blend ------------------------------------------------------------
 *
 * The one thing worth having that plain clips cannot express: a locomotion
 * blend where the parameter is speed and the pose is a mix of Idle / Walk / Run.
 * It is playable exactly like a clip, so nothing else in the API changes.
 *
 * Member clips are time SYNCHRONISED: they share a normalised playback head, so
 * a 1.2 s walk cycle and a 0.7 s run cycle stay in step instead of sliding.
 * Events come from the member with the highest weight, at its own timing. */

/* Defines (or redefines) a blend called `name` from `count` clips and their
 * thresholds along the parameter axis. Thresholds must ascend. Returns 1. */
DAI_API int dai_anim_blend1d(dai_anim *a, const char *name, const char *const *clips,
                             const float *thresholds, uint32_t count);
/* The value the blend reads. Below the first threshold or above the last, the
 * end member is used unmixed. */
DAI_API void  dai_anim_set_parameter(dai_anim *a, float value);
DAI_API float dai_anim_parameter(const dai_anim *a);
/* How much of `clip` is in the pose right now, blend members included. */
DAI_API float dai_anim_member_weight(const dai_anim *a, const char *clip);

/* ---- glTF and the skinning pipeline -------------------------------------
 *
 * Implemented in src/dai_anim_gltf.cpp - a separate translation unit so that
 * linking the player does not pull the importer (and therefore the renderer)
 * in with it. */

/* Builds clips out of everything dai_gltf_load() imported. Targets are glTF
 * NODE indices, which is what dai_model_pose_local() expects, so no mapping
 * table is needed anywhere.
 *
 * CUBICSPLINE channels are downgraded to LINEAR: the value key of each triple
 * is kept and the tangents are dropped. The motion is the same at every
 * keyframe and slightly straighter between them. Blender does not export
 * CUBICSPLINE by default; this is for files that do.
 *
 * Returns how many clips exist and fills up to `max`. The caller owns them
 * (dai_anim_add(.., own=1) is the usual answer). */
DAI_API uint32_t dai_anim_clips_from_model(const dai_model *m, dai_anim_clip **out, uint32_t max);

/* The whole thing in one call: a player sized to the model, its rest pose
 * loaded, every clip imported and owned. NULL if the model has no animations. */
DAI_API dai_anim *dai_anim_from_model(const dai_model *m);

/* Poses `model` with the player's current pose and writes the joint matrices,
 * column major, exactly like dai_model_pose(). Returns how many were written -
 * hand it straight to dai_render_joints(). */
DAI_API uint32_t dai_anim_apply(const dai_anim *a, dai_model *model,
                                float *joints, uint32_t max_joints);

#ifdef __cplusplus
}
#endif

#endif /* DAI_ANIM_H */
