// Daidalos - internal engine state. Not part of the public ABI.
//
// Note what is NOT included here: any physics header. The engine only knows
// dai::IPhysicsBackend. See dai_physics.hpp for why that matters.
#pragma once

#include "daidalos.h"
#include "dai_physics.hpp"

#include <string>
#include <vector>
#include <cstring>

namespace dai {

// PCG32. Small, fast, and - the only property that matters here - exactly
// reproducible from a saved 128 bit state. Never use rand(): global mutable
// state is the classic way to lose determinism without noticing.
struct Rng {
    uint64_t state = 0x853c49e6748fea9bULL;
    uint64_t inc   = 0xda3e39cb94b95bdbULL;

    void seed(uint64_t s) {
        state = 0; inc = (s << 1u) | 1u;
        next(); state += 0x853c49e6748fea9bULL; next();
    }
    uint32_t next() {
        uint64_t old = state;
        state = old * 6364136223846793005ULL + inc;
        uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
        uint32_t rot = (uint32_t)(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((0u - rot) & 31u));
    }
    float next_float() { return (float)(next() >> 8) * (1.0f / 16777216.0f); }
};

// FNV-1a 64, used for the desync checksum.
struct Checksum {
    uint64_t h = 1469598103934665603ULL;
    void bytes(const void *p, size_t n) {
        const uint8_t *b = (const uint8_t *)p;
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
    }
    template <class T> void val(const T &v) { bytes(&v, sizeof(T)); }
    // Floats go in by bit pattern on purpose. A checksum that tolerates one
    // ULP is a checksum that misses the desync you actually care about.
    void f32(float v) { uint32_t u; std::memcpy(&u, &v, 4); val(u); }
};

struct BodySlot {
    bool          alive          = false;
    uint32_t      generation     = 0;
    dai_body_desc desc{};
    std::vector<dai_compound_part> parts;    // owned; desc.parts points in here

    dai_tick      created_tick   = 0;
    dai_tick      destroyed_tick = UINT64_MAX;

    // presentation only, never fed back into the simulation
    dai_vec3      prev_pos{}, cur_pos{};
    dai_quat      prev_rot{ 0, 0, 0, 1 }, cur_rot{ 0, 0, 0, 1 };
};

struct JointSlot {
    bool           alive = false;
    uint32_t       generation = 0;
    dai_joint_desc desc{};
    uint32_t       slot_a = UINT32_MAX, slot_b = UINT32_MAX;
    dai_tick       created_tick = 0;
    dai_tick       destroyed_tick = UINT64_MAX;
};

enum class CmdType : uint8_t {
    Create, Destroy, Impulse, SetVelocity, Gravity,
    CreateJoint, DestroyJoint, Motor
};

struct Command {
    dai_tick  tick = 0;
    CmdType   type = CmdType::Create;
    uint32_t  slot = 0;
    dai_vec3  a{}, b{};
    int32_t   desc_index = -1;
};

struct Snapshot {
    bool        valid = false;
    dai_tick    tick  = 0;
    std::string physics;     // opaque backend blob
    Rng         rng;
};

struct PendingAudio {
    dai_audio_event ev{};
    bool            played = false;
};

} // namespace dai

// Audio backend (dai_audio.cpp) - the only file that knows Aulos exists.
struct dai_audio_backend;
extern "C" {
dai_audio_backend *dai_audio_open(const char *bank, const char *asset_root, int enable_device, char *err, size_t err_len);
void               dai_audio_close(dai_audio_backend *);
void               dai_audio_play(dai_audio_backend *, const dai_audio_event *);
void               dai_audio_update(dai_audio_backend *);
void               dai_audio_listener(dai_audio_backend *, dai_vec3, dai_vec3, dai_vec3, dai_vec3);
uint32_t           dai_audio_voices(dai_audio_backend *);
uint32_t           dai_audio_render(dai_audio_backend *, float *, uint32_t);
} // extern "C"
