/*
 * Daidalos particles.
 *
 * Presentation, not simulation - and that is a design decision, not a
 * shortcut. Sparks, smoke and dust do not belong in the deterministic tick:
 * they would have to be snapshotted, rolled back and re-simulated, which
 * multiplies the cost of every rollback by the number of pretty effects in the
 * frame. They also must not be able to desync a multiplayer session.
 *
 * So particles live here: fed by the same events the audio layer consumes,
 * updated with wall clock delta time, drawn as instanced billboards. A rolled
 * back explosion cancels its sound and its sparks the same way, because both
 * hang off the same cancelled event.
 *
 * When an effect DOES need to affect gameplay (a crate that must actually be
 * pushed), it is not a particle - it is a body.
 *
 * Each emitter owns a seeded RNG, so a replay looks identical even though the
 * particles never touch the simulation.
 */
#ifndef DAI_PARTICLES_H
#define DAI_PARTICLES_H

#include "dai_render.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dai_particles dai_particles;
typedef uint32_t dai_emitter;
#define DAI_INVALID_EMITTER ((dai_emitter)0xFFFFFFFFu)

typedef enum dai_particle_blend {
    DAI_BLEND_ALPHA = 0,   /* smoke, dust, debris                       */
    DAI_BLEND_ADD          /* fire, sparks, magic: brightens what is behind */
} dai_particle_blend;

typedef struct dai_emitter_desc {
    dai_vec3 position;
    dai_vec3 direction;        /* 0,0,0 -> straight up                      */
    float    spread_deg;       /* cone half angle around direction          */
    float    rate;             /* particles per second, 0 -> bursts only    */
    float    lifetime;         /* seconds, 0 -> 1                           */
    float    lifetime_jitter;  /* 0..1, fraction of lifetime                */
    float    speed;
    float    speed_jitter;     /* 0..1                                      */
    float    gravity;          /* multiplier on -9.81 m/s^2                 */
    float    drag;             /* per second velocity damping               */
    float    size_start, size_end;
    dai_vec3 color_start, color_end;
    float    alpha_start, alpha_end;
    float    spin;             /* radians per second                        */
    int      blend;            /* dai_particle_blend                        */
    uint32_t max_particles;    /* 0 -> derived from rate * lifetime         */
    uint32_t atlas_frames;     /* 0/1 -> no atlas animation                 */
    int      atlas_animate;    /* 1 -> walk the frames over the lifetime,
                                  0 -> one random frame per particle        */
    uint32_t seed;
    int      inherit_velocity; /* 1 -> new particles start with emitter velocity */
} dai_emitter_desc;

DAI_API dai_emitter_desc dai_emitter_desc_default(void);

DAI_API dai_particles *dai_particles_create(uint32_t max_particles);
DAI_API void           dai_particles_destroy(dai_particles *p);

DAI_API dai_emitter dai_particles_add(dai_particles *p, const dai_emitter_desc *desc);
DAI_API void        dai_particles_remove(dai_particles *p, dai_emitter e);
DAI_API void        dai_particles_move(dai_particles *p, dai_emitter e, dai_vec3 position);
DAI_API void        dai_particles_enable(dai_particles *p, dai_emitter e, int on);
/* One off puff: count particles at once, at the emitter's current position. */
DAI_API void        dai_particles_burst(dai_particles *p, dai_emitter e, uint32_t count);
/* Fire and forget burst without keeping an emitter around. */
DAI_API void        dai_particles_burst_at(dai_particles *p, const dai_emitter_desc *desc,
                                           dai_vec3 position, uint32_t count);

/* Advances every live particle. dt is wall clock time: this is presentation. */
DAI_API void     dai_particles_update(dai_particles *p, float dt);
DAI_API uint32_t dai_particles_count(const dai_particles *p);
DAI_API uint32_t dai_particles_capacity(const dai_particles *p);
/* Drops everything, e.g. after a rollback cancelled the events that spawned them. */
DAI_API void     dai_particles_clear(dai_particles *p);

/* Writes render instances sorted back to front from the camera, which is what
 * alpha blending needs. Returns how many were written. */
DAI_API uint32_t dai_particles_fill(dai_particles *p, dai_particle *out, uint32_t max, dai_vec3 camera);

#ifdef __cplusplus
}
#endif

#endif /* DAI_PARTICLES_H */
