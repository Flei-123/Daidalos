// The seam between the glTF importer and the animation player.
//
// Its own translation unit for one reason: dai_anim.cpp must stay linkable on
// its own. The player is arithmetic - a test, a headless server or a tool can
// link it without dragging in the importer, and through the importer the whole
// renderer. Everything that knows about BOTH sides lives here, in a file you
// only link when you actually load models.
//
// The dependency direction is worth stating: the importer knows nothing about
// dai_anim. It hands out plain arrays (dai_model_animation_channel_at) and
// takes plain arrays back (dai_model_pose_local). This file is the only place
// the two vocabularies meet.

#include "dai_anim.h"
#include "dai_gltf.h"

#include <cstdio>
#include <cstring>
#include <vector>

// dai_model_pose_local() reads ten floats per node in exactly this order. If
// the struct ever grows a field, that call starts reading garbage - so it does
// not get to grow silently.
static_assert(sizeof(dai_anim_trs) == 10 * sizeof(float),
              "dai_anim_trs must stay 10 tightly packed floats: dai_model_pose_local reads it raw");

namespace {

// glTF's CUBICSPLINE stores three values per key: in-tangent, value,
// out-tangent. Keeping the middle one and dropping the tangents is a
// downgrade to LINEAR - identical at every keyframe, slightly straighter
// between them. Documented in dai_anim.h; Blender does not export it by
// default, so this path is for files from other tools.
void strip_cubic(const float *values, uint32_t keys, int comps, std::vector<float> &out) {
    out.resize((size_t)keys * (size_t)comps);
    for (uint32_t k = 0; k < keys; ++k)
        for (int c = 0; c < comps; ++c)
            out[(size_t)k * comps + c] = values[((size_t)k * 3 + 1) * comps + c];
}

} // namespace

extern "C" {

uint32_t dai_anim_clips_from_model(const dai_model *m, dai_anim_clip **out, uint32_t max) {
    if (!m) return 0;
    const uint32_t count = dai_model_animation_count(m);
    if (!out) return count;

    std::vector<float> tmp;
    for (uint32_t i = 0; i < count && i < max; ++i) {
        const dai_animation_info info = dai_model_animation_at(m, i);
        char name[64];
        if (info.name[0]) std::snprintf(name, sizeof(name), "%s", info.name);
        else              std::snprintf(name, sizeof(name), "clip%u", i);

        dai_anim_clip *clip = dai_anim_clip_create(name);
        const uint32_t nch = dai_model_animation_channel_count(m, i);
        for (uint32_t c = 0; c < nch; ++c) {
            dai_animation_channel ch{};
            if (!dai_model_animation_channel_at(m, i, c, &ch)) continue;
            if (ch.node < 0 || ch.keys == 0 || !ch.times || !ch.values) continue;
            if (ch.path > 2) continue;                      // morph weights: not a transform

            const dai_anim_path path = (dai_anim_path)ch.path;
            const int comps = ch.path == 1 ? 4 : 3;
            const float *values = ch.values;
            if (ch.interpolation == 2) { strip_cubic(ch.values, ch.keys, comps, tmp); values = tmp.data(); }

            dai_anim_clip_add_channel(clip, (uint32_t)ch.node, path,
                                      ch.interpolation == 1 ? DAI_ANIM_STEP : DAI_ANIM_LINEAR,
                                      ch.times, values, ch.keys);
        }
        // The importer already knows the duration; a channel that ends early
        // must not shorten the clip.
        if (info.duration > dai_anim_clip_duration(clip)) dai_anim_clip_set_duration(clip, info.duration);
        dai_anim_clip_set_loop(clip, 1);
        out[i] = clip;
    }
    return count;
}

dai_anim *dai_anim_from_model(const dai_model *m) {
    if (!m) return nullptr;
    const uint32_t nodes = dai_model_hierarchy_count(m);
    const uint32_t clips = dai_model_animation_count(m);
    if (clips == 0) return nullptr;

    dai_anim *a = dai_anim_create(nodes);

    // The rest pose. Without it every node no clip mentions collapses to the
    // identity, which is a character folded into a point at the origin.
    std::vector<dai_anim_trs> rest(nodes ? nodes : 1, dai_anim_trs_identity());
    if (nodes) dai_model_rest_pose(m, &rest[0].t.x, nodes);
    dai_anim_set_rest(a, rest.data(), nodes);

    std::vector<dai_anim_clip *> list(clips, nullptr);
    dai_anim_clips_from_model(m, list.data(), clips);
    for (dai_anim_clip *c : list) if (c) dai_anim_add(a, c, 1);
    return a;
}

uint32_t dai_anim_apply(const dai_anim *a, dai_model *model, float *joints, uint32_t max_joints) {
    if (!a || !model) return 0;
    uint32_t n = 0;
    const dai_anim_trs *pose = dai_anim_pose(a, &n);
    if (!pose || n == 0) return dai_model_pose(model, -1, 0.0f, joints, max_joints);   // bind pose
    return dai_model_pose_local(model, &pose[0].t.x, n, joints, max_joints);
}

} // extern "C"
