import io

p = 'tools/run_tests.sh'
s = io.open(p, encoding='utf-8').read()

old = """TOTAL_PASS=0
TOTAL_FAIL=0"""
new = """# test_image needs fixtures produced by a REAL encoder (Python zlib + PIL) -
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
        SUITES=$(printf '%s\\n' "$SUITES" | grep -v '^test_image$')
        SUITES_SKIPPED="test_image"
    fi
fi

TOTAL_PASS=0
TOTAL_FAIL=0"""
assert s.count(old) == 1, 'totals block not found'
s = s.replace(old, new)

# the suite takes the fixture directory as argv[1]
old = """    OUT=$(DAI_SHADER_DIR=shaders timeout 120 "$BIN" 2>&1)"""
new = """    ARGS=""
    [ "$s" = "test_image" ] && ARGS="$PNGFIX"
    OUT=$(DAI_SHADER_DIR=shaders timeout 120 "$BIN" $ARGS 2>&1)"""
assert s.count(old) == 1, 'suite invocation not found'
s = s.replace(old, new)

old = """[ -n "$MISSING" ] && echo "not built:$MISSING\""""
new = """[ -n "$MISSING" ] && echo "not built:$MISSING"
[ -n "$SUITES_SKIPPED" ] && echo "skipped:$SUITES_SKIPPED\""""
assert s.count(old) == 1, 'missing line not found'
s = s.replace(old, new)

io.open(p, 'w', encoding='utf-8').write(s)
print('run_tests.sh: png fixtures are generated, not assumed')
