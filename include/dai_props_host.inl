// The component table, in ONE place: what "light.intensity" means, and the six
// functions a script host answers with.
//
// Statement seam like include/dai_blockout_props.inl, one level up: that file
// is a few LINES of a table, this is the whole table plus the get/set pairs
// around it. Included by every host that runs behaviours -
// examples/editor_demo.cpp, tools/modeling_shot.cpp and
// examples/runtime_main.cpp - because there were two copies of it and the
// third host, the SHIPPED GAME, had none: node.setNum() in an exported game
// did nothing at all, since dai_script_node_host's component half was left
// null. A behaviour that dims a torch in the editor and not in the game is
// worse than one that never worked.
//
// The including file says which document it means before it includes:
//
//     #define DAI_PROPS_DOC g_doc
//     #include "dai_props_host.inl"
//
// It writes with dai_doc_set and NO dai_doc_begin/commit: a behaviour changing
// a colour sixty times a second must not put sixty entries on the undo stack.
//
// Expected in scope: <cstring>, <cstdio>, dai_doc.h, and a `dai_doc *`
// named by DAI_PROPS_DOC.

#ifndef DAI_PROPS_DOC
#error "define DAI_PROPS_DOC to the dai_doc* this host owns before including"
#endif

#include "dai_euler.h"
#include <string>

