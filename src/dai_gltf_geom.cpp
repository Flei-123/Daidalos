// Reading geometry out of a .glb without a renderer.
//
// The importer proper turns a file into GPU meshes, which means it needs a
// device, a queue and a Vulkan backend. A build tool - the fracture baker is
// the first - wants the triangles and nothing else. Sharing the parser through
// dai_gltf_common.hpp keeps one implementation of glTF; keeping this in its own
// translation unit keeps the tool free of the graphics stack.
//
// External .bin sidecars are refused on purpose: resolving them is the asset
// layer's job, and it has the mount table. A tool gets one self contained file.

#include "dai_gltf_common.hpp"

#include <cstdlib>

extern "C" {

uint32_t dai_gltf_read_geometry(const void *data, size_t size, dai_mesh_data *out,
                                uint32_t max, char *err, size_t err_len) {
    auto fail = [&](const char *m) -> uint32_t {
        if (err && err_len) std::snprintf(err, err_len, "%s", m);
        return 0;
    };
    if (!data || !size) return fail("no bytes");
    const uint8_t *file = (const uint8_t *)data;

    // Same container handling as the full loader, minus everything that needs
    // a renderer.
    const char *json_text = nullptr;
    size_t json_len = 0;
    const uint8_t *bin = nullptr;
    size_t bin_size = 0;
    if (size > 12 && !std::memcmp(file, "glTF", 4)) {
        size_t pos = 12;
        while (pos + 8 <= size) {
            uint32_t clen, ctype;
            std::memcpy(&clen, file + pos, 4);
            std::memcpy(&ctype, file + pos + 4, 4);
            if (pos + 8 + clen > size) return fail("truncated GLB chunk");
            const uint8_t *payload = file + pos + 8;
            if (ctype == 0x4E4F534A) { json_text = (const char *)payload; json_len = clen; }
            else if (ctype == 0x004E4942) { bin = payload; bin_size = clen; }
            pos += 8 + clen + ((4 - (clen & 3)) & 3);
        }
        if (!json_text) return fail("GLB without a JSON chunk");
    } else {
        json_text = (const char *)file;
        json_len = size;
    }

    Loader ld;
    ld.err = err; ld.err_len = err_len;
    ld.glb_bin = bin; ld.glb_bin_size = bin_size;
    std::string jerr;
    if (!ld.doc.parse(json_text, json_len, &jerr)) return fail(jerr.c_str());
    ld.root = ld.doc.root();
    if (!ld.root || ld.root->type != Value::OBJECT) return fail("glTF root is not an object");

    if (const Value *bufs = ld.root->get("buffers")) {
        for (size_t i = 0; i < bufs->size(); ++i) {
            const Value *b = bufs->at(i);
            std::vector<uint8_t> bytes;
            const char *uri = b->str_at("uri", "");
            if (!uri[0]) {
                if (!ld.glb_bin) return fail("buffer without uri and no GLB binary chunk");
                bytes.assign(ld.glb_bin, ld.glb_bin + ld.glb_bin_size);
            } else if (!std::strncmp(uri, "data:", 5)) {
                const char *comma = std::strchr(uri, ',');
                if (!comma) return fail("malformed data uri");
                if (!base64_decode(comma + 1, std::strlen(comma + 1), bytes))
                    return fail("bad base64 in a buffer uri");
            } else {
                // A tool is handed one file; chasing sidecars is the asset
                // layer's job and it has the mount table to do it with.
                return fail("external buffers are not read here - use a self contained .glb");
            }
            ld.buffers.push_back(std::move(bytes));
        }
    }

    uint32_t found = 0;
    const Value *meshes = ld.root->get("meshes");
    for (size_t i = 0; meshes && i < meshes->size(); ++i) {
        const Value *mesh = meshes->at(i);
        const Value *plist = mesh->get("primitives");
        const char *mesh_name = mesh->str_at("name", "");
        for (size_t p = 0; plist && p < plist->size(); ++p) {
            const Value *prim = plist->at(p);
            if (prim->int_at("mode", 4) != 4) continue;      // triangles only
            const Value *attrs = prim->get("attributes");
            if (!attrs) continue;
            int a_pos = attrs->int_at("POSITION", -1);
            if (a_pos < 0) continue;
            uint32_t slot = found++;
            if (!out || slot >= max) continue;               // counting pass

            int a_nrm = attrs->int_at("NORMAL", -1);
            int a_uv  = attrs->int_at("TEXCOORD_0", -1);
            std::vector<float> pos, nrm, uv;
            size_t vcount = 0;
            if (!ld.read_accessor_float(a_pos, 3, pos, &vcount)) return fail("bad POSITION accessor");
            if (a_nrm >= 0) ld.read_accessor_float(a_nrm, 3, nrm, nullptr);
            if (a_uv  >= 0) ld.read_accessor_float(a_uv,  2, uv,  nullptr);

            std::vector<uint32_t> idx;
            int a_idx = prim->int_at("indices", -1);
            if (a_idx >= 0) { if (!ld.read_indices(a_idx, idx)) return fail("bad index accessor"); }
            else { idx.resize(vcount); for (size_t k = 0; k < vcount; ++k) idx[k] = (uint32_t)k; }

            dai_mesh_data &m = out[slot];
            m = dai_mesh_data{};
            m.vertex_count = (uint32_t)vcount;
            m.index_count = (uint32_t)idx.size();
            m.vertices = (dai_vertex *)std::malloc(vcount * sizeof(dai_vertex));
            m.indices = (uint32_t *)std::malloc(idx.size() * sizeof(uint32_t));
            if (!m.vertices || !m.indices) return fail("out of memory");
            for (size_t v = 0; v < vcount; ++v) {
                dai_vertex &d = m.vertices[v];
                d.position = { pos[v*3], pos[v*3+1], pos[v*3+2] };
                d.normal = nrm.size() >= (v+1)*3 ? dai_vec3{ nrm[v*3], nrm[v*3+1], nrm[v*3+2] }
                                                 : dai_vec3{ 0, 1, 0 };
                d.cap = 0.0f;
                d.u = uv.size() >= (v+1)*2 ? uv[v*2] : 0.0f;
                d.v = uv.size() >= (v+1)*2 ? uv[v*2+1] : 0.0f;
                for (int k = 0; k < 4; ++k) { d.joints[k] = 0; d.weights[k] = 0.0f; }
            }
            std::memcpy(m.indices, idx.data(), idx.size() * sizeof(uint32_t));
            std::snprintf(m.name, sizeof(m.name), "%s", mesh_name);
        }
    }
    return found;
}

void dai_gltf_free_geometry(dai_mesh_data *m, uint32_t count) {
    if (!m) return;
    for (uint32_t i = 0; i < count; ++i) {
        std::free(m[i].vertices);
        std::free(m[i].indices);
        m[i].vertices = nullptr;
        m[i].indices = nullptr;
        m[i].vertex_count = m[i].index_count = 0;
    }
}

} // extern "C"
