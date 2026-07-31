// Particle simulation. Pure C++, no Vulkan, no engine dependency: it produces
// dai_particle structs and nothing else, so it works with any renderer backend
// and can be unit tested without a GPU.

#include "dai_particles.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

// Same PCG the simulation uses. Per emitter, so effects replay identically
// without ever touching the deterministic tick.
struct Rng {
    uint64_t state = 0x853c49e6748fea9bULL, inc = 0xda3e39cb94b95bdbULL;
    explicit Rng(uint32_t seed = 0) { state = 0x853c49e6748fea9bULL ^ ((uint64_t)seed * 6364136223846793005ULL + 1); next(); }
    uint32_t next() {
        uint64_t old = state;
        state = old * 6364136223846793005ULL + (inc | 1);
        uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
        uint32_t rot = (uint32_t)(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31));
    }
    float unit() { return (float)(next() >> 8) * (1.0f / 16777216.0f); }      // [0,1)
    float sym()  { return unit() * 2.0f - 1.0f; }                             // [-1,1)
};

struct Particle {
    uint32_t frame0;
    float px, py, pz;
    float vx, vy, vz;
    float age, life;
    float rot, spin;
    uint32_t emitter;
};

struct Emitter {
    dai_emitter_desc d{};
    Rng rng;
    float accum = 0.0f;
    bool  active = true;
    bool  alive = true;
    float last_x = 0, last_y = 0, last_z = 0;
    float vel_x = 0, vel_y = 0, vel_z = 0;
    Emitter() : rng(0) {}
};

dai_vec3 lerp3(dai_vec3 a, dai_vec3 b, float t) {
    return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
}

} // namespace

struct dai_particles {
    std::vector<Particle> live;
    std::vector<Emitter>  emitters;
    uint32_t cap = 4096;
    std::vector<uint32_t> order;      // scratch for depth sorting
};

