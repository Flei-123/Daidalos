// Audio tests: buses, mute, 3D attenuation, panning - all offline.
//
// Runs with no sound card: the backend is opened with enable_device = 0 and
// samples are pulled out through dai_audio_render. Numbers are asserted, not
// eyeballed: the reference signal is dc_half.wav, a constant 0.5 DC signal,
// so a mean absolute value IS the gain the voice went through (times the
// constant-power centre pan, which cancels in every ratio used here).

#include "dai_audio.h"
#include "dai_internal.hpp"   // dai_audio_open/render/listener/update

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::printf("  FAIL %s:%d  ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

static const char *kBank = "/root/projects/aulos/examples/test_bank.json";
static const char *kAssets = "/root/projects/aulos/assets";

static dai_audio_backend *open_backend() {
    char err[256] = { 0 };
    dai_audio_backend *b = dai_audio_open(kBank, kAssets, 0, err, sizeof(err));
    if (!b) std::printf("  open failed: %s\n", err);
    return b;
}

static void set_event(dai_audio_event *ev, const char *name, float x, float y, float z, int is_3d) {
    std::memset(ev, 0, sizeof(*ev));
    std::snprintf(ev->name, sizeof(ev->name), "%s", name);
    ev->position = { x, y, z };
    ev->volume = 1.0f;
    ev->pitch = 1.0f;
    ev->is_3d = is_3d;
}

// Renders frames, returns the mean absolute value of one channel (0 = left).
static float render_level(dai_audio_backend *b, int channel) {
    const uint32_t frames = 4096;
    std::vector<float> buf(frames * 2);
    dai_audio_render(b, buf.data(), frames);
    double acc = 0.0;
    for (uint32_t n = 0; n < frames; ++n) acc += std::fabs(buf[n * 2 + channel]);
    return (float)(acc / frames);
}

static float render_peak(dai_audio_backend *b) {
    const uint32_t frames = 2048;
    std::vector<float> buf(frames * 2);
    dai_audio_render(b, buf.data(), frames);
    float peak = 0.0f;
    for (float s : buf) peak = std::max(peak, std::fabs(s));
    return peak;
}

// Throws away a few blocks so bus/voice gain ramps have settled.
static void warm_up(dai_audio_backend *b) {
    float scratch[512 * 2];
    for (int i = 0; i < 4; ++i) dai_audio_render(b, scratch, 512);
}

static void listener_default(dai_audio_backend *b) {
    dai_audio_listener(b, { 0, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 }, { 0, 0, 0 });
}

int main() {
    std::printf("[open]\n");
    {
        dai_audio_backend *b = open_backend();
        CHECK(b != nullptr, "backend opens offline");
        CHECK(dai_audio_active() == b, "dai_audio_active tracks the open backend");
        if (!b) return 1;
        /* the four mixer buses exist even though the bank only declares
         * master/sfx/music - dai_audio_open ensures them */
        for (int bus = 0; bus < DAI_AUDIO_BUS_COUNT; ++bus)
            CHECK(dai_audio_bus_get(b, bus) == 1.0f, "bus %d defaults to gain 1.0", bus);
        CHECK(dai_audio_bus_get(b, 9) == 0.0f, "out of range bus reads 0");
        dai_audio_close(b);
        CHECK(dai_audio_active() == nullptr, "dai_audio_active clears on close");
        dai_audio_close(nullptr);
    }

    std::printf("[bus gain is multiplicative]\n");
    {
        dai_audio_backend *b = open_backend();
        dai_audio_event ev;
        set_event(&ev, "dc", 0, 0, 0, 0);   // constant 0.5, on the sfx bus

        dai_audio_play(b, &ev);
        warm_up(b);
        float base = render_level(b, 0);
        CHECK(base > 0.3f && base < 0.4f, "unity gain level ~0.354 (0.5 DC x centre pan), got %.4f", base);

        dai_audio_bus_set(b, DAI_AUDIO_BUS_SFX, 0.5f);
        CHECK(dai_audio_bus_get(b, DAI_AUDIO_BUS_SFX) == 0.5f, "bus_get roundtrip");
        warm_up(b);
        float half = render_level(b, 0);
        CHECK(std::fabs(half / base - 0.5f) < 0.01f, "sfx 0.5 halves the level: ratio %.4f", half / base);

        dai_audio_bus_set(b, DAI_AUDIO_BUS_MASTER, 0.5f);
        warm_up(b);
        float quarter = render_level(b, 0);
        CHECK(std::fabs(quarter / base - 0.25f) < 0.01f, "master x sfx = 0.25: ratio %.4f", quarter / base);

        dai_audio_close(b);
    }

    std::printf("[mute is exact silence, fader survives]\n");
    {
        dai_audio_backend *b = open_backend();
        dai_audio_event ev;
        set_event(&ev, "dc", 0, 0, 0, 0);
        dai_audio_play(b, &ev);
        warm_up(b);

        dai_audio_bus_mute(b, DAI_AUDIO_BUS_SFX, 1);
        CHECK(dai_audio_bus_muted(b, DAI_AUDIO_BUS_SFX) == 1, "muted flag reads back");
        warm_up(b);
        float peak = render_peak(b);
        CHECK(peak == 0.0f, "muted bus renders bit exact silence, peak %.8f", peak);
        CHECK(dai_audio_bus_get(b, DAI_AUDIO_BUS_SFX) == 1.0f, "fader position survives mute");

        dai_audio_bus_mute(b, DAI_AUDIO_BUS_SFX, 0);
        warm_up(b);
        CHECK(render_peak(b) > 0.3f, "unmute restores the signal");
        dai_audio_close(b);
    }

    std::printf("[distance attenuation, inverse rolloff min 1 m]\n");
    {
        // dc_3d: inverse rolloff, min 1, max 100 -> attenuation = 1/d beyond 1 m
        float level[3];
        const float dist[3] = { 1.0f, 10.0f, 50.0f };
        for (int i = 0; i < 3; ++i) {
            dai_audio_backend *b = open_backend();
            listener_default(b);
            dai_audio_event ev;
            set_event(&ev, "dc_3d", 0, 0, -dist[i], 1);
            dai_audio_play(b, &ev);
            warm_up(b);
            level[i] = render_level(b, 0);
            dai_audio_close(b);
        }
        CHECK(level[0] > level[1] && level[1] > level[2],
              "monotonic falloff: %.4f > %.4f > %.4f", level[0], level[1], level[2]);
        CHECK(std::fabs(level[1] / level[0] - 0.1f) < 0.005f,
              "10 m is 1/10 of 1 m: ratio %.4f", level[1] / level[0]);
        CHECK(std::fabs(level[2] / level[0] - 0.02f) < 0.002f,
              "50 m is 1/50 of 1 m: ratio %.4f", level[2] / level[0]);
    }

    std::printf("[linear rolloff reaches zero at max_distance]\n");
    {
        // dc_3d_linear: min 1, max 11, linear -> at 11 m exactly 0
        dai_audio_backend *b = open_backend();
        listener_default(b);
        dai_audio_event ev;
        set_event(&ev, "dc_3d_linear", 0, 0, -11.0f, 1);
        dai_audio_play(b, &ev);
        warm_up(b);
        CHECK(render_peak(b) == 0.0f, "silent at max_distance");
        dai_audio_close(b);
    }

    std::printf("[panning moves energy between channels]\n");
    {
        // right = cross(forward, up) = +x with the default listener
        dai_audio_backend *b = open_backend();
        listener_default(b);
        dai_audio_event ev;
        set_event(&ev, "dc_3d", 5, 0, 0, 1);   // hard right
        dai_audio_play(b, &ev);
        warm_up(b);
        float right_l = render_level(b, 0);
        float right_r = render_level(b, 1);
        CHECK(right_r > 0.05f && right_l < 1e-5f,
              "source at +x: right %.4f, left %.7f", right_r, right_l);
        dai_audio_close(b);

        b = open_backend();
        listener_default(b);
        set_event(&ev, "dc_3d", -5, 0, 0, 1);  // hard left
        dai_audio_play(b, &ev);
        warm_up(b);
        float left_l = render_level(b, 0);
        float left_r = render_level(b, 1);
        CHECK(left_l > 0.05f && left_r < 1e-5f,
              "source at -x: left %.4f, right %.7f", left_l, left_r);
        dai_audio_close(b);

        b = open_backend();
        listener_default(b);
        set_event(&ev, "dc_3d", 0, 0, -5, 1);  // dead centre
        dai_audio_play(b, &ev);
        warm_up(b);
        float cl = render_level(b, 0), cr = render_level(b, 1);
        CHECK(std::fabs(cl - cr) < 1e-4f, "centre source is balanced: %.5f vs %.5f", cl, cr);
        dai_audio_close(b);
    }

    std::printf("[play_ex: bus override + moving a voice]\n");
    {
        dai_audio_backend *b = open_backend();
        listener_default(b);
        dai_audio_event ev;

        // "dc" lives on sfx in the bank; routing it to music must make the
        // sfx fader irrelevant and the music fader decisive
        set_event(&ev, "dc", 0, 0, 0, 0);
        uint32_t h = dai_audio_play_ex(b, &ev, DAI_AUDIO_BUS_MUSIC);
        CHECK(h != 0, "play_ex returns a handle");
        dai_audio_bus_mute(b, DAI_AUDIO_BUS_SFX, 1);
        warm_up(b);
        CHECK(render_peak(b) > 0.3f, "overridden voice ignores the sfx mute");
        dai_audio_bus_mute(b, DAI_AUDIO_BUS_MUSIC, 1);
        warm_up(b);
        CHECK(render_peak(b) == 0.0f, "music mute silences the overridden voice");
        dai_audio_close(b);

        // fresh backend: the looping dc voice above would swamp the 3D level
        b = open_backend();
        listener_default(b);
        set_event(&ev, "dc_3d", 0, 0, -1, 1);
        uint32_t m = dai_audio_play_ex(b, &ev, DAI_AUDIO_BUS_EVENT);
        CHECK(m != 0, "3d play_ex returns a handle");
        warm_up(b);
        float near_level = render_level(b, 0);
        dai_audio_move(b, m, { 0, 0, -10 }, { 0, 0, 0 });
        warm_up(b);
        float far_level = render_level(b, 0);
        CHECK(std::fabs(far_level / near_level - 0.1f) < 0.01f,
              "moved 1 m -> 10 m: %.4f -> %.4f (ratio %.4f)",
              near_level, far_level, far_level / near_level);
        dai_audio_move(b, m + 999, { 0, 0, 0 }, { 0, 0, 0 });  // stale handle: no crash
        dai_audio_close(b);
    }

    std::printf("[mixer binding drives the buses every update]\n");
    {
        dai_audio_backend *b = open_backend();
        dai_audio_update(b);                    // no mixer bound: must not crash

        // a fake panel: music at 25 %, everything else full
        static float gains[DAI_AUDIO_BUS_COUNT] = { 1.0f, 0.25f, 1.0f, 1.0f };
        dai_audio_bind_mixer(b, [](const void *user, int bus) -> float {
            return ((const float *)user)[bus];
        }, gains);
        dai_audio_update(b);                    // applies the faders once
        CHECK(dai_audio_bus_get(b, DAI_AUDIO_BUS_MUSIC) == 0.25f,
              "mixer set music to %.2f", dai_audio_bus_get(b, DAI_AUDIO_BUS_MUSIC));

        dai_audio_event ev;
        set_event(&ev, "dc_on_music", 0, 0, 0, 0);   // bank routes it to music
        dai_audio_play(b, &ev);
        warm_up(b);
        float mixed = render_level(b, 0);
        dai_audio_bind_mixer(b, nullptr, nullptr);   // unbind: fader stays where it was
        CHECK(mixed > 0.05f && mixed < 0.15f,
              "music at 25%% renders %.4f (unity would be ~0.354)", mixed);
        dai_audio_close(b);
    }

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