namespace {

// Where the name points. Split so a getter and a setter cannot disagree about
// which field a name means, which is exactly the bug a second switch invites.
enum PropKind { P_NONE, P_NUM, P_VEC, P_STR };

struct PropRef {
    PropKind kind = P_NONE;
    float   *f = nullptr;
    int     *i = nullptr;
    dai_vec3 *v = nullptr;
    char    *str = nullptr;
    size_t   str_len = 0;
    int      bool_of_int = 0;   // 1 = the int is a flag, and 0/1 is the answer
    int      invert = 0;        // no_body is "enabled" upside down
};

PropRef prop_ref(dai_node_desc &r, const char *name) {
    PropRef p;
    auto num = [&](float *f) { p.kind = P_NUM; p.f = f; return p; };
    auto ival = [&](int *i) { p.kind = P_NUM; p.i = i; return p; };
    auto flag = [&](int *i, int inv) { p.kind = P_NUM; p.i = i; p.bool_of_int = 1; p.invert = inv; return p; };
    auto vec = [&](dai_vec3 *v) { p.kind = P_VEC; p.v = v; return p; };
    auto text = [&](char *c, size_t n) { p.kind = P_STR; p.str = c; p.str_len = n; return p; };
    if (!name) return p;

    if (!std::strcmp(name, "node.name"))            return text(r.name, sizeof(r.name));
    if (!std::strcmp(name, "node.tag"))             return text(r.tag, sizeof(r.tag));
    if (!std::strcmp(name, "node.asset"))           return text(r.asset, sizeof(r.asset));

    if (!std::strcmp(name, "transform.scale"))      return vec(&r.scale);
    // transform.position and transform.rotation are NOT in this table: they
    // are not plain vec3 fields on the record - position is a vec3 but every
    // host already answers it through node.getPos/setPos, and rotation is a
    // QUATERNION that has to be spelled in degrees to be usable from a script.
    // Both are handled in comp_get_vec/comp_set_vec below, before the table is
    // consulted. Until they were, "transform.rotation" was a name every host
    // accepted and none stored: the handset in the phone box and the whole
    // staircase in the hall were written turned and came out straight.

    if (!std::strcmp(name, "rigidbody.density"))    return num(&r.density);
    if (!std::strcmp(name, "rigidbody.friction"))   return num(&r.friction);
    if (!std::strcmp(name, "rigidbody.restitution"))return num(&r.restitution);
    if (!std::strcmp(name, "rigidbody.motion"))     return ival(&r.motion);
    if (!std::strcmp(name, "rigidbody.trigger"))    return flag(&r.trigger, 0);
    // "enabled" is the readable side of no_body: a node WITH a rigidbody is
    // the normal case, and a script should not have to spell a double negative.
    if (!std::strcmp(name, "rigidbody.enabled"))    return flag(&r.no_body, 1);

    // The RENDERER, as its own switch. A node that is only a place - the root
    // of a room, an anchor, a spawn point - is still a 1 m box to the scene,
    // and the example room came out with a crate standing in the middle of it.
    // Named like the other component switches, and inverted the same way
    // rigidbody.enabled is: the field says "hidden", the property says "on".
    if (!std::strcmp(name, "renderer.enabled"))     return flag(&r.hidden, 1);

    if (!std::strcmp(name, "camera.mode"))          return ival(&r.camera);
    if (!std::strcmp(name, "camera.enabled"))       return flag(&r.camera, 0);
    if (!std::strcmp(name, "camera.fov"))           return num(&r.camera_fov);
    if (!std::strcmp(name, "camera.size"))          return num(&r.camera_size);

    if (!std::strcmp(name, "light.mode"))           return ival(&r.light);
    if (!std::strcmp(name, "light.enabled"))        return flag(&r.light, 0);
    if (!std::strcmp(name, "light.range"))          return num(&r.light_range);
    if (!std::strcmp(name, "light.intensity"))      return num(&r.light_intensity);
    if (!std::strcmp(name, "light.cone"))           return num(&r.light_cone);
    if (!std::strcmp(name, "light.color"))          return vec(&r.light_color);

    if (!std::strcmp(name, "text.enabled"))         return flag(&r.text_on, 0);
    if (!std::strcmp(name, "text.value"))           return text(r.text, sizeof(r.text));
    if (!std::strcmp(name, "text.size"))            return num(&r.text_size);
    if (!std::strcmp(name, "text.anchor"))          return ival(&r.text_anchor);
    if (!std::strcmp(name, "text.color"))           return vec(&r.text_color);

    // Seam: the blockout components' properties, under the same names the
    // bridge and the behaviours use. One table, so a getter and a setter
    // cannot disagree - the reason this function is shaped this way.
    #include "dai_blockout_props.inl"

    // Seam: the actor properties - the script list and the collider - for the
    // same reason, so a player can be placed over the bridge and not only with
    // a mouse.
    #include "dai_actor_props.inl"

    if (!std::strcmp(name, "image.enabled"))        return flag(&r.sprite, 0);
    if (!std::strcmp(name, "image.size"))           return vec(&r.sprite_size);
    if (!std::strcmp(name, "image.asset"))          return text(r.asset, sizeof(r.asset));
    return p;                                        // unknown: answers the fallback
}

// The words a component property can be read out of. Returned by pointer, so
// the buffer has to outlive the call - a static one is right here: the value
// is copied into a JS string or handed to a behaviour that uses it that frame.
char g_prop_str[512];

double comp_get_num(dai_node id, const char *name, double fallback) {
    if (!DAI_PROPS_DOC) return fallback;
    dai_node_desc r{};
    if (dai_doc_get(DAI_PROPS_DOC, id, &r) != DAI_OK) return fallback;
    PropRef p = prop_ref(r, name);
    if (p.kind != P_NUM) return fallback;
    if (p.f) return (double)*p.f;
    if (!p.i) return fallback;
    int v = *p.i;
    if (p.bool_of_int) return (p.invert ? (v == 0) : (v != 0)) ? 1.0 : 0.0;
    return (double)v;
}

void comp_set_num(dai_node id, const char *name, double value) {
    if (!DAI_PROPS_DOC) return;
    dai_node_desc r{};
    if (dai_doc_get(DAI_PROPS_DOC, id, &r) != DAI_OK) return;
    PropRef p = prop_ref(r, name);
    if (p.kind != P_NUM) return;
    if (p.f) *p.f = (float)value;
    else if (p.i) {
        if (p.bool_of_int) {
            int on = value != 0.0 ? 1 : 0;
            // A flag that names a KIND (camera 1/2, light 1/2/3) must not be
            // stamped back to 1 when it was already 3 - turning a sun on
            // would quietly make it a point light.
            if (p.invert)                  *p.i = on ? 0 : 1;
            else if (!on)                  *p.i = 0;
            else if (*p.i == 0)            *p.i = 1;
        } else {
            *p.i = (int)value;
        }
    }
    dai_doc_set(DAI_PROPS_DOC, id, &r);
}

// The two names that are not fields: the node's own place and orientation.
// Degrees in ZYX, the same three numbers the inspector shows, converted by
// include/dai_euler.h - one conversion for the editor and for scripts, so a
// staircase turned by hand and one turned by a level script end up identical.
int comp_transform_vec(const char *name) {
    if (!name) return 0;
    if (!std::strcmp(name, "transform.position")) return 1;
    if (!std::strcmp(name, "transform.rotation")) return 2;
    return 0;
}

int comp_get_vec(dai_node id, const char *name, double *xyz) {
    xyz[0] = xyz[1] = xyz[2] = 0.0;
    if (!DAI_PROPS_DOC) return 0;
    dai_node_desc r{};
    if (dai_doc_get(DAI_PROPS_DOC, id, &r) != DAI_OK) return 0;
    if (int which = comp_transform_vec(name)) {
        if (which == 1) {
            xyz[0] = r.position.x; xyz[1] = r.position.y; xyz[2] = r.position.z;
        } else {
            float deg[3];
            dai_quat_to_euler(r.rotation, deg);
            xyz[0] = deg[0]; xyz[1] = deg[1]; xyz[2] = deg[2];
        }
        return 1;
    }
    PropRef p = prop_ref(r, name);
    if (p.kind != P_VEC || !p.v) return 0;
    xyz[0] = p.v->x; xyz[1] = p.v->y; xyz[2] = p.v->z;
    return 1;
}

void comp_set_vec(dai_node id, const char *name, const double *xyz) {
    if (!DAI_PROPS_DOC) return;
    dai_node_desc r{};
    if (dai_doc_get(DAI_PROPS_DOC, id, &r) != DAI_OK) return;
    if (int which = comp_transform_vec(name)) {
        if (which == 1) {
            r.position = { (float)xyz[0], (float)xyz[1], (float)xyz[2] };
        } else {
            float deg[3] = { (float)xyz[0], (float)xyz[1], (float)xyz[2] };
            r.rotation = dai_euler_to_quat(deg);
        }
        dai_doc_set(DAI_PROPS_DOC, id, &r);
        return;
    }
    PropRef p = prop_ref(r, name);
    if (p.kind != P_VEC || !p.v) return;
    p.v->x = (float)xyz[0]; p.v->y = (float)xyz[1]; p.v->z = (float)xyz[2];
    dai_doc_set(DAI_PROPS_DOC, id, &r);
}

// "material" is not in the table above because it is a STACK on the node
// (dai_node_desc::materials, ';' separated) and the name means SLOT 0 - the
// object's own surface, which is what anybody assigning "the wall material"
// means. Written the same way the bridge's editor.setMaterial writes it, so a
// wall re-skinned by a behaviour and one re-skinned by a tool end up with the
// same field. Returns 1 when the name was the material.
int comp_material_str(dai_node_desc &r, const char *name, const char *set, char *out, size_t out_len) {
    if (!name || std::strcmp(name, "material")) return 0;
    std::string all = r.materials;
    size_t semi = all.find(';');
    if (!set) {
        std::string first = (semi == std::string::npos) ? all : all.substr(0, semi);
        std::snprintf(out, out_len, "%s", first.c_str());
        return 1;
    }
    std::string rest = (semi == std::string::npos) ? std::string() : all.substr(semi);
    std::string joined = std::string(set) + rest;
    if (joined.size() < sizeof(r.materials))
        std::snprintf(r.materials, sizeof(r.materials), "%s", joined.c_str());
    return 1;
}

const char *comp_get_str(dai_node id, const char *name) {
    g_prop_str[0] = 0;
    if (!DAI_PROPS_DOC) return g_prop_str;
    dai_node_desc r{};
    if (dai_doc_get(DAI_PROPS_DOC, id, &r) != DAI_OK) return g_prop_str;
    if (comp_material_str(r, name, nullptr, g_prop_str, sizeof(g_prop_str))) return g_prop_str;
    PropRef p = prop_ref(r, name);
    if (p.kind != P_STR || !p.str) return g_prop_str;
    std::snprintf(g_prop_str, sizeof(g_prop_str), "%s", p.str);
    return g_prop_str;
}

void comp_set_str(dai_node id, const char *name, const char *value) {
    if (!DAI_PROPS_DOC) return;
    dai_node_desc r{};
    if (dai_doc_get(DAI_PROPS_DOC, id, &r) != DAI_OK) return;
    if (comp_material_str(r, name, value ? value : "", nullptr, 0)) {
        dai_doc_set(DAI_PROPS_DOC, id, &r);
        return;
    }
    PropRef p = prop_ref(r, name);
    if (p.kind != P_STR || !p.str) return;
    std::snprintf(p.str, p.str_len, "%s", value ? value : "");
    // Putting words in a Text turns it on, the same shortcut sh_set_text has.
    if (!std::strcmp(name, "text.value") && !r.text_on) r.text_on = 1;
    dai_doc_set(DAI_PROPS_DOC, id, &r);
}

} // namespace
