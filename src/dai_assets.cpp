// Assets: Mnemosyne for the bytes, dai_gltf for the meaning.
//
// The one rule that shapes this file: a loader runs on a WORKER THREAD and
// must not touch the GPU, but importing a glTF creates meshes and textures
// inside the renderer. So the work is split exactly where Mnemosyne already
// splits it - `load` copies the bytes off-thread, `finalize` runs inside
// dai_assets_poll() on the thread that owns the Vulkan context and does the
// import there. Without that split every model load would stall the frame.

#include "dai_assets.h"
#include "mnemosyne.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// What Mnemosyne stores per asset. It exists from the moment the bytes are
// read; `model` appears one poll later, when the finaliser has run.
struct ModelAsset {
    dai_assets          *owner = nullptr;
    std::string          path;      // normalised, without the #selector
    std::vector<uint8_t> bytes;     // dropped once the import succeeded
    dai_model           *model = nullptr;
    char                 err[192] = { 0 };
};

std::string strip_selector(const char *path, std::string *out_node) {
    std::string p = path ? path : "";
    size_t hash = p.find('#');
    if (hash == std::string::npos) { if (out_node) out_node->clear(); return p; }
    if (out_node) *out_node = p.substr(hash + 1);
    return p.substr(0, hash);
}

std::string dir_of(const std::string &p) {
    size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? std::string() : p.substr(0, slash);
}

} // namespace

struct dai_assets {
    dai_renderer *r = nullptr;
    mne_registry *reg = nullptr;
    uint32_t      revision = 0;
    char          err[256] = { 0 };
};

namespace {

// ---- the Mnemosyne type ---------------------------------------------------

mne_result model_load(const char *path, const void *bytes, size_t size, void **out, void *user) {
    dai_assets *a = (dai_assets *)user;
    if (!size) return MNE_ERR_FORMAT;
    ModelAsset *m = new ModelAsset();
    m->owner = a;
    m->path = path ? path : "";
    m->bytes.assign((const uint8_t *)bytes, (const uint8_t *)bytes + size);
    *out = m;
    return MNE_OK;   // nothing GPU shaped has happened yet, on purpose
}

// External buffers and images a .gltf refers to. Resolved through the SAME
// mount table, so a .gltf + .bin + .png trio works inside a pack file, where
// there is no directory to look next to.
struct SidecarCtx {
    dai_assets          *a;
    std::string          base;   // directory of the glTF inside the mounts
    std::vector<uint8_t> buf;
};

int sidecar_read(const char *uri, const void **out_bytes, size_t *out_size, void *user) {
    SidecarCtx *c = (SidecarCtx *)user;
    std::string full = c->base.empty() ? std::string(uri) : c->base + "/" + uri;
    size_t need = mne_read_file(c->a->reg, full.c_str(), nullptr, 0);
    if (!need) return 0;
    c->buf.resize(need);
    if (mne_read_file(c->a->reg, full.c_str(), c->buf.data(), c->buf.size()) != need) return 0;
    *out_bytes = c->buf.data();
    *out_size = c->buf.size();
    return 1;
}

mne_result model_finalize(void *object, void *user) {
    ModelAsset *m = (ModelAsset *)object;
    dai_assets *a = (dai_assets *)user;
    if (!m || !a) return MNE_ERR_INVALID_ARG;

    SidecarCtx ctx{ a, dir_of(m->path), {} };
    char err[192] = { 0 };
    dai_model *model = dai_gltf_load_memory(a->r, m->bytes.data(), m->bytes.size(),
                                            sidecar_read, &ctx, err, sizeof(err));
    if (!model) {
        std::snprintf(m->err, sizeof(m->err), "%s", err[0] ? err : "import failed");
        std::snprintf(a->err, sizeof(a->err), "%s: %s", m->path.c_str(), m->err);
        return MNE_ERR_FORMAT;
    }
    // A hot reload builds a fresh ModelAsset and frees the old one, so this is
    // always the first import for this object. The meshes and textures the
    // PREVIOUS version created stay in the renderer: the engine has no mesh
    // free path yet, so editing a model in a long session grows memory. That
    // is a real limit, written down rather than papered over.
    m->model = model;
    m->err[0] = 0;
    m->bytes.clear();
    m->bytes.shrink_to_fit();
    a->revision++;
    return MNE_OK;
}

void model_free(void *object, void *user) {
    ModelAsset *m = (ModelAsset *)object;
    dai_assets *a = (dai_assets *)user;
    if (!m) return;
    // Give the meshes and textures back, not just the CPU side struct. This is
    // what makes a hot reload cost nothing over a long session: Mnemosyne frees
    // the old asset right after the new one finalised, so the freed ranges are
    // there for the reload after that.
    if (m->model) dai_model_release(a ? a->r : nullptr, m->model);
    delete m;
}

size_t model_size(const void *object, void *user) {
    (void)user;
    const ModelAsset *m = (const ModelAsset *)object;
    if (!m) return 0;
    size_t n = m->bytes.size() + sizeof(ModelAsset);
    if (m->model) {
        dai_model_info info = dai_model_get_info(m->model);
        // The vertices live on the GPU, not here; count them anyway so the
        // budget reflects what a model actually costs the machine.
        n += (size_t)info.vertices * 32 + (size_t)info.triangles * 12;
    }
    return n;
}

// Loads (or finds) the asset for a path without its #selector.
//
// mne_load takes a reference every time it is called, and the resolver runs
// for every node on every rebuild - so ask first and only load once. The one
// reference taken here is deliberately never released: a mesh handle already
// handed to the live scene must not be evicted out from under it.
ModelAsset *asset_for(dai_assets *a, const std::string &file, bool blocking) {
    if (!a || file.empty()) return nullptr;
    mne_asset h = mne_find(a->reg, mne_id_from_path(file.c_str()));
    if (h == MNE_INVALID_ASSET) h = mne_load(a->reg, file.c_str());
    if (h == MNE_INVALID_ASSET) return nullptr;
    if (blocking) mne_wait(a->reg, h);
    return (ModelAsset *)mne_get(a->reg, h);   // NULL unless READY
}

} // namespace

