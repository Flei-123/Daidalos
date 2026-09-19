/* dai_ui_layout.inl - what the frame wrote, as data a test can fail on.
 *
 * WHY. AGENTENPLAN.md, M3: cut-off and overlapping text is not looked for in
 * the picture. The gauntlet jury found "Block.Beve", "Pos...", "Rot..." and a
 * timeline ending in "13" instead of "130" by READING screenshots, and a
 * reviewer who reads screenshots is the most expensive test in the project.
 * The layout knows all of it exactly: dai_ui_text_record already writes every
 * drawn string down with the box it went into and the clip that was in force.
 * This file turns that log into three assertions and one dump.
 *
 *   CLIPPED   the string was shortened by the panel itself and now ends in an
 *             ellipsis. "Pos..." is not a label, it is a label that did not
 *             fit. One honest ellipsis exists - "Add Component..." is Unity's
 *             "this opens a dialog" - so callers pass the ones they mean.
 *
 *   CUT_OFF   the ink runs past the clip it sits under. This is the one that
 *             produced "Block.Beve": nothing shortened it, the panel simply
 *             ends mid-glyph, and the reader cannot tell that he is missing
 *             anything.
 *
 *   OVERLAP   two strings occupy the same pixels. Filters are not optional
 *             here or the result drowns in false positives: only strings under
 *             the SAME clip are compared (a tooltip over a panel is not a
 *             bug), identical strings at the same spot are one string drawn
 *             twice (shadow, hover redraw) and not an overlap, and the
 *             comparison is on the ink box the recorder measured, not on a
 *             line box with its leading - line-height padding overlaps by
 *             design and reporting it teaches everybody to ignore the check.
 *
 * Header-only and included by the hosts that have a dai_ui, the same shape as
 * dai_props_host.inl: one implementation, and the editor, the shot tool and a
 * test cannot drift on what "cut off" means.
 */

#ifndef DAI_UI_LAYOUT_INL
#define DAI_UI_LAYOUT_INL

#include <cstdio>
#include <cstring>
#include <cstdlib>

typedef struct dai_layout_issue {
    int   kind;            /* 0 CLIPPED, 1 CUT_OFF, 2 OVERLAP */
    float x, y, w, h;
    float clip_x, clip_y, clip_w, clip_h;
    char  text[96];
    char  other[96];       /* OVERLAP: the string it collides with */
    float overrun;         /* CUT_OFF: pixels past the clip; OVERLAP: area */
} dai_layout_issue;

static const char *dai_layout_kind_name(int k) {
    return k == 0 ? "CLIPPED" : (k == 1 ? "CUT_OFF" : "OVERLAP");
}

static int dai_layout_ends_in_ellipsis(const char *s) {
    size_t n = std::strlen(s);
    if (n >= 3 && !std::strcmp(s + n - 3, "...")) return 1;
    /* U+2026, the single-character ellipsis some panels use */
    if (n >= 3 && (unsigned char)s[n - 3] == 0xE2 && (unsigned char)s[n - 2] == 0x80
        && (unsigned char)s[n - 1] == 0xA6) return 1;
    return 0;
}

static int dai_layout_same_clip(const dai_ui_text_rec &a, const dai_ui_text_rec &b) {
    const float e = 0.5f;
    return (a.clip_x > b.clip_x - e && a.clip_x < b.clip_x + e &&
            a.clip_y > b.clip_y - e && a.clip_y < b.clip_y + e &&
            a.clip_w > b.clip_w - e && a.clip_w < b.clip_w + e &&
            a.clip_h > b.clip_h - e && a.clip_h < b.clip_h + e);
}

/* Collect the problems of the CURRENT frame's text log.
 *
 * `allowed` is a NULL-terminated list of strings that may legitimately end in
 * an ellipsis. `max_out` issues are written; the return value is how many
 * there are in total, so a caller can report "42 problems" while printing ten.
 */
