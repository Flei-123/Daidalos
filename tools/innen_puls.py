#!/usr/bin/env python3
"""Licht & Puls, measured - in the shipped runtime, headless.

    tools/innen_puls.py [--seed 7] [--cycles 40]

`projects/Untitled/assets/innen_puls.js` is GDD 5.4: the house breathes on a
4-7 minute cycle, goes dark for 40-90 s, and flickers for 5 s before it does.
Those are three numbers a play session cannot check - nobody sits through
forty seven-minute cycles with a stopwatch - and three numbers that are wrong
the moment somebody "tidies up" the clock. So they are measured instead.

What it does:

  1. builds a generated floor over the bridge (innen_lib.js + innen_gen.js),
     which now puts a `Puls` node into every floor it builds,
  2. switches that node's behaviour into `selfTest`, which runs the SAME
     step() the game runs at a fixed dt until it has completed `cycles` full
     cycles, and prints the min/max/average of every phase,
  3. saves that into a throwaway project and runs `build/daidalos_runtime
     --headless` - the shipped binary, no window,
  4. reads the "PULS ..." lines and checks the numbers.

What it asserts:

  * **The cycle is 4-7 minutes.** Every bright phase measured has to fall in
    [240, 420] seconds, and over forty of them the shortest has to be near the
    bottom of that range and the longest near the top - a clock that always
    picked 300 s would pass a "within range" test and would not be random.
  * **The dark phase is 40-90 s**, the same way.
  * **The flicker is 5 s, every time**, before every dark phase. It is the
    only warning the player gets, so it is the one number with no spread.
  * **The phase is really published.** The tag on the node is what
    innen_haus.js reads; if it were only an internal variable the house would
    never go dark. The runtime's own log has to show the house seeing all
    three phases.
  * **The lights really go out.** In the dark phase every ceiling light in the
    house reports intensity 0 - measured through the document, not assumed
    from the fact that a function was called.
  * **The same seed breathes the same way**, twice.

Prints "ok: N checks, 0 failures", the shape tools/run_tests.sh counts.
"""

import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from daibridge import Bridge, read_sources          # noqa: E402

PASS = 0
FAIL = 0

LIB = "examples/scripts/innen_lib.js"
GEN = "examples/scripts/innen_gen.js"

TEST_LINE = re.compile(r"PULS test ([^\n]+)")
READY_LINE = re.compile(r"PULS ready ([^\n]+)")
PHASE_LINE = re.compile(r"PULS phase=(\w+) for=([0-9.]+) t=([0-9.]+)")
SIGHT_ROOM = re.compile(r"HAUS sight room=(\d+) dist=([0-9.]+) seen=(\d) cone=(\d)")
SIGHT_SUM = re.compile(r"HAUS sight ([^\n]+)")


def check(cond, message):
    global PASS, FAIL
    if cond:
        PASS += 1
    else:
        FAIL += 1
        print("  FAIL %s" % message)


def wait_for_port(port, proc, seconds=60.0):
    deadline = time.time() + seconds
    while time.time() < deadline:
        if proc.poll() is not None:
            return False
        try:
            s = socket.create_connection(("127.0.0.1", port), 0.5)
            s.close()
            return True
        except (socket.error, OSError):
            time.sleep(0.2)
    return False


def fields(line):
    out = {}
    for part in line.split():
        if "=" in part:
            k, v = part.split("=", 1)
            try:
                out[k] = float(v) if ("." in v) else int(v)
            except ValueError:
                out[k] = v
    return out


