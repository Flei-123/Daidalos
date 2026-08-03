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

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
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
    // Mnemosyne does not enumerate its mounts - it is built to ANSWER for a
    // path, not to list them - so the browser's source is kept here.
    std::vector<std::string> dirs;
    std::vector<std::string> packs;
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
    a->dirs.push_back(dir);
    return DAI_OK;
}

dai_result dai_assets_mount_pack(dai_assets *a, const char *pack_path, int priority) {
    if (!a || !pack_path) return DAI_ERR_INVALID_ARG;
    if (mne_mount_pack(a->reg, pack_path, priority) != MNE_OK) {
        std::snprintf(a->err, sizeof(a->err), "%s", mne_last_error(a->reg));
        return DAI_ERR_NOT_FOUND;
    }
    a->packs.push_back(pack_path);
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

uint32_t dai_assets_resolve(const char *path, dai_render_part *out, uint32_t max, void *user) {
    dai_assets *a = (dai_assets *)user;
    if (!a || !path || !path[0]) return 0;

    std::string node_name;
    std::string file = strip_selector(path, &node_name);
    ModelAsset *m = asset_for(a, file, false);
    if (!m || !m->model) return 0;          // missing, still loading, or failed

    auto write = [&](const dai_model_node *n, uint32_t at) {
        if (!out || at >= max) return;
        out[at].mesh = n->mesh;
        out[at].material = n->material;
        out[at].position = n->position;
        out[at].rotation = n->rotation;
        out[at].scale = n->scale;
    };

    // "file.glb#Object" is one piece out of a file that holds several.
    if (!node_name.empty()) {
        const dai_model_node *n = dai_model_find(m->model, node_name.c_str());
        if (!n) {
            std::snprintf(a->err, sizeof(a->err), "%s: no node named '%s'",
                          file.c_str(), node_name.c_str());
            return 0;                        // a typo must not silently draw
        }                                    // the wrong object
        write(n, 0);
        return 1;
    }

    // No selector: the whole model. A five object Blender export is one scene
    // node with five pieces, not five scene nodes the user has to keep in step.
    uint32_t count = dai_model_node_count(m->model);
    for (uint32_t i = 0; i < count; ++i) {
        const dai_model_node *n = dai_model_node_at(m->model, i);
        if (n) write(n, i);
    }
    return count;
}

void dai_assets_bind(dai_assets *a, dai_doc_sync *sync) {
    if (!a || !sync) return;
    dai_doc_sync_resolver(sync, dai_assets_resolve, a);
}

dai_node dai_assets_instantiate(dai_assets *a, dai_doc *doc, const char *path, dai_node parent) {
    if (!a || !doc || !path || !path[0]) return 0;

    std::string sel;
    std::string file = strip_selector(path, &sel);
    ModelAsset *m = asset_for(a, file, false);
    if (!m || !m->model) {
        std::snprintf(a->err, sizeof(a->err), "%s is not loaded yet", file.c_str());
        return 0;                       // deliberately not blocking - see the header
    }
    uint32_t count = dai_model_node_count(m->model);
    if (!count) {
        std::snprintf(a->err, sizeof(a->err), "%s has nothing to draw", file.c_str());
        return 0;
    }

    dai_doc_begin(doc, "Instantiate model");

    // A piece can point at a parent that has not been created yet only if the
    // file is malformed - glTF children always follow their parent in the walk
    // - but map defensively anyway and fall back to the requested parent.
    std::vector<dai_node> made(count, 0);
    dai_node root = 0;

    for (uint32_t i = 0; i < count; ++i) {
        const dai_model_node *n = dai_model_node_at(m->model, i);
        if (!n) continue;

        dai_node_desc d = dai_node_desc_default();
        std::snprintf(d.name, sizeof(d.name), "%s", n->name[0] ? n->name : "Piece");
        // Each node draws exactly its own piece. Without the selector every
        // node would draw the whole model and the scene would be a pile of
        // copies of itself.
        if (n->name[0]) std::snprintf(d.asset, sizeof(d.asset), "%s#%s", file.c_str(), n->name);
        else            std::snprintf(d.asset, sizeof(d.asset), "%s", file.c_str());

        d.position = n->local_position;
        d.rotation = n->local_rotation;
        d.scale = n->local_scale;

        // Collision shape from the piece's own box. The pivot is assumed to be
        // inside it - a Blender object whose origin is far outside its mesh
        // gets a box in the wrong place, and there is nowhere in the document
        // to put a shape offset yet.
        dai_vec3 half{ (n->bounds_max.x - n->bounds_min.x) * 0.5f,
                       (n->bounds_max.y - n->bounds_min.y) * 0.5f,
                       (n->bounds_max.z - n->bounds_min.z) * 0.5f };
        if (half.x < 1e-4f) half.x = 1e-4f;
        if (half.y < 1e-4f) half.y = 1e-4f;
        if (half.z < 1e-4f) half.z = 1e-4f;
        d.half_extent = half;
        d.shape = DAI_SHAPE_BOX;
        d.motion = DAI_STATIC;          // the scene decides what moves, not the file

        dai_node p = parent;
        if (n->parent >= 0 && (uint32_t)n->parent < count && made[(size_t)n->parent])
            p = made[(size_t)n->parent];
        d.parent = p;

        dai_node created = dai_doc_add(doc, &d);
        made[i] = created;
        if (!root) root = created;
    }

    dai_doc_commit(doc);
    return root;
}

uint32_t dai_assets_list(dai_assets *a, char *out, uint32_t max, uint32_t stride) {
    if (!a) return 0;

    std::vector<std::string> found;

    // Folders, walked depth first. Only what the model loader can actually
    // open - listing a .txt the browser cannot place would be a lie.
    struct Walk {
        static bool loadable(const std::string &p) {
            size_t dot = p.find_last_of('.');
            if (dot == std::string::npos) return false;
            std::string e = p.substr(dot + 1);
            for (char &c : e) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            // .js is not placeable but ATTACHABLE ("Assign to selection") -
            // a script the browser cannot show could never be assigned.
            return e == "glb" || e == "gltf" || e == "js";
        }
        static void go(const std::string &root, const std::string &rel,
                       std::vector<std::string> &out, int depth) {
            if (depth > 8) return;                  // a symlink loop is not a reason to hang
            std::string dir = rel.empty() ? root : root + "/" + rel;
            DIR *d = opendir(dir.c_str());
            if (!d) return;
            while (struct dirent *e = readdir(d)) {
                std::string n = e->d_name;
                if (n == "." || n == ".." || n[0] == '.') continue;
                std::string child = rel.empty() ? n : rel + "/" + n;
                struct stat st;
                if (stat((root + "/" + child).c_str(), &st) != 0) continue;
                if ((st.st_mode & S_IFMT) == S_IFDIR) go(root, child, out, depth + 1);
                else if (loadable(child)) out.push_back(child);
            }
            closedir(d);
        }
    };
    for (const std::string &dir : a->dirs) Walk::go(dir, "", found, 0);

    // Packs know their own contents.
    for (const std::string &pack : a->packs) {
        uint32_t n = mne_pack_list(pack.c_str(), nullptr, 0);
        if (!n) continue;
        std::vector<const char *> names(n);
        mne_pack_list(pack.c_str(), names.data(), n);
        for (uint32_t i = 0; i < n; ++i)
            if (names[i] && Walk::loadable(names[i])) found.push_back(names[i]);
    }

    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());

    if (out && max && stride) {
        uint32_t n = (uint32_t)found.size() < max ? (uint32_t)found.size() : max;
        for (uint32_t i = 0; i < n; ++i)
            std::snprintf(out + (size_t)i * stride, stride, "%s", found[i].c_str());
    }
    return (uint32_t)found.size();
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
