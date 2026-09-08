// MODULE 3 (texture baker) OWNS THIS FILE. Statement seam, see include/dai_ext.h.
//
// Included INSIDE asset_inspector_body() in src/dai_editor_ui.cpp, before the
// branches that handle a material, a behaviour and a scene. In scope:
//
//   dai_editor_ui     *p       p->inspect_text is the whole file, already read
//   dai_ui            *ui
//   const dai_ui_style *st
//   const std::string &path    the asset, relative to the project's assets
//   const std::string &ext     lower case, no dot - guard on ext == "daitex"
//
// What belongs here: the .daitex inspector. The node list of the graph, every
// node's parameters as sliders, the seed, the output resolution, a Re-bake
// button - and the PREVIEW: the baked base colour as a thumbnail, drawn with
// dai_ui_image_at through the host's thumbnail callback. A generator panel
// with no picture in it is a form, and nobody tunes noise by typing numbers.
//
// Anything this seam recognises should `return;` when it is done, the way the
// folder branch above it does: the byte count and "Open externally" at the
// bottom of the panel are what an asset the editor does NOT understand gets,
// and a graph the editor understands should not end with them.

/* module 3 fills this in */
