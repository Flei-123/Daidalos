// MODULE 2 (triplanar) OWNS THIS FILE. File scope seam, see include/dai_ext.h.
//
// Included once by every host that draws a scene: examples/editor_demo.cpp and
// tools/modeling_shot.cpp. It is where a `.daimat` becomes a real
// dai_material in the renderer - with its maps loaded, its triplanar switch
// and its tiling - and gets attached to every entity whose node names it.
//
// Today (before this module lands) a `.daimat` only pushes three numbers into
// the node: colour, roughness, emissive (see apply_materials in
// examples/editor_demo.cpp). Nothing creates a dai_material, so no object can
// carry a texture that did not come out of a .glb. That is what this seam is
// for, and it is why the baker of module 3 needs no material code of its own:
// it writes PNGs, this file loads them.
//
// The contract:
//
//   * One dai_material per .daimat path, cached. Two hundred walls on one
//     material are one material and one draw setup, not two hundred.
//   * dai_render_material_update() on a change - the handle stays, so nothing
//     that already points at it has to be told.
//   * Colour space by slot, not by guess: base colour and emissive sRGB, ORM
//     and normal linear (docs/MATERIALS.md).
//   * Attach with dai_scene_set_material() on the entity behind the node. Like
//     the mesh in dai_blockout_host.inl this is derived data: the document
//     stores the PATH, never the handle.
//   * A missing map file is a material that still works, with the default
//     texture in that slot. Never a failed load.
//
// Called once per frame, AFTER dai_doc_sync_apply() and after the host's own
// apply_materials().

#include "dai_material.h"

#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>

// When the file on disk was last written. 0 for a file that is not there,
// which is a value that differs from every real stamp and therefore rebuilds
// the material the moment the file appears.
static long dai_material_host_stamp(const char *path) {
    struct stat st;
    if (!path || !*path || stat(path, &st) != 0) return 0;
    return (long)st.st_mtime;
}

struct dai_material_host_tex {
    std::string path;
    int         srgb = 0;
    dai_texture tex = 0;
    long        stamp = 0;
};

struct dai_material_host_entry {
    std::string  rel;            // the .daimat, relative to Assets
    dai_material mat = 0;        // the handle every node with this path wears
    dai_matfile  file{};
    long         stamp = 0;      // of the .daimat itself
    long         map_stamp = 0;  // of its three maps together
};

// One cache for the whole process. The seam is called once per frame by one
// host, so this is a per host table in practice - and it is keyed on the PATH,
// which is the only identity a material has in the document.
static std::vector<dai_material_host_entry> g_material_host_cache;
static std::vector<dai_material_host_tex>   g_material_host_textures;
static const dai_renderer                  *g_material_host_renderer = nullptr;

// One picture, from wherever this host keeps its assets: a folder (fopen, the
// editor) or an archive (h->read, the shipped game).
static dai_texture dai_material_host_load_tex(const dai_ext_host *h, const char *full, int srgb) {
    if (!h->read) return dai_render_texture_load(h->renderer, full, srgb);
    void *bytes = nullptr; size_t len = 0;
    if (!h->read(full, &bytes, &len, h->read_user) || !bytes) return 0;
    dai_texture t = dai_render_texture_load_memory(h->renderer, bytes, len, srgb);
    if (h->release) h->release(bytes, h->read_user);
    return t;
}

// A map, loaded once and shared by every material that names it. The stamp is
// carried so a re-bake by module 3 (which rewrites the PNG in place) reaches
// the screen without restarting the editor.
static dai_texture dai_material_host_texture(const dai_ext_host *h,
                                             const char *rel, int srgb, long *stamp_out) {
    if (!rel || !*rel) { if (stamp_out) *stamp_out = 0; return 0; }
    dai_renderer *r = h->renderer;
    char full[700];
    std::snprintf(full, sizeof(full), "%s/%s", h->assets_dir, rel);
    /* An archive entry cannot change while the game runs, so its stamp is
     * constant - the whole re-bake path below is an editor concern. */
    long stamp = h->read ? 1 : dai_material_host_stamp(full);
    if (stamp_out) *stamp_out += stamp;

    for (auto &t : g_material_host_textures) {
        if (t.path != rel || t.srgb != srgb) continue;
        if (t.stamp == stamp) return t.tex;
        // The file changed underneath us. The slot is handed back first, so a
        // material still pointing at it is re-pointed at the default rather
        // than left with a dangling descriptor.
        if (t.tex) dai_render_texture_destroy(r, t.tex);
        t.tex = dai_material_host_load_tex(h, full, srgb);
        t.stamp = stamp;
        return t.tex;
    }
    dai_material_host_tex t;
    t.path = rel;
    t.srgb = srgb;
    t.stamp = stamp;
    // A map that is not on disk yet is texture 0 - white, which multiplies to
    // exactly the material's own colour. A material is never broken by a file
    // that has not been baked yet.
    t.tex = dai_material_host_load_tex(h, full, srgb);
    g_material_host_textures.push_back(t);
    return t.tex;
}

// The .daimat as the renderer wants it. Everything the file does not say keeps
// dai_material_desc_default(), so a two line material is a complete one.
static dai_material_desc dai_material_host_desc(const dai_matfile &m, const char *name,
                                                dai_texture base, dai_texture orm,
                                                dai_texture normal) {
    dai_material_desc d = dai_material_desc_default();
    d.base_color = m.color;
    d.roughness = m.roughness;
    d.metallic = m.metallic;
    d.emissive = dai_vec3{ m.emissive, m.emissive, m.emissive };
    d.normal_strength = m.normal_strength;
    d.base_color_tex = base;
    d.orm_tex = orm;
    d.normal_tex = normal;
    d.flags = m.triplanar ? (uint32_t)DAI_MAT_TRIPLANAR : 0u;
    d.triplanar_scale = m.triplanar_scale;
    d.triplanar_blend = m.triplanar_blend;
    d.name = name;
    return d;
}

