# tools/parallel.sh - queue the compiler calls of a section, run them N at a
# time, and wait wherever an artefact is consumed.
#
# WHY THIS FILE EXISTS. build.sh calls the compiler 98 times, strictly one
# after the other. Measured on this machine (19.09.2026, 20 cores / 19 GB):
# a full sequential build is 2:35, one middling translation unit 2.38 s with
# -O3. That is not a nuisance, it is why the gauntlet round of 17./18.09.
# failed: three builder agents hit "maximum number of turns" waiting for it.
#
# WHY NOT NINJA. A generated build.ninja is the faster tool and it is a SECOND
# description of how this project is built, next to build.sh - and build.sh is
# not a list of commands, it is an argument: the leak test that compiles
# dai_engine.cpp without the Jolt include path, the rule that compile and run
# are two statements so a failed compile can never pass as a test, the `rm`
# before `ar rcs` that was added after eleven ghost objects were found in the
# archive. Each of those is a sentence in a comment above the line it protects.
# A generator would have to reproduce all of it, and would drift from it the
# first time someone edits one file and not the other. Same file, same order,
# same rules - only the compiler calls inside a section run together.
#
# WHY xargs AND NOT A BACKGROUND-JOB THROTTLE. The first version of this file
# started each call with `&` and throttled on `jobs -rp | wc -l`. Measured: the
# build got SLOWER, 7:08 against 2:35 sequential, with vmstat showing 40-44
# runnable processes against a limit of 9 and 109k context switches a second.
# The throttle does not hold, because one queued call is not one process: g++
# forks cc1plus, as and collect2, and a call with six .cpp files walks through
# them one after another - so "nine jobs" was thirty-odd runnable processes on
# twenty cores, and the machine spent its time switching rather than compiling.
# xargs -P enforces the limit on the thing that is actually being limited.
#
# THE JOB LIMIT IS RAM, NOT CORES. g++ -O3 on this codebase peaks around 1-2 GB
# per process with templates, so the default is min(nproc, RAM_GB / 2) - 9 here.
# DAI_JOBS=n overrides it.
#
# OUTPUT IS PER CALL. Nine compilers writing template errors into one terminal
# interleave into something nobody can read, so each call writes into its own
# file and it is printed in one piece when the call is done - and on failure
# with the command that produced it in front.

if [ -z "${DAI_JOBS:-}" ]; then
    _cores=$(nproc 2>/dev/null || echo 4)
    _ramgb=$(awk '/MemTotal/{printf "%d", $2/1048576}' /proc/meminfo 2>/dev/null || echo 4)
    _byram=$(( _ramgb / 2 ))
    [ "$_byram" -lt 1 ] && _byram=1
    if [ "$_cores" -lt "$_byram" ]; then DAI_JOBS=$_cores; else DAI_JOBS=$_byram; fi
fi

_J_DIR="$(mktemp -d "${TMPDIR:-/tmp}/dai-build.XXXXXX")"
_J_QUEUE="$_J_DIR/queue"
: > "$_J_QUEUE"

_j_cleanup() { rm -rf "$_J_DIR"; }
trap _j_cleanup EXIT

# The runner, as a file rather than an inline string: the commands carry quotes
# and $-signs of their own, and passing them through another layer of shell
# quoting is how a build script starts miscompiling one file in ten.
cat > "$_J_DIR/run-one" <<'RUNONE'
#!/usr/bin/env bash
log=$(mktemp "${TMPDIR:-/tmp}/dai-job.XXXXXX")
if eval "$1" > "$log" 2>&1; then
    [ -s "$log" ] && cat "$log"
    rm -f "$log"
    exit 0
fi
{
    echo "!! FAILED: $1"
    cat "$log"
    echo "!! (end of failed step)"
} >&2
rm -f "$log"
exit 1
RUNONE
chmod +x "$_J_DIR/run-one"

# J <command...> - queue it. Nothing runs until the next Jwait.
J() {
    local q="" a
    for a in "$@"; do
        q+=" $(printf '%q' "$a")"
    done
    printf '%s\n' "${q# }" >> "$_J_QUEUE"
}

# Jwait - run everything queued, N at a time, then stop the build if any of it
# failed. This is also the barrier: the next line may look at an .o, an .a or a
# binary only after this returns.
Jwait() {
    [ -s "$_J_QUEUE" ] || return 0
    local n; n=$(wc -l < "$_J_QUEUE")
    local rc=0
    xargs -a "$_J_QUEUE" -d '\n' -P "$DAI_JOBS" -I CMD "$_J_DIR/run-one" CMD || rc=$?
    : > "$_J_QUEUE"
    if [ "$rc" -ne 0 ]; then
        echo ""
        echo "!! build failed in a parallel section of $n step(s) (xargs rc=$rc)"
        exit 1
    fi
}
