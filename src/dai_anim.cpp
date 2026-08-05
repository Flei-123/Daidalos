// The animation player. See include/dai_anim.h.
//
// Deliberately free of every other subsystem: no renderer, no model, no scene.
// It is arithmetic on arrays of dai_anim_trs, which is why it can be linked
// into a test on its own and why a change here can be checked numerically
// instead of by squinting at a character.
//
// The two things that are easy to get subtly wrong, and are therefore written
// out here rather than inlined:
//
//   rotation blending  slerp, shortest arc, renormalised. A lerp of two
//                      quaternions is shorter than unit length, and a bone
//                      built from it is literally shorter - the limb pumps.
//
//   event crossing     counted, not compared. "is the event time between the
//                      old and the new position" is wrong the moment a clip
//                      loops inside one update. The count of crossings over an
//                      UNWRAPPED playback head is right for every dt, in both
//                      directions, over any number of loops.

#include "dai_anim.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------- small math

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Shortest arc slerp between two quaternions, result normalised.
// out may alias a.
void quat_slerp(const float *a, const float *b, float u, float *out) {
    float dot = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
    float sign = dot < 0.0f ? -1.0f : 1.0f;
    dot = dot < 0.0f ? -dot : dot;
    if (dot > 1.0f) dot = 1.0f;
    float k0, k1;
    if (dot > 0.9995f) {                 // nearly parallel: lerp, then normalise
        k0 = 1.0f - u; k1 = u;
    } else {
        float theta = acosf(dot), st = sinf(theta);
        k0 = sinf((1.0f - u) * theta) / st;
        k1 = sinf(u * theta) / st;
    }
    float r[4], len = 0.0f;
    for (int i = 0; i < 4; ++i) { r[i] = k0 * a[i] + k1 * sign * b[i]; len += r[i] * r[i]; }
    len = sqrtf(len);
    if (len > 1e-8f) for (int i = 0; i < 4; ++i) out[i] = r[i] / len;
    else { out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f; }
}

void quat_normalize(float *q) {
    float len = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (len > 1e-8f) { for (int i = 0; i < 4; ++i) q[i] /= len; }
    else { q[0] = q[1] = q[2] = 0.0f; q[3] = 1.0f; }
}

// ---------------------------------------------------------------- clip

struct Channel {
    uint32_t target = 0;
    int      path = 0;                // dai_anim_path
    int      interp = 0;              // dai_anim_interp
    int      comps = 3;
    std::vector<float> times;
    std::vector<float> values;
};

struct Event {
    float       time = 0.0f;
    std::string name;
};

} // namespace

struct dai_anim_clip {
    std::string name;
    std::vector<Channel> channels;
    std::vector<Event>   events;
    float duration = 0.0f;
    int   loop = 1;
    uint32_t targets = 0;             // highest target + 1
};

namespace {

// Samples one channel at `t`. Times are sorted, so this is a binary search:
// a rig with a few hundred keys per bone times a crowd of characters is where
// a linear scan stops being free.
void sample_channel(const Channel &c, float t, float *out) {
    const int comps = c.comps;
    const size_t n = c.times.size();
    if (n == 0) return;
    if (n == 1 || t <= c.times.front()) {
        for (int i = 0; i < comps; ++i) out[i] = c.values[(size_t)i];
        return;
    }
    if (t >= c.times.back()) {
        const size_t base = (n - 1) * (size_t)comps;
        for (int i = 0; i < comps; ++i) out[i] = c.values[base + (size_t)i];
        return;
    }
    size_t lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (c.times[mid] <= t) lo = mid; else hi = mid;
    }
    const float span = c.times[hi] - c.times[lo];
    const float u = span > 1e-9f ? (t - c.times[lo]) / span : 0.0f;
    const float *va = &c.values[lo * (size_t)comps];
    const float *vb = &c.values[hi * (size_t)comps];

    if (c.interp == DAI_ANIM_STEP) {
        for (int i = 0; i < comps; ++i) out[i] = va[i];
        return;
    }
    if (comps == 4) quat_slerp(va, vb, u, out);
    else for (int i = 0; i < comps; ++i) out[i] = va[i] + (vb[i] - va[i]) * u;
}

} // namespace

