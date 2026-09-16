#!/usr/bin/env python3
"""Anker, measured - in the shipped runtime, headless.

    tools/innen_anker.py [--seed 7] [--steps 4000]

`projects/Untitled/assets/innen_anker.js` is GDD 5.3 and the inventory of
5.9: five real objects out of the player's car, two jacket pockets each, and
the one thing that makes them worth carrying - a room with one lying in it is
never rebuilt again.

That promise is the kind that is easy to write and easy to have quietly not be
true: the anchor list is one behaviour's idea, the rebuild dice are another's,
and the day the two stop agreeing nothing looks wrong. So it is measured, in
the binary that ships:

  1. a generated floor over the bridge (innen_lib.js + innen_gen.js), which
     now places the five objects and an `Anker` node,
  2. the `Anker` behaviour is given a written programme (`testPlan`) - take,
     drop, take too many - and the `Haus` behaviour is told to anchor one room
     for the first half of a few thousand simulated moves and release it for
     the second,
  3. `build/daidalos_runtime --headless`, the shipped binary,
  4. the "ANKER ..." and "HAUS ..." lines are read.

What it asserts:

  * **A room with an anchor in it is never rebuilt.** Not "rarely": the dice
    must not run at all. Counted over thousands of moves, it has to be 0.
  * **Picking the anchor up gives the room back to the house.** The same room,
    in the second half of the same walk, has to be rebuilt again - otherwise
    the test above would also pass if anchors simply froze everything forever.
  * **The inventory holds, 5.9.** Four slots, an anchor takes two: the third
    anchor is refused, and it is refused because the pockets are full and not
    because something went wrong.
  * **Dropping and taking are symmetric** - what is in a room is out of the
    jacket and the other way round, with nothing lost or duplicated.
  * **The five objects of 5.3 are really in the level**, by name.
  * **The house and the anchors agree.** The anchored room list the anchor
    behaviour publishes is the list the house's rules actually use - measured
    through the runtime's own log, not by reading both files and hoping.

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

ANKER_READY = re.compile(r"ANKER ready ([^\n]+)")
ANKER_TEST = re.compile(r"ANKER test ([^\n]+)")
ANKER_STEP = re.compile(r"ANKER step=(\d+) ([^\n]+)")
HAUS_PROBE = re.compile(r"HAUS anchor probe ([^\n]+)")
HAUS_TEST = re.compile(r"HAUS test (steps=[^\n]+)")
ANCHORED = re.compile(r"ANKER anchored=([0-9,]+)")

# The five of GDD 5.3, in the order the document lists them.
WANT_ITEMS = ["Anker.Warndreieck", "Anker.ErsteHilfe", "Anker.Eiskratzer",
              "Anker.Tankquittung", "Anker.Foto"]


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


def build_project(port, seed, rooms, steps, plan_text, probe_room, proj):
    """A floor whose Anker node runs `plan_text` and whose Haus anchors a room."""
    shutil.rmtree(proj, ignore_errors=True)
    os.makedirs(proj + "/scenes")
    shutil.copytree("projects/Untitled/assets", proj + "/assets")
    with open(proj + "/project.daidalos", "w") as f:
        f.write("daidalos-project 1\nname INNEN anker\nengine 0.2.0\n")
    with open(proj + "/boot.cfg", "w") as f:
        f.write("daidalos-boot 1\nscene scenes/innen_anker.daidalos\n"
                "title INNEN anker\nwidth 640\nheight 360\ntick_hz 60\n")

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

        # The anchors get their programme...
        switch = ("""
        (function () {
          var a = scene.find('Anker');
          var h = scene.find('Haus');
          if (a < 0 || h < 0) return JSON.stringify({ found: false });
          var ca = node.getStr(a, 'script');
          node.setStr(a, 'script',
             ca.replace('}', ',selfTest=true,testPlan=%s}'));
          // ...and the house is asked to prove the rule the anchors rely on,
          // over thousands of moves it would take a play session an hour to
          // walk: room %d is anchored for the first half and released for the
          // second.
          var ch = node.getStr(h, 'script');
          node.setStr(h, 'script',
             ch.replace('}', ',selfTest=true,testSteps=%d,testBounce=0.15,'
                             + 'testAnchorRoom=%d}'));
          return JSON.stringify({ found: true,
                                  anker: node.getStr(a, 'script'),
                                  haus: node.getStr(h, 'script') });
        })();
        """ % (plan_text, probe_room, steps, probe_room))
        sw = b.js(switch)
        if not sw.get("ok"):
            return None, "could not switch into self test: %r" % sw.get("error")
        info = json.loads(sw["result"])
        if not info.get("found"):
            return None, "the generated floor has no Anker or no Haus node"

        # What the level really contains, asked of the document itself.
        found = b.js("""
        (function () {
          var names = %s, out = [];
          for (var i = 0; i < names.length; i++)
            out.push([names[i], scene.find(names[i])]);
          return JSON.stringify(out);
        })();
        """ % json.dumps(WANT_ITEMS))
        present = json.loads(found["result"]) if found.get("ok") else []

        saved = b.js("editor.save('%s/scenes/innen_anker.daidalos')" % proj)
        if not saved.get("ok"):
            return None, "the scene could not be saved: %r" % saved.get("error")
        b.close()
        plan["_present"] = present
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
    ap.add_argument("--steps", type=int, default=4000)
    ap.add_argument("--room", type=int, default=5, help="the room to anchor")
    ap.add_argument("--port", type=int, default=8393)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args(argv[1:])

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    for needed in ("build/daidalos_runtime", "build/modeling_shot"):
        if not os.path.exists(needed):
            print("innen_anker: %s missing" % needed)
            return 2

    # take two anchors (4 of 4 pockets), try a third (refused), put one down in
    # room 3, take it back, and look at the state after every step.
    plan_text = ("take:0|check|take:1|check|take:2|check|drop:0@3|check"
                 "|take:2|check|take:0|check")

    proj = "/tmp/innen_anker"
    plan, err = build_project(args.port, args.seed, args.rooms, args.steps,
                              plan_text, args.room, proj)
    check(plan is not None, "the floor could not be built: %s" % err)
    if plan is None:
        return 1

    # ---- the five objects of 5.3 exist -----------------------------------
    present = dict(plan.get("_present", []))
    for name in WANT_ITEMS:
        check(present.get(name, -1) >= 0,
              "GDD 5.3 lists %s and the level does not contain it" % name)
    check(plan.get("anchors", 0) == 5,
          "the generator reported %s anchors placed, GDD 5.3 says 5"
          % plan.get("anchors"))

    out = run_headless(proj)
    ready = ANKER_READY.search(out)
    check(ready is not None,
          "the anchors never started in the shipped runtime.\n%s" % out[-900:])
    if not ready:
        return 1
    r = fields(ready.group(1))
    check(r.get("items", 0) == 5 and r.get("found", 0) == 5,
          "the behaviour knows %s anchors and found %s of them in the scene"
          % (r.get("items"), r.get("found")))
    check(r.get("slots", 0) == 4 and r.get("ankerSlots", 0) == 2,
          "GDD 5.9: 4 jacket slots and 2 per anchor; the level says %s and %s"
          % (r.get("slots"), r.get("ankerSlots")))

    # ---- the inventory, 5.9 ----------------------------------------------
    steps = {int(m.group(1)): fields(m.group(2)) for m in ANKER_STEP.finditer(out)}
    check(len(steps) >= 10, "only %d test steps were logged" % len(steps))

    line = ANKER_TEST.search(out)
    check(line is not None, "the anchor self test printed nothing")
    if line:
        f = fields(line.group(1))
        # Two anchors fill the jacket; the third has to be refused.
        check(f.get("refusedFull", 0) >= 1,
              "a third anchor was accepted into a 4 slot jacket - 5.9 says an "
              "anchor takes 2 slots, so two is the most anybody can carry")
        check(f.get("used", -1) <= 4,
              "the jacket ended up holding %s slots of 4" % f.get("used"))
        check(f.get("takes", 0) >= 3 and f.get("drops", 0) >= 1,
              "the programme took %s and dropped %s" % (f.get("takes"), f.get("drops")))

    # The step right after taking two: full, and the next take refused.
    full_steps = [s for s in steps.values() if s.get("used") == 4]
    check(full_steps, "the jacket was never full during the programme")
    refused = [l for l in out.splitlines() if "ANKER refused=full" in l]
    check(len(refused) >= 1,
          "nothing was ever refused for want of a pocket")

    # ---- the anchored room is never rebuilt ------------------------------
    probe = HAUS_PROBE.search(out)
    check(probe is not None,
          "the house never ran the anchor probe.\n%s" % out[-900:])
    if probe:
        p = fields(probe.group(1))
        check(p.get("anchoredRebuilds", -1) == 0,
              "room %s held an anchor and was rebuilt %s times - GDD 5.3 says "
              "a room with something real in it never changes again"
              % (args.room, p.get("anchoredRebuilds")))
        # ...and the other half: it is not simply frozen forever.
        check(p.get("releasedVisits", 0) > 5,
              "the walk only entered the released room %s times - too few to "
              "say whether picking the anchor up gave it back"
              % p.get("releasedVisits"))
        check(p.get("releasedRebuilds", 0) > 0,
              "room %s was visited %s times after the anchor was picked up and "
              "never changed - then the freeze is not the anchor's doing"
              % (args.room, p.get("releasedVisits")))

    haus = HAUS_TEST.search(out)
    check(haus is not None, "the house self test printed nothing")
    if haus:
        h = fields(haus.group(1))
        check(h.get("anchorViolations", -1) == 0,
              "the house rebuilt an anchored room %s times"
              % h.get("anchorViolations"))
        check(h.get("anchorFrozen", 0) > 0,
              "the anchor rule never fired in %s moves" % h.get("steps"))
        check(h.get("violations", -1) == 0,
              "a warm room was rebuilt %s times while anchors were in play - "
              "the older rule must not have been disturbed" % h.get("violations"))

    # ---- the list the anchors publish is the list the house uses ---------
    published = ANCHORED.findall(out)
    check(published, "the anchor behaviour never published its room list")
    heard = [l.split("anchors now ", 1)[1].strip()
             for l in out.splitlines() if "HAUS anchors now " in l]
    if published and heard:
        check(heard[-1] in published or published[-1] in heard[-1],
              "the anchors published %r and the house is using %r"
              % (published[-1], heard[-1]))

    print("seed %d: 5 anchors placed, jacket 4 slots / 2 per anchor, "
          "%s refused when full" % (args.seed, len(refused)))
    if probe:
        p = fields(probe.group(1))
        print("      room %s: 0 rebuilds while anchored, %s rebuilds in %s "
              "visits after it was picked up again"
              % (args.room, p.get("releasedRebuilds"), p.get("releasedVisits")))
    if not args.keep:
        shutil.rmtree(proj, ignore_errors=True)
    print("ok: %d checks, %d failures" % (PASS + FAIL, FAIL))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
