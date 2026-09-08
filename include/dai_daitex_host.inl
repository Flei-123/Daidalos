// MODULE 3 (texture baker) OWNS THIS FILE. File scope seam, see include/dai_ext.h.
//
// Included once by every host that draws a scene: examples/editor_demo.cpp and
// tools/modeling_shot.cpp. It is where a `.daitex` node graph in the project
// becomes three PNGs on disk - base colour, ORM, normal - which module 2's
// material seam then loads like any other map.
//
// The rule from docs/MATERIALS.md is NOT bent by this: the engine renders
// MAPS. The graph is a bake tool that happens to live in the editor instead of
// in Blender, and what reaches the renderer is a texture, exactly as before.
//
// The contract:
//
//   * Bake on load, and re-bake when the .daitex is newer than its output.
//     Never every frame: a 1024x1024 Worley graph is not a per frame cost.
//   * DETERMINISTIC. Same graph plus same seed = bit identical PNG bytes, on
//     every machine, every run. No clock, no unseeded random, no threading
//     order that can reorder a reduction. The test for this compares two bakes
//     byte for byte, so a float that comes out one ULP different is a failure,
//     not a rounding detail.
//   * Never block the editor for longer than a bake takes; a graph that fails
//     to parse writes nothing and says so in the console, leaving the previous
//     PNGs in place.
//
// Called once per frame. `dai_daitex_host_thumb` answers the Project panel's
// thumbnail callback: it returns 0 for anything that is not a .daitex, which
// leaves the host's own thumbnail path in charge of everything else.

static void dai_daitex_host_poll(const dai_ext_host *h) {
    (void)h;   /* module 3 fills this in */
}

static dai_texture dai_daitex_host_thumb(const dai_ext_host *h, const char *path) {
    (void)h; (void)path;   /* module 3 fills this in; 0 = not a .daitex */
    return 0;
}