extern "C" {

dai_anim_trs dai_anim_trs_identity(void) {
    dai_anim_trs r;
    r.t = dai_vec3{ 0, 0, 0 };
    r.r = dai_quat{ 0, 0, 0, 1 };
    r.s = dai_vec3{ 1, 1, 1 };
    return r;
}

dai_anim_clip *dai_anim_clip_create(const char *name) {
    dai_anim_clip *c = new dai_anim_clip();
    c->name = name ? name : "";
    return c;
}

void dai_anim_clip_destroy(dai_anim_clip *c) { delete c; }

int dai_anim_clip_add_channel(dai_anim_clip *c, uint32_t target, dai_anim_path path,
                              dai_anim_interp interp, const float *times,
                              const float *values, uint32_t key_count) {
    if (!c || !times || !values || key_count == 0) return -1;
    if (path != DAI_ANIM_TRANSLATION && path != DAI_ANIM_ROTATION && path != DAI_ANIM_SCALE) return -1;
    for (uint32_t i = 1; i < key_count; ++i)
        if (times[i] < times[i - 1]) return -1;      // a binary search needs this to be true

    Channel ch;
    ch.target = target;
    ch.path   = (int)path;
    ch.interp = interp == DAI_ANIM_STEP ? DAI_ANIM_STEP : DAI_ANIM_LINEAR;
    ch.comps  = path == DAI_ANIM_ROTATION ? 4 : 3;
    ch.times.assign(times, times + key_count);
    ch.values.assign(values, values + (size_t)key_count * (size_t)ch.comps);
    if (ch.comps == 4)
        for (uint32_t k = 0; k < key_count; ++k) quat_normalize(&ch.values[(size_t)k * 4]);

    if (times[key_count - 1] > c->duration) c->duration = times[key_count - 1];
    if (target + 1 > c->targets) c->targets = target + 1;
    c->channels.push_back(ch);
    return (int)c->channels.size() - 1;
}

void dai_anim_clip_add_event(dai_anim_clip *c, float time, const char *name) {
    if (!c || !name) return;
    Event e; e.time = time < 0.0f ? 0.0f : time; e.name = name;
    size_t at = c->events.size();
    while (at > 0 && c->events[at - 1].time > e.time) --at;
    c->events.insert(c->events.begin() + (long)at, e);
}

void  dai_anim_clip_set_loop(dai_anim_clip *c, int loop) { if (c) c->loop = loop ? 1 : 0; }
int   dai_anim_clip_loop(const dai_anim_clip *c) { return c ? c->loop : 0; }
float dai_anim_clip_duration(const dai_anim_clip *c) { return c ? c->duration : 0.0f; }
void  dai_anim_clip_set_duration(dai_anim_clip *c, float s) { if (c && s >= 0.0f) c->duration = s; }
const char *dai_anim_clip_name(const dai_anim_clip *c) { return c ? c->name.c_str() : ""; }
uint32_t dai_anim_clip_channel_count(const dai_anim_clip *c) { return c ? (uint32_t)c->channels.size() : 0; }
uint32_t dai_anim_clip_event_count(const dai_anim_clip *c) { return c ? (uint32_t)c->events.size() : 0; }
uint32_t dai_anim_clip_target_count(const dai_anim_clip *c) { return c ? c->targets : 0; }

const char *dai_anim_clip_event_at(const dai_anim_clip *c, uint32_t index, float *out_time) {
    if (!c || index >= c->events.size()) return nullptr;
    if (out_time) *out_time = c->events[index].time;
    return c->events[index].name.c_str();
}

void dai_anim_clip_sample(const dai_anim_clip *c, float time, dai_anim_trs *pose, uint32_t count) {
    if (!c || !pose) return;
    const float t = clampf(time, 0.0f, c->duration);
    for (const Channel &ch : c->channels) {
        if (ch.target >= count) continue;
        dai_anim_trs &p = pose[ch.target];
        if (ch.path == DAI_ANIM_TRANSLATION)   sample_channel(ch, t, &p.t.x);
        else if (ch.path == DAI_ANIM_ROTATION) sample_channel(ch, t, &p.r.x);
        else                                   sample_channel(ch, t, &p.s.x);
    }
}

uint32_t dai_anim_clip_sample_channel(const dai_anim_clip *c, uint32_t channel,
                                      float time, float *out) {
    if (!c || !out || channel >= c->channels.size()) return 0;
    const Channel &ch = c->channels[channel];
    sample_channel(ch, clampf(time, 0.0f, c->duration), out);
    return (uint32_t)ch.comps;
}

void dai_anim_pose_blend(dai_anim_trs *dst, const dai_anim_trs *src, uint32_t count, float w) {
    if (!dst || !src) return;
    w = clampf(w, 0.0f, 1.0f);
    for (uint32_t i = 0; i < count; ++i) {
        float *a = &dst[i].t.x; const float *b = &src[i].t.x;
        for (int k = 0; k < 3; ++k) a[k] += (b[k] - a[k]) * w;
        quat_slerp(&dst[i].r.x, &src[i].r.x, w, &dst[i].r.x);
        float *as = &dst[i].s.x; const float *bs = &src[i].s.x;
        for (int k = 0; k < 3; ++k) as[k] += (bs[k] - as[k]) * w;
    }
}

} // extern "C"

