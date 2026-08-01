// daifracture - break a mesh into pieces, once, at build time.
//
//   daifracture <in.glb> <out.glb> [pieces] [seed]
//
// Cutting geometry is expensive, fiddly and - done live - different every run,
// which is the one thing a deterministic engine cannot have. Baking means the
// game only ever swaps one body for N bodies, which is cheap and exact. The
// seed is a parameter, so the same command always produces the same rubble.
//
// The work is in src/dai_fracture.cpp; this is the file handling around it.

#include "dai_fracture.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    if (argc < 3) {
        std::printf("usage: daifracture <in.glb> <out.glb> [pieces] [seed]\n");
        return 2;
    }
    const char *in_path = argv[1], *out_path = argv[2];
    dai_fracture_opts opts = dai_fracture_opts_default();
    if (argc > 3) opts.pieces = (uint32_t)std::atoi(argv[3]);
    if (argc > 4) opts.seed = (uint64_t)std::strtoull(argv[4], nullptr, 10);

    std::vector<uint8_t> file;
    {
        FILE *f = std::fopen(in_path, "rb");
        if (!f) { std::printf("cannot read %s\n", in_path); return 1; }
        char chunk[8192];
        size_t n;
        while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0)
            file.insert(file.end(), chunk, chunk + n);
        std::fclose(f);
    }

    char err[256] = { 0 };
    uint32_t prim_count = dai_gltf_read_geometry(file.data(), file.size(), nullptr, 0, err, sizeof(err));
    if (!prim_count) { std::printf("no geometry in %s: %s\n", in_path, err); return 1; }
    std::vector<dai_mesh_data> prims(prim_count);
    dai_gltf_read_geometry(file.data(), file.size(), prims.data(), prim_count, err, sizeof(err));

    uint32_t n = dai_fracture(prims.data(), prim_count, &opts, nullptr, 0, err, sizeof(err));
    if (!n) { std::printf("fracture failed: %s\n", err); return 1; }
    std::vector<dai_mesh_data> pieces(n);
    n = dai_fracture(prims.data(), prim_count, &opts, pieces.data(), n, err, sizeof(err));
    dai_gltf_free_geometry(prims.data(), prim_count);

    std::vector<dai_mesh_write> writes(n);
    size_t tris = 0;
    for (uint32_t i = 0; i < n; ++i) {
        writes[i].vertices = pieces[i].vertices;
        writes[i].vertex_count = pieces[i].vertex_count;
        writes[i].indices = pieces[i].indices;
        writes[i].index_count = pieces[i].index_count;
        writes[i].name = pieces[i].name;
        tris += pieces[i].index_count / 3;
    }
    dai_result rc = dai_gltf_write(out_path, writes.data(), n, err, sizeof(err));
    dai_gltf_free_geometry(pieces.data(), n);
    if (rc != DAI_OK) { std::printf("writing %s failed: %s\n", out_path, err); return 1; }

    std::printf("wrote %s: %u pieces, %zu triangles, seed %llu\n",
                out_path, n, tris, (unsigned long long)opts.seed);
    return 0;
}
