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
//
// What this module keeps them in is p->inspect_text itself: the rows in
// dai_editor_ui_material.inl are parsed out of that text when they are drawn
// and written straight back into it when they change, so there is no second
// copy of the maps to load here and none to get out of step with the file.
// The text has just been read one line above; that IS the parse.
//
// What DOES belong to "a different material was selected" is the caret. Three
// of this module's rows are text fields, and a focused text field keyed by its
// label would otherwise survive the selection change: click into the Normal
// map of one material, click a second material in the browser, and the caret
// (with the first material's half typed path still in the field's edit buffer)
// would sit in the second material's Normal row. Dropping the focus with the
// selection is one line, and it is this one.
dai_ui_text_defocus(p->ui);