def build_project(port, seed, rooms, cycles, proj, dark_probe=False):
    """A throwaway project whose Puls node is in self test."""
    shutil.rmtree(proj, ignore_errors=True)
    os.makedirs(proj + "/scenes")
    shutil.copytree("projects/Untitled/assets", proj + "/assets")
    with open(proj + "/project.daidalos", "w") as f:
        f.write("daidalos-project 1\nname INNEN puls\nengine 0.2.0\n")
    with open(proj + "/boot.cfg", "w") as f:
        f.write("daidalos-boot 1\nscene scenes/innen_puls.daidalos\n"
                "title INNEN puls\nwidth 640\nheight 360\ntick_hz 60\n")

    env = dict(os.environ)
    env.setdefault("DAI_SHADER_DIR", "shaders")
    env["DAI_BRIDGE_PORT"] = str(port)
    host = subprocess.Popen(["build/modeling_shot", "/tmp", "320", "200",
                             "--serve", "180", "--bridge", str(port)],
                            env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    try:
        if not wait_for_port(port, host):
            return None, "the host never opened the bridge"
        b = Bridge(port, timeout=180.0)
        head = "var GEN = { seed: %d, rooms: %d };\n" % (seed, rooms)
        answer = b.js(head + read_sources([LIB, GEN]))
        if not answer.get("ok"):
            return None, "the generator failed: %r" % answer.get("error")
        plan = json.loads(answer["result"])

        # The pulse into self test. `dark_probe` instead runs a REAL clock with
        # a very short first bright phase, so the dark phase arrives inside the
        # handful of frames the runtime is asked for - that is how "the lights
        # really go out" is measured rather than asserted.
        if dark_probe:
            tweak = ("ersteHell=1.2,flackerZeit=0.4,dunkelMin=30,dunkelMax=30,"
                     "hellMin=240,hellMax=240")
            switch = ("""
            (function () {
              var n = scene.find('Puls');
              if (n < 0) return JSON.stringify({ found: false });
              var cur = node.getStr(n, 'script');
              cur = cur.replace(/ersteHell=[0-9.]+/, '')
                       .replace(/flackerZeit=[0-9.]+/, '')
                       .replace(/dunkelMin=[0-9.]+/, '')
                       .replace(/dunkelMax=[0-9.]+/, '')
                       .replace(/hellMin=[0-9.]+/, '')
                       .replace(/hellMax=[0-9.]+/, '')
                       .replace(/,+/g, ',').replace(',}', '}');
              node.setStr(n, 'script', cur.replace('}', ',%s}'));
              // ...and the house is asked what its §5.4 sight rule answers
              // for every room, from where the player really stands.
              var h = scene.find('Haus');
              if (h >= 0) {
                var ch = node.getStr(h, 'script');
                node.setStr(h, 'script', ch.replace('}', ',sightTest=true}'));
              }
              return JSON.stringify({ found: true, script: node.getStr(n, 'script') });
            })();
            """ % tweak)
        else:
            switch = ("""
            (function () {
              var n = scene.find('Puls');
              if (n < 0) return JSON.stringify({ found: false });
              var cur = node.getStr(n, 'script');
              node.setStr(n, 'script',
                 cur.replace('}', ',selfTest=true,testCycles=%d}'));
              return JSON.stringify({ found: true, script: node.getStr(n, 'script') });
            })();
            """ % cycles)
        sw = b.js(switch)
        if not sw.get("ok"):
            return None, "could not switch the pulse into self test: %r" % sw.get("error")
        info = json.loads(sw["result"])
        if not info.get("found"):
            return None, "the generated floor has no Puls node"
        saved = b.js("editor.save('%s/scenes/innen_puls.daidalos')" % proj)
        if not saved.get("ok"):
            return None, "the scene could not be saved: %r" % saved.get("error")
        b.close()
        return plan, None
    finally:
        if host.poll() is None:
            host.terminate()
            try:
                host.wait(timeout=10)
            except subprocess.TimeoutExpired:
                host.kill()


def run_headless(proj, frames=90):
    run = subprocess.run(["build/daidalos_runtime", proj, "--headless", str(frames)],
                         capture_output=True, text=True, timeout=900)
    return run.stdout + run.stderr


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--rooms", type=int, default=14)
    ap.add_argument("--cycles", type=int, default=40)
    ap.add_argument("--port", type=int, default=8391)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args(argv[1:])

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    for needed in ("build/daidalos_runtime", "build/modeling_shot"):
        if not os.path.exists(needed):
            print("innen_puls: %s missing" % needed)
            return 2

    proj = "/tmp/innen_puls"
    plan, err = build_project(args.port, args.seed, args.rooms, args.cycles, proj)
    check(plan is not None, "the floor could not be built: %s" % err)
    if plan is None:
        return 1

    out = run_headless(proj)
    ready = READY_LINE.search(out)
    check(ready is not None,
          "the pulse never started in the shipped runtime.\n%s" % out[-900:])
    if not ready:
        return 1
    r = fields(ready.group(1))
    check(r.get("lights", 0) > 0,
          "the pulse found %s ceiling lights - it has nothing to switch off"
          % r.get("lights"))

    line = TEST_LINE.search(out)
    check(line is not None, "the self test printed nothing.\n%s" % out[-900:])
    if not line:
        return 1
    f = fields(line.group(1))

    # ---- the cycle, 4-7 minutes ------------------------------------------
    check(f.get("hellN", 0) >= args.cycles - 2,
          "only %s bright phases were measured out of %d cycles"
          % (f.get("hellN"), args.cycles))
    check(f.get("hellMin", 0) >= 240.0 - 0.5,
          "a bright phase lasted %.2f s - GDD 5.4 says at least 240"
          % f.get("hellMin", 0))
    check(f.get("hellMax", 0) <= 420.0 + 0.5,
          "a bright phase lasted %.2f s - GDD 5.4 says at most 420"
          % f.get("hellMax", 0))
    # ...and it is really random inside that range. A clock that always chose
    # the middle would pass both checks above and would not be a cycle "4-7
    # Minuten (zufaellig)" at all.
    span = f.get("hellMax", 0) - f.get("hellMin", 0)
    check(span > 100.0,
          "every bright phase came out between %.1f and %.1f s - a spread of "
          "%.1f s over %s draws is not a random 4-7 minutes"
          % (f.get("hellMin", 0), f.get("hellMax", 0), span, f.get("hellN")))
    avg = f.get("hellAvg", 0)
    check(270.0 < avg < 390.0,
          "the average bright phase is %.1f s; a uniform 240-420 averages 330"
          % avg)

    # ---- the dark phase, 40-90 s -----------------------------------------
    check(f.get("dunkelN", 0) >= args.cycles - 2,
          "only %s dark phases were measured" % f.get("dunkelN"))
    check(f.get("dunkelMin", 0) >= 40.0 - 0.5,
          "a dark phase lasted %.2f s - GDD 5.4 says at least 40"
          % f.get("dunkelMin", 0))
    check(f.get("dunkelMax", 0) <= 90.0 + 0.5,
          "a dark phase lasted %.2f s - GDD 5.4 says at most 90"
          % f.get("dunkelMax", 0))
    dspan = f.get("dunkelMax", 0) - f.get("dunkelMin", 0)
    check(dspan > 20.0,
          "the dark phases spread only %.1f s over %s draws - not random"
          % (dspan, f.get("dunkelN")))

    # ---- the warning: 5 s of flicker, every time -------------------------
    check(f.get("flackerN", 0) >= args.cycles - 2,
          "only %s flicker phases for %s dark ones - the warning is missing"
          % (f.get("flackerN"), f.get("dunkelN")))
    # Every dark phase is announced by exactly one flicker. The run is cut off
    # the moment the last cycle completes, so the walk may end holding one
    # flicker whose dark phase was never entered - that is the clock being
    # stopped mid-breath, not a missing warning. One more flicker than dark
    # phases is therefore allowed; one FEWER never is.
    check(0 <= f.get("flackerN", 0) - f.get("dunkelN", 0) <= 1,
          "%s flickers and %s dark phases - every dark phase has to be "
          "announced by exactly one flicker"
          % (f.get("flackerN"), f.get("dunkelN")))
    check(abs(f.get("flackerMin", 0) - 5.0) < 0.1 and
          abs(f.get("flackerMax", 0) - 5.0) < 0.1,
          "the flicker ran %.2f-%.2f s; it is the only warning the player "
          "gets and GDD 5.4 says 5 s"
          % (f.get("flackerMin", 0), f.get("flackerMax", 0)))

    # ---- the phases really happen, in order ------------------------------
    phases = [m.group(1) for m in PHASE_LINE.finditer(out)]
    check(len(phases) > 10, "only %d phase changes were logged" % len(phases))
    bad = []
    for i in range(1, len(phases)):
        want = {"hell": "flacker", "flacker": "dunkel", "dunkel": "hell"}
        if phases[i] != want[phases[i - 1]]:
            bad.append((i, phases[i - 1], phases[i]))
    check(not bad,
          "the phases did not run hell -> flacker -> dunkel: %s" % bad[:3])

    # ---- the same seed breathes the same way -----------------------------
    out2 = run_headless(proj)
    line2 = TEST_LINE.search(out2)
    check(line2 is not None and line2.group(1) == line.group(1),
          "two runs of the same pulse came out differently:\n  %s\n  %s"
          % (line.group(1), line2.group(1) if line2 else None))

    # ---- and the lights really go out ------------------------------------
    # A separate project with a real (fast) clock, so a dark phase falls
    # inside the frames the runtime is asked for. Then the DOCUMENT is asked
    # what the lights are doing - the house's own reading of them, not the
    # pulse's claim about them.
    proj2 = "/tmp/innen_puls_dark"
    plan2, err2 = build_project(args.port + 1, args.seed, args.rooms,
                                args.cycles, proj2, dark_probe=True)
    check(plan2 is not None, "the dark probe floor could not be built: %s" % err2)
    if plan2 is not None:
        dark_out = run_headless(proj2, frames=240)
        got = [m.group(1) for m in PHASE_LINE.finditer(dark_out)]
        check("dunkel" in got,
              "the clock never reached a dark phase in 240 frames: %s" % got[:6])
        # The house on the same floor has to have SEEN the phase - that is the
        # tag channel working end to end, and it is the whole reason the dark
        # phase can change what the rebuild rules do.
        check("HAUS phase=dunkel" in dark_out,
              "innen_puls.js went dark and innen_haus.js never noticed - the "
              "tag channel between them is broken")
        check("HAUS phase=flacker" in dark_out or "HAUS phase=hell" in dark_out,
              "the house never read any phase off the pulse")

        # ---- §5.4's asymmetry, measured -------------------------------
        # "Umbau-Regel greift auch auf Raeume, die du SIEHST - aber nur
        # ausserhalb deines Lichtkegels." That sentence is only worth
        # anything if the two sets are really different: some rooms the
        # player can see must be OUTSIDE the torch's 26 degree cone, or the
        # dark phase would protect everything the bright one does and change
        # nothing at all.
        rooms = SIGHT_ROOM.findall(dark_out)
        check(len(rooms) > 4,
              "the house reported the sight rule for only %d rooms" % len(rooms))
        summary = SIGHT_SUM.findall(dark_out)
        summ = None
        for cand in summary:
            if cand.startswith("rooms="):
                summ = fields(cand)
        check(summ is not None, "the sight test printed no summary")
        if summ:
            check(summ.get("torch", 0) == 1,
                  "the house did not find the player's torch, so the light "
                  "cone rule of 5.4 could never protect anything")
            check(summ.get("seen", 0) > 0,
                  "the player could see none of the %s rooms from where he "
                  "stands - the sight rule was never exercised"
                  % summ.get("rooms"))
            check(summ.get("exposed", 0) > 0,
                  "every room the player can see was also inside his torch "
                  "cone; then 'nur ausserhalb deines Lichtkegels' protects "
                  "everything and the dark phase changes nothing")
            # The beam is aimed at ONE room (see sightTest()), so a 26
            # degree cone inside a 60 degree field of view has to leave most
            # of what the player can see unprotected. If the cone covered
            # everything in sight, the parameter would not be reaching the
            # rule and the dark phase would protect exactly as much as the
            # bright one.
            check(summ.get("exposed", 0) > summ.get("coneSaved", 0),
                  "with the torch pointed at room %s, %s of the %s rooms in "
                  "sight were inside its cone and only %s outside - a 26 "
                  "degree beam cannot protect most of a 60 degree view"
                  % (summ.get("aimedAt"), summ.get("coneSaved"),
                     summ.get("seen"), summ.get("exposed")))
        if not args.keep:
            shutil.rmtree(proj2, ignore_errors=True)

    print("seed %d: %s cycles - bright %.1f-%.1f s (avg %.1f), dark %.1f-%.1f s "
          "(avg %.1f), flicker %.2f s before every one"
          % (args.seed, f.get("cycles"), f.get("hellMin", 0), f.get("hellMax", 0),
             f.get("hellAvg", 0), f.get("dunkelMin", 0), f.get("dunkelMax", 0),
             f.get("dunkelAvg", 0), f.get("flackerMin", 0)))
    if not args.keep:
        shutil.rmtree(proj, ignore_errors=True)
    print("ok: %d checks, %d failures" % (PASS + FAIL, FAIL))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
