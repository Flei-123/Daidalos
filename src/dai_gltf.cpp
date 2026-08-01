#include "dai_gltf_common.hpp"

// ---------------------------------------------------------------- model

// Everything below the drawable nodes: the hierarchy has to survive the import
// or animation is impossible - you cannot pose a scene you flattened away.
struct RawNode {
    float t[3] = { 0, 0, 0 };
    float r[4] = { 0, 0, 0, 1 };
    float s[3] = { 1, 1, 1 };
    int mesh = -1, skin = -1;
    std::vector<int> children;
    bool has_matrix = false;
    M4 matrix{};
};

struct Skin {
    std::vector<int> joints;          // node indices
    std::vector<M4>  inverse_bind;
    uint32_t offset = 0;              // where this skin's matrices start
};

struct Channel {
    int node = -1;
    int path = 0;                     // 0 = translation, 1 = rotation, 2 = scale
    int interpolation = 0;            // 0 = linear, 1 = step, 2 = cubic spline
    std::vector<float> times;
    std::vector<float> values;        // 3 or 4 floats per key (x3 for cubic)
};

struct Animation {
    std::string name;
    float duration = 0.0f;
    std::vector<Channel> channels;
};

struct dai_model {
    // Everything this import created inside the renderer. Nothing else uses
    // these handles, so the model can give them all back - which is what
    // dai_model_release does and what a hot reload needs to not leak.
    std::vector<dai_mesh>     owned_meshes;
    std::vector<dai_texture>  owned_textures;
    std::vector<dai_material> owned_materials;

    std::vector<dai_model_node> nodes;      // drawable pieces
    std::vector<int> node_of_draw;          // which RawNode each piece came from
    std::vector<int> skin_of_draw;          // and which skin, -1 for rigid
    std::vector<RawNode> raw;
    std::vector<Skin> skins;
    std::vector<Animation> anims;
    std::vector<M4> world;                  // scratch, one per raw node
    dai_model_info info{};
};

