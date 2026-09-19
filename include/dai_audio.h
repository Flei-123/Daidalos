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

/* ---- lifecycle ----------------------------------------------------------
 *
 * A host that is not the engine's own world - the runtime, a test, a tool -
 * opens the backend itself: bank path, the folder its samples are relative
 * to, and whether to open a device (0 on a build server). These four used to
 * be internal, which meant "only dai_engine may make a sound"; the shipped
 * game is a host like any other. */
DAI_API dai_audio_backend *dai_audio_open(const char *bank, const char *asset_root,
                                          int enable_device, char *err, size_t err_len);
/* The same, for a game that runs out of its own executable. A shipped build
 * has no "assets/audio" folder - the bank and every wav are entries in an
 * archive - so the host hands the mixer a way to fetch bytes by path instead
 * of a directory. `read` returns 1 and fills out/len; `release` frees what it
 * returned. Pass nulls and this is exactly dai_audio_open. */
typedef int  (*dai_audio_read_fn)(const char *path, void **out, size_t *len, void *user);
typedef void (*dai_audio_release_fn)(void *bytes, void *user);
DAI_API dai_audio_backend *dai_audio_open_reader(const char *bank, const char *asset_root,
                                                 int enable_device,
                                                 dai_audio_read_fn read,
                                                 dai_audio_release_fn release, void *user,
                                                 char *err, size_t err_len);
DAI_API void dai_audio_close(dai_audio_backend *b);
/* Once per frame: copies the mixer gains in and retires finished voices. */
DAI_API void dai_audio_update(dai_audio_backend *b);
/* Where the ears are. Position, forward, up, velocity - velocity only matters
 * for doppler. */
DAI_API void dai_audio_listener(dai_audio_backend *b, dai_vec3 pos, dai_vec3 fwd,
                                dai_vec3 up, dai_vec3 vel);

/* Pulls `frames` stereo frames out of the mixer into an interleaved buffer.
 * The offline path: with enable_device = 0 nothing is heard, and this is the
 * only way to get the mix - which is how a headless run proves a sound was
 * actually made. Returns the frames written. */
DAI_API uint32_t dai_audio_render(dai_audio_backend *b, float *out_stereo, uint32_t frames);
/* How many voices are alive right now. */
DAI_API uint32_t dai_audio_voices(dai_audio_backend *b);

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
