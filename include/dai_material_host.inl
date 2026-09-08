// MODULE 2 (triplanar) OWNS THIS FILE. File scope seam, see include/dai_ext.h.
//
// Included once by every host that draws a scene: examples/editor_demo.cpp and
// tools/modeling_shot.cpp. It is where a `.daimat` becomes a real
// dai_material in the renderer - with its maps loaded, its triplanar switch
// and its tiling - and gets attached to every entity whose node names it.
//
// Today (before this module lands) a `.daimat` only pushes three numbers into
// the node: colour, roughness, emissive (see apply_materials in
// examples/editor_demo.cpp). Nothing creates a dai_material, so no object can
// carry a texture that did not come out of a .glb. That is what this seam is
// for, and it is why the baker of module 3 needs no material code of its own:
// it writes PNGs, this file loads them.
//
// The contract:
//
//   * One dai_material per .daimat path, cached. Two hundred walls on one
//     material are one material and one draw setup, not two hundred.
//   * dai_render_material_update() on a change - the handle stays, so nothing
//     that already points at it has to be told.
//   * Colour space by slot, not by guess: base colour and emissive sRGB, ORM
//     and normal linear (docs/MATERIALS.md).
//   * Attach with dai_scene_set_material() on the entity behind the node. Like
//     the mesh in dai_blockout_host.inl this is derived data: the document
//     stores the PATH, never the handle.
//   * A missing map file is a material that still works, with the default
//     texture in that slot. Never a failed load.
//
// Called once per frame, AFTER dai_doc_sync_apply() and after the host's own
// apply_materials().

static void dai_material_host_apply(const dai_ext_host *h) {
    (void)h;   /* module 2 fills this in */
}
