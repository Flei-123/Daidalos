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
test_daitex
test_gltf_headless
test_ui_headless
test_viewport_headless
"
# ...and the ones build.sh cannot build. That script is frozen and names every
# translation unit it compiles one by one, so a feature whose implementation is
# a HEADER (include/dai_daitex.h - see the comment at the top of it) has
# nowhere to be compiled except next to where it is run. One g++ line each, and
# a compile error turns the run red exactly like a failing check would.
HEADER_ONLY="test_daitex"

# The GPU-less HALVES of three suites that are on the GPU list as wholes.
#
# test_gltf, test_ui and test_viewport each open a Vulkan device - test_gltf in
# its second statement, test_ui in its last section, test_viewport in its first
# - so all three are excluded here, and with them everything they check that
# never wanted a device: the glTF container, the accessors, the writer and the
# round trip; the UI layer's batching, clicks, sliders and layout; the
# projection and picking arithmetic the whole editor's gizmo work stands on.
# Excluded tests rot - that sentence is written twice more in this file, and
# these three are the largest thing it was still true of.
#
# So the halves live in tests/test_*_headless.cpp (and tests/ui_cases.hpp, which
# test_ui itself now runs as well - the cases were MOVED, not copied), and they
# are compiled here for the same reason the header-only suite above is: build.sh
# is frozen and names its translation units one by one. Same flags, the archives
# that script produced, and a compile error turns the run red like a failing
# check would.
SPLIT_SUITES="test_gltf_headless test_ui_headless test_viewport_headless"

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

for h in $HEADER_ONLY; do
    [ -f "tests/$h.cpp" ] || continue
    if g++ -std=c++17 -O2 -Wall -Wno-unused-parameter -Iinclude -Isrc \
           "tests/$h.cpp" src/dai_json.cpp src/dai_image.cpp -o "build/$h" \
           >"build/$h.build.log" 2>&1; then
        echo "-- $h compiled (header-only feature, not in build.sh)"
    else
        echo "-- $h did NOT compile:"
        head -12 "build/$h.build.log" | sed 's/^/    /'
        FAILED="$FAILED $h"
        rm -f "build/$h"
    fi
done

JOLT_LIB=${JOLT_LIB:-/root/projects/jolt-build}
TALOS=${TALOS:-/root/projects/talos}
AULOS=${AULOS:-/root/projects/aulos}
SPLIT_LIBS=""
[ -f "$AULOS/build/libaulos.a" ] && SPLIT_LIBS="$SPLIT_LIBS $AULOS/build/libaulos.a"
[ -f "$TALOS/build/libtalos.a" ] && SPLIT_LIBS="$SPLIT_LIBS $TALOS/build/libtalos.a"
SPLIT_X11=""
[ -f /usr/include/X11/Xlib.h ] && SPLIT_X11="-lX11"
for s in $SPLIT_SUITES; do
    [ -f "tests/$s.cpp" ] || continue
    if [ ! -f build/libdaidalos.a ] || [ ! -f build/libdaidalos_vk.a ]; then
        echo "-- $s NOT compiled: build/libdaidalos*.a missing - run ./build.sh first"
        MISSING="$MISSING $s"
        continue
    fi
    # shellcheck disable=SC2086
    if g++ -std=c++17 -O2 -fno-rtti -ffp-contract=fast -pthread -Wall -Wno-unused-parameter \
           -Iinclude -Itests "tests/$s.cpp" \
           build/libdaidalos_vk.a build/libdaidalos.a build/libdaidalos_vk.a $SPLIT_LIBS \
           -L"$JOLT_LIB" -lJolt -lvulkan $SPLIT_X11 -lpthread -lm -ldl -o "build/$s" \
           >"build/$s.build.log" 2>&1; then
        echo "-- $s compiled (the GPU-less half of a suite build.sh does not name)"
    else
        echo "-- $s did NOT compile:"
        head -12 "build/$s.build.log" | sed 's/^/    /'
        FAILED="$FAILED $s"
        rm -f "build/$s"
    fi
done

for s in $SUITES; do
    BIN="build/$s"
    if [ ! -x "$BIN" ]; then
        MISSING="$MISSING $s"
        continue
    fi
    ARGS=""
    [ "$s" = "test_image" ] && ARGS="$PNGFIX"
    # 120 s is the cap for a suite that is arithmetic on a document. One suite
    # is not: test_droneshow's last plan case flies 10000 drones through the
    # separation solver, which is the O(n^2) pass the whole show mode rests on,
    # and it needs 181 s of -O3 on this machine (measured; the 10000 row of its
    # own scaling table reports 20.9 s of that for one layer alone). It was
    # being killed at 120 s and reported "0/0 rc=124 FAIL", which cost the run
    # its 491 green checks and painted it RED over a suite that passes.
    #
    # The cap is raised for that ONE suite, by name, and not for the rest: a
    # blanket 600 s would turn a real hang anywhere else into a ten minute
    # wait. build.sh runs the same binary to completion at its line 363, so the
    # number below is the runtime that script already pays.
    TMO=120
    [ "$s" = "test_droneshow" ] && TMO=600
    if [ "$s" = "test_window_two" ]; then
        OUT=$(DAI_SHADER_DIR=shaders DISPLAY="$DAI_TEST_DISPLAY" timeout "$TMO" "$BIN" 2>&1)
    else
        OUT=$(DAI_SHADER_DIR=shaders timeout "$TMO" "$BIN" $ARGS 2>&1)
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

