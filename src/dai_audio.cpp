// Daidalos - audio backend. The only file that knows Aulos exists.
//
// The simulation never reaches this code. It emits dai_audio_event records,
// and the presentation layer (dai_present) hands them over here. That is the
// whole reason a rollback can cancel a sound: until dai_present ran, nothing
// has been heard yet.
//
// The mixer works like this: every voice sits on exactly one bus (the bank
// decides, or dai_audio_play_ex overrides it). Music/SFX/UI feed master,
// master feeds the device. A voice's loudness is voice x bus x master, and
// muting a bus is a flag - the fader position survives it.
//
// Build without Aulos by defining DAI_NO_AUDIO - every entry point becomes a
// no-op and the engine still links.

#include "daidalos.h"
#include "dai_audio.h"
#include <cstdio>
#include <cstring>

#ifndef DAI_NO_AUDIO
extern "C" {
#include "aulos.h"
}
#endif

/* Bus index <-> Aulos bus name. The order is fixed by the editor panel. */
static const char *const kBusNames[DAI_AUDIO_BUS_COUNT] = {
    "master", "music", "sfx", "ui"
};

static int dai_bus_valid(int bus) { return bus >= 0 && bus < DAI_AUDIO_BUS_COUNT; }

struct dai_audio_backend {
#ifndef DAI_NO_AUDIO
    aul_system *sys = nullptr;
#endif
    dai_audio_mixer_fn mixer_fn = nullptr;  /* bound mixer source, may be null */
    const void *mixer_user = nullptr;
    int dummy = 0;
};

static dai_audio_backend *g_active = nullptr;

