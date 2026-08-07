"""Builds a glTF test scene in Blender and exports it as GLB.

This is the real end of the pipeline test: the engine's importer has to read
what BLENDER writes, not what a hand rolled generator writes. Covers indexed
triangle meshes, per object transforms, four materials (metal, rough
dielectric, emissive, textured) and an embedded PNG texture.

  blender --background --python make_testscene.py
"""
import bpy, os, math

out_dir = r"C:\daidalos"
os.makedirs(out_dir, exist_ok=True)

bpy.ops.wm.read_factory_settings(use_empty=True)


def mat(name, color, metallic=0.0, roughness=0.5, emission=None):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (color[0], color[1], color[2], 1.0)
    bsdf.inputs["Metallic"].default_value = metallic
    bsdf.inputs["Roughness"].default_value = roughness
    if emission is not None:
        if "Emission Color" in bsdf.inputs:
            bsdf.inputs["Emission Color"].default_value = (emission[0], emission[1], emission[2], 1.0)
            bsdf.inputs["Emission Strength"].default_value = 1.0
        else:
            bsdf.inputs["Emission"].default_value = (emission[0], emission[1], emission[2], 1.0)
    return m


def add(obj, material, loc, scale=(1, 1, 1), rot=(0, 0, 0)):
    obj.location = loc
    obj.scale = scale
    obj.rotation_euler = rot
    obj.data.materials.append(material)
    return obj


bpy.ops.mesh.primitive_cube_add(size=2)
add(bpy.context.object, mat("Metal", (0.85, 0.25, 0.20), metallic=1.0, roughness=0.25), (-3, 0, 1))

bpy.ops.mesh.primitive_uv_sphere_add(radius=1.0, segments=32, ring_count=16)
bpy.ops.object.shade_smooth()
add(bpy.context.object, mat("Rough", (0.30, 0.62, 0.85), metallic=0.0, roughness=0.9), (0, 0, 1))

bpy.ops.mesh.primitive_cone_add(radius1=0.8, depth=2.0)
add(bpy.context.object, mat("Glow", (0.95, 0.80, 0.25), roughness=0.4, emission=(1.0, 0.7, 0.1)), (3, 0, 1))

img = bpy.data.images.new("Grid", width=256, height=256)
img.generated_type = 'COLOR_GRID'
img.source = 'GENERATED'
tex_mat = mat("Textured", (1, 1, 1), metallic=0.0, roughness=0.55)
nodes = tex_mat.node_tree.nodes
tex = nodes.new("ShaderNodeTexImage")
tex.image = img
tex_mat.node_tree.links.new(tex.outputs["Color"], nodes["Principled BSDF"].inputs["Base Color"])

bpy.ops.mesh.primitive_monkey_add(size=1.6)
add(bpy.context.object, tex_mat, (0, 3.5, 1.2), rot=(math.radians(90), 0, math.radians(180)))

bpy.ops.mesh.primitive_plane_add(size=2)
add(bpy.context.object, mat("Ground", (0.32, 0.34, 0.28), roughness=1.0), (0, 0, 0), scale=(12, 12, 1))

glb = os.path.join(out_dir, "blender_scene.glb")
bpy.ops.export_scene.gltf(filepath=glb, export_format='GLB', export_apply=True)

sep = os.path.join(out_dir, "sep", "blender_scene.gltf")
os.makedirs(os.path.dirname(sep), exist_ok=True)
bpy.ops.export_scene.gltf(filepath=sep, export_format='GLTF_SEPARATE', export_apply=True)

meshes = [o for o in bpy.data.objects if o.type == 'MESH']
total_v = sum(len(o.data.vertices) for o in meshes)
print("EXPORTED", glb, os.path.getsize(glb), "bytes")
print("OBJECTS", len(meshes), "VERTS", total_v, "MATERIALS", len(bpy.data.materials))