# The Jarvis bridge, end to end against the editor that build.sh just made.
# A suite like any other - it prints "ok: N checks, 0 failures" and its checks
# are counted in the total - except that it is written in Python, because what
# it is testing is a SOCKET, and a test that links the thing it talks to can
# only prove the function call. It starts build/editor_demo twice (once
# without DAI_BRIDGE_PORT, to prove the socket stays shut) so it needs the
# virtual screen the two-window test already brought up.
if [ -x build/editor_demo ] && command -v python3 >/dev/null 2>&1; then
    OUT=$(DAI_SHADER_DIR=shaders DISPLAY="$DAI_TEST_DISPLAY" timeout 300 \
          python3 tools/bridge_check.py 2>&1)
    RC=$?
    P=$(printf '%s\n' "$OUT" | grep -oE '[0-9]+ checks' | tail -1 | grep -oE '[0-9]+')
    F=$(printf '%s\n' "$OUT" | grep -oE '[0-9]+ failures' | tail -1 | grep -oE '[0-9]+')
    [ -z "${P:-}" ] && P=0
    [ -z "${F:-}" ] && F=0
    TOTAL_PASS=$((TOTAL_PASS + P - F))
    TOTAL_FAIL=$((TOTAL_FAIL + F))
    if [ "$RC" = "0" ] && [ "$F" = "0" ] && [ "$P" != "0" ]; then
        printf '%-20s %3s/%-3s  ok\n' "bridge_check" "$P" "$F"
        [ "$VERBOSE" = "1" ] && printf '%s\n' "$OUT" | sed 's/^/    /'
    else
        FAILED="$FAILED bridge_check"
        printf '%-20s %3s/%-3s  rc=%s  FAIL\n' "bridge_check" "$P" "$F" "$RC"
        printf '%s\n' "$OUT" | tail -12 | sed 's/^/    /'
    fi
else
    MISSING="$MISSING bridge_check"
fi