// ---------------------------------------------------------------- the player

namespace {

// How many times playback moving from p0 to p1 crosses an event at `e`.
// Positions are UNWRAPPED: they keep growing past the clip's duration, so one
// update covering three loops answers 3 instead of "somewhere between".
//
// The interval is half open on the left, (p0, p1], which is what makes an
// event fire exactly once per pass: the position it fired at is excluded from
// the next update's interval. Backwards playback is the mirror image.
int crossings(float e, float p0, float p1, float dur, int loop) {
    if (p1 == p0) return 0;
    if (!loop || dur <= 1e-9f) {
        const float a = p0 < p1 ? p0 : p1;
        const float b = p0 < p1 ? p1 : p0;
        return (e > a && e <= b) ? 1 : 0;
    }
    const float f1 = floorf((p1 - e) / dur);
    const float f0 = floorf((p0 - e) / dur);
    int n = (int)(f1 - f0);
    if (n < 0) n = -n;
    return n > 64 ? 64 : n;            // a dt measured in minutes must not wedge the frame
}

struct Blend {
    std::string name;
    std::vector<int>   clips;          // indices into dai_anim::clips
    std::vector<float> thresholds;
};

// One thing being played. A blend state plays several clips at once against a
// shared normalised head; a plain state is the same machinery with one member
// at weight 1, which is why there is only one code path below.
struct State {
    int   blend = -1;                  // index into blends, or -1 for a plain clip
    int   clip  = -1;                  // index into clips  (blend < 0)
    float pos = 0.0f;                  // unwrapped playback head, NORMALISED (1.0 = one pass)
    float prev = 0.0f;                 // where it was before this update
    float weight = 0.0f;               // current fade weight, 0..1
    float target = 1.0f;               // what it is fading towards
    float rate = 0.0f;                 // weight per second, 0 = snap
    int   alive = 1;
    int   first = 1;                   // the update that has not happened yet
};

} // namespace

struct dai_anim {
    std::vector<dai_anim_clip *> clips;
    std::vector<char>            owned;
    std::vector<Blend>           blends;
    std::vector<State>           states;      // [0] is the one being faded IN

    std::vector<dai_anim_trs> rest;
    std::vector<dai_anim_trs> pose;
    std::vector<dai_anim_trs> scratch;        // one state's pose
    std::vector<dai_anim_trs> sub;            // one blend member's pose

    dai_anim_event_fn on_event = nullptr;
    void *event_user = nullptr;

    float speed = 1.0f;
    float parameter = 0.0f;
    uint32_t targets = 0;
    std::string current_name;                 // stable storage for dai_anim_current

    // Weights of the members of the current blend, recomputed per update, in
    // clip-index space. Only used to answer dai_anim_member_weight.
    std::vector<float> member_w;
};

