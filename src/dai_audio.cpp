// Daidalos - audio backend. The only file that knows Aulos exists.
//
// The simulation never reaches this code. It emits dai_audio_event records,
// and the presentation layer (dai_present) hands them over here. That is the
// whole reason a rollback can cancel a sound: until dai_present ran, nothing
// has been heard yet.
//
// Build without Aulos by defining DAI_NO_AUDIO - every entry point becomes a
// no-op and the engine still links.

#include "daidalos.h"
#include <cstdio>
#include <cstring>

#ifndef DAI_NO_AUDIO
extern "C" {
#include "aulos.h"
}
#endif

struct dai_audio_backend {
#ifndef DAI_NO_AUDIO
    aul_system *sys = nullptr;
#endif
    int dummy = 0;
};

extern "C" {

dai_audio_backend *dai_audio_open(const char *bank, const char *asset_root,
                                  int enable_device, char *err, size_t err_len) {
#ifdef DAI_NO_AUDIO
    (void)bank; (void)asset_root; (void)enable_device;
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
    if (aul_load_bank(sys, bank) != AUL_OK) {
        if (err && err_len) std::snprintf(err, err_len, "aul_load_bank: %s", aul_last_error(sys));
        aul_destroy(sys);
        return nullptr;
    }
    dai_audio_backend *b = new dai_audio_backend();
    b->sys = sys;
    return b;
#endif
}

void dai_audio_close(dai_audio_backend *b) {
    if (!b) return;
#ifndef DAI_NO_AUDIO
    if (b->sys) aul_destroy(b->sys);
#endif
    delete b;
}

void dai_audio_play(dai_audio_backend *b, const dai_audio_event *ev) {
    if (!b || !ev) return;
#ifndef DAI_NO_AUDIO
    aul_instance inst;
    if (ev->is_3d) {
        aul_vec3 p = { ev->position.x, ev->position.y, ev->position.z };
        inst = aul_play_3d(b->sys, ev->name, p);
    } else {
        inst = aul_play(b->sys, ev->name);
    }
    if (inst == AUL_INVALID_INSTANCE) return;
    if (ev->volume > 0.0f && ev->volume != 1.0f) aul_set_volume(b->sys, inst, ev->volume);
    if (ev->pitch  > 0.0f && ev->pitch  != 1.0f) aul_set_pitch(b->sys, inst, ev->pitch);
#endif
}

void dai_audio_update(dai_audio_backend *b) {
    if (!b) return;
#ifndef DAI_NO_AUDIO
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