# The modelling round's pictures: a room built through the bridge, in the real
# editor, with the blockout, the materials and the door socket in it.
# tools/build_modeling_shot.sh compiles the tool as well as running it - see
# the note at the top of that file for why a compile happens here at all.
#
# Three sets, like the drone show's: the plain names at 1600x900, `wide-` at
# 1920x1080 and `narrow-` at 1100x700. Only the first one compiles the tool;
# the other two run the binary it just made, because compiling the same
# translation unit three times to take three pictures is two minutes of nothing.
#
# And a check that is not a picture: shot 13 is taken by the BRIDGE, from the
# camera the bridge was handed, while 12 is taken from C++ - so if the two files
# are byte identical the bridge's camera was ignored and the "photograph from
# here" command is decoration. md5sum of both, and equal turns the run red.
if [ -f tools/modeling_shot.cpp ]; then
    for SET in 1600x900: 1920x1080:wide- 1100x700:narrow-; do
        DIM=${SET%%:*}; TAG=${SET#*:}
        SW=${DIM%%x*}; SH=${DIM##*x}
        if [ -z "$TAG" ]; then
            OUT=$(DISPLAY="$DAI_TEST_DISPLAY" timeout 600 \
                  ./tools/build_modeling_shot.sh "$SHOTS" "$SW" "$SH" 2>&1)
        elif [ -x build/modeling_shot ]; then
            OUT=$(DAI_SHADER_DIR=shaders DISPLAY="$DAI_TEST_DISPLAY" timeout 600 \
                  ./build/modeling_shot "$SHOTS" "$SW" "$SH" --prefix "$TAG" 2>&1)
        else
            OUT="modeling_shot was not compiled"; false
        fi
        RC=$?
        NAME="modeling_shot ${TAG:-plain}"
        M12=$(md5sum "$SHOTS/${TAG}12-modeling-materials.png" 2>/dev/null | cut -d' ' -f1)
        M13=$(md5sum "$SHOTS/${TAG}13-modeling-bridge-shot.png" 2>/dev/null | cut -d' ' -f1)
        if [ "$RC" = "0" ] && [ -n "$M13" ] && [ "$M12" = "$M13" ]; then
            RC=90
            OUT="$OUT
    ${TAG}12 and ${TAG}13 are the same file ($M12) - the bridge's camera was ignored"
        fi
        if [ "$RC" = "0" ]; then
            printf '%-20s %3s/%-3s  ok  (%s/%s10..13-modeling-*.png %sx%s)\n' \
                   "$NAME" "-" "-" "$SHOTS" "$TAG" "$SW" "$SH"
            [ "$VERBOSE" = "1" ] && printf '%s\n' "$OUT" | sed 's/^/    /'
        else
            FAILED="$FAILED modeling_shot(${SW}x${SH})"
            printf '%-20s %3s/%-3s  rc=%s  FAIL\n' "$NAME" "-" "-" "$RC"
            printf '%s\n' "$OUT" | tail -10 | sed 's/^/    /'
        fi
    done
else
    MISSING="$MISSING modeling_shot"
fi

# The blockout round's pictures: the room built through dai_doc in C++, drawn
# by the real editor with the blockout host attached - the CSG wall with its
# door hole, the stairs, the arch, the socket gizmo. Two sizes, same as the
# drone show sets: `wide-` at 1920x1080 and `narrow-` at 1100x700, where a
# panel that guesses its layout collides with itself. Compiled by the same
# script that runs it, for the reason build_modeling_shot.sh gives.
if [ -f tools/blockout_shot.cpp ]; then
    for SET in 1920x1080:wide- 1100x700:narrow-; do
        DIM=${SET%%:*}; TAG=${SET#*:}
        SW=${DIM%%x*}; SH=${DIM##*x}
        OUT=$(DISPLAY="$DAI_TEST_DISPLAY" timeout 600 \
              ./tools/build_blockout_shot.sh "$SHOTS" "$SW" "$SH" "$TAG" 2>&1)
        RC=$?
        NAME="blockout_shot ${TAG}"
        if [ "$RC" = "0" ]; then
            printf '%-20s %3s/%-3s  ok  (%s/%s14..15-blockout-*.png %sx%s)\n' "$NAME" "-" "-" "$SHOTS" "$TAG" "$SW" "$SH"
            [ "$VERBOSE" = "1" ] && printf '%s\n' "$OUT" | sed 's/^/    /'
        else
            FAILED="$FAILED blockout_shot(${SW}x${SH})"
            printf '%-20s %3s/%-3s  rc=%s  FAIL\n' "$NAME" "-" "-" "$RC"
            printf '%s\n' "$OUT" | tail -10 | sed 's/^/    /'
        fi
    done
else
    MISSING="$MISSING blockout_shot"
fi

# The Windows build's own last lines, copied into RUN.md.
#
# RUN.md quotes the end of ./build_win.sh - the line that says the editor
# linked - and it quoted it by hand. A hand copied log line is a log line that
# is one round out of date by the next review, and this one was worse than
# that: the document went out with the word WIN_TAIL_PLACEHOLDER where the
# output belongs, which is a promise nobody kept. So the block is GENERATED,
# out of build/last_win.log, between two markers in the file - and a RUN.md
# that still has a placeholder in it turns the run red, exactly like a failing
# check would.
WIN_LOG=build/last_win.log
if [ -f RUN.md ]; then
    if [ -f "$WIN_LOG" ]; then
        # The tail worth quoting: the section headers and the "ok:" lines the
        # script prints, up to and including the editor. Compiler warnings in
        # between are not what the document is claiming.
        WIN_TAIL=$(grep -E '^(-- |   ok: )' "$WIN_LOG" | tail -5)
        case "$WIN_TAIL" in
            *"ok: build-win/editor_demo.exe"*)
                if grep -q 'BEGIN win tail' RUN.md; then
                    awk -v tail="$WIN_TAIL" '
                        /BEGIN win tail/ { print; print "```"; print tail; print "```"; skip = 1; next }
                        /END win tail/   { skip = 0 }
                        !skip            { print }
                    ' RUN.md > build/RUN.md.win && mv build/RUN.md.win RUN.md
                    echo "-- RUN.md: the Windows tail block refreshed from $WIN_LOG"
                else
                    echo "-- RUN.md has no 'BEGIN win tail' marker - nothing refreshed"
                fi
                ;;
            *)
                echo "-- $WIN_LOG does not end in 'ok: build-win/editor_demo.exe' - RUN.md left alone"
                ;;
        esac
    else
        echo "-- no $WIN_LOG (run ./build_win.sh) - RUN.md's Windows tail is whatever it was"
    fi
    if grep -q 'WIN_TAIL_PLACEHOLDER' RUN.md; then
        FAILED="$FAILED RUN.md(win-tail-placeholder)"
        echo "-- RUN.md still contains WIN_TAIL_PLACEHOLDER"
    fi
    # And the number this script just made, next to the one the document
    # quotes. Said out loud rather than enforced: RUN.md is written after the
    # run it documents, so a run that is still going cannot be wrong about it -
    # but a difference nobody prints is a difference nobody notices.
    RUN_MD_TOTAL=$(grep -oE '^TOTAL [0-9]+ passed' RUN.md | tail -1 | grep -oE '[0-9]+')
    if [ -n "${RUN_MD_TOTAL:-}" ] && [ "$RUN_MD_TOTAL" != "$TOTAL_PASS" ]; then
        echo "-- RUN.md quotes TOTAL $RUN_MD_TOTAL passed, this run made $TOTAL_PASS"
    fi
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
