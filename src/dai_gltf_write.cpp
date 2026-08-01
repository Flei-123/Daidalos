// Writing glTF, which until now the engine could only read.
//
// A tool that produces geometry - the fracture baker is the first - has to put
// it somewhere the engine can load again, and "somewhere" has to be the format
// everything else already speaks. Writing a .glb is not hard: a JSON header
// describing accessors into one binary blob, both wrapped in chunks.
//
// Deliberately narrow: positions, normals, UVs and indices, one node per mesh
// at the identity transform. No materials, no textures, no hierarchy. A baker
// splits a mesh; it does not invent a scene.

#include "dai_gltf.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

void put_u32(std::vector<uint8_t> &b, uint32_t v) {
    b.insert(b.end(), (const uint8_t *)&v, (const uint8_t *)&v + 4);
}

void pad_to_4(std::vector<uint8_t> &b, uint8_t fill) {
    while (b.size() % 4) b.push_back(fill);
}

std::string fnum(float f) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.9g", (double)f);
    return buf;
}

// glTF requires min/max on the POSITION accessor - a viewer uses it to frame
// the model without walking every vertex, and some validators reject the file
// without it.
struct Bounds {
    float lo[3] = { 1e30f, 1e30f, 1e30f };
    float hi[3] = { -1e30f, -1e30f, -1e30f };
    void add(const dai_vec3 &p) {
        const float v[3] = { p.x, p.y, p.z };
        for (int i = 0; i < 3; ++i) { if (v[i] < lo[i]) lo[i] = v[i]; if (v[i] > hi[i]) hi[i] = v[i]; }
    }
    bool valid() const { return lo[0] <= hi[0]; }
};

} // namespace

extern "C" {

dai_result dai_gltf_write(const char *path, const dai_mesh_write *meshes, uint32_t count,
                          char *err, size_t err_len) {
    auto fail = [&](const char *m) {
        if (err && err_len) std::snprintf(err, err_len, "%s", m);
        return DAI_ERR_INVALID_ARG;
    };
    if (!path || !meshes || !count) return fail("nothing to write");

    std::vector<uint8_t> bin;
    std::string accessors, views, mesh_json, nodes, scene_nodes;
    uint32_t acc_index = 0, view_index = 0, written = 0;

    for (uint32_t i = 0; i < count; ++i) {
        const dai_mesh_write &m = meshes[i];
        if (!m.vertices || !m.vertex_count || !m.indices || !m.index_count) continue;

        Bounds b;
        for (uint32_t v = 0; v < m.vertex_count; ++v) b.add(m.vertices[v].position);
        if (!b.valid()) continue;

        // Three views per mesh: positions, normals, indices. Interleaving would
        // be denser on the GPU, but this file is read once by an importer that
        // de-interleaves anyway, and separate views keep the writer honest.
        const size_t pos_off = bin.size();
        for (uint32_t v = 0; v < m.vertex_count; ++v) {
            const dai_vec3 &p = m.vertices[v].position;
            const float f[3] = { p.x, p.y, p.z };
            bin.insert(bin.end(), (const uint8_t *)f, (const uint8_t *)f + 12);
        }
        const size_t nrm_off = bin.size();
        for (uint32_t v = 0; v < m.vertex_count; ++v) {
            const dai_vec3 &n = m.vertices[v].normal;
            const float f[3] = { n.x, n.y, n.z };
            bin.insert(bin.end(), (const uint8_t *)f, (const uint8_t *)f + 12);
        }
        const size_t idx_off = bin.size();
        for (uint32_t k = 0; k < m.index_count; ++k) put_u32(bin, m.indices[k]);

        char buf[1024];
        std::snprintf(buf, sizeof(buf),
            "%s{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34962},"
            "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34962},"
            "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34963}",
            view_index ? "," : "",
            pos_off, (size_t)m.vertex_count * 12,
            nrm_off, (size_t)m.vertex_count * 12,
            idx_off, (size_t)m.index_count * 4);
        views += buf;

        std::snprintf(buf, sizeof(buf),
            "%s{\"bufferView\":%u,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\","
            "\"min\":[%s,%s,%s],\"max\":[%s,%s,%s]},"
            "{\"bufferView\":%u,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\"},"
            "{\"bufferView\":%u,\"componentType\":5125,\"count\":%u,\"type\":\"SCALAR\"}",
            acc_index ? "," : "",
            view_index, m.vertex_count,
            fnum(b.lo[0]).c_str(), fnum(b.lo[1]).c_str(), fnum(b.lo[2]).c_str(),
            fnum(b.hi[0]).c_str(), fnum(b.hi[1]).c_str(), fnum(b.hi[2]).c_str(),
            view_index + 1, m.vertex_count,
            view_index + 2, m.index_count);
        accessors += buf;

        char name[80];
        std::snprintf(name, sizeof(name), "%s", m.name && m.name[0] ? m.name : "piece");
        for (char *c = name; *c; ++c) if (*c == '"' || *c == '\\') *c = '_';   // keep the JSON valid

        std::snprintf(buf, sizeof(buf),
            "%s{\"name\":\"%s\",\"primitives\":[{\"attributes\":{\"POSITION\":%u,\"NORMAL\":%u},"
            "\"indices\":%u}]}",
            written ? "," : "", name, acc_index, acc_index + 1, acc_index + 2);
        mesh_json += buf;

        std::snprintf(buf, sizeof(buf), "%s{\"name\":\"%s\",\"mesh\":%u}",
                      written ? "," : "", name, written);
        nodes += buf;
        std::snprintf(buf, sizeof(buf), "%s%u", written ? "," : "", written);
        scene_nodes += buf;

        view_index += 3;
        acc_index += 3;
        ++written;
    }
    if (!written) return fail("every mesh was empty");

    std::string json =
        "{\"asset\":{\"version\":\"2.0\",\"generator\":\"daidalos\"},"
        "\"scene\":0,\"scenes\":[{\"nodes\":[" + scene_nodes + "]}],"
        "\"nodes\":[" + nodes + "],"
        "\"meshes\":[" + mesh_json + "],"
        "\"accessors\":[" + accessors + "],"
        "\"bufferViews\":[" + views + "],"
        "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}]}";

    std::vector<uint8_t> jchunk(json.begin(), json.end());
    pad_to_4(jchunk, ' ');          // JSON pads with spaces, BIN with zeroes
    pad_to_4(bin, 0);

    std::vector<uint8_t> out;
    out.reserve(28 + jchunk.size() + bin.size());
    const char magic[4] = { 'g', 'l', 'T', 'F' };
    out.insert(out.end(), magic, magic + 4);
    put_u32(out, 2);
    put_u32(out, (uint32_t)(12 + 8 + jchunk.size() + 8 + bin.size()));
    put_u32(out, (uint32_t)jchunk.size());
    put_u32(out, 0x4E4F534A);       // JSON
    out.insert(out.end(), jchunk.begin(), jchunk.end());
    put_u32(out, (uint32_t)bin.size());
    put_u32(out, 0x004E4942);       // BIN
    out.insert(out.end(), bin.begin(), bin.end());

    // Temp file and rename: an interrupted bake must not leave a half written
    // .glb where the good one used to be.
    std::string tmp = std::string(path) + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) return fail("cannot create the output file");
    bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    ok = (std::fflush(f) == 0) && ok;
    std::fclose(f);
    if (!ok) { std::remove(tmp.c_str()); return fail("write failed"); }
    if (std::rename(tmp.c_str(), path) != 0) { std::remove(tmp.c_str()); return fail("rename failed"); }
    return DAI_OK;
}

} // extern "C"