extern "C" {

dai_model *dai_gltf_load_memory(dai_renderer *r, const void *data, size_t size,
                                dai_gltf_read_fn sidecar, void *user,
                                char *err, size_t err_len) {
    auto bail = [&](const char *m) -> dai_model * {
        if (err && err_len) std::snprintf(err, err_len, "%s", m);
        return nullptr;
    };
    if (!r || !data || !size) return bail("no renderer or no bytes");

    // A view, not a copy: the caller owns the bytes for the duration of the
    // call, which is exactly what an asset cache wants to hear.
    struct { const uint8_t *p; size_t n;
             size_t size() const { return n; } const uint8_t *data() const { return p; } }
        file{ (const uint8_t *)data, size };

    // Collected while importing, moved into the model once it exists.
    std::vector<dai_mesh>     owned_meshes;
    std::vector<dai_texture>  owned_textures;
    std::vector<dai_material> owned_materials;

    Loader ld;
    ld.r = r; ld.err = err; ld.err_len = err_len;
    ld.sidecar = sidecar; ld.sidecar_user = user;

    const char *json_text = nullptr;
    size_t json_len = 0;

    if (file.size() > 12 && !std::memcmp(file.data(), "glTF", 4)) {
        // GLB container: 12 byte header, then chunks (length, type, payload)
        uint32_t total; std::memcpy(&total, file.data() + 8, 4);
        size_t pos = 12;
        while (pos + 8 <= file.size()) {
            uint32_t clen, ctype;
            std::memcpy(&clen, file.data() + pos, 4);
            std::memcpy(&ctype, file.data() + pos + 4, 4);
            const uint8_t *payload = file.data() + pos + 8;
            if (pos + 8 + clen > file.size()) return bail("truncated GLB chunk");
            if (ctype == 0x4E4F534A) { json_text = (const char *)payload; json_len = clen; }
            else if (ctype == 0x004E4942) { ld.glb_bin = payload; ld.glb_bin_size = clen; }
            pos += 8 + clen + ((4 - (clen & 3)) & 3);
        }
        if (!json_text) return bail("GLB without a JSON chunk");
    } else {
        json_text = (const char *)file.data();
        json_len = file.size();
    }

    std::string jerr;
    if (!ld.doc.parse(json_text, json_len, &jerr)) {
        if (err && err_len) std::snprintf(err, err_len, "%s", jerr.c_str());
        return nullptr;
    }
    ld.root = ld.doc.root();
    if (!ld.root || ld.root->type != Value::OBJECT) return bail("glTF root is not an object");

    // ---- buffers
    if (const Value *bufs = ld.root->get("buffers")) {
        for (size_t i = 0; i < bufs->size(); ++i) {
            const Value *b = bufs->at(i);
            std::vector<uint8_t> data;
            const char *uri = b->str_at("uri", "");
            if (!uri[0]) {
                if (!ld.glb_bin) return bail("buffer without uri and no GLB binary chunk");
                data.assign(ld.glb_bin, ld.glb_bin + ld.glb_bin_size);
            } else if (!std::strncmp(uri, "data:", 5)) {
                const char *comma = std::strchr(uri, ',');
                if (!comma) return bail("malformed data uri");
                base64_decode(comma + 1, std::strlen(comma + 1), data);
            } else {
                if (!ld.read_ref(uri_decode(uri), data))
                    return bail("cannot read external buffer");
            }
            ld.buffers.push_back(std::move(data));
        }
    }

    // ---- textures, cached per (image, colour space): the same image can be
    //      a base colour map (sRGB) and an ORM map (linear) in one file
    std::unordered_map<uint64_t, dai_texture> tex_cache;
    auto load_texture = [&](int tex_index, bool srgb) -> dai_texture {
        if (tex_index < 0) return 0;
        const Value *texs = ld.root->get("textures");
        const Value *t = texs ? texs->at((size_t)tex_index) : nullptr;
        if (!t) return 0;
        int img_index = t->int_at("source", -1);
        if (img_index < 0) return 0;
        uint64_t key = ((uint64_t)img_index << 1) | (srgb ? 1u : 0u);
        auto it = tex_cache.find(key);
        if (it != tex_cache.end()) return it->second;

        const Value *imgs = ld.root->get("images");
        const Value *im = imgs ? imgs->at((size_t)img_index) : nullptr;
        if (!im) return 0;

        std::vector<uint8_t> bytes;
        const char *uri = im->str_at("uri", "");
        if (uri[0] && std::strncmp(uri, "data:", 5)) {
            if (!ld.read_ref(uri_decode(uri), bytes)) return 0;
        } else if (uri[0]) {
            const char *comma = std::strchr(uri, ',');
            if (!comma) return 0;
            base64_decode(comma + 1, std::strlen(comma + 1), bytes);
        } else {
            int view = im->int_at("bufferView", -1);
            size_t size = 0;
            const uint8_t *p = ld.view_data(view, &size, nullptr);
            if (!p) return 0;
            bytes.assign(p, p + size);
        }

        std::vector<uint8_t> rgba;
        uint32_t w = 0, h = 0;
        char terr[128] = {0};
        if (!daiimg::read_png(bytes.data(), bytes.size(), rgba, &w, &h, terr, sizeof(terr))) {
            // JPEG and KTX2 are legal in glTF but not decoded here; a white
            // texture keeps the material usable instead of failing the load
            tex_cache[key] = 0;
            return 0;
        }
        dai_texture tex = dai_render_texture_create(r, rgba.data(), w, h, srgb ? 1 : 0);
        tex_cache[key] = tex;
        if (tex) owned_textures.push_back(tex);
        return tex;
    };

    // ---- materials
    std::vector<dai_material> materials;
    if (const Value *mats = ld.root->get("materials")) {
        for (size_t i = 0; i < mats->size(); ++i) {
            const Value *m = mats->at(i);
            dai_material_desc d = dai_material_desc_default();
            std::string name = m->str_at("name", "");
            if (!name.empty()) d.name = name.c_str();

            if (const Value *pbr = m->get("pbrMetallicRoughness")) {
                if (const Value *bc = pbr->get("baseColorFactor")) {
                    d.base_color = { (float)bc->at(0)->num(1), (float)bc->at(1)->num(1), (float)bc->at(2)->num(1) };
                }
                d.metallic = (float)pbr->num_at("metallicFactor", 1.0);
                d.roughness = (float)pbr->num_at("roughnessFactor", 1.0);
                if (const Value *t = pbr->get("baseColorTexture"))
                    d.base_color_tex = load_texture(t->int_at("index", -1), true);
                if (const Value *t = pbr->get("metallicRoughnessTexture"))
                    d.orm_tex = load_texture(t->int_at("index", -1), false);
            }
            if (const Value *t = m->get("normalTexture")) {
                d.normal_tex = load_texture(t->int_at("index", -1), false);
                d.normal_strength = (float)t->num_at("scale", 1.0);
            }
            if (const Value *t = m->get("occlusionTexture")) {
                dai_texture occ = load_texture(t->int_at("index", -1), false);
                if (!d.orm_tex) d.orm_tex = occ;           // glTF packs AO in R of the same map
                d.occlusion = (float)t->num_at("strength", 1.0);
            }
            if (const Value *e = m->get("emissiveFactor"))
                d.emissive = { (float)e->at(0)->num(0), (float)e->at(1)->num(0), (float)e->at(2)->num(0) };
            if (const Value *t = m->get("emissiveTexture"))
                d.emissive_tex = load_texture(t->int_at("index", -1), true);
            if (!std::strcmp(m->str_at("alphaMode", "OPAQUE"), "MASK"))
                d.alpha_cutoff = (float)m->num_at("alphaCutoff", 0.5);

            dai_material created = dai_render_material_create(r, &d);
            materials.push_back(created);
            if (created) owned_materials.push_back(created);
        }
    }

    // ---- meshes: one renderer mesh per primitive
    struct Prim { dai_mesh mesh; dai_material material; uint32_t tris; uint32_t verts;
                  float lo[3]; float hi[3]; };   // local AABB, for real bounds
    std::vector<std::vector<Prim>> mesh_prims;
    uint32_t total_tris = 0, total_verts = 0;
    if (const Value *meshes = ld.root->get("meshes")) {
        for (size_t i = 0; i < meshes->size(); ++i) {
            const Value *mesh = meshes->at(i);
            std::vector<Prim> prims;
            const Value *plist = mesh->get("primitives");
            for (size_t p = 0; plist && p < plist->size(); ++p) {
                const Value *prim = plist->at(p);
                if (prim->int_at("mode", 4) != 4) continue;          // triangles only
                const Value *attrs = prim->get("attributes");
                if (!attrs) continue;
                int a_pos = attrs->int_at("POSITION", -1);
                int a_nrm = attrs->int_at("NORMAL", -1);
                int a_uv  = attrs->int_at("TEXCOORD_0", -1);
                int a_jnt = attrs->int_at("JOINTS_0", -1);
                int a_wgt = attrs->int_at("WEIGHTS_0", -1);
                if (a_pos < 0) continue;

                std::vector<float> pos, nrm, uv, jnt, wgt;
                size_t vcount = 0;
                if (!ld.read_accessor_float(a_pos, 3, pos, &vcount)) return nullptr;
                if (a_nrm >= 0) ld.read_accessor_float(a_nrm, 3, nrm, nullptr);
                if (a_uv  >= 0) ld.read_accessor_float(a_uv,  2, uv,  nullptr);
                // joints arrive as unsigned bytes or shorts, weights as floats or
                // normalised integers; read_accessor_float handles both
                if (a_jnt >= 0) ld.read_accessor_float(a_jnt, 4, jnt, nullptr);
                if (a_wgt >= 0) ld.read_accessor_float(a_wgt, 4, wgt, nullptr);

                std::vector<uint32_t> idx;
                int a_idx = prim->int_at("indices", -1);
                if (a_idx >= 0) { if (!ld.read_indices(a_idx, idx)) return nullptr; }
                else { idx.resize(vcount); for (size_t k = 0; k < vcount; ++k) idx[k] = (uint32_t)k; }

                std::vector<dai_vertex> verts(vcount);
                for (size_t v = 0; v < vcount; ++v) {
                    verts[v].position = { pos[v*3], pos[v*3+1], pos[v*3+2] };
                    verts[v].normal = nrm.size() ? dai_vec3{ nrm[v*3], nrm[v*3+1], nrm[v*3+2] } : dai_vec3{ 0, 1, 0 };
                    verts[v].cap = 0.0f;
                    verts[v].u = uv.size() ? uv[v*2] : 0.0f;
                    verts[v].v = uv.size() ? uv[v*2+1] : 0.0f;
                    if (jnt.size() >= (v + 1) * 4 && wgt.size() >= (v + 1) * 4) {
                        float sum = 0.0f;
                        for (int k = 0; k < 4; ++k) {
                            int idx = (int)(jnt[v*4 + (size_t)k] + 0.5f);
                            verts[v].joints[k] = (uint8_t)(idx < 0 ? 0 : (idx > 255 ? 255 : idx));
                            verts[v].weights[k] = wgt[v*4 + (size_t)k];
                            sum += verts[v].weights[k];
                        }
                        // exporters are not always exact; the shader divides by the
                        // sum anyway, but normalising here keeps the data honest
                        if (sum > 1e-6f) for (int k = 0; k < 4; ++k) verts[v].weights[k] /= sum;
                    }
                }
                // no normals in the file: flat shade from the triangles
                if (nrm.empty()) {
                    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                        dai_vertex &A = verts[idx[t]], &B = verts[idx[t+1]], &C = verts[idx[t+2]];
                        float e1[3] = { B.position.x-A.position.x, B.position.y-A.position.y, B.position.z-A.position.z };
                        float e2[3] = { C.position.x-A.position.x, C.position.y-A.position.y, C.position.z-A.position.z };
                        float n[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0] };
                        float len = sqrtf(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
                        if (len > 1e-12f) { n[0]/=len; n[1]/=len; n[2]/=len; }
                        A.normal = B.normal = C.normal = { n[0], n[1], n[2] };
                    }
                }

                Prim pr{};
                pr.lo[0] = pr.lo[1] = pr.lo[2] = 1e30f;
                pr.hi[0] = pr.hi[1] = pr.hi[2] = -1e30f;
                for (size_t v = 0; v < vcount; ++v) {
                    const float *q = &pos[v * 3];
                    for (int k = 0; k < 3; ++k) { if (q[k] < pr.lo[k]) pr.lo[k] = q[k]; if (q[k] > pr.hi[k]) pr.hi[k] = q[k]; }
                }
                pr.mesh = dai_render_mesh_create(r, verts.data(), (uint32_t)verts.size(), idx.data(), (uint32_t)idx.size());
                if (pr.mesh >= DAI_MESH_BUILTIN_COUNT) owned_meshes.push_back(pr.mesh);
                int mat = prim->int_at("material", -1);
                pr.material = (mat >= 0 && (size_t)mat < materials.size()) ? materials[(size_t)mat] : 0;
                pr.tris = (uint32_t)(idx.size() / 3);
                pr.verts = (uint32_t)vcount;
                total_tris += pr.tris;
                total_verts += pr.verts;
                prims.push_back(pr);
            }
            mesh_prims.push_back(std::move(prims));
        }
    }

    // ---- nodes, flattened to world space
    dai_model *model = new dai_model();

    // raw hierarchy first: animation needs it, and the flattening below only
    // produces the initial pose
    if (const Value *nlist = ld.root->get("nodes")) {
        model->raw.resize(nlist->size());
        for (size_t i = 0; i < nlist->size(); ++i) {
            const Value *n = nlist->at(i);
            RawNode &rn = model->raw[i];
            rn.mesh = n->int_at("mesh", -1);
            rn.skin = n->int_at("skin", -1);
            if (const Value *m = n->get("matrix")) {
                rn.has_matrix = true;
                for (int k = 0; k < 16; ++k) rn.matrix.m[k] = (float)m->at((size_t)k)->num(0);
            } else {
                if (const Value *v = n->get("translation")) for (int k = 0; k < 3; ++k) rn.t[k] = (float)v->at((size_t)k)->num(0);
                if (const Value *v = n->get("rotation")) for (int k = 0; k < 4; ++k) rn.r[k] = (float)v->at((size_t)k)->num(k == 3 ? 1 : 0);
                if (const Value *v = n->get("scale")) for (int k = 0; k < 3; ++k) rn.s[k] = (float)v->at((size_t)k)->num(1);
            }
            if (const Value *ch = n->get("children"))
                for (size_t k = 0; k < ch->size(); ++k) rn.children.push_back(ch->at(k)->integer(-1));
        }
    }

    // ---- skins
    if (const Value *skins = ld.root->get("skins")) {
        uint32_t offset = 0;
        for (size_t i = 0; i < skins->size(); ++i) {
            const Value *sk = skins->at(i);
            Skin s{};
            if (const Value *j = sk->get("joints"))
                for (size_t k = 0; k < j->size(); ++k) s.joints.push_back(j->at(k)->integer(-1));
            int ibm = sk->int_at("inverseBindMatrices", -1);
            s.inverse_bind.resize(s.joints.size());
            for (M4 &m : s.inverse_bind) m = m4_identity();
            if (ibm >= 0) {
                std::vector<float> raw16;
                size_t count = 0;
                if (ld.read_accessor_float(ibm, 16, raw16, &count)) {
                    for (size_t k = 0; k < s.joints.size() && k < count; ++k)
                        for (int e = 0; e < 16; ++e) s.inverse_bind[k].m[e] = raw16[k * 16 + (size_t)e];
                }
            }
            s.offset = offset;
            offset += (uint32_t)s.joints.size();
            model->skins.push_back(std::move(s));
        }
        model->info.joints = offset;
    }

    // ---- animations
    if (const Value *anims = ld.root->get("animations")) {
        for (size_t i = 0; i < anims->size(); ++i) {
            const Value *a = anims->at(i);
            Animation an;
            an.name = a->str_at("name", "");
            const Value *samplers = a->get("samplers");
            const Value *channels = a->get("channels");
            for (size_t c = 0; channels && c < channels->size(); ++c) {
                const Value *ch = channels->at(c);
                const Value *target = ch->get("target");
                if (!target) continue;
                Channel out;
                out.node = target->int_at("node", -1);
                std::string path = target->str_at("path", "");
                if (path == "translation") out.path = 0;
                else if (path == "rotation") out.path = 1;
                else if (path == "scale") out.path = 2;
                else continue;                       // weights (morph targets) not supported
                int si = ch->int_at("sampler", -1);
                const Value *sm = samplers ? samplers->at((size_t)si) : nullptr;
                if (!sm) continue;
                std::string interp = sm->str_at("interpolation", "LINEAR");
                out.interpolation = interp == "STEP" ? 1 : interp == "CUBICSPLINE" ? 2 : 0;
                size_t nkeys = 0;
                if (!ld.read_accessor_float(sm->int_at("input", -1), 1, out.times, &nkeys)) continue;
                int comps = out.path == 1 ? 4 : 3;
                if (!ld.read_accessor_float(sm->int_at("output", -1), comps, out.values, nullptr)) continue;
                if (!out.times.empty()) an.duration = fmaxf(an.duration, out.times.back());
                an.channels.push_back(std::move(out));
            }
            model->anims.push_back(std::move(an));
        }
    }
    float bmin[3] = { 1e30f, 1e30f, 1e30f }, bmax[3] = { -1e30f, -1e30f, -1e30f };

    const Value *nodes = ld.root->get("nodes");
    std::vector<char> visited(nodes ? nodes->size() : 0, 0);

    struct Walker {
        Loader &ld; const Value *nodes; dai_model *model;
        std::vector<std::vector<Prim>> &mesh_prims;
        float *bmin, *bmax;
        std::vector<char> &visited;

        // parent_draw: the piece a child should point at, -1 at the root.
        // parent_world: that piece's world matrix, needed to turn a flattened
        // world transform back into a local one.
        void walk(int index, const M4 &parent, int parent_draw, const M4 &parent_world) {
            const Value *n = nodes ? nodes->at((size_t)index) : nullptr;
            if (!n || visited[(size_t)index]) return;
            visited[(size_t)index] = 1;

            M4 local = m4_identity();
            if (const Value *m = n->get("matrix")) {
                for (int i = 0; i < 16; ++i) local.m[i] = (float)m->at((size_t)i)->num(0);
            } else {
                float t[3] = { 0,0,0 }, q[4] = { 0,0,0,1 }, s[3] = { 1,1,1 };
                if (const Value *v = n->get("translation")) for (int i = 0; i < 3; ++i) t[i] = (float)v->at((size_t)i)->num(0);
                if (const Value *v = n->get("rotation")) for (int i = 0; i < 4; ++i) q[i] = (float)v->at((size_t)i)->num(i == 3 ? 1 : 0);
                if (const Value *v = n->get("scale")) for (int i = 0; i < 3; ++i) s[i] = (float)v->at((size_t)i)->num(1);
                local = m4_trs(t, q, s);
            }
            M4 world = m4_mul(parent, local);

            int first_here = -1;
            int mesh = n->int_at("mesh", -1);
            if (mesh >= 0 && (size_t)mesh < mesh_prims.size()) {
                // Where this node sits relative to the nearest ancestor that
                // also draws something. A group node in between contributes its
                // transform but no piece, so the link skips it.
                M4 rel = parent_draw >= 0 ? m4_mul(m4_invert_affine(parent_world), world) : world;
                for (const Prim &p : mesh_prims[(size_t)mesh]) {
                    if (first_here < 0) first_here = (int)model->nodes.size();
                    dai_model_node mn{};
                    mn.mesh = p.mesh;
                    mn.material = p.material;
                    m4_decompose(world, &mn.position, &mn.rotation, &mn.scale);
                    // Several primitives on one node are siblings, not a chain:
                    // they all sit at the same place and share one parent.
                    mn.parent = parent_draw;
                    m4_decompose(rel, &mn.local_position, &mn.local_rotation, &mn.local_scale);
                    mn.bounds_min = { p.lo[0], p.lo[1], p.lo[2] };
                    mn.bounds_max = { p.hi[0], p.hi[1], p.hi[2] };
                    std::snprintf(mn.name, sizeof(mn.name), "%s", n->str_at("name", ""));
                    model->nodes.push_back(mn);
                    model->node_of_draw.push_back(index);
                    model->skin_of_draw.push_back(n->int_at("skin", -1));
                    // real bounds: transform the mesh AABB corners, not the pivot.
                    // A pivot based box says a 4 m limb is 1 m tall, and every
                    // camera that frames the model then sits inside it.
                    for (int cx = 0; cx < 8; ++cx) {
                        float local[3] = { (cx & 1) ? p.hi[0] : p.lo[0],
                                           (cx & 2) ? p.hi[1] : p.lo[1],
                                           (cx & 4) ? p.hi[2] : p.lo[2] };
                        float wp[3];
                        for (int i = 0; i < 3; ++i)
                            wp[i] = world.m[0*4+i]*local[0] + world.m[1*4+i]*local[1] + world.m[2*4+i]*local[2] + world.m[3*4+i];
                        for (int i = 0; i < 3; ++i) {
                            if (wp[i] < bmin[i]) bmin[i] = wp[i];
                            if (wp[i] > bmax[i]) bmax[i] = wp[i];
                        }
                    }
                }
            }
            // A node that drew something becomes the parent for everything
            // below it; one that did not passes its own parent through.
            int child_parent = first_here >= 0 ? first_here : parent_draw;
            const M4 &child_world = first_here >= 0 ? world : parent_world;
            if (const Value *ch = n->get("children"))
                for (size_t i = 0; i < ch->size(); ++i)
                    walk(ch->at(i)->integer(-1), world, child_parent, child_world);
        }
    } walker{ ld, nodes, model, mesh_prims, bmin, bmax, visited };

    M4 ident = m4_identity();
    bool walked_any = false;
    if (const Value *scenes = ld.root->get("scenes")) {
        int si = ld.root->int_at("scene", 0);
        const Value *sc = scenes->at((size_t)(si < 0 ? 0 : si));
        if (sc) {
            if (const Value *list = sc->get("nodes")) {
                for (size_t i = 0; i < list->size(); ++i) walker.walk(list->at(i)->integer(-1), ident, -1, ident);
                walked_any = true;
            }
        }
    }
    if (!walked_any && nodes)                       // no scene: take every root node
        for (size_t i = 0; i < nodes->size(); ++i) walker.walk((int)i, ident, -1, ident);

    model->info.animations = (uint32_t)model->anims.size();
    model->info.skins = (uint32_t)model->skins.size();
    model->info.nodes = (uint32_t)model->nodes.size();
    model->info.meshes = 0;
    for (auto &v : mesh_prims) model->info.meshes += (uint32_t)v.size();
    model->info.materials = (uint32_t)materials.size();
    model->info.textures = (uint32_t)tex_cache.size();
    model->info.triangles = total_tris;
    model->info.vertices = total_verts;
    if (model->nodes.empty()) { bmin[0]=bmin[1]=bmin[2]=bmax[0]=bmax[1]=bmax[2]=0; }
    model->info.bounds_min = { bmin[0], bmin[1], bmin[2] };
    model->info.bounds_max = { bmax[0], bmax[1], bmax[2] };
    model->owned_meshes = std::move(owned_meshes);
    model->owned_textures = std::move(owned_textures);
    model->owned_materials = std::move(owned_materials);
    return model;
}