extern "C" {

dai_emitter_desc dai_emitter_desc_default(void) {
    dai_emitter_desc d{};
    d.direction = { 0, 1, 0 };
    d.spread_deg = 25.0f;
    d.rate = 60.0f;
    d.lifetime = 1.2f;
    d.lifetime_jitter = 0.3f;
    d.speed = 3.0f;
    d.speed_jitter = 0.35f;
    d.gravity = 1.0f;
    d.drag = 0.4f;
    d.size_start = 0.25f;
    d.size_end = 0.05f;
    d.color_start = { 1.0f, 0.75f, 0.35f };
    d.color_end = { 0.6f, 0.15f, 0.05f };
    d.alpha_start = 1.0f;
    d.alpha_end = 0.0f;
    d.spin = 1.0f;
    d.blend = DAI_BLEND_ALPHA;
    d.seed = 1;
    return d;
}

dai_particles *dai_particles_create(uint32_t max_particles) {
    dai_particles *p = new dai_particles();
    p->cap = max_particles ? max_particles : 4096;
    p->live.reserve(p->cap);
    return p;
}

void dai_particles_destroy(dai_particles *p) { delete p; }

dai_emitter dai_particles_add(dai_particles *p, const dai_emitter_desc *desc) {
    if (!p || !desc) return DAI_INVALID_EMITTER;
    Emitter e;
    e.d = *desc;
    if (e.d.lifetime <= 0.0f) e.d.lifetime = 1.0f;
    if (e.d.direction.x == 0 && e.d.direction.y == 0 && e.d.direction.z == 0) e.d.direction = { 0, 1, 0 };
    e.rng = Rng(desc->seed);
    e.last_x = e.d.position.x; e.last_y = e.d.position.y; e.last_z = e.d.position.z;
    for (uint32_t i = 0; i < p->emitters.size(); ++i)
        if (!p->emitters[i].alive) { p->emitters[i] = e; return i; }
    p->emitters.push_back(e);
    return (dai_emitter)(p->emitters.size() - 1);
}

void dai_particles_remove(dai_particles *p, dai_emitter e) {
    if (!p || e >= p->emitters.size()) return;
    p->emitters[e].alive = false;
    p->emitters[e].active = false;
}

void dai_particles_move(dai_particles *p, dai_emitter e, dai_vec3 pos) {
    if (!p || e >= p->emitters.size()) return;
    p->emitters[e].d.position = pos;
}

void dai_particles_enable(dai_particles *p, dai_emitter e, int on) {
    if (!p || e >= p->emitters.size()) return;
    p->emitters[e].active = on != 0;
}

static void spawn_one(dai_particles *p, Emitter &e, uint32_t emitter_index) {
    if (p->live.size() >= p->cap) return;

    // direction inside a cone: pick a random tilt, then a random roll around
    // the emitter axis. Uniform enough for effects, and cheap.
    dai_vec3 dir = e.d.direction;
    float len = std::sqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
    if (len > 1e-6f) { dir.x/=len; dir.y/=len; dir.z/=len; }
    dai_vec3 up = (std::fabs(dir.y) > 0.99f) ? dai_vec3{ 1, 0, 0 } : dai_vec3{ 0, 1, 0 };
    dai_vec3 right{ up.y*dir.z - up.z*dir.y, up.z*dir.x - up.x*dir.z, up.x*dir.y - up.y*dir.x };
    float rl = std::sqrt(right.x*right.x + right.y*right.y + right.z*right.z);
    if (rl > 1e-6f) { right.x/=rl; right.y/=rl; right.z/=rl; }
    dai_vec3 up2{ dir.y*right.z - dir.z*right.y, dir.z*right.x - dir.x*right.z, dir.x*right.y - dir.y*right.x };

    float spread = e.d.spread_deg * 3.14159265f / 180.0f;
    float tilt = spread * std::sqrt(e.rng.unit());
    float roll = e.rng.unit() * 6.2831853f;
    float st = std::sin(tilt), ct = std::cos(tilt);
    dai_vec3 d{
        dir.x * ct + (right.x * std::cos(roll) + up2.x * std::sin(roll)) * st,
        dir.y * ct + (right.y * std::cos(roll) + up2.y * std::sin(roll)) * st,
        dir.z * ct + (right.z * std::cos(roll) + up2.z * std::sin(roll)) * st,
    };

    float speed = e.d.speed * (1.0f + e.d.speed_jitter * e.rng.sym());
    Particle q{};
    q.px = e.d.position.x; q.py = e.d.position.y; q.pz = e.d.position.z;
    q.vx = d.x * speed; q.vy = d.y * speed; q.vz = d.z * speed;
    if (e.d.inherit_velocity) { q.vx += e.vel_x; q.vy += e.vel_y; q.vz += e.vel_z; }
    q.life = e.d.lifetime * (1.0f + e.d.lifetime_jitter * e.rng.sym());
    if (q.life <= 0.01f) q.life = 0.01f;
    q.age = 0.0f;
    q.rot = e.rng.unit() * 6.2831853f;
    q.spin = e.d.spin * e.rng.sym();
    q.emitter = emitter_index;
    q.frame0 = e.d.atlas_frames > 1 ? (e.rng.next() % e.d.atlas_frames) : 0;
    p->live.push_back(q);
}

void dai_particles_burst(dai_particles *p, dai_emitter e, uint32_t count) {
    if (!p || e >= p->emitters.size() || !p->emitters[e].alive) return;
    for (uint32_t i = 0; i < count; ++i) spawn_one(p, p->emitters[e], e);
}

void dai_particles_burst_at(dai_particles *p, const dai_emitter_desc *desc, dai_vec3 position, uint32_t count) {
    if (!p || !desc) return;
    dai_emitter_desc d = *desc;
    d.position = position;
    d.rate = 0.0f;
    dai_emitter e = dai_particles_add(p, &d);
    if (e == DAI_INVALID_EMITTER) return;
    dai_particles_burst(p, e, count);
    // the emitter itself is not needed any more, but the particles keep a copy
    // of the index, so it stays alive and merely stops emitting
    p->emitters[e].active = false;
}

void dai_particles_update(dai_particles *p, float dt) {
    if (!p || dt <= 0.0f) return;
    if (dt > 0.25f) dt = 0.25f;                  // a stalled frame must not teleport everything

    for (uint32_t i = 0; i < p->emitters.size(); ++i) {
        Emitter &e = p->emitters[i];
        if (!e.alive) continue;
        e.vel_x = (e.d.position.x - e.last_x) / dt;
        e.vel_y = (e.d.position.y - e.last_y) / dt;
        e.vel_z = (e.d.position.z - e.last_z) / dt;
        e.last_x = e.d.position.x; e.last_y = e.d.position.y; e.last_z = e.d.position.z;
        if (!e.active || e.d.rate <= 0.0f) continue;
        e.accum += e.d.rate * dt;
        while (e.accum >= 1.0f) { e.accum -= 1.0f; spawn_one(p, e, i); }
    }

    const float G = -9.81f;
    for (size_t i = 0; i < p->live.size(); ) {
        Particle &q = p->live[i];
        const Emitter &e = p->emitters[q.emitter];
        q.age += dt;
        if (q.age >= q.life) {
            p->live[i] = p->live.back();        // swap remove: order does not matter,
            p->live.pop_back();                 // the fill step sorts anyway
            continue;
        }
        float damp = 1.0f - e.d.drag * dt;
        if (damp < 0.0f) damp = 0.0f;
        q.vy += G * e.d.gravity * dt;
        q.vx *= damp; q.vy *= damp; q.vz *= damp;
        q.px += q.vx * dt; q.py += q.vy * dt; q.pz += q.vz * dt;
        q.rot += q.spin * dt;
        ++i;
    }
}

uint32_t dai_particles_count(const dai_particles *p) { return p ? (uint32_t)p->live.size() : 0; }
uint32_t dai_particles_capacity(const dai_particles *p) { return p ? p->cap : 0; }
void dai_particles_clear(dai_particles *p) { if (p) p->live.clear(); }

uint32_t dai_particles_fill(dai_particles *p, dai_particle *out, uint32_t max, dai_vec3 cam) {
    if (!p || !out || !max) return 0;
    uint32_t n = (uint32_t)std::min<size_t>(p->live.size(), max);

    // sort back to front: alpha blended sprites are order dependent, and the
    // artefact when you skip this ("the smoke in front vanished") is the kind
    // of bug that gets blamed on the renderer for a week
    p->order.resize(p->live.size());
    for (uint32_t i = 0; i < p->order.size(); ++i) p->order[i] = i;
    std::sort(p->order.begin(), p->order.end(), [&](uint32_t a, uint32_t b) {
        const Particle &qa = p->live[a], &qb = p->live[b];
        float da = (qa.px-cam.x)*(qa.px-cam.x) + (qa.py-cam.y)*(qa.py-cam.y) + (qa.pz-cam.z)*(qa.pz-cam.z);
        float db = (qb.px-cam.x)*(qb.px-cam.x) + (qb.py-cam.y)*(qb.py-cam.y) + (qb.pz-cam.z)*(qb.pz-cam.z);
        return da > db;
    });

    for (uint32_t i = 0; i < n; ++i) {
        const Particle &q = p->live[p->order[i]];
        const dai_emitter_desc &d = p->emitters[q.emitter].d;
        float t = q.age / q.life;
        dai_particle &o = out[i];
        o.position = { q.px, q.py, q.pz };
        o.size = d.size_start + (d.size_end - d.size_start) * t;
        o.color = lerp3(d.color_start, d.color_end, t);
        o.alpha = d.alpha_start + (d.alpha_end - d.alpha_start) * t;
        o.rotation = q.rot;
        o.blend = (uint32_t)d.blend;
        if (d.atlas_frames > 1) {
            o.frame = d.atlas_animate
                    ? (uint32_t)((float)d.atlas_frames * t) % d.atlas_frames   // walk the flipbook
                    : q.frame0;                                               // one cell, chosen at birth
        } else o.frame = 0;
    }
    return n;
}

} // extern "C"