extern "C" {

dai_audio_backend *dai_audio_active(void) { return g_active; }

dai_audio_backend *dai_audio_open_reader(const char *bank, const char *asset_root,
                                        int enable_device,
                                        dai_audio_read_fn read_fn,
                                        dai_audio_release_fn release_fn, void *user,
                                        char *err, size_t err_len) {
#ifdef DAI_NO_AUDIO
    (void)bank; (void)asset_root; (void)enable_device;
    (void)read_fn; (void)release_fn; (void)user;
    if (err && err_len) std::snprintf(err, err_len, "built without audio (DAI_NO_AUDIO)");
    return nullptr;
#else
    aul_config cfg = {};
    cfg.sample_rate   = 48000;
    cfg.max_voices    = 128;
    cfg.enable_device = enable_device;
    cfg.asset_root    = asset_root;

    aul_system *sys = nullptr;
    if (aul_create(&cfg, &sys) != AUL_OK || sys == nullptr) {
        if (err && err_len) std::snprintf(err, err_len, "aul_create failed");
        return nullptr;
    }
    /* Before the bank is read, not after: the bank IS the first file. */
    if (read_fn) aul_set_reader(sys, (aul_read_fn)read_fn, (aul_release_fn)release_fn, user);
    if (aul_load_bank(sys, bank) != AUL_OK) {
        if (err && err_len) std::snprintf(err, err_len, "aul_load_bank: %s", aul_last_error(sys));
        aul_destroy(sys);
        return nullptr;
    }
    /* The four mixer buses must exist even if the bank is minimal: music,
     * sfx and ui as children of master, so the panel's master fader scales
     * everything and the per-bus faders scale their own branch. A bank that
     * declares them differently wins - ensure never overwrites. */
    aul_ensure_bus(sys, "master", nullptr);
    aul_ensure_bus(sys, "music", "master");
    aul_ensure_bus(sys, "sfx", "master");
    aul_ensure_bus(sys, "ui", "master");

    dai_audio_backend *b = new dai_audio_backend();
    b->sys = sys;
    g_active = b;
    return b;
#endif
}

dai_audio_backend *dai_audio_open(const char *bank, const char *asset_root,
                                  int enable_device, char *err, size_t err_len) {
    return dai_audio_open_reader(bank, asset_root, enable_device,
                                 nullptr, nullptr, nullptr, err, err_len);
}

void dai_audio_close(dai_audio_backend *b) {
    if (!b) return;
#ifndef DAI_NO_AUDIO
    if (b->sys) aul_destroy(b->sys);
#endif
    if (g_active == b) g_active = nullptr;
    delete b;
}

uint32_t dai_audio_play_ex(dai_audio_backend *b, const dai_audio_event *ev, int bus) {
    if (!b || !ev) return 0;
#ifdef DAI_NO_AUDIO
    (void)bus;
    return 0;
#else
    const char *bus_name = dai_bus_valid(bus) ? kBusNames[bus] : nullptr;
    aul_instance inst;
    if (ev->is_3d) {
        aul_vec3 p = { ev->position.x, ev->position.y, ev->position.z };
        inst = aul_play_3d_on_bus(b->sys, ev->name, p, bus_name);
    } else {
        inst = aul_play_on_bus(b->sys, ev->name, bus_name);
    }
    if (inst == AUL_INVALID_INSTANCE) return 0;
    if (ev->volume > 0.0f && ev->volume != 1.0f) aul_set_volume(b->sys, inst, ev->volume);
    if (ev->pitch  > 0.0f && ev->pitch  != 1.0f) aul_set_pitch(b->sys, inst, ev->pitch);
    return inst;
#endif
}

void dai_audio_play(dai_audio_backend *b, const dai_audio_event *ev) {
    dai_audio_play_ex(b, ev, DAI_AUDIO_BUS_EVENT);
}

void dai_audio_move(dai_audio_backend *b, uint32_t instance, dai_vec3 position, dai_vec3 velocity) {
    if (!b || instance == 0) return;
#ifndef DAI_NO_AUDIO
    aul_vec3 p = { position.x, position.y, position.z };
    aul_vec3 v = { velocity.x, velocity.y, velocity.z };
    aul_set_position(b->sys, instance, p, v);
#else
    (void)position; (void)velocity;
#endif
}

void dai_audio_bus_set(dai_audio_backend *b, int bus, float gain) {
    if (!b || !dai_bus_valid(bus)) return;
#ifndef DAI_NO_AUDIO
    aul_set_bus_volume(b->sys, kBusNames[bus], gain);
#else
    (void)gain;
#endif
}

float dai_audio_bus_get(dai_audio_backend *b, int bus) {
    if (!b || !dai_bus_valid(bus)) return 0.0f;
#ifndef DAI_NO_AUDIO
    return aul_get_bus_volume(b->sys, kBusNames[bus]);
#else
    return 0.0f;
#endif
}

void dai_audio_bus_mute(dai_audio_backend *b, int bus, int muted) {
    if (!b || !dai_bus_valid(bus)) return;
#ifndef DAI_NO_AUDIO
    aul_set_bus_mute(b->sys, kBusNames[bus], muted);
#else
    (void)muted;
#endif
}

int dai_audio_bus_muted(dai_audio_backend *b, int bus) {
    if (!b || !dai_bus_valid(bus)) return 0;
#ifndef DAI_NO_AUDIO
    return aul_get_bus_mute(b->sys, kBusNames[bus]);
#else
    return 0;
#endif
}

void dai_audio_bind_mixer(dai_audio_backend *b, dai_audio_mixer_fn fn, const void *user) {
    if (!b) return;
    b->mixer_fn = fn;
    b->mixer_user = user;
}

void dai_audio_update(dai_audio_backend *b) {
    if (!b) return;
#ifndef DAI_NO_AUDIO
    /* A bound mixer wins over anything set through dai_audio_bus_set: it is
     * the user's hand on the faders, refreshed once per frame. The source
     * reports 0 for a muted bus, which lands as a plain fader value - the
     * Aulos mute flag stays for hosts without a mixer. */
    if (b->mixer_fn) {
        for (int bus = 0; bus < DAI_AUDIO_BUS_COUNT; ++bus)
            aul_set_bus_volume(b->sys, kBusNames[bus],
                               b->mixer_fn(b->mixer_user, bus));
    }
    aul_update(b->sys);
#endif
}

void dai_audio_listener(dai_audio_backend *b, dai_vec3 pos, dai_vec3 fwd, dai_vec3 up, dai_vec3 vel) {
    if (!b) return;
#ifndef DAI_NO_AUDIO
    aul_vec3 p = { pos.x, pos.y, pos.z }, f = { fwd.x, fwd.y, fwd.z };
    aul_vec3 u = { up.x, up.y, up.z },    v = { vel.x, vel.y, vel.z };
    aul_set_listener(b->sys, p, f, u, v);
#endif
}

uint32_t dai_audio_voices(dai_audio_backend *b) {
    if (!b) return 0;
#ifndef DAI_NO_AUDIO
    aul_stats s = {};
    aul_get_stats(b->sys, &s);
    return s.active_voices;
#else
    return 0;
#endif
}

// Offline render passthrough - used by the tests to prove that audio is
// produced without ever opening a device.
uint32_t dai_audio_render(dai_audio_backend *b, float *out_stereo, uint32_t frames) {
#ifdef DAI_NO_AUDIO
    (void)b; (void)out_stereo; (void)frames; return 0;
#else
    if (!b || !out_stereo) return 0;
    aul_render(b->sys, out_stereo, frames);
    return frames;
#endif
}

} // extern "C"