namespace {
// The path based entry point is the memory one with a sidecar that reads from
// the file's own directory. One code path for both, so a .gltf with external
// buffers behaves the same whether it came off disk or out of a pack.
struct DiskSidecar { std::string dir; std::vector<uint8_t> buf; };
int disk_sidecar(const char *uri, const void **out, size_t *out_size, void *user) {
    DiskSidecar *d = (DiskSidecar *)user;
    if (!read_file(d->dir + "/" + uri, d->buf)) return 0;
    *out = d->buf.data(); *out_size = d->buf.size();
    return 1;
}
} // namespace

dai_model *dai_gltf_load(dai_renderer *r, const char *path, char *err, size_t err_len) {
    if (!r || !path) {
        if (err && err_len) std::snprintf(err, err_len, "no renderer or path");
        return nullptr;
    }
    std::vector<uint8_t> file;
    if (!read_file(path, file)) {
        if (err && err_len) std::snprintf(err, err_len, "cannot read file");
        return nullptr;
    }
    DiskSidecar side;
    side.dir = dir_of(path);
    return dai_gltf_load_memory(r, file.data(), file.size(), disk_sidecar, &side, err, err_len);
}

void dai_model_free(dai_model *m) { delete m; }

void dai_model_release(dai_renderer *r, dai_model *m) {
    if (!m) return;
    if (r) {
        // Order matters: materials stop referencing textures first, then the
        // textures go. Freeing the other way round makes unbind_texture walk
        // materials that are already recycled.
        for (dai_material mat : m->owned_materials) dai_render_material_destroy(r, mat);
        for (dai_texture t : m->owned_textures) dai_render_texture_destroy(r, t);
        for (dai_mesh me : m->owned_meshes) dai_render_mesh_destroy(r, me);
    }
    delete m;
}

