// Procedural meshes for the built in shapes, plus a small OBJ reader.
// Pure C++: no Vulkan, no engine dependency, so it can be unit tested and
// reused by any other rendering backend.
#ifndef DAI_MESHGEN_HPP
#define DAI_MESHGEN_HPP

#include "dai_render.h"
#include <vector>

namespace daimesh {

struct Mesh {
    std::vector<dai_vertex> verts;
    std::vector<uint32_t>   idx;
};

// Winding of every generator below is counter clockwise seen from OUTSIDE.
Mesh box();                          // half extent 1
Mesh sphere(int segments = 32, int rings = 16);
Mesh capsule(int segments = 24, int rings = 8);   // radius 1, cap offset +-1 via vertex.cap
Mesh cylinder(int segments = 32);                 // radius 1, y in [-1,1]
Mesh cone(int segments = 32);                     // radius 1 at y=-1, tip at y=+1
Mesh plane();                                     // 1x1 quad in XZ, facing +Y

bool load_obj(const char *path, Mesh *out);

} // namespace daimesh

#endif