namespace {

void grow_targets(dai_anim *a, uint32_t n) {
    if (n <= a->targets) return;
    a->targets = n;
    a->rest.resize(n, dai_anim_trs_identity());
    a->pose.resize(n);
    a->scratch.resize(n);
    a->sub.resize(n);
    for (uint32_t i = 0; i < n; ++i) a->pose[i] = a->rest[i];
}

int find_clip(const dai_anim *a, const char *name) {
    if (!name) return -1;
    for (size_t i = 0; i < a->clips.size(); ++i)
        if (a->clips[i] && a->clips[i]->name == name) return (int)i;
    return -1;
}

int find_blend(const dai_anim *a, const char *name) {
    if (!name) return -1;
    for (size_t i = 0; i < a->blends.size(); ++i)
        if (a->blends[i].name == name) return (int)i;
    return -1;
}

// A state's members and their weights at the current parameter value, plus the
// duration the shared normalised head advances against.
struct Members {
    int   clip[8];
    float weight[8];
    int   count = 0;
    float duration = 0.0f;
    int   dominant = -1;
};

Members resolve(const dai_anim *a, const State &st) {
    Members m;
    if (st.blend < 0) {
        if (st.clip < 0 || (size_t)st.clip >= a->clips.size()) return m;
        m.clip[0] = st.clip; m.weight[0] = 1.0f; m.count = 1;
        m.duration = a->clips[(size_t)st.clip]->duration;
        m.dominant = st.clip;
        return m;
    }
    const Blend &b = a->blends[(size_t)st.blend];
    const size_t n = b.clips.size();
    if (n == 0) return m;
    const float p = a->parameter;

    // Below the first threshold or above the last, the end member stands alone.
    size_t lo = 0, hi = 0;
    float u = 0.0f;
    if (p <= b.thresholds.front())      { lo = hi = 0; }
    else if (p >= b.thresholds.back())  { lo = hi = n - 1; }
    else {
        while (hi + 1 < n && b.thresholds[hi + 1] <= p) ++hi;
        lo = hi; hi = lo + 1;
        const float span = b.thresholds[hi] - b.thresholds[lo];
        u = span > 1e-9f ? (p - b.thresholds[lo]) / span : 0.0f;
    }
    if (lo == hi) {
        m.clip[0] = b.clips[lo]; m.weight[0] = 1.0f; m.count = 1;
    } else {
        m.clip[0] = b.clips[lo]; m.weight[0] = 1.0f - u;
        m.clip[1] = b.clips[hi]; m.weight[1] = u;
        m.count = 2;
    }
    // Synchronised: the head advances against the weighted average of the
    // durations, so a 1.2 s walk and a 0.7 s run stay in step.
    float best = -1.0f;
    for (int i = 0; i < m.count; ++i) {
        const dai_anim_clip *c = a->clips[(size_t)m.clip[i]];
        m.duration += c->duration * m.weight[i];
        if (m.weight[i] > best) { best = m.weight[i]; m.dominant = m.clip[i]; }
    }
    return m;
}

int state_loops(const dai_anim *a, const Members &m) {
    if (m.count == 0) return 0;
    // A blend loops when its dominant member does.
    const int c = m.dominant >= 0 ? m.dominant : m.clip[0];
    return a->clips[(size_t)c]->loop;
}

// Fires whatever the move from st.prev to st.pos crossed. Positions are
// normalised, so an event at 0.5 s in a 2 s clip sits at 0.25.
void fire_events(dai_anim *a, const State &st, const Members &m) {
    if (!a->on_event || m.dominant < 0) return;
    const dai_anim_clip *c = a->clips[(size_t)m.dominant];
    if (c->events.empty()) return;
    const int loop = c->loop;
    const float dur = c->duration;
    for (const Event &e : c->events) {
        const float en = dur > 1e-9f ? e.time / dur : 0.0f;
        const int n = crossings(en, st.prev, st.pos, 1.0f, loop);
        for (int k = 0; k < n; ++k)
            a->on_event(e.name.c_str(), c->name.c_str(), e.time, a->event_user);
    }
}

// Everything else fades out; `keep` fades in.
void retarget(dai_anim *a, size_t keep, float fade) {
    const float rate = fade > 1e-6f ? 1.0f / fade : 0.0f;
    for (size_t i = 0; i < a->states.size(); ++i) {
        State &s = a->states[i];
        s.target = (i == keep) ? 1.0f : 0.0f;
        s.rate = rate;
        if (rate == 0.0f) s.weight = s.target;        // snap
    }
}

int start_state(dai_anim *a, int blend, int clip, float fade, float start_norm) {
    // The new state goes to the front: [0] is always "what is being played".
    State st;
    st.blend = blend;
    st.clip = clip;
    // Half a microsecond of head start, so an event sitting exactly at the
    // clip's start time is crossed on the first update instead of only on the
    // second loop. Relative, so it survives any duration.
    st.prev = start_norm - 1e-5f;
    st.pos = start_norm;
    st.first = 1;
    st.weight = fade > 1e-6f ? 0.0f : 1.0f;
    st.target = 1.0f;
    st.rate = fade > 1e-6f ? 1.0f / fade : 0.0f;
    a->states.insert(a->states.begin(), st);
    if (a->states.size() > 8) a->states.resize(8);    // oldest fades are dropped, not tracked forever
    retarget(a, 0, fade);
    a->states[0].weight = fade > 1e-6f ? 0.0f : 1.0f;
    return 1;
}

void rebuild_pose(dai_anim *a) {
    if (a->targets == 0) return;
    for (uint32_t i = 0; i < a->targets; ++i) a->pose[i] = a->rest[i];
    a->member_w.assign(a->clips.size(), 0.0f);

    float total = 0.0f;
    for (const State &s : a->states) total += s.weight;
    if (total <= 1e-6f) return;                       // nothing playing: rest pose

    float acc = 0.0f;
    for (const State &s : a->states) {
        if (s.weight <= 1e-6f) continue;
        const Members m = resolve(a, s);
        if (m.count == 0) continue;
        const float w = s.weight / total;

        // this state's own pose, itself a blend when it is a 1D blend
        for (uint32_t i = 0; i < a->targets; ++i) a->scratch[i] = a->rest[i];
        float macc = 0.0f;
        for (int k = 0; k < m.count; ++k) {
            if (m.weight[k] <= 1e-6f) continue;
            const dai_anim_clip *c = a->clips[(size_t)m.clip[k]];
            float t = s.pos - floorf(s.pos);          // normalised head, wrapped
            if (!c->loop) t = clampf(s.pos, 0.0f, 1.0f);
            for (uint32_t i = 0; i < a->targets; ++i) a->sub[i] = a->rest[i];
            dai_anim_clip_sample(c, t * c->duration, a->sub.data(), a->targets);
            if (macc <= 1e-6f) {
                for (uint32_t i = 0; i < a->targets; ++i) a->scratch[i] = a->sub[i];
                macc = m.weight[k];
            } else {
                const float f = m.weight[k] / (macc + m.weight[k]);
                dai_anim_pose_blend(a->scratch.data(), a->sub.data(), a->targets, f);
                macc += m.weight[k];
            }
            if ((size_t)m.clip[k] < a->member_w.size()) a->member_w[(size_t)m.clip[k]] += w * m.weight[k];
        }
        if (macc <= 1e-6f) continue;

        if (acc <= 1e-6f) {
            for (uint32_t i = 0; i < a->targets; ++i) a->pose[i] = a->scratch[i];
            acc = w;
        } else {
            // Normalised accumulation: with two states this is exactly a slerp
            // by the second one's weight, which is what a crossfade means.
            const float f = w / (acc + w);
            dai_anim_pose_blend(a->pose.data(), a->scratch.data(), a->targets, f);
            acc += w;
        }
    }
}

} // namespace