dai_model_info dai_model_get_info(const dai_model *m) { return m ? m->info : dai_model_info{}; }
uint32_t dai_model_node_count(const dai_model *m) { return m ? (uint32_t)m->nodes.size() : 0; }

const dai_model_node *dai_model_node_at(const dai_model *m, uint32_t i) {
    return (m && i < m->nodes.size()) ? &m->nodes[i] : nullptr;
}

const dai_model_node *dai_model_find(const dai_model *m, const char *name) {
    if (!m || !name) return nullptr;
    for (const dai_model_node &n : m->nodes) if (!std::strcmp(n.name, name)) return &n;
    return nullptr;
}

// ---------------------------------------------------------------- animation

uint32_t dai_model_animation_count(const dai_model *m) { return m ? (uint32_t)m->anims.size() : 0; }

dai_animation_info dai_model_animation_at(const dai_model *m, uint32_t index) {
    dai_animation_info info{};
    if (!m || index >= m->anims.size()) return info;
    const Animation &a = m->anims[index];
    std::snprintf(info.name, sizeof(info.name), "%s", a.name.c_str());
    info.duration = a.duration;
    info.channels = (uint32_t)a.channels.size();
    return info;
}

namespace {

// Sampling one channel. glTF guarantees the times are sorted, so a binary
// search is correct and O(log n) - which matters once a rig has a few hundred
// keys per bone and you are posing a crowd.
void sample_channel(const Channel &c, float t, float *out, int comps) {
    if (c.times.empty()) return;
    size_t n = c.times.size();
    if (t <= c.times.front()) {
        for (int i = 0; i < comps; ++i) out[i] = c.values[(c.interpolation == 2 ? comps : 0) + i];
        return;
    }
    if (t >= c.times.back()) {
        size_t last = n - 1;
        size_t base = c.interpolation == 2 ? (last * 3 + 1) * (size_t)comps : last * (size_t)comps;
        for (int i = 0; i < comps; ++i) out[i] = c.values[base + (size_t)i];
        return;
    }
    size_t lo = 0, hi = n - 1;
    while (hi - lo > 1) { size_t mid = (lo + hi) / 2; if (c.times[mid] <= t) lo = mid; else hi = mid; }
    float span = c.times[hi] - c.times[lo];
    float u = span > 1e-9f ? (t - c.times[lo]) / span : 0.0f;

    if (c.interpolation == 1) {                       // STEP
        for (int i = 0; i < comps; ++i) out[i] = c.values[lo * (size_t)comps + (size_t)i];
        return;
    }
    if (c.interpolation == 2) {                       // CUBICSPLINE: in-tangent, value, out-tangent
        const float *v0 = &c.values[(lo * 3 + 1) * (size_t)comps];
        const float *b0 = &c.values[(lo * 3 + 2) * (size_t)comps];
        const float *a1 = &c.values[(hi * 3 + 0) * (size_t)comps];
        const float *v1 = &c.values[(hi * 3 + 1) * (size_t)comps];
        float u2 = u * u, u3 = u2 * u;
        float h00 = 2*u3 - 3*u2 + 1, h10 = u3 - 2*u2 + u, h01 = -2*u3 + 3*u2, h11 = u3 - u2;
        for (int i = 0; i < comps; ++i)
            out[i] = h00 * v0[i] + h10 * span * b0[i] + h01 * v1[i] + h11 * span * a1[i];
        return;
    }

    const float *va = &c.values[lo * (size_t)comps];
    const float *vb = &c.values[hi * (size_t)comps];
    if (comps == 4) {                                  // rotations: shortest arc slerp
        float dot = va[0]*vb[0] + va[1]*vb[1] + va[2]*vb[2] + va[3]*vb[3];
        float sign = dot < 0.0f ? -1.0f : 1.0f;
        dot = fabsf(dot);
        float k0, k1;
        if (dot > 0.9995f) { k0 = 1.0f - u; k1 = u; }
        else {
            float theta = acosf(dot), st = sinf(theta);
            k0 = sinf((1.0f - u) * theta) / st;
            k1 = sinf(u * theta) / st;
        }
        float len = 0.0f;
        for (int i = 0; i < 4; ++i) { out[i] = k0 * va[i] + k1 * sign * vb[i]; len += out[i] * out[i]; }
        len = sqrtf(len);
        if (len > 1e-8f) for (int i = 0; i < 4; ++i) out[i] /= len;
    } else {
        for (int i = 0; i < comps; ++i) out[i] = va[i] + (vb[i] - va[i]) * u;
    }
}

} // namespace

