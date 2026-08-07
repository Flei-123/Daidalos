#!/bin/bash
# Runs every test binary that can run WITHOUT a GPU or a display, and prints
# one total at the end.
#
# Why this script exists: build.sh compiles ~35 test binaries and only ever
# RAN seven of them. tests/test_doc.cpp went red in commit 96b3db2 (the prefab
# root stopped being an empty wrapper, the test still counted the wrapper) and
# nobody noticed for two releases, because nothing ever executed it. A test
# that is only compiled is a syntax check with extra steps.
#
#   tools/run_tests.sh          # the headless set
#   tools/run_tests.sh -v       # show the output of every suite, not just fails
#
# Anything needing a swapchain, an X11 display or a real font atlas is NOT in
# this list - those stay manual, and the reason is written next to each one.
set -u
cd "$(dirname "$0")/.."

VERBOSE=0
[ "${1:-}" = "-v" ] && VERBOSE=1

# Headless: arithmetic, documents, parsing, layout. No Vulkan instance is
# created, no window is opened.
SUITES="
test_daidalos
test_image
test_merge
test_save
test_input
test_editor
test_editor_live
test_doc
test_play
test_cam
test_talos
test_font
test_svg
test_keys
test_fracture
test_update
test_ui_window
test_ui_field
test_dock
test_project
test_script
test_strings
test_hud
test_objmodel
test_assets
test_thumb
"
# Deliberately NOT here (each needs a GPU or a display):
#   test_render_visual test_ui_text test_gltf test_particles test_skinning
#   test_ui test_viewport test_editor_ui test_window
#
# test_assets USED to be on that list and does not belong there: it mounts a
# folder and reads geometry, and the only thing it ever wanted a renderer for
# is a handle it never draws. It was excluded, so nobody noticed it had gone
# red - [8c] was still asserting that a .png is junk months after the browser
# started opening textures on purpose. Excluded tests rot.

# test_image needs fixtures produced by a REAL encoder (Python zlib + PIL) -
# checking our decoder against our own encoder would prove nothing. Without
# them the suite reported eight failures that were not decoder bugs at all,
# just a missing directory. Generate them here, and if Python cannot, say so
# and skip the suite instead of painting the run red for the wrong reason.
PNGFIX=${PNGFIX:-/tmp/pngfix}
SUITES_SKIPPED=""
if [ ! -f "$PNGFIX/gradient.png" ]; then
    if python3 tools/make_png_fixtures.py "$PNGFIX" >/dev/null 2>&1; then
        echo "-- png fixtures generated in $PNGFIX"
    else
        echo "-- png fixtures NOT generated (needs python3 + numpy + Pillow) - skipping test_image"
        SUITES=$(printf '%s\n' "$SUITES" | grep -v '^test_image$')
        SUITES_SKIPPED="test_image"
    fi
fi

TOTAL_PASS=0
TOTAL_FAIL=0
MISSING=""
FAILED=""
NOCOUNT=""

for s in $SUITES; do
    BIN="build/$s"
    if [ ! -x "$BIN" ]; then
        MISSING="$MISSING $s"
        continue
    fi
    ARGS=""
    [ "$s" = "test_image" ] && ARGS="$PNGFIX"
    OUT=$(DAI_SHADER_DIR=shaders timeout 120 "$BIN" $ARGS 2>&1)
    RC=$?
    # The suites do NOT all print the same summary. Four shapes exist:
    #
    #   "51 passed, 0 failed"            most of the newer C++ suites
    #   "51 bestanden, 0 fehlgeschlagen" the ones written in German
    #   "ok: 184 checks, 0 failures"     the project/fracture/update suites
    #   "ok: 32 key codes match ..."     test_keys, which counts nothing
    #
    # Reading only the first shape is how eight suites - among them
    # test_daidalos with its 51 cases - were reported as "0/0  ok" for months.
    # Zero passed and zero failed is not a pass, it is a suite nobody read.
    P=$(printf '%s\n' "$OUT" | grep -oE '[0-9]+ (passed|bestanden|checks)' | tail -1 | grep -oE '[0-9]+')
    F=$(printf '%s\n' "$OUT" | grep -oE '[0-9]+ (failed|fehlgeschlagen|failures|wrong)' | tail -1 | grep -oE '[0-9]+')
    [ -z "${P:-}" ] && P=0
    [ -z "${F:-}" ] && F=0
    # A suite that ran, exited 0 and reported no counts at all is a suite whose
    # summary line changed shape - flag it instead of scoring it as fine.
    if [ "$P" = "0" ] && [ "$F" = "0" ] && [ "$RC" = "0" ]; then
        NOCOUNT="$NOCOUNT $s"
    fi
    TOTAL_PASS=$((TOTAL_PASS + P))
    TOTAL_FAIL=$((TOTAL_FAIL + F))
    if [ "$RC" != "0" ] || [ "$F" != "0" ]; then
        FAILED="$FAILED $s"
        printf '%-20s %3s/%-3s  rc=%s  FAIL\n' "$s" "$P" "$F" "$RC"
        printf '%s\n' "$OUT" | grep -iE 'fail|assert|error' | head -12 | sed 's/^/    /'
    else
        printf '%-20s %3s/%-3s  ok\n' "$s" "$P" "$F"
        [ "$VERBOSE" = "1" ] && printf '%s\n' "$OUT" | sed 's/^/    /'
    fi
done

echo "-------------------------------------------"
printf 'TOTAL %d passed, %d failed\n' "$TOTAL_PASS" "$TOTAL_FAIL"
[ -n "$MISSING" ] && echo "not built:$MISSING"
[ -n "$SUITES_SKIPPED" ] && echo "skipped:$SUITES_SKIPPED"
[ -n "$NOCOUNT" ] && echo "no counts parsed (summary line changed shape?):$NOCOUNT"
if [ -n "$FAILED" ]; then
    echo "RED:$FAILED"
    exit 1
fi
echo "all green"