extern "C" {

dai_anim *dai_anim_create(uint32_t targets) {
    dai_anim *a = new dai_anim();
    grow_targets(a, targets);
    return a;
}

void dai_anim_destroy(dai_anim *a) {
    if (!a) return;
    for (size_t i = 0; i < a->clips.size(); ++i)
        if (a->owned[i]) dai_anim_clip_destroy(a->clips[i]);
    delete a;
}

int dai_anim_add(dai_anim *a, dai_anim_clip *clip, int own) {
    if (!a || !clip) return -1;
    const int existing = find_clip(a, clip->name.c_str());
    if (existing >= 0) {
        if (a->owned[(size_t)existing] && a->clips[(size_t)existing] != clip)
            dai_anim_clip_destroy(a->clips[(size_t)existing]);
        a->clips[(size_t)existing] = clip;
        a->owned[(size_t)existing] = own ? 1 : 0;
        grow_targets(a, clip->targets);
        return existing;
    }
    a->clips.push_back(clip);
    a->owned.push_back(own ? 1 : 0);
    grow_targets(a, clip->targets);
    return (int)a->clips.size() - 1;
}

dai_anim_clip *dai_anim_find(const dai_anim *a, const char *name) {
    if (!a) return nullptr;
    const int i = find_clip(a, name);
    return i < 0 ? nullptr : a->clips[(size_t)i];
}

uint32_t dai_anim_count(const dai_anim *a) { return a ? (uint32_t)a->clips.size() : 0; }

dai_anim_clip *dai_anim_at(const dai_anim *a, uint32_t index) {
    return (a && index < a->clips.size()) ? a->clips[index] : nullptr;
}

void dai_anim_set_rest(dai_anim *a, const dai_anim_trs *rest, uint32_t count) {
    if (!a) return;
    grow_targets(a, count);
    if (!rest) return;
    for (uint32_t i = 0; i < count && i < a->targets; ++i) a->rest[i] = rest[i];
    for (uint32_t i = 0; i < a->targets; ++i) a->pose[i] = a->rest[i];
}

uint32_t dai_anim_targets(const dai_anim *a) { return a ? a->targets : 0; }

int dai_anim_play(dai_anim *a, const char *name, float fade) {
    if (!a) return 0;
    const int bi = find_blend(a, name);
    const int ci = bi >= 0 ? -1 : find_clip(a, name);
    if (bi < 0 && ci < 0) return 0;

    // Already the one being played: keep going rather than snapping to frame 0.
    if (!a->states.empty() && a->states[0].alive &&
        ((bi >= 0 && a->states[0].blend == bi) || (ci >= 0 && a->states[0].clip == ci && a->states[0].blend < 0))) {
        retarget(a, 0, fade);
        return 1;
    }
    return start_state(a, bi, ci, fade, 0.0f);
}

int dai_anim_crossfade(dai_anim *a, const char *name, float fade) { return dai_anim_play(a, name, fade); }

int dai_anim_restart(dai_anim *a, const char *name, float fade, float start) {
    if (!a) return 0;
    const int bi = find_blend(a, name);
    const int ci = bi >= 0 ? -1 : find_clip(a, name);
    if (bi < 0 && ci < 0) return 0;
    float dur = 0.0f;
    if (ci >= 0) dur = a->clips[(size_t)ci]->duration;
    else if (!a->blends[(size_t)bi].clips.empty()) dur = a->clips[(size_t)a->blends[(size_t)bi].clips[0]]->duration;
    return start_state(a, bi, ci, fade, dur > 1e-9f ? start / dur : 0.0f);
}

void dai_anim_stop(dai_anim *a, float fade) {
    if (!a) return;
    const float rate = fade > 1e-6f ? 1.0f / fade : 0.0f;
    for (State &s : a->states) { s.target = 0.0f; s.rate = rate; if (rate == 0.0f) s.weight = 0.0f; }
    if (rate == 0.0f) { a->states.clear(); rebuild_pose(a); }
}

void dai_anim_update(dai_anim *a, float dt) {
    if (!a) return;

    // A state that was snapped to zero weight is already gone - it just has not
    // been swept up yet. Letting it run one more tick makes it fire its events
    // a second time, which is exactly the double footstep you get for one frame
    // after restarting a clip. Sweep first, then advance.
    for (size_t i = a->states.size(); i-- > 0; )
        if (a->states[i].weight <= 1e-6f && a->states[i].target <= 0.0f)
            a->states.erase(a->states.begin() + (long)i);

    for (size_t i = 0; i < a->states.size(); ++i) {
        State &s = a->states[i];
        const Members m = resolve(a, s);
        const float dur = m.duration;
        if (!s.first) s.prev = s.pos;   // the first tick keeps the head start from start_state
        s.first = 0;
        if (dur > 1e-9f) s.pos += dt * a->speed / dur;
        const int loop = state_loops(a, m);
        if (!loop) {
            if (s.pos > 1.0f) s.pos = 1.0f;
            if (s.pos < 0.0f) s.pos = 0.0f;
        }
        fire_events(a, s, m);

        // fade
        if (s.rate <= 0.0f) s.weight = s.target;
        else if (s.weight < s.target) { s.weight += s.rate * dt; if (s.weight > s.target) s.weight = s.target; }
        else if (s.weight > s.target) { s.weight -= s.rate * dt; if (s.weight < s.target) s.weight = s.target; }
        if (s.target <= 0.0f && s.weight <= 1e-6f) s.alive = 0;
    }
    // a faded out state is gone; index 0 stays "what is being played"
    for (size_t i = a->states.size(); i-- > 0; )
        if (!a->states[i].alive) a->states.erase(a->states.begin() + (long)i);

    rebuild_pose(a);
}

const dai_anim_trs *dai_anim_pose(const dai_anim *a, uint32_t *out_count) {
    if (!a) { if (out_count) *out_count = 0; return nullptr; }
    if (out_count) *out_count = a->targets;
    return a->pose.empty() ? nullptr : a->pose.data();
}

void  dai_anim_set_speed(dai_anim *a, float s) { if (a) a->speed = s; }
float dai_anim_speed(const dai_anim *a) { return a ? a->speed : 0.0f; }

int dai_anim_is_playing(const dai_anim *a, const char *name) {
    if (!a || !name) return 0;
    const int bi = find_blend(a, name);
    const int ci = find_clip(a, name);
    for (const State &s : a->states) {
        if (s.weight <= 1e-6f && s.target <= 0.0f) continue;
        if (bi >= 0 && s.blend == bi) return 1;
        if (s.blend < 0 && ci >= 0 && s.clip == ci) return 1;
        if (s.blend >= 0 && ci >= 0) {                  // a member of a playing blend
            const Blend &b = a->blends[(size_t)s.blend];
            for (int c : b.clips) if (c == ci) return 1;
        }
    }
    return 0;
}

const char *dai_anim_current(const dai_anim *a) {
    if (!a || a->states.empty()) return "";
    const State &s = a->states[0];
    dai_anim *self = const_cast<dai_anim *>(a);
    if (s.blend >= 0) self->current_name = a->blends[(size_t)s.blend].name;
    else if (s.clip >= 0 && (size_t)s.clip < a->clips.size()) self->current_name = a->clips[(size_t)s.clip]->name;
    else self->current_name.clear();
    return self->current_name.c_str();
}

float dai_anim_weight(const dai_anim *a, const char *name) {
    if (!a) return 0.0f;
    const int bi = find_blend(a, name);
    const int ci = find_clip(a, name);
    float total = 0.0f, mine = 0.0f;
    for (const State &s : a->states) {
        total += s.weight;
        if ((bi >= 0 && s.blend == bi) || (s.blend < 0 && ci >= 0 && s.clip == ci)) mine += s.weight;
    }
    return total > 1e-6f ? mine / total : 0.0f;
}

float dai_anim_member_weight(const dai_anim *a, const char *name) {
    if (!a) return 0.0f;
    const int ci = find_clip(a, name);
    if (ci < 0 || (size_t)ci >= a->member_w.size()) return 0.0f;
    return a->member_w[(size_t)ci];
}

float dai_anim_time(const dai_anim *a) {
    if (!a || a->states.empty()) return 0.0f;
    const Members m = resolve(a, a->states[0]);
    const float p = a->states[0].pos;
    const float n = state_loops(a, m) ? p - floorf(p) : clampf(p, 0.0f, 1.0f);
    return n * m.duration;
}

float dai_anim_normalized_time(const dai_anim *a) {
    if (!a || a->states.empty()) return 0.0f;
    const Members m = resolve(a, a->states[0]);
    const float p = a->states[0].pos;
    return state_loops(a, m) ? p - floorf(p) : clampf(p, 0.0f, 1.0f);
}

void dai_anim_set_time(dai_anim *a, float seconds) {
    if (!a || a->states.empty()) return;
    const Members m = resolve(a, a->states[0]);
    const float n = m.duration > 1e-9f ? seconds / m.duration : 0.0f;
    a->states[0].pos = n;
    a->states[0].prev = n;               // a seek is a jump: it fires nothing
    a->states[0].first = 0;
    rebuild_pose(a);
}

int dai_anim_finished(const dai_anim *a) {
    if (!a || a->states.empty()) return 0;
    const Members m = resolve(a, a->states[0]);
    if (state_loops(a, m)) return 0;
    return a->states[0].pos >= 1.0f ? 1 : 0;
}

void dai_anim_on_event(dai_anim *a, dai_anim_event_fn fn, void *user) {
    if (!a) return;
    a->on_event = fn;
    a->event_user = user;
}

int dai_anim_blend1d(dai_anim *a, const char *name, const char *const *clips,
                     const float *thresholds, uint32_t count) {
    if (!a || !name || !clips || !thresholds || count == 0 || count > 8) return 0;
    for (uint32_t i = 1; i < count; ++i) if (thresholds[i] < thresholds[i - 1]) return 0;
    Blend b;
    b.name = name;
    for (uint32_t i = 0; i < count; ++i) {
        const int ci = find_clip(a, clips[i]);
        if (ci < 0) return 0;
        b.clips.push_back(ci);
        b.thresholds.push_back(thresholds[i]);
    }
    const int existing = find_blend(a, name);
    if (existing >= 0) a->blends[(size_t)existing] = b;
    else a->blends.push_back(b);
    return 1;
}

void  dai_anim_set_parameter(dai_anim *a, float v) { if (a) a->parameter = v; }
float dai_anim_parameter(const dai_anim *a) { return a ? a->parameter : 0.0f; }

} // extern "C"