namespace {

// Samples one clip into a copy of the rest hierarchy.
void apply_clip(dai_model *m, std::vector<RawNode> &posed, int animation, float time);

void blend_nodes(std::vector<RawNode> &dst, const std::vector<RawNode> &b, float w) {
    for (size_t i = 0; i < dst.size() && i < b.size(); ++i) {
        RawNode &a = dst[i];
        const RawNode &bb = b[i];
        for (int k = 0; k < 3; ++k) {
            a.t[k] += (bb.t[k] - a.t[k]) * w;
            a.s[k] += (bb.s[k] - a.s[k]) * w;
        }
        // rotations: shortest arc, normalised - a linear mix of quaternions
        // shortens the bone, which reads as a limb that shrinks mid stride
        float dot = a.r[0]*bb.r[0] + a.r[1]*bb.r[1] + a.r[2]*bb.r[2] + a.r[3]*bb.r[3];
        float sign = dot < 0.0f ? -1.0f : 1.0f;
        dot = fabsf(dot);
        float k0, k1;
        if (dot > 0.9995f) { k0 = 1.0f - w; k1 = w; }
        else {
            float theta = acosf(dot), st = sinf(theta);
            k0 = sinf((1.0f - w) * theta) / st;
            k1 = sinf(w * theta) / st;
        }
        float len = 0.0f;
        for (int k = 0; k < 4; ++k) { a.r[k] = k0 * a.r[k] + k1 * sign * bb.r[k]; len += a.r[k] * a.r[k]; }
        len = sqrtf(len);
        if (len > 1e-8f) for (int k = 0; k < 4; ++k) a.r[k] /= len;
    }
}

// Turns a posed hierarchy into joint matrices (and moves the rigid pieces).
uint32_t finish_pose(dai_model *m, std::vector<RawNode> &posed, float *joints, uint32_t max_joints);

} // namespace

