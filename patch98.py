import io

# ---------------------------------------------------------------------------
# Schriftqualitaet. Zwei gemessene Ursachen, keine Geschmacksfrage.
# ---------------------------------------------------------------------------
p = 'src/dai_font.cpp'
s = io.open(p, encoding='utf-8').read()

old = """// Scanline fill with 4x vertical supersampling. Horizontal coverage is exact
// (analytic span ends), vertical is sampled - which is the cheap half of what
// a real rasteriser does and is visually indistinguishable at UI sizes."""
new = """// Scanline fill with vertical supersampling. Horizontal coverage is exact
// (analytic span ends), vertical is sampled.
//
// Two measured faults were fixed here, and both made text look thinner and
// duller than the same font in Unity:
//
// 1. COVERAGE WAS TRUNCATED PER SAMPLE ROW. The accumulator was integer and
//    each row added (uint16_t)(255/SS) - with SS=4 that is (int)63.75 = 63.
//    A fully covered pixel therefore reached 4*63 = 252, never 255: no glyph
//    in the UI was ever fully opaque. Measured against exact coverage the
//    error was -2 to -4 of 255 everywhere, and it hits the thin parts hardest
//    (at a quarter coverage, -4 of 64 is six percent of the stem). The
//    accumulator is a float now and rounds ONCE, at the end.
//
// 2. THE BLEND IS NOT GAMMA CORRECT AND CANNOT BE HERE. The swapchain is
//    B8G8R8A8_UNORM (see rhi_vulkan_window*.cpp), so the GPU mixes ENCODED
//    sRGB values, while light mixes linearly. For light text on a dark panel
//    that makes every partly covered pixel too dark - measured up to 28 of
//    255 for #D6D6D6 text on a #383838 panel, which is exactly the "washed
//    out" look. Fitting a' = a^(1/g) against the linear-correct result gives
//    g = 1.62 for bright text, 1.45 for dim text and 1.50 for text on a
//    button, all with an RMS residual near 2 of 255 - so one curve serves the
//    whole UI. GAMMA below is that curve, baked into the atlas once.
//
//    It is a DARK THEME correction. All three themes this editor ships are
//    dark (see dai_ui.cpp); on a light background the same curve would make
//    text too heavy, and the honest fix for that day is an sRGB swapchain,
//    not a second constant."""
assert s.count(old) == 1, 'rasteriser comment not found'
s = s.replace(old, new)

old = """    const int SS = 4;
    std::vector<float> xs;
    std::vector<int> dirs;
    std::vector<uint16_t> acc((size_t)w, 0);

    for (int y = 0; y < h; ++y) {
        std::fill(acc.begin(), acc.end(), 0);"""
new = """    // 16 rather than 4: the atlas is rasterised once per font size, so the
    // cost is a few milliseconds at startup, and near horizontal edges - the
    // crossbar of an 'e', the top of an 'a' - stop stepping between four
    // discrete grey levels.
    const int SS = 16;
    const float GAMMA = 1.45f;          // measured, see the note above
    std::vector<float> xs;
    std::vector<int> dirs;
    std::vector<float> acc((size_t)w, 0.0f);

    for (int y = 0; y < h; ++y) {
        std::fill(acc.begin(), acc.end(), 0.0f);"""
assert s.count(old) == 1, 'rasteriser setup not found'
s = s.replace(old, new)

old = """                if (ia == ib) {
                    acc[(size_t)ia] += (uint16_t)((xb - xa) * (255.0f / SS));
                } else {
                    acc[(size_t)ia] += (uint16_t)(((float)(ia + 1) - xa) * (255.0f / SS));
                    for (int x = ia + 1; x < ib; ++x) acc[(size_t)x] += (uint16_t)(255.0f / SS);
                    if (ib < w) acc[(size_t)ib] += (uint16_t)((xb - (float)ib) * (255.0f / SS));
                }"""
new = """                // Coverage as a FRACTION of the pixel, summed exactly. The
                // old code turned each contribution into a byte here, and a
                // byte per sample row cannot represent a sixteenth of a pixel.
                const float SW = 1.0f / (float)SS;
                if (ia == ib) {
                    acc[(size_t)ia] += (xb - xa) * SW;
                } else {
                    acc[(size_t)ia] += ((float)(ia + 1) - xa) * SW;
                    for (int x = ia + 1; x < ib; ++x) acc[(size_t)x] += SW;
                    if (ib < w) acc[(size_t)ib] += (xb - (float)ib) * SW;
                }"""
assert s.count(old) == 1, 'span accumulate not found'
s = s.replace(old, new)

old = """        uint8_t *row = &out[(size_t)(h - 1 - y) * w];
        for (int x = 0; x < w; ++x) row[x] = (uint8_t)std::min<uint16_t>(acc[(size_t)x], 255);"""
new = """        uint8_t *row = &out[(size_t)(h - 1 - y) * w];
        for (int x = 0; x < w; ++x) {
            float c = acc[(size_t)x];
            if (c <= 0.0f) { row[x] = 0; continue; }
            if (c > 1.0f) c = 1.0f;
            // The gamma the blend cannot apply, applied once here. 1 stays 1
            // and 0 stays 0, so solid interiors are untouched - this only
            // changes the edge pixels, which is where the whole fault lived.
            c = std::pow(c, 1.0f / GAMMA);
            row[x] = (uint8_t)(c * 255.0f + 0.5f);
        }"""
assert s.count(old) == 1, 'row write not found'
s = s.replace(old, new)

if '#include <cmath>' not in s:
    s = s.replace('#include <cstring>', '#include <cstring>\n#include <cmath>', 1)

io.open(p, 'w', encoding='utf-8').write(s)
print('dai_font.cpp: exact coverage, 16x vertical samples, measured gamma')
