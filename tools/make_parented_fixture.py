#!/usr/bin/env python3
"""Writes assets/test/parented.gltf - a crate with a lid PARENTED to it.

Blender's usual answer to "a box that opens" is a child object, and the
question that fixture answers is whether that relationship survives the
import. It cannot be checked against blender_scene.glb, whose five objects
are all roots.

Deliberately hand written rather than exported: it is four triangles and a
transform, and a fixture whose expected values are computable on paper beats
one that needs a Blender install to regenerate.

  Crate  translation (2, 0, 0)          -> world (2, 0, 0)
  Lid    child, local translation (0, 1, 0) -> world (2, 1, 0)

so world and local differ, which is exactly what a test needs to tell them
apart. Run from the repo root:

  python3 tools/make_parented_fixture.py
"""
import base64
import json
import os
import struct

OUT = os.path.join("assets", "test", "parented.gltf")

# A unit box, indexed. Position and normal only - the point of this fixture is
# the hierarchy, not the shading.
P = [
    (-0.5, -0.5, -0.5), (0.5, -0.5, -0.5), (0.5, 0.5, -0.5), (-0.5, 0.5, -0.5),
    (-0.5, -0.5, 0.5), (0.5, -0.5, 0.5), (0.5, 0.5, 0.5), (-0.5, 0.5, 0.5),
]
N = [(0.0, 0.0, -1.0)] * 4 + [(0.0, 0.0, 1.0)] * 4
IDX = [
    0, 1, 2, 0, 2, 3,      # back
    4, 6, 5, 4, 7, 6,      # front
    0, 4, 5, 0, 5, 1,      # bottom
    3, 2, 6, 3, 6, 7,      # top
    0, 3, 7, 0, 7, 4,      # left
    1, 5, 6, 1, 6, 2,      # right
]

pos_bytes = b"".join(struct.pack("<3f", *p) for p in P)
nrm_bytes = b"".join(struct.pack("<3f", *n) for n in N)
idx_bytes = b"".join(struct.pack("<H", i) for i in IDX)
while len(idx_bytes) % 4:
    idx_bytes += b"\x00"

blob = pos_bytes + nrm_bytes + idx_bytes
pos_off, nrm_off, idx_off = 0, len(pos_bytes), len(pos_bytes) + len(nrm_bytes)

gltf = {
    "asset": {"version": "2.0", "generator": "daidalos make_parented_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": [0]}],
    "nodes": [
        {"name": "Crate", "mesh": 0, "translation": [2.0, 0.0, 0.0], "children": [1]},
        # The lid sits one metre above its parent, IN THE PARENT'S SPACE.
        {"name": "Lid", "mesh": 1, "translation": [0.0, 1.0, 0.0]},
    ],
    "meshes": [
        {"name": "CrateMesh", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1},
                                              "indices": 2, "material": 0}]},
        {"name": "LidMesh", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1},
                                            "indices": 2, "material": 1}]},
    ],
    # Two materials so a test can tell the pieces apart by more than position.
    "materials": [
        {"name": "CrateMat", "pbrMetallicRoughness": {"baseColorFactor": [0.6, 0.4, 0.2, 1.0],
                                                      "metallicFactor": 0.0, "roughnessFactor": 0.9}},
        {"name": "LidMat", "pbrMetallicRoughness": {"baseColorFactor": [0.2, 0.5, 0.7, 1.0],
                                                    "metallicFactor": 0.0, "roughnessFactor": 0.4}},
    ],
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": len(P), "type": "VEC3",
         "min": [-0.5, -0.5, -0.5], "max": [0.5, 0.5, 0.5]},
        {"bufferView": 1, "componentType": 5126, "count": len(N), "type": "VEC3"},
        {"bufferView": 2, "componentType": 5123, "count": len(IDX), "type": "SCALAR"},
    ],
    "bufferViews": [
        {"buffer": 0, "byteOffset": pos_off, "byteLength": len(pos_bytes), "target": 34962},
        {"buffer": 0, "byteOffset": nrm_off, "byteLength": len(nrm_bytes), "target": 34962},
        {"buffer": 0, "byteOffset": idx_off, "byteLength": len(idx_bytes), "target": 34963},
    ],
    "buffers": [{"byteLength": len(blob),
                 "uri": "data:application/octet-stream;base64," + base64.b64encode(blob).decode()}],
}

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w") as f:
    json.dump(gltf, f, indent=1)
print("wrote %s (%d bytes, %d triangles per piece)" % (OUT, os.path.getsize(OUT), len(IDX) // 3))
