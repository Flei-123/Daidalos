import io

p = 'tools/run_tests.sh'
s = io.open(p, encoding='utf-8').read()

old = """    # "N passed, M failed" is the shape every suite here prints.
    P=$(printf '%s\\n' "$OUT" | grep -oE '[0-9]+ passed' | tail -1 | grep -oE '[0-9]+')
    F=$(printf '%s\\n' "$OUT" | grep -oE '[0-9]+ failed' | tail -1 | grep -oE '[0-9]+')
    [ -z "${P:-}" ] && P=0
    [ -z "${F:-}" ] && F=0"""
new = """    # The suites do NOT all print the same summary. Four shapes exist:
    #
    #   "51 passed, 0 failed"            most of the newer C++ suites
    #   "51 bestanden, 0 fehlgeschlagen" the ones written in German
    #   "ok: 184 checks, 0 failures"     the project/fracture/update suites
    #   "ok: 32 key codes match ..."     test_keys, which counts nothing
    #
    # Reading only the first shape is how eight suites - among them
    # test_daidalos with its 51 cases - were reported as "0/0  ok" for months.
    # Zero passed and zero failed is not a pass, it is a suite nobody read.
    P=$(printf '%s\\n' "$OUT" | grep -oE '[0-9]+ (passed|bestanden|checks)' | tail -1 | grep -oE '[0-9]+')
    F=$(printf '%s\\n' "$OUT" | grep -oE '[0-9]+ (failed|fehlgeschlagen|failures|wrong)' | tail -1 | grep -oE '[0-9]+')
    [ -z "${P:-}" ] && P=0
    [ -z "${F:-}" ] && F=0
    # A suite that ran, exited 0 and reported no counts at all is a suite whose
    # summary line changed shape - flag it instead of scoring it as fine.
    if [ "$P" = "0" ] && [ "$F" = "0" ] && [ "$RC" = "0" ]; then
        NOCOUNT="$NOCOUNT $s"
    fi"""
assert s.count(old) == 1, 'count parser not found'
s = s.replace(old, new)

old = """TOTAL_PASS=0
TOTAL_FAIL=0
MISSING=""
FAILED="\""""
new = """TOTAL_PASS=0
TOTAL_FAIL=0
MISSING=""
FAILED=""
NOCOUNT="\""""
assert s.count(old) == 1, 'counter init not found'
s = s.replace(old, new)

old = """[ -n "$SUITES_SKIPPED" ] && echo "skipped:$SUITES_SKIPPED\""""
new = """[ -n "$SUITES_SKIPPED" ] && echo "skipped:$SUITES_SKIPPED"
[ -n "$NOCOUNT" ] && echo "no counts parsed (summary line changed shape?):$NOCOUNT\""""
assert s.count(old) == 1, 'skipped line not found'
s = s.replace(old, new)

io.open(p, 'w', encoding='utf-8').write(s)
print('run_tests.sh: reads all four summary shapes, and says when it reads none')
