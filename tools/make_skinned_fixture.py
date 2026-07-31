#!/usr/bin/env python3
"""Builds a minimal skinned+animated GLB by hand (no Blender needed).

Two bones, a strip of quads bound to them, and a rotation animation on the
second bone. Small enough to reason about, complete enough that every part of
the import path is exercised: skin, inverse bind matrices, joint/weight vertex
attributes, an animation sampler and the node hierarchy.

  python3 tools/make_skinned_fixture.py assets/test/skinned.glb
"""
import json, struct, sys, math

out = sys.argv[1] if len(sys.argv) > 1 else "assets/test/skinned.glb"

SEG = 8            # segments along Y
H = 4.0            # total height
W = 0.5            # half width

positions, normals, joints, weights, indices = [], [], [], [], []
for i in range(SEG + 1):
    y = H * i / SEG
    w = y / H                       # 0 at the base, 1 at the tip
    for sx in (-W, W):
        positions.append((sx, y, 0.0))
        normals.append((0.0, 0.0, 1.0))
        joints.append((0, 1, 0, 0))
        weights.append((1.0 - w, w, 0.0, 0.0))
for i in range(SEG):
    a = i * 2
    indices += [a, a + 1, a + 3, a, a + 3, a + 2]

# bone 0 at the origin, bone 1 half way up; inverse bind = inverse of the rest pose
def inverse_bind(y):
    m = [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,-y,0,1]      # column major translate(-y)
    return m

bin_parts, views, accessors = [], [], []

def add_view(data, target=None):
    while len(bin_parts) and sum(len(b) for b in bin_parts) % 4:
        bin_parts.append(b"\0")
    offset = sum(len(b) for b in bin_parts)
    bin_parts.append(data)
    v = {"buffer": 0, "byteOffset": offset, "byteLength": len(data)}
    if target: v["target"] = target
    views.append(v)
    return len(views) - 1

def add_accessor(view, ctype, count, atype, mn=None, mx=None, normalized=False):
    a = {"bufferView": view, "componentType": ctype, "count": count, "type": atype}
    if mn: a["min"] = mn
    if mx: a["max"] = mx
    if normalized: a["normalized"] = True
    accessors.append(a)
    return len(accessors) - 1

pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
jnt_bytes = b"".join(struct.pack("<4H", *j) for j in joints)
wgt_bytes = b"".join(struct.pack("<4f", *w) for w in weights)
idx_bytes = b"".join(struct.pack("<H", i) for i in indices)
ibm_bytes = b"".join(struct.pack("<16f", *inverse_bind(y)) for y in (0.0, H / 2))

xs = [p[0] for p in positions]; ys = [p[1] for p in positions]; zs = [p[2] for p in positions]
a_pos = add_accessor(add_view(pos_bytes, 34962), 5126, len(positions), "VEC3",
                     [min(xs), min(ys), min(zs)], [max(xs), max(ys), max(zs)])
a_nrm = add_accessor(add_view(nrm_bytes, 34962), 5126, len(normals), "VEC3")
a_jnt = add_accessor(add_view(jnt_bytes, 34962), 5123, len(joints), "VEC4")
a_wgt = add_accessor(add_view(wgt_bytes, 34962), 5126, len(weights), "VEC4")
a_idx = add_accessor(add_view(idx_bytes, 34963), 5123, len(indices), "SCALAR")
a_ibm = add_accessor(add_view(ibm_bytes), 5126, 2, "MAT4")

# animation: bone 1 rotates about Z, 0 -> 60 deg -> 0 over two seconds
times = [0.0, 1.0, 2.0]
angles = [0.0, math.radians(60.0), 0.0]
quats = [(0.0, 0.0, math.sin(a / 2), math.cos(a / 2)) for a in angles]
a_time = add_accessor(add_view(struct.pack("<%df" % len(times), *times)), 5126, len(times), "SCALAR",
                      [min(times)], [max(times)])
a_rot = add_accessor(add_view(b"".join(struct.pack("<4f", *q) for q in quats)), 5126, len(quats), "VEC4")

gltf = {
    "asset": {"version": "2.0", "generator": "daidalos fixture"},
    "scene": 0,
    "scenes": [{"nodes": [0, 1]}],
    "nodes": [
        {"name": "Limb", "mesh": 0, "skin": 0},
        {"name": "bone_0", "translation": [0, 0, 0], "children": [2]},
        {"name": "bone_1", "translation": [0, H / 2, 0]},
    ],
    "meshes": [{"primitives": [{
        "attributes": {"POSITION": a_pos, "NORMAL": a_nrm, "JOINTS_0": a_jnt, "WEIGHTS_0": a_wgt},
        "indices": a_idx, "material": 0}]}],
    "materials": [{"name": "Limb", "pbrMetallicRoughness": {
        "baseColorFactor": [0.85, 0.45, 0.25, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.5}}],
    "skins": [{"joints": [1, 2], "inverseBindMatrices": a_ibm, "skeleton": 1}],
    "animations": [{
        "name": "bend",
        "samplers": [{"input": a_time, "output": a_rot, "interpolation": "LINEAR"}],
        "channels": [{"sampler": 0, "target": {"node": 2, "path": "rotation"}}],
    }],
    "bufferViews": views,
    "accessors": accessors,
    "buffers": [{"byteLength": sum(len(b) for b in bin_parts)}],
}

bin_blob = b"".join(bin_parts)
while len(bin_blob) % 4: bin_blob += b"\0"
json_blob = json.dumps(gltf, separators=(",", ":")).encode()
while len(json_blob) % 4: json_blob += b" "

glb = b"glTF" + struct.pack("<II", 2, 12 + 8 + len(json_blob) + 8 + len(bin_blob))
glb += struct.pack("<II", len(json_blob), 0x4E4F534A) + json_blob
glb += struct.pack("<II", len(bin_blob), 0x004E4942) + bin_blob
open(out, "wb").write(glb)
print("wrote", out, len(glb), "bytes:", len(positions), "verts,", len(indices)//3, "tris, 2 joints, 1 animation")