uint32_t dai_model_pose_blend(dai_model *m, int anim_a, float time_a,
                              int anim_b, float time_b, float weight,
                              float *joints, uint32_t max_joints) {
    if (!m) return 0;
    std::vector<RawNode> a = m->raw;
    apply_clip(m, a, anim_a, time_a);
    if (weight > 0.0f && anim_b >= 0) {
        std::vector<RawNode> b = m->raw;
        apply_clip(m, b, anim_b, time_b);
        blend_nodes(a, b, weight > 1.0f ? 1.0f : weight);
    }
    return finish_pose(m, a, joints, max_joints);
}

uint32_t dai_model_pose(dai_model *m, int animation, float time, float *joints, uint32_t max_joints) {
    if (!m) return 0;

    std::vector<RawNode> posed = m->raw;
    if (animation >= 0 && (size_t)animation < m->anims.size()) {
        const Animation &a = m->anims[(size_t)animation];
        float t = a.duration > 0.0f ? fmodf(time, a.duration) : 0.0f;
        if (t < 0.0f) t += a.duration;
        for (const Channel &c : a.channels) {
            if (c.node < 0 || (size_t)c.node >= posed.size()) continue;
            RawNode &n = posed[(size_t)c.node];
            n.has_matrix = false;                       // an animated node is TRS by definition
            if (c.path == 0) sample_channel(c, t, n.t, 3);
            else if (c.path == 1) sample_channel(c, t, n.r, 4);
            else sample_channel(c, t, n.s, 3);
        }
    }

    return finish_pose(m, posed, joints, max_joints);
}