extern "C" {

dai_assets *dai_assets_create(dai_renderer *r, int watch_files) {
    if (!r) return nullptr;
    dai_assets *a = new dai_assets();
    a->r = r;

    mne_config cfg{};
    cfg.watch_files = watch_files ? 1 : 0;
    cfg.memory_budget = 0;      // no eviction: a mesh handle handed to the
                                // scene must not stop existing underneath it
    a->reg = mne_create(&cfg);
    if (!a->reg) { delete a; return nullptr; }

    mne_type_desc t{};
    t.name = "model";
    t.extensions = "glb,gltf";
    t.load = model_load;
    t.finalize = model_finalize;   // the GPU half, on the polling thread
    t.free_object = model_free;
    t.size_of = model_size;
    t.user = a;
    mne_register_type(a->reg, &t);
    return a;
}

void dai_assets_destroy(dai_assets *a) {
    if (!a) return;
    mne_destroy(a->reg);           // frees every ModelAsset through model_free
    delete a;
}

dai_result dai_assets_mount_dir(dai_assets *a, const char *dir, int priority) {
    if (!a || !dir) return DAI_ERR_INVALID_ARG;
    if (mne_mount_dir(a->reg, dir, priority) != MNE_OK) {
        std::snprintf(a->err, sizeof(a->err), "%s", mne_last_error(a->reg));
        return DAI_ERR_NOT_FOUND;
    }
    return DAI_OK;
}

dai_result dai_assets_mount_pack(dai_assets *a, const char *pack_path, int priority) {
    if (!a || !pack_path) return DAI_ERR_INVALID_ARG;
    if (mne_mount_pack(a->reg, pack_path, priority) != MNE_OK) {
        std::snprintf(a->err, sizeof(a->err), "%s", mne_last_error(a->reg));
        return DAI_ERR_NOT_FOUND;
    }
    return DAI_OK;
}

uint32_t dai_assets_poll(dai_assets *a) {
    if (!a) return 0;
    uint32_t before = a->revision;
    mne_poll(a->reg);
    // mne_poll counts every state change, including failures; the revision
    // only moves when a model actually became usable. Report the second -
    // re-resolving is only worth it when handles really appeared.
    return a->revision - before;
}

dai_model *dai_assets_model(dai_assets *a, const char *path) {
    std::string node;
    std::string file = strip_selector(path, &node);
    ModelAsset *m = asset_for(a, file, false);
    return m ? m->model : nullptr;
}

dai_model *dai_assets_model_blocking(dai_assets *a, const char *path) {
    std::string node;
    std::string file = strip_selector(path, &node);
    ModelAsset *m = asset_for(a, file, true);
    return m ? m->model : nullptr;
}

int dai_assets_resolve(const char *path, uint32_t *out_mesh, uint32_t *out_material,
                       dai_vec3 *out_render_scale, void *user) {
    dai_assets *a = (dai_assets *)user;
    if (!a || !path || !path[0]) return 0;

    std::string node_name;
    std::string file = strip_selector(path, &node_name);
    ModelAsset *m = asset_for(a, file, false);
    if (!m || !m->model) return 0;          // missing, still loading, or failed

    const dai_model_node *n = nullptr;
    if (!node_name.empty()) {
        n = dai_model_find(m->model, node_name.c_str());
        if (!n) {
            std::snprintf(a->err, sizeof(a->err), "%s: no node named '%s'",
                          file.c_str(), node_name.c_str());
            return 0;                        // a typo must not silently draw
        }                                    // the wrong object
    } else {
        if (!dai_model_node_count(m->model)) return 0;
        n = dai_model_node_at(m->model, 0);
    }
    if (!n) return 0;

    if (out_mesh) *out_mesh = n->mesh;
    if (out_material) *out_material = n->material;
    // The node's own scale from the file. The sync layer multiplies it by the
    // document node's scale, so a model authored at 2m stays 2m and the
    // editor's scale still works on top of it.
    if (out_render_scale) *out_render_scale = n->scale;
    return 1;
}

void dai_assets_bind(dai_assets *a, dai_doc_sync *sync) {
    if (!a || !sync) return;
    dai_doc_sync_resolver(sync, dai_assets_resolve, a);
}

uint32_t dai_assets_tracked(dai_assets *a) {
    if (!a) return 0;
    mne_stats st{};
    mne_get_stats(a->reg, &st);
    return st.tracked;
}

uint32_t dai_assets_ready(dai_assets *a) {
    if (!a) return 0;
    mne_stats st{};
    mne_get_stats(a->reg, &st);
    return st.resident;
}

uint32_t dai_assets_failed(dai_assets *a) {
    if (!a) return 0;
    mne_stats st{};
    mne_get_stats(a->reg, &st);
    return st.failed;
}

uint32_t dai_assets_revision(dai_assets *a) { return a ? a->revision : 0; }

const char *dai_assets_last_error(dai_assets *a) { return a ? a->err : ""; }

const char *dai_assets_error_of(dai_assets *a, const char *path) {
    if (!a || !path) return "";
    std::string node;
    std::string file = strip_selector(path, &node);
    mne_asset h = mne_find(a->reg, mne_id_from_path(file.c_str()));
    if (h == MNE_INVALID_ASSET) return "";
    mne_state s = mne_state_of(a->reg, h);
    if (s == MNE_STATE_MISSING) return "not found in any mounted source";
    ModelAsset *m = (ModelAsset *)mne_get(a->reg, h);
    if (m && m->err[0]) return m->err;
    const char *e = mne_error_of(a->reg, h);
    return e ? e : "";
}

} // extern "C"
