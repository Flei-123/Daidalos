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
test_euler
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
test_spawn
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

# INNEN M0 - the game's first three rooms, built through the bridge and then
# ASKED about: do the door sockets of two neighbours sit on the same point in
# the world, are the openings the same size, do the rooms stand next to each
# other instead of inside each other, is a doorway a CSG hole, and is the whole
# level one Ctrl-Z. That invariant is what the room generator will stand on,
# so it is a test and not a screenshot. Headless: it drives tools/modeling_shot
# in --serve mode, so it needs no screen.
if [ -x build/modeling_shot ] && command -v python3 >/dev/null 2>&1; then
    OUT=$(DAI_SHADER_DIR=shaders timeout 300 \
          python3 tools/innen_check.py --binary build/modeling_shot --port 8397 2>&1)
    RC=$?
    P=$(printf '%s\n' "$OUT" | grep -oE '[0-9]+ checks' | tail -1 | grep -oE '[0-9]+')
    F=$(printf '%s\n' "$OUT" | grep -oE '[0-9]+ failures' | tail -1 | grep -oE '[0-9]+')
    [ -z "${P:-}" ] && P=0
    [ -z "${F:-}" ] && F=0
    TOTAL_PASS=$((TOTAL_PASS + P - F))
    TOTAL_FAIL=$((TOTAL_FAIL + F))
    if [ "$RC" = "0" ] && [ "$F" = "0" ] && [ "$P" != "0" ]; then
        printf '%-20s %3s/%-3s  ok\n' "innen_check" "$P" "$F"
        [ "$VERBOSE" = "1" ] && printf '%s\n' "$OUT" | sed 's/^/    /'
    else
        FAILED="$FAILED innen_check"
        printf '%-20s %3s/%-3s  rc=%s  FAIL\n' "innen_check" "$P" "$F" "$RC"
        printf '%s\n' "$OUT" | tail -12 | sed 's/^/    /'
    fi
else
    MISSING="$MISSING innen_check"
fi

# INNEN - the ROOM GENERATOR. Grows a floor from the phone box over the bridge
# and then asks the document what stands there: same seed same floor, no room
# inside another, every door docking with opposite normals, every room
# reachable from the start, zones rising by one per door, and the whole floor
# one Ctrl-Z. This is the test that lets the game have more than three rooms.
if [ -x build/modeling_shot ] && command -v python3 >/dev/null 2>&1; then
    OUT=$(DAI_SHADER_DIR=shaders timeout 500 \
          python3 tools/innen_gen.py --binary build/modeling_shot --port 8398 2>&1)
    RC=$?
    P=$(printf '%s\n' "$OUT" | grep -oE '[0-9]+ checks' | tail -1 | grep -oE '[0-9]+')
    F=$(printf '%s\n' "$OUT" | grep -oE '[0-9]+ failures' | tail -1 | grep -oE '[0-9]+')
    [ -z "${P:-}" ] && P=0
    [ -z "${F:-}" ] && F=0
    TOTAL_PASS=$((TOTAL_PASS + P - F))
    TOTAL_FAIL=$((TOTAL_FAIL + F))
    if [ "$RC" = "0" ] && [ "$F" = "0" ] && [ "$P" != "0" ]; then
        printf '%-20s %3s/%-3s  ok\n' "innen_gen" "$P" "$F"
        [ "$VERBOSE" = "1" ] && printf '%s\n' "$OUT" | sed 's/^/    /'
    else
        FAILED="$FAILED innen_gen"
        printf '%-20s %3s/%-3s  rc=%s  FAIL\n' "innen_gen" "$P" "$F" "$RC"
        printf '%s\n' "$OUT" | tail -12 | sed 's/^/    /'
    fi
else
    MISSING="$MISSING innen_gen"
fi

