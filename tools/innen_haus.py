#!/usr/bin/env python3
"""Das Gehaeuse, measured - in the shipped runtime, headless.

    tools/innen_haus.py [--seed 7] [--steps 4000]

`projects/Untitled/assets/innen_haus.js` is the rule set of GDD 5.2: warm
rooms, the rebuild dice, and the reaction to somebody walking through the same
door over and over. Rules like that are exactly the kind of thing that "looks
right" in a play session and is wrong by a third in the numbers, so they are
not judged by looking.

What it does:

  1. builds a generated floor over the bridge (innen_lib.js + innen_gen.js),
  2. switches the `Haus` node's behaviour into `selfTest`, which walks the
     room graph a few thousand times at startup - with a bias for turning
     round, so the pingpong rule is provoked - and runs the SAME enterRoom()
     the game runs,
  3. saves that into a throwaway project and runs `build/daidalos_runtime
     --headless` - the shipped binary, no window,
  4. reads the "HAUS ..." lines and checks the numbers.

What it asserts:

  * **A warm room is never rebuilt.** The self test knows before every move
    what the answer has to be and counts every time it is not. This is the
    promise the whole anti-pingpong rule rests on: the way back is the way
    back. It has to be exactly zero.
  * **The dice are the dice the GDD says**: 50 % identical, 30 % a detail,
    15 % the same kind of room again, 5 % something wrong - within the
    tolerance of a few thousand draws.
  * **The two rolls that BUILD a room really build it.** 15 % "the same kind
    of room again" and 5 % "something wrong" are done with scene.spawn() /
    scene.destroy() at play time - so every drawn one has to come back
    `built`, and the house must not grow or shrink while doing it: a spawn
    that forgets to destroy leaks a room per rebuild, and the instance count
    of the last frame is where that shows.
  * **The house reacts** to pingpong at all, and uses all three reactions.
  * **The same seed behaves the same way**, twice.

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

TEST_LINE = re.compile(r"HAUS test ([^\n]+)")
READY_LINE = re.compile(r"HAUS ready rooms=(\d+) doors=(\d+)")


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
                out[k] = int(v)
            except ValueError:
                out[k] = v
    return out


def build_project(port, seed, rooms, steps, bounce, proj):
    """A throwaway project with a generated floor whose Haus is in self test."""
    shutil.rmtree(proj, ignore_errors=True)
    os.makedirs(proj + "/scenes")
    shutil.copytree("projects/Untitled/assets", proj + "/assets")
    with open(proj + "/project.daidalos", "w") as f:
        f.write("daidalos-project 1\nname INNEN haus\nengine 0.2.0\n")
    with open(proj + "/boot.cfg", "w") as f:
        f.write("daidalos-boot 1\nscene scenes/innen_haus.daidalos\n"
                "title INNEN haus\nwidth 640\nheight 360\ntick_hz 60\n")

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

        switch = ("""
        (function () {
          var h = scene.find('Haus');
          if (h < 0) return JSON.stringify({ found: false });
          var cur = node.getStr(h, 'script');
          node.setStr(h, 'script',
             cur.replace('}', ',selfTest=true,testSteps=%d,testBounce=%.2f}'));
          return JSON.stringify({ found: true, script: node.getStr(h, 'script') });
        })();
        """ % (steps, bounce))
        sw = b.js(switch)
        if not sw.get("ok"):
            return None, "could not switch the house into self test: %r" % sw.get("error")
        info = json.loads(sw["result"])
        if not info.get("found"):
            return None, "the generated floor has no Haus node"
        saved = b.js("editor.save('%s/scenes/innen_haus.daidalos')" % proj)
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
                         capture_output=True, text=True, timeout=600)
    return run.stdout + run.stderr


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--rooms", type=int, default=14)
    ap.add_argument("--steps", type=int, default=4000)
    ap.add_argument("--bounce", type=float, default=0.55)
    ap.add_argument("--port", type=int, default=8395)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args(argv[1:])

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    for needed in ("build/daidalos_runtime", "build/modeling_shot"):
        if not os.path.exists(needed):
            print("innen_haus: %s missing" % needed)
            return 2

    proj = "/tmp/innen_haus"
    plan, err = build_project(args.port, args.seed, args.rooms,
                              args.steps, args.bounce, proj)
    check(plan is not None, "the floor could not be built: %s" % err)
    if plan is None:
        return 1

    out = run_headless(proj)
    ready = READY_LINE.search(out)
    check(ready is not None,
          "the house never started in the shipped runtime.\n%s" % out[-900:])
    if not ready:
        return 1
    check(int(ready.group(1)) == len(plan["rooms"]),
          "the house found %s rooms, the floor has %d"
          % (ready.group(1), len(plan["rooms"])))
    check(int(ready.group(2)) >= len(plan["edges"]) - 1,
          "the house found %s doors, the floor has %d"
          % (ready.group(2), len(plan["edges"])))

    line = TEST_LINE.search(out)
    check(line is not None, "the self test printed nothing.\n%s" % out[-900:])
    if not line:
        return 1
    f = fields(line.group(1))

    # ---- the promise the rest rests on -----------------------------------
    check(f.get("warmChecked", 0) > 200,
          "only %s of the moves went into a warm or anchored room - the walk "
          "never turned round, so the rule was never tested"
          % f.get("warmChecked"))
    check(f.get("violations", -1) == 0,
          "a warm room was rebuilt %s times - the way back is not the way back"
          % f.get("violations"))

    # ---- the dice ---------------------------------------------------------
    drawn = (f.get("identical", 0) + f.get("detail", 0) +
             f.get("wantReplace", 0) + f.get("wantWrong", 0))
    check(drawn > 300,
          "only %d rooms were cold enough to be rebuilt in %s moves - the "
          "distribution below would be noise" % (drawn, f.get("steps")))
    if drawn > 300:
        share = {
            "identical": f.get("identical", 0) / float(drawn),
            "detail": f.get("detail", 0) / float(drawn),
            "replace": f.get("wantReplace", 0) / float(drawn),
            "wrong": f.get("wantWrong", 0) / float(drawn),
        }
        want = {"identical": 0.50, "detail": 0.30, "replace": 0.15, "wrong": 0.05}
        for k in want:
            check(abs(share[k] - want[k]) < 0.05,
                  "%s came out at %.3f, the GDD says %.2f (%d draws)"
                  % (k, share[k], want[k], drawn))

    # ---- the two rolls that BUILD ----------------------------------------
    # Before include/dai_spawn_host.inl these were counted and logged and
    # nothing happened. "Drawn" and "built" have to be the same number now,
    # otherwise the rule is back to being 80 % of itself.
    check(f.get("builtReplace", 0) == f.get("wantReplace", 0),
          "%s rooms were drawn to be rebuilt and %s were really built"
          % (f.get("wantReplace"), f.get("builtReplace")))
    check(f.get("builtWrong", 0) == f.get("wantWrong", 0),
          "%s wrong rooms were drawn and %s were really built"
          % (f.get("wantWrong"), f.get("builtWrong")))
    check("spawn refused" not in out,
          "the spawn budget refused a room during a normal run")

    # A spawn without its destroy leaks one room per rebuild - about 70 nodes,
    # hundreds of times. The runtime prints how many instances it drew on the
    # first and the last frame, and that is the honest place to see it.
    counts = [int(m) for m in re.findall(r"frame \d+: tick \d+, (\d+) instances", out)]
    check(len(counts) >= 2, "the runtime did not report its instance count")
    if len(counts) >= 2:
        first, last = counts[0], counts[-1]
        check(abs(last - first) < max(40, first * 0.25),
              "the house drew %d instances on the first frame and %d on the "
              "last - %d rebuilds either leaked rooms or lost them"
              % (first, last, f.get("builtReplace", 0) + f.get("builtWrong", 0)))

    # ---- the house reacts -------------------------------------------------
    check(f.get("reactions", 0) > 5,
          "the house reacted %s times to somebody walking through the same "
          "door over and over" % f.get("reactions"))
    for kind in ("lock", "dark", "flicker"):
        check(f.get(kind, 0) > 0,
              "the '%s' reaction never happened in %s reactions"
              % (kind, f.get("reactions")))
    locks = [l for l in out.splitlines() if "reaction=lock" in l]
    check(len(locks) == f.get("lock", 0),
          "%d lock reactions were logged and %s counted"
          % (len(locks), f.get("lock")))

    # ---- and a walk that does NOT turn round rebuilds far more ------------
    # 88 % of the moves above went into a warm or a near room, which is what a
    # walker that keeps doubling back deserves. If the same number came out of
    # a walk that goes forwards, the rule would not be "warm rooms are frozen",
    # it would be "nothing ever changes".
    proj2 = "/tmp/innen_haus_fwd"
    plan2, err2 = build_project(args.port + 1, args.seed, args.rooms,
                                args.steps, 0.05, proj2)
    check(plan2 is not None, "the forward floor could not be built: %s" % err2)
    if plan2 is not None:
        fwd_line = TEST_LINE.search(run_headless(proj2))
        check(fwd_line is not None, "the forward self test printed nothing")
        if fwd_line:
            g = fields(fwd_line.group(1))
            frozen_share = g.get("frozen", 0) / float(max(1, g.get("enters", 1)))
            back_share = f.get("frozen", 0) / float(max(1, f.get("enters", 1)))
            check(frozen_share < back_share - 0.10,
                  "a walk that goes forwards froze %.2f of the rooms and one "
                  "that doubles back froze %.2f - the rule does not depend on "
                  "the route" % (frozen_share, back_share))
            check(g.get("violations", -1) == 0,
                  "the forward walk rebuilt a warm room %s times"
                  % g.get("violations"))
        if not args.keep:
            shutil.rmtree(proj2, ignore_errors=True)

    # ---- the same seed behaves the same way -------------------------------
    out2 = run_headless(proj)
    line2 = TEST_LINE.search(out2)
    check(line2 is not None and line2.group(1) == line.group(1),
          "two runs of the same house came out differently:\n  %s\n  %s"
          % (line.group(1), line2.group(1) if line2 else None))

    print("seed %d: %s moves, %s rebuilds drawn, %s frozen, %s reactions "
          "(lock %s / dark %s / flicker %s), %s violations"
          % (args.seed, f.get("steps"), drawn, f.get("frozen"),
             f.get("reactions"), f.get("lock"), f.get("dark"),
             f.get("flicker"), f.get("violations")))
    print("      built: %s same-kind rooms, %s wrong ones, at play time"
          % (f.get("builtReplace"), f.get("builtWrong")))
    if not args.keep:
        shutil.rmtree(proj, ignore_errors=True)
    print("ok: %d checks, %d failures" % (PASS + FAIL, FAIL))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