namespace {

void apply_clip(dai_model *m, std::vector<RawNode> &posed, int animation, float time) {
    if (animation < 0 || (size_t)animation >= m->anims.size()) return;
    const Animation &a = m->anims[(size_t)animation];
    float t = a.duration > 0.0f ? fmodf(time, a.duration) : 0.0f;
    if (t < 0.0f) t += a.duration;
    for (const Channel &c : a.channels) {
        if (c.node < 0 || (size_t)c.node >= posed.size()) continue;
        RawNode &n = posed[(size_t)c.node];
        n.has_matrix = false;
        if (c.path == 0) sample_channel(c, t, n.t, 3);
        else if (c.path == 1) sample_channel(c, t, n.r, 4);
        else sample_channel(c, t, n.s, 3);
    }
}

uint32_t finish_pose(dai_model *m, std::vector<RawNode> &posed, float *joints, uint32_t max_joints) {
    // world transforms, parents before children
    m->world.assign(posed.size(), m4_identity());
    std::vector<char> done(posed.size(), 0);
    std::vector<int> parent(posed.size(), -1);
    for (size_t i = 0; i < posed.size(); ++i)
        for (int c : posed[i].children)
            if (c >= 0 && (size_t)c < parent.size()) parent[(size_t)c] = (int)i;

    struct Resolver {
        std::vector<RawNode> &posed;
        std::vector<M4> &world;
        std::vector<char> &done;
        std::vector<int> &parent;
        void resolve(int i) {
            if (i < 0 || done[(size_t)i]) return;
            done[(size_t)i] = 1;
            RawNode &n = posed[(size_t)i];
            M4 local = n.has_matrix ? n.matrix : m4_trs(n.t, n.r, n.s);
            int p = parent[(size_t)i];
            if (p >= 0) { resolve(p); world[(size_t)i] = m4_mul(world[(size_t)p], local); }
            else world[(size_t)i] = local;
        }
    } res{ posed, m->world, done, parent };
    for (size_t i = 0; i < posed.size(); ++i) res.resolve((int)i);

    // joint matrices: world * inverse bind, per skin, packed back to back
    uint32_t written = 0;
    for (const Skin &sk : m->skins) {
        for (size_t j = 0; j < sk.joints.size(); ++j) {
            uint32_t slot = sk.offset + (uint32_t)j;
            if (!joints || slot >= max_joints) continue;
            int node = sk.joints[j];
            M4 w = (node >= 0 && (size_t)node < m->world.size()) ? m->world[(size_t)node] : m4_identity();
            M4 jm = m4_mul(w, sk.inverse_bind[j]);
            std::memcpy(joints + (size_t)slot * 16, jm.m, 64);
            if (slot + 1 > written) written = slot + 1;
        }
    }

    // rigid pieces follow their node, so an animated prop moves too
    for (size_t i = 0; i < m->nodes.size(); ++i) {
        int node = i < m->node_of_draw.size() ? m->node_of_draw[i] : -1;
        if (node < 0 || (size_t)node >= m->world.size()) continue;
        if (i < m->skin_of_draw.size() && m->skin_of_draw[i] >= 0) continue;    // skinned: joints do it
        m4_decompose(m->world[(size_t)node], &m->nodes[i].position, &m->nodes[i].rotation, &m->nodes[i].scale);
    }
    return written;
}

} // namespace