# INNEN - das Gehaeuse: the rules of GDD 5.2 (warm rooms, the rebuild dice, the
# reaction to pingpong) measured in the SHIPPED runtime. The behaviour walks
# the generated floor's own graph a few thousand times at startup and prints
# what the rules did; the checker reads the distribution. Rules like these look
# right in a play session and are wrong by a third in the numbers.
if [ -x build/daidalos_runtime ] && [ -x build/modeling_shot ] && command -v python3 >/dev/null 2>&1; then
    OUT=$(DAI_SHADER_DIR=shaders timeout 700 \
          python3 tools/innen_haus.py --port 8395 2>&1)
    RC=$?
    P=$(printf '%s\n' "$OUT" | grep -oE '[0-9]+ checks' | tail -1 | grep -oE '[0-9]+')
    F=$(printf '%s\n' "$OUT" | grep -oE '[0-9]+ failures' | tail -1 | grep -oE '[0-9]+')
    [ -z "${P:-}" ] && P=0
    [ -z "${F:-}" ] && F=0
    TOTAL_PASS=$((TOTAL_PASS + P - F))
    TOTAL_FAIL=$((TOTAL_FAIL + F))
    if [ "$RC" = "0" ] && [ "$F" = "0" ] && [ "$P" != "0" ]; then
        printf '%-20s %3s/%-3s  ok\n' "innen_haus" "$P" "$F"
        [ "$VERBOSE" = "1" ] && printf '%s\n' "$OUT" | sed 's/^/    /'
    else
        FAILED="$FAILED innen_haus"
        printf '%-20s %3s/%-3s  rc=%s  FAIL\n' "innen_haus" "$P" "$F" "$RC"
        printf '%s\n' "$OUT" | tail -12 | sed 's/^/    /'
    fi
else
    MISSING="$MISSING innen_haus"
fi

# INNEN - Licht & Puls: GDD 5.4, measured in the SHIPPED runtime. The house
# breathes on a 4-7 minute cycle, goes dark for 40-90 s and flickers for 5 s
# first. Nobody can check three numbers like that by playing - it would take
# forty seven-minute cycles with a stopwatch - so the behaviour runs its own
# clock at speed and prints the min/max of every phase it completed.
if [ -x build/daidalos_runtime ] && [ -x build/modeling_shot ] && command -v python3 >/dev/null 2>&1; then
    OUT=$(DAI_SHADER_DIR=shaders timeout 700 \
          python3 tools/innen_puls.py --port 8391 2>&1)
    RC=$?
    P=$(printf '%s\n' "$OUT" | grep -oE '[0-9]+ checks' | tail -1 | grep -oE '[0-9]+')
    F=$(printf '%s\n' "$OUT" | grep -oE '[0-9]+ failures' | tail -1 | grep -oE '[0-9]+')
    [ -z "${P:-}" ] && P=0
    [ -z "${F:-}" ] && F=0
    TOTAL_PASS=$((TOTAL_PASS + P - F))
    TOTAL_FAIL=$((TOTAL_FAIL + F))
    if [ "$RC" = "0" ] && [ "$F" = "0" ] && [ "$P" != "0" ]; then
        printf '%-20s %3s/%-3s  ok\n' "innen_puls" "$P" "$F"
        [ "$VERBOSE" = "1" ] && printf '%s\n' "$OUT" | sed 's/^/    /'
    else
        FAILED="$FAILED innen_puls"
        printf '%-20s %3s/%-3s  rc=%s  FAIL\n' "innen_puls" "$P" "$F" "$RC"
        printf '%s\n' "$OUT" | tail -12 | sed 's/^/    /'
    fi
else
    MISSING="$MISSING innen_puls"
fi