// The material behind one path: created the first time it is asked for,
// rewritten in place when the file or one of its maps has changed, and handed
// back unchanged on every other frame. 0 means "not a material file", which is
// the caller's signal to leave the entity alone.
static dai_material dai_material_host_for(const dai_ext_host *h, const std::string &rel) {
    char full[700];
    std::snprintf(full, sizeof(full), "%s/%s", h->assets_dir, rel.c_str());
    long stamp = h->read ? 1 : dai_material_host_stamp(full);

    dai_material_host_entry *e = nullptr;
    for (auto &c : g_material_host_cache)
        if (c.rel == rel) { e = &c; break; }
    if (e && e->stamp == stamp) {
        // The material itself is unchanged - but a map it names may have been
        // re-baked, and that is a texture swap, not a new material.
        long map_stamp = 0;
        dai_texture base = dai_material_host_texture(h, e->file.base_color_map, 1, &map_stamp);
        dai_texture orm = dai_material_host_texture(h, e->file.orm_map, 0, &map_stamp);
        dai_texture nrm = dai_material_host_texture(h, e->file.normal_map, 0, &map_stamp);
        if (map_stamp != e->map_stamp) {
            dai_material_desc d = dai_material_host_desc(e->file, e->rel.c_str(), base, orm, nrm);
            dai_render_material_update(h->renderer, e->mat, &d);
            e->map_stamp = map_stamp;
        }
        return e->mat;
    }

    dai_matfile m = dai_matfile_default();
    char merr[256] = { 0 };
    if (h->read) {
        void *bytes = nullptr; size_t len = 0;
        if (!h->read(full, &bytes, &len, h->read_user) || !bytes) return e ? e->mat : 0;
        dai_result mr = dai_matfile_from_text(&m, (const char *)bytes, len);
        if (h->release) h->release(bytes, h->read_user);
        if (mr != DAI_OK) return e ? e->mat : 0;
    } else if (dai_matfile_load(&m, full, merr, sizeof(merr)) != DAI_OK) {
        return e ? e->mat : 0;
    }

    long map_stamp = 0;
    dai_texture base = dai_material_host_texture(h, m.base_color_map, 1, &map_stamp);
    dai_texture orm = dai_material_host_texture(h, m.orm_map, 0, &map_stamp);
    dai_texture nrm = dai_material_host_texture(h, m.normal_map, 0, &map_stamp);

    if (!e) {
        g_material_host_cache.push_back(dai_material_host_entry{});
        e = &g_material_host_cache.back();
        e->rel = rel;
    }
    e->file = m;
    e->stamp = stamp;
    e->map_stamp = map_stamp;
    dai_material_desc d = dai_material_host_desc(m, e->rel.c_str(), base, orm, nrm);
    // The handle is created once and REWRITTEN afterwards: every entity that
    // already wears this material sees the edit without being told about it,
    // which is the whole reason a material is a file and not a set of numbers
    // on the object.
    if (e->mat) dai_render_material_update(h->renderer, e->mat, &d);
    else        e->mat = dai_render_material_create(h->renderer, &d);
    return e->mat;
}

static void dai_material_host_apply(const dai_ext_host *h) {
    if (!h || !h->doc || !h->sync || !h->scene || !h->renderer) return;
    if (!h->assets_dir || !h->assets_dir[0]) return;

    // A different renderer means every handle in the cache belongs to a device
    // that is gone. Start over rather than hand out an index into nothing.
    if (g_material_host_renderer != h->renderer) {
        g_material_host_cache.clear();
        g_material_host_textures.clear();
        g_material_host_renderer = h->renderer;
    }

    uint32_t count = dai_doc_count(h->doc);
    if (!count) return;
    std::vector<dai_node> ids(count);
    dai_doc_nodes(h->doc, ids.data(), count);

    // ONE resolve per material PATH, not per node. A scene of 569 blockouts
    // painted with a dozen materials used to call dai_material_host_for 569
    // times a frame, and every one of those stat()s its .daimat and all three
    // of its maps - roughly 2000 syscalls per frame, which measured 6.8 ms and
    // ate half the 16.7 ms a 60 Hz frame has before anything is drawn. The
    // handles it returns are identical for identical paths, so the second ask
    // was never anything but waste. Cleared every call: a material file that
    // changed on disk is still picked up on the next frame, not cached away.
    std::map<std::string, dai_material> resolved_this_frame;

    for (dai_node n : ids) {
        dai_node_desc r{};
        if (dai_doc_get(h->doc, n, &r) != DAI_OK || !r.materials[0]) continue;
        // Slot 0 is the surface of the whole object, exactly as the host's own
        // apply_materials() reads it.
        std::string first = r.materials;
        size_t semi = first.find(';');
        if (semi != std::string::npos) first = first.substr(0, semi);
        if (!dai_matfile_is_file(first.c_str())) continue;

        dai_material mat;
        std::map<std::string, dai_material>::iterator rit = resolved_this_frame.find(first);
        if (rit != resolved_this_frame.end()) {
            mat = rit->second;
        } else {
            mat = dai_material_host_for(h, first);
            resolved_this_frame[first] = mat;
        }
        if (!mat) continue;
        dai_entity e = dai_doc_sync_entity(h->sync, n);
        if (!e) continue;
        // Set every frame, not only on a change: the sync layer throws an
        // entity away and respawns it whenever a node's mesh or scale moves,
        // and a respawned entity wears the default material again. Writing a
        // uint32 per object per frame is cheaper than finding out it was
        // missed one frame in a thousand.
        dai_scene_set_material(h->scene, e, mat);
    }
}
