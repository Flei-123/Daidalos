// MODULE 1 (blockout) OWNS THIS FILE. File scope seam, see include/dai_ext.h.
//
// Included once by every host that draws a scene: examples/editor_demo.cpp and
// tools/modeling_shot.cpp. It is where a blockout node (Box, Cylinder, Stairs,
// Arch, Wedge) and a CSG node turn into an actual mesh the renderer can draw.
//
// The contract this file has to keep:
//
//   * Build the mesh with daimesh/dai_blockout - deterministic, the same
//     fields in give the same triangles out, on every machine.
//   * Cache by CONTENT, not by frame: rebuilding a wall every frame would cost
//     a mesh upload per frame per wall. Rebuild when the fields change.
//   * Attach the result WITHOUT touching the document. The mesh is derived
//     data; writing it back would put a mesh id in the undo stack and in the
//     scene file, and the scene file already has the fields it was built from.
//     dai_scene_set_render() on the entity behind the node is the way in
//     (dai_doc_sync_entity gives the entity).
//   * Release the mesh of a node that stopped being a blockout node, or the
//     renderer leaks one mesh per edit.
//
// Called once per frame, AFTER dai_doc_sync_apply().

static void dai_blockout_host_sync(const dai_ext_host *h) {
    (void)h;   /* module 1 fills this in */
}