# INNEN - Anker: GDD 5.3 and the inventory of 5.9. Five real objects, two
# jacket pockets each, and the promise the whole mechanic rests on - a room
# with one lying in it is never rebuilt again. The anchor list is one
# behaviour's idea and the rebuild dice are another's, so the day they stop
# agreeing nothing would look wrong. This walks a few thousand moves with a
# room anchored for the first half and released for the second.
if [ -x build/daidalos_runtime ] && [ -x build/modeling_shot ] && command -v python3 >/dev/null 2>&1; then
    OUT=$(DAI_SHADER_DIR=shaders timeout 700 \
          python3 tools/innen_anker.py --port 8393 2>&1)
    RC=$?
    P=$(printf '%s\n' "$OUT" | grep -oE '[0-9]+ checks' | tail -1 | grep -oE '[0-9]+')
    F=$(printf '%s\n' "$OUT" | grep -oE '[0-9]+ failures' | tail -1 | grep -oE '[0-9]+')
    [ -z "${P:-}" ] && P=0
    [ -z "${F:-}" ] && F=0
    TOTAL_PASS=$((TOTAL_PASS + P - F))
    TOTAL_FAIL=$((TOTAL_FAIL + F))
    if [ "$RC" = "0" ] && [ "$F" = "0" ] && [ "$P" != "0" ]; then
        printf '%-20s %3s/%-3s  ok\n' "innen_anker" "$P" "$F"
        [ "$VERBOSE" = "1" ] && printf '%s\n' "$OUT" | sed 's/^/    /'
    else
        FAILED="$FAILED innen_anker"
        printf '%-20s %3s/%-3s  rc=%s  FAIL\n' "innen_anker" "$P" "$F" "$RC"
        printf '%s\n' "$OUT" | tail -12 | sed 's/^/    /'
    fi
else
    MISSING="$MISSING innen_anker"
fi

# ...and that floor photographed: a plan view framed around whatever the seed
# built, plus two from eye height inside it.
if [ -x build/modeling_shot ] && command -v python3 >/dev/null 2>&1; then
    OUT=$(DAI_SHADER_DIR=shaders timeout 400 \
          python3 tools/innen_gen_shot.py --seed 7 --port 8399 --out "$SHOTS" 2>&1)
    RC=$?
    if [ "$RC" = "0" ]; then
        printf '%-20s %3s/%-3s  ok  (%s/2[7-9]-innen-gen-*.png)\n' \
               "innen_gen_shot" "-" "-" "$SHOTS"
        [ "$VERBOSE" = "1" ] && printf '%s\n' "$OUT" | sed 's/^/    /'
    else
        FAILED="$FAILED innen_gen_shot"
        printf '%-20s %3s/%-3s  rc=%s  FAIL\n' "innen_gen_shot" "-" "-" "$RC"
        printf '%s\n' "$OUT" | tail -10 | sed 's/^/    /'
    fi
else
    MISSING="$MISSING innen_gen_shot"
fi

