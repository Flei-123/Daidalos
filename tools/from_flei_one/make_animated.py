"""Exports a skinned, animated GLB from Blender for the engine's skinning tests.

A tapered cylinder with a three bone chain and a bend animation: enough to
catch every mistake that matters (bind pose, joint order, weights that do not
sum to one, sampler interpolation, and the inverse bind matrices).

  blender --background --python make_animated.py
"""
import bpy, os, math

out_dir = r"C:\daidalos"
bpy.ops.wm.read_factory_settings(use_empty=True)

# ---- mesh: a tall cylinder, subdivided so bending is visible
bpy.ops.mesh.primitive_cylinder_add(radius=0.35, depth=4.0, vertices=16, location=(0, 0, 2))
arm_mesh = bpy.context.object
arm_mesh.name = "Limb"
bpy.ops.object.mode_set(mode='EDIT')
bpy.ops.mesh.select_all(action='SELECT')
bpy.ops.mesh.subdivide(number_cuts=12)
bpy.ops.object.mode_set(mode='OBJECT')

mat = bpy.data.materials.new("LimbMat")
mat.use_nodes = True
bsdf = mat.node_tree.nodes["Principled BSDF"]
bsdf.inputs["Base Color"].default_value = (0.85, 0.45, 0.25, 1.0)
bsdf.inputs["Roughness"].default_value = 0.45
arm_mesh.data.materials.append(mat)

# ---- armature: three bones stacked along Z
bpy.ops.object.armature_add(location=(0, 0, 0))
arm = bpy.context.object
arm.name = "Rig"
bpy.ops.object.mode_set(mode='EDIT')
eb = arm.data.edit_bones
root = eb[0]
root.name = "bone_0"
root.head = (0, 0, 0)
root.tail = (0, 0, 1.33)
b1 = eb.new("bone_1"); b1.head = root.tail; b1.tail = (0, 0, 2.66); b1.parent = root; b1.use_connect = True
b2 = eb.new("bone_2"); b2.head = b1.tail;   b2.tail = (0, 0, 4.0);  b2.parent = b1;   b2.use_connect = True
bpy.ops.object.mode_set(mode='OBJECT')

# ---- bind with automatic weights
bpy.ops.object.select_all(action='DESELECT')
arm_mesh.select_set(True)
arm.select_set(True)
bpy.context.view_layer.objects.active = arm
bpy.ops.object.parent_set(type='ARMATURE_AUTO')

# ---- animation: bend the upper two bones, 40 frames
bpy.context.scene.frame_start = 1
bpy.context.scene.frame_end = 40
bpy.ops.object.mode_set(mode='POSE')
pb1 = arm.pose.bones["bone_1"]
pb2 = arm.pose.bones["bone_2"]
for frame, a1, a2 in ((1, 0.0, 0.0), (20, 0.7, 0.9), (40, 0.0, 0.0)):
    bpy.context.scene.frame_set(frame)
    pb1.rotation_mode = 'XYZ'
    pb2.rotation_mode = 'XYZ'
    pb1.rotation_euler = (a1, 0, 0)
    pb2.rotation_euler = (a2, 0, 0)
    pb1.keyframe_insert(data_path="rotation_euler", frame=frame)
    pb2.keyframe_insert(data_path="rotation_euler", frame=frame)
bpy.ops.object.mode_set(mode='OBJECT')

glb = os.path.join(out_dir, "blender_anim.glb")
bpy.ops.export_scene.gltf(filepath=glb, export_format='GLB',
                          export_animations=True, export_skins=True, export_apply=False)

print("EXPORTED", glb, os.path.getsize(glb), "bytes")
print("BONES", len(arm.data.bones), "VERTS", len(arm_mesh.data.vertices),
      "ACTIONS", len(bpy.data.actions))
