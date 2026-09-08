// MODULE 3 (texture baker) OWNS THIS FILE. Statement seam, see include/dai_ext.h.
//
// Every .daitex parameter and the range its slider gets, ONCE. Two very
// different places need this table and only one of them can include a header:
//
//   * daitex::param_range() in include/dai_daitex.h, which clamps and
//     documents;
//   * the inspector in src/dai_editor_ui_daitex.inl, which is itself a block
//     of statements inside a function of src/dai_editor_ui.cpp and therefore
//     cannot include anything at all.
//
// So the table is a block of statements too. Whoever includes it defines
// DAI_DAITEX_RANGE(type, key, lo, hi, step) first and undefines it after.
// A second copy of this list is how a slider that goes to 64 and a clamp that
// stops at 32 end up in the same build.
//
// The ranges are the useful ones, not the representable ones: a noise with 400
// cells at 256 px is white sand, and a slider that spends 90% of its travel in
// that region is a slider nobody can aim.

DAI_DAITEX_RANGE("noise",     "cells",      1.0f,  64.0f, 1.0f)
DAI_DAITEX_RANGE("noise",     "octaves",    1.0f,   8.0f, 1.0f)
DAI_DAITEX_RANGE("noise",     "gain",       0.0f,   1.0f, 0.01f)
DAI_DAITEX_RANGE("noise",     "lacunarity", 1.0f,   4.0f, 0.05f)
DAI_DAITEX_RANGE("noise",     "seed",       0.0f, 999.0f, 1.0f)
DAI_DAITEX_RANGE("gradient",  "from",       0.0f,   1.0f, 0.01f)
DAI_DAITEX_RANGE("gradient",  "to",         0.0f,   1.0f, 0.01f)
DAI_DAITEX_RANGE("brick",     "rows",       1.0f,  32.0f, 1.0f)
DAI_DAITEX_RANGE("brick",     "cols",       1.0f,  32.0f, 1.0f)
DAI_DAITEX_RANGE("brick",     "gap",        0.0f,   0.5f, 0.005f)
DAI_DAITEX_RANGE("brick",     "shift",      0.0f,   1.0f, 0.01f)
DAI_DAITEX_RANGE("brick",     "bevel",      0.0f,   0.5f, 0.005f)
DAI_DAITEX_RANGE("brick",     "variation",  0.0f,   1.0f, 0.01f)
DAI_DAITEX_RANGE("checker",   "rows",       1.0f,  32.0f, 1.0f)
DAI_DAITEX_RANGE("checker",   "cols",       1.0f,  32.0f, 1.0f)
DAI_DAITEX_RANGE("mix",       "factor",     0.0f,   1.0f, 0.01f)
DAI_DAITEX_RANGE("levels",    "in_min",     0.0f,   1.0f, 0.01f)
DAI_DAITEX_RANGE("levels",    "in_max",     0.0f,   1.0f, 0.01f)
DAI_DAITEX_RANGE("levels",    "out_min",    0.0f,   1.0f, 0.01f)
DAI_DAITEX_RANGE("levels",    "out_max",    0.0f,   1.0f, 0.01f)
DAI_DAITEX_RANGE("levels",    "gamma",      0.1f,   4.0f, 0.02f)
DAI_DAITEX_RANGE("blur",      "radius",     0.0f,  32.0f, 1.0f)
DAI_DAITEX_RANGE("blur",      "passes",     1.0f,   4.0f, 1.0f)
DAI_DAITEX_RANGE("normal",    "strength",   0.0f,   8.0f, 0.05f)
DAI_DAITEX_RANGE("ao",        "radius",     1.0f,  32.0f, 1.0f)
DAI_DAITEX_RANGE("ao",        "strength",   0.0f,   4.0f, 0.05f)
DAI_DAITEX_RANGE("const",     "value",      0.0f,   1.0f, 0.01f)

// The three the graph itself has, rather than one of its nodes. The type is
// the empty string because they belong to no node - which is also how the
// inspector asks for them.
DAI_DAITEX_RANGE("",          "resolution", 16.0f, 2048.0f, 1.0f)
DAI_DAITEX_RANGE("",          "seed",        0.0f, 99999.0f, 1.0f)
DAI_DAITEX_RANGE("",          "tiling_m",    0.1f,   16.0f, 0.05f)