# ...and the same three rooms photographed from eye height, in two sizes. The
# pictures come out of the bridge, from examples/scripts/innen_m0.js, so a
# level that changes changes them - which is the point of having them in git.
if [ -x build/modeling_shot ]; then
    for SET in 1600x900: 1100x700:narrow-; do
        DIM=${SET%%:*}; TAG=${SET#*:}
        SW=${DIM%%x*}; SH=${DIM##*x}
        OUT=$(timeout 600 ./tools/innen_shots.sh "$SHOTS" "$SW" "$SH" "$TAG" 2>&1)
        RC=$?
        if [ "$RC" = "0" ]; then
            printf '%-20s %3s/%-3s  ok  (%s/%s2[0-3]-innen-*.png %sx%s)\n' \
                   "innen_shots ${TAG:-plain}" "-" "-" "$SHOTS" "$TAG" "$SW" "$SH"
            [ "$VERBOSE" = "1" ] && printf '%s\n' "$OUT" | sed 's/^/    /'
        else
            FAILED="$FAILED innen_shots(${SW}x${SH})"
            printf '%-20s %3s/%-3s  rc=%s  FAIL\n' "innen_shots ${TAG:-plain}" "-" "-" "$RC"
            printf '%s\n' "$OUT" | tail -10 | sed 's/^/    /'
        fi
    done
else
    MISSING="$MISSING innen_shots"
fi

# The player, WALKED - in the shipped runtime, headless, with no window and no
# keyboard: the level is built over the bridge, the player is switched to
# autoWalk, and build/daidalos_runtime is asked to simulate it while a probe
# behaviour prints where the capsule is. It is the only test in this suite that
# proves BEHAVIOURS RUN IN THE EXPORT - which they did not until the runtime
# learned the object model, the component table and the play bindings.
#
# The runtime template is not built by build.sh (it links what build.sh made),
# so it is built here if it is missing.
if [ -x build/modeling_shot ] && command -v python3 >/dev/null 2>&1; then
    [ -x build/daidalos_runtime ] || ./tools/build_runtime.sh linux >/dev/null 2>&1
fi
if [ -x build/daidalos_runtime ] && [ -x build/modeling_shot ] && command -v python3 >/dev/null 2>&1; then
    OUT=$(DAI_SHADER_DIR=shaders timeout 600 \
          python3 tools/innen_walk.py --seconds 18 --port 8394 2>&1)
    RC=$?
    P=$(printf '%s\n' "$OUT" | grep -oE '[0-9]+ checks' | tail -1 | grep -oE '[0-9]+')
    F=$(printf '%s\n' "$OUT" | grep -oE '[0-9]+ failures' | tail -1 | grep -oE '[0-9]+')
    [ -z "${P:-}" ] && P=0
    [ -z "${F:-}" ] && F=0
    TOTAL_PASS=$((TOTAL_PASS + P - F))
    TOTAL_FAIL=$((TOTAL_FAIL + F))
    if [ "$RC" = "0" ] && [ "$F" = "0" ] && [ "$P" != "0" ]; then
        printf '%-20s %3s/%-3s  ok\n' "innen_walk" "$P" "$F"
        [ "$VERBOSE" = "1" ] && printf '%s\n' "$OUT" | sed 's/^/    /'
    else
        FAILED="$FAILED innen_walk"
        printf '%-20s %3s/%-3s  rc=%s  FAIL\n' "innen_walk" "$P" "$F" "$RC"
        printf '%s\n' "$OUT" | tail -12 | sed 's/^/    /'
    fi
else
    MISSING="$MISSING innen_walk"
fi

# The blockout round's pictures: the room built through dai_doc in C++, drawn
# by the real editor with the blockout host attached - the CSG wall with its
# door hole, the stairs, the arch, the socket gizmo, and the modifier stack's
# three (16..18: the same block without and with a bevel, a stair built by an
# array of one step, a mirrored bracket - inspector open on the stack). Two sizes, same as the
# drone show sets: `wide-` at 1920x1080 and `narrow-` at 1100x700, where a
# panel that guesses its layout collides with itself. Compiled by the same
# script that runs it, for the reason build_modeling_shot.sh gives.
if [ -f tools/blockout_shot.cpp ]; then
    # Three sizes, and only the first of them compiles the tool - the same
    # shape the modeling shots use, for the same reason. The plain 1600x900
    # names are in the set because they are in git: leaving them out meant the
    # repository kept a `16-modifier-bevel.png` from an older layout next to the
    # current `wide-` and `narrow-` ones, which is a stale picture that looks
    # exactly as current as the two beside it.
    for SET in 1600x900: 1920x1080:wide- 1100x700:narrow-; do
        DIM=${SET%%:*}; TAG=${SET#*:}
        SW=${DIM%%x*}; SH=${DIM##*x}
        if [ -z "$TAG" ]; then
            OUT=$(DISPLAY="$DAI_TEST_DISPLAY" timeout 600 \
                  ./tools/build_blockout_shot.sh "$SHOTS" "$SW" "$SH" 2>&1)
        elif [ -x build/blockout_shot ]; then
            OUT=$(DAI_SHADER_DIR=shaders DISPLAY="$DAI_TEST_DISPLAY" timeout 600 \
                  ./build/blockout_shot "$SHOTS" "$SW" "$SH" --prefix "$TAG" 2>&1)
        else
            OUT="blockout_shot was not compiled"; false
        fi
        RC=$?
        NAME="blockout_shot ${TAG:-plain}"
        if [ "$RC" = "0" ]; then
            printf '%-20s %3s/%-3s  ok  (%s/%s14..15-blockout-*, %s16..19-modifier-*.png %sx%s)\n' "$NAME" "-" "-" "$SHOTS" "$TAG" "$TAG" "$SW" "$SH"
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
# Every picture in the shots directory, hashed against every other one.
#
# Four of them - 04-storyboard, 05-parameters, 06-validation, 07-preview-full -
# went out byte identical in all three sizes: the three panels those names
# promise had moved into the editor's own dock and the shot tool was still
# calling a function that registers nothing, so it photographed the preview
# four times. Nothing noticed, because nothing looked. Two files with the same
# content are one picture with two captions, and a captioned duplicate is worse
# than a missing file: it reads as evidence. One md5 per file, and any pair
# that matches turns the run red with both names printed.
if [ -d "$SHOTS" ]; then
    DUPES=$(md5sum "$SHOTS"/*.png 2>/dev/null | sort | awk '
        { if ($1 == last_h) { if (!shown[$1]++) printf "   %s\n", last_f; printf "   %s\n", $2; n++ }
          last_h = $1; last_f = $2 }
        END { exit (n > 0 ? 1 : 0) }')
    if [ -n "$DUPES" ]; then
        FAILED="$FAILED shots(duplicate-pictures)"
        echo "-- two shots in $SHOTS are the SAME picture under different names:"
        printf '%s\n' "$DUPES"
    else
        SHOT_N=$(ls "$SHOTS"/*.png 2>/dev/null | wc -l)
        echo "-- $SHOT_N shots in $SHOTS, no two of them the same picture"
    fi
fi

# The SHIPPED binary, and whether it is still this checkout.
#
# innen_walk, innen_haus and the door test all measure build/daidalos_runtime -
# the exported game - on purpose: an editor that behaves and a game that does
# not is the bug those tests exist to catch. But ./build.sh does not build the
# runtime (tools/build_runtime.sh does), so the day somebody changes a host
# binding and runs only ./build.sh, those tests keep passing against
# YESTERDAY'S game and report numbers about code that no longer exists. That
# happened here with scene.spawn: the rule worked, the test said "the house
# never started", and the reason was a binary an hour old. Older than a source
# file it was built from is now RED, not a surprise at three in the morning.
RT_BIN=build/daidalos_runtime
if [ -f "$RT_BIN" ]; then
    RT_NEWER=$(find include src examples -type f \( -name '*.h' -o -name '*.inl' -o -name '*.cpp' -o -name '*.hpp' \) -newer "$RT_BIN" -print 2>/dev/null | head -3)
    if [ -n "$RT_NEWER" ]; then
        FAILED="$FAILED runtime(stale)"
        echo "-- $RT_BIN is older than sources it was built from:"
        echo "$RT_NEWER" | sed 's/^/     /'
        echo "   re-run tools/build_runtime.sh linux - the game tests measure this binary"
    fi
fi

WIN_LOG=build/last_win.log
WIN_EXE=build-win/editor_demo.exe
# ...and before any of it is quoted, whether the receipt is still true.
#
# A log is a claim about a binary that was built from sources. Two ways that
# claim goes stale, and both of them happened here: the log was written by
# hand from a piped run and then the exe was rebuilt without it (log older
# than the exe), or the exe is genuinely from this log but somebody has edited
# the engine since (log older than a source file). Either way RUN.md would go
# out quoting "ok: build-win/editor_demo.exe" about a build that no longer
# describes the checkout. Both turn the run RED rather than being explained in
# a paragraph nobody reads. ./build_win.sh writes the log itself now, so the
# fix for both is the same one command.
if [ -f "$WIN_EXE" ]; then
    if [ ! -f "$WIN_LOG" ]; then
        FAILED="$FAILED win-log(missing)"
        echo "-- $WIN_EXE exists but $WIN_LOG does not - run ./build_win.sh"
    elif [ "$WIN_EXE" -nt "$WIN_LOG" ]; then
        FAILED="$FAILED win-log(older-than-exe)"
        echo "-- $WIN_LOG is older than $WIN_EXE - the log is not this build's receipt"
    fi
fi
if [ -f "$WIN_LOG" ]; then
    WIN_NEWER=$(find include src -type f -newer "$WIN_LOG" -print 2>/dev/null | head -3)
    if [ -n "$WIN_NEWER" ]; then
        FAILED="$FAILED win-log(stale)"
        echo "-- $WIN_LOG is older than sources it claims to have built:"
        echo "$WIN_NEWER" | sed 's/^/     /'
        echo "   re-run ./build_win.sh"
    fi
fi
if [ -f RUN.md ]; then
    if [ -f "$WIN_LOG" ]; then
        # The tail worth quoting: the section headers and the "ok:" lines the
        # script prints, up to and including the editor. Compiler warnings in
        # between are not what the document is claiming.
        # The log's own timestamp goes into the block with the lines: a quoted
        # "ok:" with no date on it cannot be told from one a year old, and the
        # staleness check above is a line in a terminal nobody keeps.
        WIN_STAMP=$(date -u -r "$WIN_LOG" '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || echo unknown)
        WIN_TAIL=$(printf '%s\n%s' \
                   "$(grep -E '^(-- |   ok: )' "$WIN_LOG" | tail -5)" \
                   "(build/last_win.log written $WIN_STAMP by ./build_win.sh)")
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
    # Only INSIDE the generated block. The paragraph under it explains what
    # went wrong last round and has to be able to say the word out loud - a
    # guard that cannot tell a quoted mistake from the mistake itself turns
    # every honest post mortem red.
    if awk '/BEGIN win tail/ { inb = 1; next } /END win tail/ { inb = 0 } inb' RUN.md |
       grep -q 'WIN_TAIL_PLACEHOLDER'; then
        FAILED="$FAILED RUN.md(win-tail-placeholder)"
        echo "-- RUN.md's Windows tail block still contains WIN_TAIL_PLACEHOLDER"
    fi
    # And the number this script just made, next to the one the document
    # quotes. Said out loud rather than enforced: RUN.md is written after the
    # run it documents, so a run that is still going cannot be wrong about it -
    # but a difference nobody prints is a difference nobody notices.
    RUN_MD_TOTAL=$(grep -oE '^TOTAL [0-9]+ passed' RUN.md | tail -1 | grep -oE '[0-9]+')
    if [ -n "${RUN_MD_TOTAL:-}" ] && [ "$RUN_MD_TOTAL" != "$TOTAL_PASS" ]; then
        echo "-- RUN.md quotes TOTAL $RUN_MD_TOTAL passed, this run made $TOTAL_PASS"
    fi
    # ...and then it stops quoting and starts REPORTING. The Windows tail above
    # is generated for a reason that applies to the total word for word: a
    # number typed into a document by hand is a number that is one round out of
    # date by the next review, and this one was - the file said 2219 while the
    # suites made thousands more. Same two marker mechanism, same three lines
    # the script prints at the bottom of this run.
    if grep -q 'BEGIN totals' RUN.md; then
        TOTALS_BLOCK=$(printf 'TOTAL %d passed, %d failed\nnot run (needs GPU): %s\n%s' \
                       "$TOTAL_PASS" "$TOTAL_FAIL" "$GPU_ONLY" \
                       "$([ -n "$FAILED" ] && echo "RED:$FAILED" || echo "all green")")
        awk -v tot="$TOTALS_BLOCK" '
            /BEGIN totals/ { print; print "```"; print tot; print "```"; skip = 1; next }
            /END totals/   { skip = 0 }
            !skip          { print }
        ' RUN.md > build/RUN.md.tot && mv build/RUN.md.tot RUN.md
        echo "-- RUN.md: the totals block refreshed from this run"
    else
        echo "-- RUN.md has no 'BEGIN totals' marker - the total in it is whatever was typed"
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