static uint32_t dai_ui_layout_check(const dai_ui *ui,
                                    const char *const *allowed,
                                    dai_layout_issue *out, uint32_t max_out) {
    const uint32_t n = dai_ui_text_record_count(ui);
    uint32_t found = 0;

    dai_ui_text_rec *recs = (dai_ui_text_rec *)std::malloc(sizeof(dai_ui_text_rec) * (n ? n : 1));
    if (!recs) return 0;
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; ++i)
        if (dai_ui_text_record_at(ui, i, &recs[m])) ++m;

    for (uint32_t i = 0; i < m; ++i) {
        const dai_ui_text_rec &t = recs[i];
        if (!t.text[0]) continue;

        int excused = 0;
        for (const char *const *a = allowed; a && *a; ++a)
            if (!std::strcmp(*a, t.text)) { excused = 1; break; }

        if (!excused && dai_layout_ends_in_ellipsis(t.text)) {
            if (found < max_out) {
                dai_layout_issue &is = out[found];
                is.kind = 0; is.x = t.x; is.y = t.y; is.w = t.w; is.h = t.h;
                is.clip_x = t.clip_x; is.clip_y = t.clip_y;
                is.clip_w = t.clip_w; is.clip_h = t.clip_h;
                std::snprintf(is.text, sizeof(is.text), "%s", t.text);
                is.other[0] = 0; is.overrun = 0;
            }
            ++found;
        }

        const float run = (t.x + t.w) - (t.clip_x + t.clip_w);
        if (run > 0.5f) {
            if (found < max_out) {
                dai_layout_issue &is = out[found];
                is.kind = 1; is.x = t.x; is.y = t.y; is.w = t.w; is.h = t.h;
                is.clip_x = t.clip_x; is.clip_y = t.clip_y;
                is.clip_w = t.clip_w; is.clip_h = t.clip_h;
                std::snprintf(is.text, sizeof(is.text), "%s", t.text);
                is.other[0] = 0; is.overrun = run;
            }
            ++found;
        }
    }

    /* OVERLAP. O(n^2) over the strings of one frame - a few hundred at most,
     * and a sweep line here would be cleverness nobody can check. */
    for (uint32_t i = 0; i < m; ++i) {
        for (uint32_t j = i + 1; j < m; ++j) {
            const dai_ui_text_rec &a = recs[i], &b = recs[j];
            if (!a.text[0] || !b.text[0]) continue;
            if (!dai_layout_same_clip(a, b)) continue;
            if (!std::strcmp(a.text, b.text)) continue;  /* drawn twice, not overlapping */

            const float x0 = a.x > b.x ? a.x : b.x;
            const float y0 = a.y > b.y ? a.y : b.y;
            const float x1 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
            const float y1 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
            const float ow = x1 - x0, oh = y1 - y0;
            /* A pixel of touching is not an overlap: rows sit flush against
             * each other by design. Ask for a quarter of the smaller line. */
            const float min_h = a.h < b.h ? a.h : b.h;
            if (ow <= 1.0f || oh <= min_h * 0.25f) continue;

            if (found < max_out) {
                dai_layout_issue &is = out[found];
                is.kind = 2; is.x = x0; is.y = y0; is.w = ow; is.h = oh;
                is.clip_x = a.clip_x; is.clip_y = a.clip_y;
                is.clip_w = a.clip_w; is.clip_h = a.clip_h;
                std::snprintf(is.text, sizeof(is.text), "%s", a.text);
                std::snprintf(is.other, sizeof(is.other), "%s", b.text);
                is.overrun = ow * oh;
            }
            ++found;
        }
    }

    std::free(recs);
    return found;
}

/* The whole log as JSON, one object per string plus the problems found.
 *
 * This is what the MCP server hands an agent: "inspector.name CLIPPED: ink
 * 118px > box 96px" instead of a picture it has to squint at. Written by hand
 * rather than through a JSON library because this repository does not have one
 * on the C++ side and a dump with three field types does not need one.
 */
static int dai_ui_layout_dump(const dai_ui *ui, const char *path,
                              const char *const *allowed) {
    std::FILE *f = std::fopen(path, "wb");
    if (!f) return 0;

    std::fprintf(f, "{\n  \"texts\": [\n");
    const uint32_t n = dai_ui_text_record_count(ui);
    uint32_t written = 0;
    for (uint32_t i = 0; i < n; ++i) {
        dai_ui_text_rec t;
        if (!dai_ui_text_record_at(ui, i, &t)) continue;
        if (written) std::fprintf(f, ",\n");
        /* The text is escaped the minimal honest way: quotes and backslashes.
         * A label with a control character in it is a different bug. */
        char esc[192]; size_t o = 0;
        for (const char *p = t.text; *p && o < sizeof(esc) - 2; ++p) {
            if (*p == '"' || *p == '\\') esc[o++] = '\\';
            esc[o++] = *p;
        }
        esc[o] = 0;
        std::fprintf(f,
            "    {\"text\":\"%s\",\"rect\":[%.1f,%.1f,%.1f,%.1f],"
            "\"clip\":[%.1f,%.1f,%.1f,%.1f]}",
            esc, (double)t.x, (double)t.y, (double)t.w, (double)t.h,
            (double)t.clip_x, (double)t.clip_y, (double)t.clip_w, (double)t.clip_h);
        ++written;
    }
    std::fprintf(f, "\n  ],\n  \"issues\": [\n");

    enum { MAXI = 256 };
    dai_layout_issue issues[MAXI];
    const uint32_t total = dai_ui_layout_check(ui, allowed, issues, MAXI);
    const uint32_t shown = total < MAXI ? total : MAXI;
    for (uint32_t i = 0; i < shown; ++i) {
        const dai_layout_issue &is = issues[i];
        std::fprintf(f,
            "    {\"kind\":\"%s\",\"text\":\"%s\"%s%s%s,"
            "\"rect\":[%.1f,%.1f,%.1f,%.1f],\"clip\":[%.1f,%.1f,%.1f,%.1f],\"overrun\":%.1f}%s\n",
            dai_layout_kind_name(is.kind), is.text,
            is.other[0] ? ",\"other\":\"" : "", is.other[0] ? is.other : "",
            is.other[0] ? "\"" : "",
            (double)is.x, (double)is.y, (double)is.w, (double)is.h,
            (double)is.clip_x, (double)is.clip_y, (double)is.clip_w, (double)is.clip_h,
            (double)is.overrun, (i + 1 < shown) ? "," : "");
    }
    std::fprintf(f, "  ],\n  \"counts\": {\"texts\": %u, \"issues\": %u}\n}\n",
                 written, total);
    std::fclose(f);
    return (int)total;
}

#endif /* DAI_UI_LAYOUT_INL */