uint32_t dai_model_instances(const dai_model *m, dai_render_instance *out, uint32_t max,
                             dai_vec3 offset, dai_quat rot, float scale) {
    if (!m || !out) return 0;
    if (scale == 0.0f) scale = 1.0f;
    auto rotate = [](dai_quat q, dai_vec3 v) {
        dai_vec3 u{ q.x, q.y, q.z };
        dai_vec3 uv{ u.y*v.z - u.z*v.y, u.z*v.x - u.x*v.z, u.x*v.y - u.y*v.x };
        dai_vec3 uu{ u.y*uv.z - u.z*uv.y, u.z*uv.x - u.x*uv.z, u.x*uv.y - u.y*uv.x };
        return dai_vec3{ v.x + 2.0f*(q.w*uv.x + uu.x), v.y + 2.0f*(q.w*uv.y + uu.y), v.z + 2.0f*(q.w*uv.z + uu.z) };
    };
    auto qmul = [](dai_quat a, dai_quat b) {
        return dai_quat{ a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
                         a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
                         a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
                         a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z };
    };
    uint32_t n = 0;
    for (size_t ni = 0; ni < m->nodes.size(); ++ni) {
        const dai_model_node &node = m->nodes[ni];
        if (n >= max) break;
        uint32_t idx = n;
        dai_render_instance &o = out[n++];
        o = dai_render_instance_default();
        dai_vec3 p = rotate(rot, dai_vec3{ node.position.x * scale, node.position.y * scale, node.position.z * scale });
        o.position = { p.x + offset.x, p.y + offset.y, p.z + offset.z };
        o.rotation = qmul(rot, node.rotation);
        o.scale = { node.scale.x * scale, node.scale.y * scale, node.scale.z * scale };
        o.mesh = node.mesh;
        o.material = node.material;
        o.color = { 1, 1, 1 };
        o.roughness = 1.0f;
        int skin = ni < m->skin_of_draw.size() ? m->skin_of_draw[ni] : -1;
        if (skin >= 0 && (size_t)skin < m->skins.size()) {
            o.joint_offset = m->skins[(size_t)skin].offset;
            o.joint_count = (uint32_t)m->skins[(size_t)skin].joints.size();
            // a skinned mesh is posed entirely by its joints: the node transform
            // is already baked into them, so the instance must stay neutral
            o.position = offset;
            o.rotation = rot;
            o.scale = { scale, scale, scale };
        }
        (void)idx;
    }
    return n;
}

} // extern "C"
