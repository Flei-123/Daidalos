// Daidalos end to end example: physics drives audio, nothing opens a device,
// and the result is written to a WAV so the whole chain can be inspected.
//
//   ./build/hello_daidalos <bank.json> <asset_root> <out.wav>
//
// What it shows:
//   * a fixed 60 Hz simulation
//   * a gameplay callback that reacts to a collision-ish condition
//   * sounds emitted by the sim, played by the presentation layer
//   * offline audio rendering in lockstep with the simulation

#include "daidalos.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>

struct Game {
    dai_body ball = DAI_INVALID_BODY;
    float    last_y = 0.0f;
    int      bounces = 0;
};

static void on_tick(dai_world *w, dai_tick t, void *user) {
    Game *g = (Game *)user;
    dai_transform tr{};
    if (dai_body_get(w, g->ball, &tr) != DAI_OK) return;

    // A "bounce" is a sign change of the vertical movement near the floor.
    // Deliberately naive - the point is that the sound comes out of the sim.
    float y = tr.position.y;
    if (y < 0.7f && y > g->last_y && g->bounces < 12) {
        dai_play(w, "blip", tr.position, 1);
        g->bounces++;
        std::printf("  tick %4llu: Aufprall %d bei y=%.3f\n", (unsigned long long)t, g->bounces, y);
    }
    g->last_y = y;
}

static void write_wav(const char *path, const std::vector<float> &interleaved, uint32_t rate) {
    FILE *f = std::fopen(path, "wb");
    if (!f) { std::printf("cannot write %s\n", path); return; }
    uint32_t frames = (uint32_t)(interleaved.size() / 2);
    uint32_t data_bytes = frames * 2 * 2;    // 16 bit stereo
    uint32_t riff = 36 + data_bytes;
    uint16_t one = 1, two = 2, bits = 16;
    uint32_t fmt_size = 16, byte_rate = rate * 4;
    uint16_t block = 4;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f);
    std::fwrite("WAVE", 1, 4, f); std::fwrite("fmt ", 1, 4, f);
    std::fwrite(&fmt_size, 4, 1, f); std::fwrite(&one, 2, 1, f); std::fwrite(&two, 2, 1, f);
    std::fwrite(&rate, 4, 1, f); std::fwrite(&byte_rate, 4, 1, f);
    std::fwrite(&block, 2, 1, f); std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f); std::fwrite(&data_bytes, 4, 1, f);
    for (float s : interleaved) {
        if (s > 1.0f) s = 1.0f; if (s < -1.0f) s = -1.0f;
        int16_t v = (int16_t)lrintf(s * 32767.0f);
        std::fwrite(&v, 2, 1, f);
    }
    std::fclose(f);
}

int main(int argc, char **argv) {
    const char *bank  = argc > 1 ? argv[1] : "/root/projects/aulos/examples/test_bank.json";
    const char *root  = argc > 2 ? argv[2] : "/root/projects/aulos/assets";
    const char *out   = argc > 3 ? argv[3] : "bounce.wav";

    dai_config cfg{};
    cfg.tick_hz = 60;
    cfg.max_bodies = 256;
    cfg.physics_threads = 1;
    cfg.seed = 99;
    cfg.audio_bank = bank;
    cfg.asset_root = root;
    cfg.enable_audio_device = 0;      // offline: no device, no audio thread

    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) { std::printf("dai_create failed\n"); return 1; }
    std::printf("%s\n", dai_version());
    if (dai_last_error(w)[0]) std::printf("Hinweis: %s\n", dai_last_error(w));

    dai_body_desc floor{};
    floor.shape = DAI_SHAPE_BOX; floor.motion = DAI_STATIC;
    floor.half_extent = { 50, 0.5f, 50 }; floor.position = { 0, -0.5f, 0 };
    floor.rotation = { 0,0,0,1 }; floor.restitution = 0.6f;
    dai_body_create(w, &floor);

    Game g;
    dai_body_desc ball{};
    ball.shape = DAI_SHAPE_SPHERE; ball.motion = DAI_DYNAMIC;
    ball.half_extent = { 0.4f, 0, 0 }; ball.position = { 0, 6.0f, 0 };
    ball.rotation = { 0,0,0,1 }; ball.restitution = 0.75f; ball.no_sleeping = 1;
    g.ball = dai_body_create(w, &ball);
    g.last_y = 6.0f;

    dai_set_tick_callback(w, on_tick, &g);
    dai_set_listener(w, dai_vec3{0,1.6f,4}, dai_vec3{0,0,-1}, dai_vec3{0,1,0}, dai_vec3{0,0,0});

    // 5 seconds, audio rendered in lockstep with the simulation
    const uint32_t rate = 48000, ticks = 300;
    const uint32_t frames_per_tick = rate / 60;
    std::vector<float> pcm;
    pcm.reserve((size_t)ticks * frames_per_tick * 2);
    std::vector<float> block(frames_per_tick * 2);

    for (uint32_t i = 0; i < ticks; ++i) {
        dai_step(w);
        dai_present(w);                                   // hands the events to Aulos
        std::memset(block.data(), 0, block.size() * sizeof(float));
        dai_render_audio(w, block.data(), frames_per_tick);
        pcm.insert(pcm.end(), block.begin(), block.end());
    }

    double peak = 0, energy = 0;
    for (float s : pcm) { peak = std::max(peak, (double)fabsf(s)); energy += (double)s * s; }
    std::printf("Aufprall-Ereignisse: %d\n", g.bounces);
    std::printf("Audio: %zu Frames, Peak %.4f, RMS %.5f\n", pcm.size() / 2, peak, sqrt(energy / pcm.size()));

    dai_stats st{}; dai_get_stats(w, &st);
    std::printf("Sim: %llu Ticks, %.4f ms/Tick, %u Bodies\n",
        (unsigned long long)st.ticks_simulated, st.avg_step_ms, st.bodies);

    write_wav(out, pcm, rate);
    std::printf("geschrieben: %s\n", out);
    dai_destroy(w);
    return peak > 0.0 ? 0 : 2;
}
