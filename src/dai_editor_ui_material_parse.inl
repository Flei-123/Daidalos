// MODULE 2 (triplanar) OWNS THIS FILE. Statement seam, see include/dai_ext.h.
//
// Included INSIDE asset_inspector_body() in src/dai_editor_ui.cpp, right after
// the four numbers of a .daimat have been read out of its text. In scope:
//
//   dai_editor_ui *p              p->inspect_text holds the whole file
//   const std::string &path       which material this is
//
// Read the lines this module added to the format here (the maps, `triplanar`,
// its tiling and its blend) into whatever this module keeps them in, so the
// panel seam can draw them and the save seam can write them back.
//
// It runs ONCE per selection, not per frame - the panel re-reads a material
// only when a different one is selected or after a save.

/* module 2 fills this in */
