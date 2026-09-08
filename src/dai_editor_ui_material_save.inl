// MODULE 2 (triplanar) OWNS THIS FILE. Statement seam, see include/dai_ext.h.
//
// Included INSIDE asset_inspector_body() in src/dai_editor_ui.cpp, inside the
// Save button, where the file's text has just been built. In scope:
//
//   std::string text          the six lines the panel writes today - APPEND
//   const std::string &path   which material is being written
//   dai_editor_ui *p
//
// Append the lines this module owns - the maps, `triplanar`, tiling, blend -
// and only when they differ from the default, which is the rule the whole
// format follows: a file holds what is not default, so it stays readable and
// so adding a field later does not rewrite every material in the project.
//
// Without this seam the Save button would write a six line file over a
// material that had ten lines in it, and the maps would be gone. That is the
// one failure mode of this seam, and it is why it exists.

/* module 2 fills this in */
