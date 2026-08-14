// The shared half of build/test_droneshow.
//
// One binary, five source files. The split is not tidiness: the suites are
// written against different halves of the pipeline and they are built in
// parallel, and two people editing one 2,000 line test file is how assertions
// get lost in a merge. Each case file exposes exactly one entry point below,
// tests/test_droneshow.cpp calls them in order and prints one total.
//
// CHECK is the same macro the rest of this repo's tests use, deliberately: a
// failure prints what was expected and what happened, on one line, and the run
// keeps going so one broken stage does not hide the other four.

#ifndef DAI_DRONESHOW_CASES_HPP
#define DAI_DRONESHOW_CASES_HPP

#include "dai_show.h"

#include <cstdio>
#include <cstdlib>

extern int g_show_pass;
extern int g_show_fail;

#define CHECK(cond, ...) do {                                            \
    if (cond) { ++g_show_pass; }                                         \
    else { ++g_show_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); \
           std::printf("\n"); }                                          \
} while (0)

// Announces a section, so the output reads like the pipeline it tests.
void show_section(const char *name);

// Deterministic test geometry, shared so three suites measure the same figure
// rather than three subtly different ones. Fills a unit cube's surface as a
// triangle soup; the caller owns nothing (static storage, one shape).
struct dai_show_test_mesh {
    const float    *positions;
    const float    *normals;
    const float    *uvs;
    uint32_t        vertex_count;
    const uint32_t *indices;
    uint32_t        index_count;
};
// `kind` 0 = a 1x1x1 box, 1 = a 10x10 ground-parallel quad, 2 = a sphere.
dai_show_test_mesh show_test_mesh(int kind);

// A formation of n points on a grid at `spacing` metres, centred on `centre`.
// The simplest thing that satisfies a minimum distance, and therefore the right
// starting point for a test about what happens BETWEEN formations.
void show_grid_formation(dai_show_point *out, uint32_t n, float spacing,
                         dai_vec3 centre);

// The five suites. Each returns the number of failures it added.
int show_cases_sample(void);   // tests/droneshow_cases_sample.cpp
int show_cases_assign(void);   // tests/droneshow_cases_assign.cpp
int show_cases_plan(void);     // tests/droneshow_cases_plan.cpp
int show_cases_io(void);       // tests/droneshow_cases_io.cpp

#endif
