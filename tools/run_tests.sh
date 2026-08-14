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

# Everything this run prints also lands in build/last_run.log, APPENDED after
# whatever ./build.sh left there. RUN.md quotes two numbers - the drone show
# suite's check count out of the build, and the TOTAL out of this script - and
# a document that quotes a number nobody can re-read is a document that ages
# into fiction. The file is the receipt for both, in the order the two commands
# are documented in.
#
# Done by re-running the script through `tee` once rather than by redirecting
# with a process substitution: the exit code has to survive (a red run must stay
# red), and a background `tee` can lose the last lines when the shell exits
# under it - which would drop exactly the TOTAL line this exists for.
if [ "${DAI_RUN_LOG:-}" != "1" ]; then
    mkdir -p build
    echo "=== tools/run_tests.sh $(date -u '+%Y-%m-%dT%H:%M:%SZ') ===" >> build/last_run.log
    DAI_RUN_LOG=1 "$0" "$@" 2>&1 | tee -a build/last_run.log
    exit "${PIPESTATUS[0]}"
fi

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
test_droneshow
test_script
test_strings
test_hud
test_objmodel
test_assets
test_thumb
test_assetkind
test_editor_ui
test_window_two
"
# Deliberately NOT here (each needs a GPU or a display). The list is a VARIABLE
# rather than a comment because the run prints it: a suite that is quietly
# absent looks exactly like a suite that passed, and "all green" over an
# unnamed exclusion is the sentence this script exists to stop.
GPU_ONLY="test_render_visual test_ui_text test_gltf test_particles test_skinning test_ui test_viewport test_window"
#
# test_editor_ui was on that list too and did not belong there either: it
# creates no renderer and opens no window, it feeds the panels a pointer and
# reads the vertices back. Excluded, it rotted - the hierarchy grew a search
# box above its tree and nine checks had been clicking one row too high for
# months. It runs here now, and it carries the check that the gizmo lands on
# the object it moves.
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

# test_window_two needs a DISPLAY - it opens two real windows and checks that
# each one shows a different part of the same frame. A virtual screen is enough
# and the machine that builds this has one, so it runs rather than being
# excluded: excluded tests rot, and this one guards the claim that a torn off
# panel needs no second renderer.
DAI_TEST_DISPLAY=${DAI_TEST_DISPLAY:-:77}
if command -v Xvfb >/dev/null 2>&1; then
    if ! xdpyinfo -display "$DAI_TEST_DISPLAY" >/dev/null 2>&1; then
        Xvfb "$DAI_TEST_DISPLAY" -screen 0 1024x640x24 >/dev/null 2>&1 &
        XVFB_PID=$!
        sleep 1
    fi
else
    echo "-- no Xvfb: test_window_two will report itself skipped"
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
    if [ "$s" = "test_window_two" ]; then
        OUT=$(DAI_SHADER_DIR=shaders DISPLAY="$DAI_TEST_DISPLAY" timeout 120 "$BIN" 2>&1)
    else
        OUT=$(DAI_SHADER_DIR=shaders timeout 120 "$BIN" $ARGS 2>&1)
    fi
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

# The drone show panels, photographed. Not a suite - it asserts nothing - but
# the pictures are half of what the show mode is judged on, and a picture that
# somebody has to remember to regenerate is a picture that is wrong by the next
# review. It needs a renderer, so it runs on the virtual screen the two-window
# test already brought up, and a non-zero exit turns the run red like any suite.
SHOTS=${DAI_SHOTS_DIR:-.gauntlet-shots}
if [ -x build/droneshow_shot ]; then
    mkdir -p "$SHOTS"
    # All three sets, exactly as build.sh makes them: the plain names at
    # 1600x900, `narrow-` at 1100x700 where a panel that guesses its layout
    # collides with itself, `wide-` at 1920x1080. One size refreshed and two
    # left from last month is worse than none, because the stale two look
    # current.
    for SET in 1600x900: 1100x700:narrow- 1920x1080:wide-; do
        DIM=${SET%%:*}; TAG=${SET#*:}
        SW=${DIM%%x*}; SH=${DIM##*x}
        OUT=$(DAI_SHADER_DIR=shaders DISPLAY="$DAI_TEST_DISPLAY" timeout 300 \
              ./build/droneshow_shot "$SHOTS" "$SW" "$SH" "$TAG" 2>&1)
        RC=$?
        NAME="droneshow_shot ${TAG:-plain}"
        if [ "$RC" = "0" ]; then
            printf '%-20s %3s/%-3s  ok  (%s %sx%s)\n' "$NAME" "-" "-" "$SHOTS" "$SW" "$SH"
            [ "$VERBOSE" = "1" ] && printf '%s\n' "$OUT" | sed 's/^/    /'
        else
            FAILED="$FAILED droneshow_shot(${SW}x${SH})"
            printf '%-20s %3s/%-3s  rc=%s  FAIL\n' "$NAME" "-" "-" "$RC"
            printf '%s\n' "$OUT" | tail -8 | sed 's/^/    /'
        fi
    done
else
    MISSING="$MISSING droneshow_shot"
fi

# The game-mode picture that sits beside the show sets, from the tool that
# makes the editor's own shots. Same reason: a picture nobody can regenerate is
# a picture that is wrong by the next review.
if [ -x build/editor_shot ]; then
    mkdir -p "$SHOTS" build/editor_shots
    OUT=$(DAI_SHADER_DIR=shaders DISPLAY="$DAI_TEST_DISPLAY" timeout 300 \
          ./build/editor_shot build/editor_shots 1600 900 "$SHOTS" 2>&1)
    RC=$?
    if [ "$RC" = "0" ]; then
        printf '%-20s %3s/%-3s  ok  (%s/09-game-mode-editor.png)\n' "editor_shot" "-" "-" "$SHOTS"
        [ "$VERBOSE" = "1" ] && printf '%s\n' "$OUT" | sed 's/^/    /'
    else
        FAILED="$FAILED editor_shot"
        printf '%-20s %3s/%-3s  rc=%s  FAIL\n' "editor_shot" "-" "-" "$RC"
        printf '%s\n' "$OUT" | tail -8 | sed 's/^/    /'
    fi
else
    MISSING="$MISSING editor_shot"
fi

echo "-------------------------------------------"
printf 'TOTAL %d passed, %d failed\n' "$TOTAL_PASS" "$TOTAL_FAIL"
echo "not run (needs GPU): $GPU_ONLY"
[ -n "$MISSING" ] && echo "not built:$MISSING"
[ -n "$SUITES_SKIPPED" ] && echo "skipped:$SUITES_SKIPPED"
[ -n "$NOCOUNT" ] && echo "no counts parsed (summary line changed shape?):$NOCOUNT"
if [ -n "$FAILED" ]; then
    echo "RED:$FAILED"
    exit 1
fi
echo "all green"
