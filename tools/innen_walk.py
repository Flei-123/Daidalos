#!/usr/bin/env python3
"""The INNEN player, WALKED - in the shipped runtime, with no window.

    tools/innen_walk.py [--seconds 26] [--keep]

What it does, in order:

  1. builds the level over the bridge (`examples/scripts/innen_m0.js`), the
     same way `tools/innen_shots.sh` does,
  2. switches the player's behaviour into `autoWalk` and stacks
     `walk_probe.js` behind it, so the capsule walks forward by itself and
     prints where it is,
  3. saves that scene into a throwaway project in /tmp together with the two
     behaviours, the materials and the baked textures,
  4. runs `build/daidalos_runtime <project> --headless N` - the SHIPPED game
     binary, not the editor - and reads the PROBE lines it prints.

Why the runtime and not the editor: the editor's Play button needs a window,
a mouse and somebody to press it. The runtime runs a project head down with no
display at all, which is the only way "the player can walk out of the phone box"
becomes a number in a test suite instead of a claim in a commit message.

What it asserts:

  * the player FALLS ONTO THE FLOOR and stays there - y never leaves a hand's
    width around the spawn height, so the capsule is neither sinking through
    the floor nor standing on air,
  * it LEAVES THE BOX through the door: z goes past the doorway, which is
    1.30 m of room and a 0.95 m opening - a capsule that clips the frame stops
    dead and never gets there,
  * it stays INSIDE the hallway walls: |x| under half the hallway's width the
    whole way,
  * it KEEPS WALKING: the last second still shows the walk speed, so it did
    not end up wedged in a corner with the motor running,
  * and it never leaves the level - z stops inside the hallway, because the
    hallway has a far wall and a wall is a wall.

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
from daibridge import Bridge, read_sources   # noqa: E402

PASS = 0
FAIL = 0


def check(cond, message):
    global PASS, FAIL
    if cond:
        PASS += 1
    else:
        FAIL += 1
        print("  FAIL %s" % message)


# The script field the player gets for the walk: the same behaviour, with
# autoWalk on, and the probe stacked behind it with ';' the way the document
# stores several behaviours.
DOOR_OPEN_AT = 7.0          # seconds; the hall door unlocks itself then

WALK_JS = """
(function () {
  var p = scene.find('Spieler');
  if (!p) return JSON.stringify({ found: false });
  node.setStr(p, 'script',
     'innen_player.js{eye=Spieler.Kamera,torch=Spieler.Lampe,walkSpeed=2.2,' +
     'eyeHeight=1.62,torchOn=true,autoWalk=true};walk_probe.js{every=0.25}');
  // The hall door opens by itself at DOOR_OPEN_AT. Before that it is a shut
  // door in a corridor, and the walk has to stop at it - that is the check.
  var d = scene.find('Tuer.Halle');
  if (d >= 0) {
    var cur = node.getStr(d, 'script');
    node.setStr(d, 'script', cur.replace('}', ',autoOpen=%.2f}'));
  }
  var q = node.getPos(p);
  return JSON.stringify({ found: true, door: d, start: [q[0], q[1], q[2]],
                          script: node.getStr(p, 'script') });
})();
"""

# The same level, with the player put at the foot of the staircase in the hall
# and walked straight at it. A picture of stairs and stairs you can climb are
# two different things, and the only difference that shows up in a number is
# whether y goes up.
STAIRS_JS = """
(function () {
  var p = scene.find('Spieler');
  if (!p) return JSON.stringify({ found: false });
  node.setPos(p, %.3f, 1.05, %.3f);
  node.setStr(p, 'script',
     'innen_player.js{eye=Spieler.Kamera,torch=Spieler.Lampe,walkSpeed=2.2,' +
     'torchOn=true,autoWalk=true,startYaw=0};walk_probe.js{every=0.25}');
  var q = node.getPos(p);
  return JSON.stringify({ found: true, start: [q[0], q[1], q[2]] });
})();
"""

PROBE = re.compile(r"PROBE t=([-\d.]+) p=([-\d.]+),([-\d.]+),([-\d.]+) "
                   r"v=([-\d.]+),([-\d.]+),([-\d.]+) g=(\d)")


# One headless run of the SHIPPED binary, as a list of measurements. Two runs
# in one test needed this to stop being inline code: the walk and the climb are
# the same procedure with a different scene.
def sample_run(project, seconds, timeout=600):
    frames = int(seconds * 60)
    run = subprocess.run(["build/daidalos_runtime", project, "--headless", str(frames)],
                         capture_output=True, text=True, timeout=timeout)
    out = run.stdout + run.stderr
    samples = []
    for m in PROBE.finditer(out):
        samples.append({
            "t": float(m.group(1)),
            "p": [float(m.group(2)), float(m.group(3)), float(m.group(4))],
            "v": [float(m.group(5)), float(m.group(6)), float(m.group(7))],
            "g": int(m.group(8)),
        })
    return samples, out


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


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=26.0)
    ap.add_argument("--stairs-seconds", type=float, default=12.0,
                    help="length of the second run, the one up the staircase")
    ap.add_argument("--port", type=int, default=8394)
    ap.add_argument("--keep", action="store_true",
                    help="leave the throwaway project in /tmp for a look")
    args = ap.parse_args(argv[1:])

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)

    if not os.path.exists("build/daidalos_runtime"):
        print("innen_walk: build/daidalos_runtime missing - "
              "run ./build.sh then tools/build_runtime.sh linux")
        return 2
    if not os.path.exists("build/modeling_shot"):
        print("innen_walk: build/modeling_shot missing - run tools/build_modeling_shot.sh")
        return 2

    proj = "/tmp/innen_walk"
    stairs_proj = "/tmp/innen_stairs"
    for d, scene_name in ((proj, "innen_walk"), (stairs_proj, "innen_stairs")):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d + "/scenes")
        shutil.copytree("projects/Untitled/assets", d + "/assets")
        with open(d + "/project.daidalos", "w") as f:
            f.write("daidalos-project 1\nname INNEN walk\nengine 0.2.0\n")
        with open(d + "/boot.cfg", "w") as f:
            # 60 Hz, Talos, and the level as the start scene. No window is
            # opened by --headless, so width and height only matter to the log.
            f.write("daidalos-boot 1\nscene scenes/%s.daidalos\n"
                    "title INNEN walk\nwidth 640\nheight 360\ntick_hz 60\n"
                    % scene_name)

    env = dict(os.environ)
    env.setdefault("DAI_SHADER_DIR", "shaders")
    env["DAI_BRIDGE_PORT"] = str(args.port)
    host = subprocess.Popen(["build/modeling_shot", "/tmp", "320", "200",
                             "--serve", "120", "--bridge", str(args.port)],
                            env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    start = None
    try:
        if not wait_for_port(args.port, host):
            print("innen_walk: the host never opened the bridge")
            return 2
        b = Bridge(args.port)
        built = b.js(read_sources(["examples/scripts/innen_lib.js",
                                   "examples/scripts/innen_m0.js"]))
        check(built.get("ok") is True, "the level script failed: %r" % built.get("error"))
        if not built.get("ok"):
            return 1
        info = json.loads(built["result"])
        check(info.get("script") is True,
              "this build cannot put a behaviour on a node, so there is no player")
        check(info.get("collider") is True,
              "this build has no collider.shape, so the player is a box, not a capsule")

        check(info.get("doors", 0) >= 5,
              "the level has %r doors - the leaves were not placed"
              % info.get("doors"))
        answer = b.js(WALK_JS % DOOR_OPEN_AT)
        check(answer.get("ok") is True, "could not switch the player to autoWalk: %r"
              % answer.get("error"))
        walk = json.loads(answer["result"]) if answer.get("ok") else {}
        check(walk.get("found") is True, "there is no node called Spieler in the level")
        start = walk.get("start")
        check(walk.get("door", -1) >= 0,
              "there is no node called Tuer.Halle - the hall door is missing")
        saved = b.js("editor.save('%s/scenes/innen_walk.daidalos')" % proj)
        check(saved.get("ok") is True, "the scene could not be saved: %r" % saved.get("error"))

        # ...and the same level again, with the player at the foot of the
        # stairs. Saved from the same document, so it is the same staircase.
        halle_z = info.get("z", {}).get("halle", -13.46)
        stairs_x, stairs_z = -3.2, halle_z + 2.90 + 0.40
        answer2 = b.js(STAIRS_JS % (stairs_x, stairs_z))
        check(answer2.get("ok") is True,
              "could not place the player at the stairs: %r" % answer2.get("error"))
        stairs_start = json.loads(answer2["result"])["start"] if answer2.get("ok") else None
        saved2 = b.js("editor.save('%s/scenes/innen_stairs.daidalos')" % stairs_proj)
        check(saved2.get("ok") is True,
              "the stairs scene could not be saved: %r" % saved2.get("error"))
        b.close()
    finally:
        if host.poll() is None:
            host.terminate()
            try:
                host.wait(timeout=10)
            except subprocess.TimeoutExpired:
                host.kill()

    if FAIL:
        return 1

    samples, out = sample_run(proj, args.seconds)

    check(len(samples) > 10,
          "the runtime printed %d probe lines - the behaviours did not run.\n%s"
          % (len(samples), out[-900:]))
    if len(samples) <= 10:
        return 1

    ys = [s["p"][1] for s in samples]
    xs = [s["p"][0] for s in samples]
    zs = [s["p"][2] for s in samples]
    y0 = start[1] if start else ys[0]

    check(min(ys) > y0 - 0.45,
          "the player sank to y=%.2f from %.2f - the floor is not holding it"
          % (min(ys), y0))
    check(max(ys) < y0 + 0.45,
          "the player rose to y=%.2f from %.2f - something is pushing it up"
          % (max(ys), y0))
    check(min(zs) < -1.6,
          "the player never left the phone box: the furthest it got was z=%.2f"
          % min(zs))

    # ---- the doors -------------------------------------------------------
    # The door behaviour prints one line per state change. The box door has to
    # slam behind the player, because that is the level's first sentence.
    doors = [l for l in out.splitlines() if l.startswith("DOOR ")]
    check(any("name=Tuer.Zelle" in l and "slam" in l for l in doors),
          "the phone box door never shut behind the player.\n  %s"
          % "\n  ".join(doors[:12]))

    # A SHUT door is a wall: before it opens, the player is still in the
    # corridor. The hall's near wall is at z = -9.86, so anything past -10.0 is
    # through the doorway.
    before = [s["p"][2] for s in samples if s["t"] < DOOR_OPEN_AT]
    check(before and min(before) > -10.0,
          "the shut hall door did not stop the player: z reached %.2f before "
          "it opened at t=%.1f" % (min(before) if before else 0.0, DOOR_OPEN_AT))
    check(before and min(before) < -8.5,
          "the player never got as far as the hall door: z reached only %.2f"
          % (min(before) if before else 0.0))
    check(any("name=Tuer.Halle" in l and "auto-opening" in l for l in doors),
          "the hall door never opened.\n  %s" % "\n  ".join(doors[:12]))

    # ...and an OPEN one is not. Through the second doorway into the hall.
    check(min(zs) < -10.4,
          "the player never reached the hall through the open door: the "
          "furthest it got was z=%.2f" % min(zs))
    # ...and stopped at the hall's far wall, which is at z = -16.96.
    check(min(zs) > -16.96,
          "the player walked through the hall's far wall: z reached %.2f" % min(zs))

    # Sideways: while in the hallway (z between -0.8 and -9.9) the walls are
    # 1.70 m apart, so |x| may not pass 0.86. In the hall it is 9 m wide.
    corridor = [s["p"][0] for s in samples if -9.9 < s["p"][2] < -0.8]
    check(not corridor or max(abs(x) for x in corridor) < 0.86,
          "the player left the corridor sideways: |x| reached %.2f"
          % (max(abs(x) for x in corridor) if corridor else 0.0))
    check(max(abs(x) for x in xs) < 4.5,
          "the player left the hall sideways: |x| reached %.2f"
          % max(abs(x) for x in xs))

    tail = [s for s in samples if s["t"] > samples[-1]["t"] - 1.0]
    speeds = [(s["v"][0] ** 2 + s["v"][2] ** 2) ** 0.5 for s in tail]
    check(tail and max(speeds) > 1.0,
          "the player stopped moving: the last second peaks at %.2f m/s"
          % (max(speeds) if speeds else 0.0))
    check(sum(s["g"] for s in samples) > len(samples) * 0.5,
          "the player was airborne for most of the walk (%d of %d samples grounded)"
          % (sum(s["g"] for s in samples), len(samples)))

    print("walked %.1f s: from z=%.2f to z=%.2f, y stayed in %.2f..%.2f, |x| max %.2f"
          % (samples[-1]["t"], zs[0], zs[-1], min(ys), max(ys),
             max(abs(x) for x in xs)))

    # ---- and up the stairs ----------------------------------------------
    st, sout = sample_run(stairs_proj, args.stairs_seconds)
    check(len(st) > 10,
          "the stairs run printed %d probe lines.\n%s" % (len(st), sout[-600:]))
    if len(st) > 10:
        sy = [q["p"][1] for q in st]
        sz = [q["p"][2] for q in st]
        y_start = stairs_start[1] if stairs_start else sy[0]
        climbed = max(sy) - min(sy[:2] + [y_start])
        # 23 steps of 0.189 m. Getting a third of the way up is already
        # something a 1 m crate in front of a picture of stairs cannot do.
        check(climbed > 1.4,
              "the player did not climb the staircase: y went from %.2f to %.2f "
              "(%.2f m) while z went %.2f -> %.2f"
              % (y_start, max(sy), climbed, sz[0], sz[-1]))
        check(max(sy) < y_start + 5.0,
              "the player was launched off the stairs to y=%.2f" % max(sy))
        check(sum(q["g"] for q in st) > len(st) * 0.4,
              "the player spent the climb in the air (%d of %d samples grounded)"
              % (sum(q["g"] for q in st), len(st)))
        print("climbed %.2f m in %.1f s, z %.2f -> %.2f"
              % (climbed, st[-1]["t"], sz[0], sz[-1]))

    if not args.keep:
        shutil.rmtree(proj, ignore_errors=True)
        shutil.rmtree(stairs_proj, ignore_errors=True)

    print("ok: %d checks, %d failures" % (PASS + FAIL, FAIL))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
