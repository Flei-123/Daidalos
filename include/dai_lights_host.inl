/* dai_lights_host.inl - the Light component, collected out of the document.
 *
 * The scene layer carries meshes and bodies; it does not carry lights. So a
 * host walks the document once per frame and hands the renderer the list -
 * which is also what keeps a lamp editable WHILE the game plays, because
 * nothing is cached anywhere in between.
 *
 * This file exists because there were two hosts and only one of them did it:
 * examples/editor_demo.cpp lit its scene, tools/modeling_shot.cpp did not, and
 * the room the bridge builds came out lit by ambient alone - a Lamp node whose
 * intensity could be set to anything at all without a single pixel changing.
 * A rule that lives in one host is a rule the other one breaks.
 *
 *   #include "dai_lights_host.inl"
 *   dai_lights_host_collect(doc, ed, renderer);   // once per frame, before draw
 *
 * `ed` may be null: without an editor the document's own transforms are used,
 * with one the LIVE transform is, so a lamp being dragged lights from where it
 * is now and not from where it will be when the drag ends.
 */
#ifndef DAI_LIGHTS_HOST_INL
#define DAI_LIGHTS_HOST_INL

#include "dai_doc.h"
#include "dai_editor.h"
#include "dai_render.h"
#include <vector>

static void dai_lights_host_collect(dai_doc *doc, dai_editor *ed, dai_renderer *r) {
    if (!doc || !r) return;
    static std::vector<dai_light> lights;
    static std::vector<dai_node> ids;
    lights.clear();
    uint32_t n = dai_doc_count(doc);
    ids.resize(n);
    if (n) dai_doc_nodes(doc, ids.data(), n);
    for (uint32_t i = 0; i < n; ++i) {
        dai_node id = ids[i];
        dai_node_desc lr{};
        if (dai_doc_get(doc, id, &lr) != DAI_OK || !lr.light) continue;
        dai_vec3 wp{}, ws{ 1, 1, 1 };
        dai_quat wr{ 0, 0, 0, 1 };
        if (!ed || !dai_editor_live_transform(ed, id, &wp, &wr, &ws))
            dai_doc_world_transform(doc, id, &wp, &wr, &ws);
        dai_vec3 col = (lr.light_color.x || lr.light_color.y || lr.light_color.z)
                     ? lr.light_color : dai_vec3{ 1, 1, 1 };
        float power = lr.light_intensity > 0.0f ? lr.light_intensity : 1.0f;
        float range = lr.light_range > 0.0f ? lr.light_range : 10.0f;
        /* A directional light is the sun, not an entry in the list. */
        if (lr.light == 3) {
            float x = wr.x, y = wr.y, z = wr.z, w = wr.w;
            dai_vec3 d{ 2*(x*z + w*y), 2*(y*z - w*x), 1 - 2*(x*x + y*y) };
            dai_render_sun(r, dai_vec3{ -d.x, -d.y, -d.z }, col, power);
            continue;
        }
        dai_light L{};
        L.position = wp; L.color = col; L.intensity = power; L.range = range;
        if (lr.light == 2) {
            float x = wr.x, y = wr.y, z = wr.z, w = wr.w;
            L.direction = { 2*(x*z + w*y), 2*(y*z - w*x), 1 - 2*(x*x + y*y) };
            L.direction = { -L.direction.x, -L.direction.y, -L.direction.z };
            L.type = DAI_LIGHT_SPOT;
            float cone = lr.light_cone > 0.0f ? lr.light_cone : 30.0f;
            L.inner_deg = cone * 0.7f;
            L.outer_deg = cone;
        } else {
            L.type = DAI_LIGHT_POINT;
        }
        lights.push_back(L);
    }
    dai_render_lights(r, lights.empty() ? nullptr : lights.data(), (uint32_t)lights.size());
}

#endif /* DAI_LIGHTS_HOST_INL */
