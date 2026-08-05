// Daidalos - public audio control. Aulos behind a small, stable surface.
//
// The simulation side of audio already lives in daidalos.h (dai_play,
// dai_present, dai_set_listener, dai_render_audio) and never touches this
// file. What lives here is the CONTROL side: the mixer the editor panel
// drives, bus routing when a sound is played, and following moving sources.
//
// The mixer has four fixed buses, matching the editor's audio panel:
//
//     0 = DAI_AUDIO_BUS_MASTER   the last stage everything passes through
//     1 = DAI_AUDIO_BUS_MUSIC
//     2 = DAI_AUDIO_BUS_SFX
//     3 = DAI_AUDIO_BUS_UI
//
// Effective loudness of a voice = voice volume x its bus x master.
// (Music/SFX/UI are children of master in Aulos; dai_audio_open guarantees
// the four buses exist even if the bank does not declare them.)
//
// Mute is a flag, not volume 0: the fader position survives it.
//
// All functions are no-ops on a NULL backend, and the whole file degrades to
// no-ops in a DAI_NO_AUDIO build - same contract as dai_audio.cpp itself.
#ifndef DAI_AUDIO_H
#define DAI_AUDIO_H

#include "daidalos.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dai_audio_backend dai_audio_backend;

enum {
    DAI_AUDIO_BUS_MASTER = 0,
    DAI_AUDIO_BUS_MUSIC  = 1,
    DAI_AUDIO_BUS_SFX    = 2,
    DAI_AUDIO_BUS_UI     = 3,
    DAI_AUDIO_BUS_COUNT  = 4
};

/* Use the bank's own routing for this voice (the default). */
#define DAI_AUDIO_BUS_EVENT (-1)

/* The most recently opened backend that is still open. Editor convenience:
 * the editor runs exactly one world, so this is unambiguous there. A game
 * with several worlds keeps the pointer it was given instead. */
DAI_API dai_audio_backend *dai_audio_active(void);

/* ---- mixer ------------------------------------------------------------- */

DAI_API void  dai_audio_bus_set(dai_audio_backend *b, int bus, float gain);
DAI_API float dai_audio_bus_get(dai_audio_backend *b, int bus);
DAI_API void  dai_audio_bus_mute(dai_audio_backend *b, int bus, int muted);
DAI_API int   dai_audio_bus_muted(dai_audio_backend *b, int bus);

/* Mixer source: returns the effective gain (0..1, already 0 when muted) for
 * a bus, 0..3. Bound once; dai_audio_update (which dai_present calls every
 * frame) then copies the four values into the buses. NULL unbinds.
 *
 * This is a callback, not a direct dai_editor_ui reference, on purpose:
 * dai_audio.cpp is part of the core library, which must keep linking with
 * no renderer and no editor UI. The editor binds its panel with one cast:
 *   dai_audio_bind_mixer(be, (dai_audio_mixer_fn)dai_editor_ui_bus_gain, panels); */
typedef float (*dai_audio_mixer_fn)(const void *user, int bus);
DAI_API void dai_audio_bind_mixer(dai_audio_backend *b, dai_audio_mixer_fn fn, const void *user);

/* ---- playback with a handle -------------------------------------------- */

/* Like the dai_audio_play the engine uses internally, but returns the Aulos
 * instance handle (0 = failed) and takes an explicit bus:
 *   bus == DAI_AUDIO_BUS_EVENT  -> the bank decides (normal case)
 *   bus == DAI_AUDIO_BUS_MUSIC  -> this voice is routed to the music bus
 *      ... any DAI_AUDIO_BUS_* value
 * The handle is what lets gameplay follow a moving object: keep it and call
 * dai_audio_move with the object's transform every frame. */
DAI_API uint32_t dai_audio_play_ex(dai_audio_backend *b, const dai_audio_event *ev, int bus);

/* Moves a playing 3D voice to a new world transform. No-op on stale handles
 * (the sound already finished) - safe to call blindly every frame. */
DAI_API void dai_audio_move(dai_audio_backend *b, uint32_t instance,
                            dai_vec3 position, dai_vec3 velocity);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DAI_AUDIO_H */
