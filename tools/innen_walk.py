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
from daibridge import Bridge          # noqa: E402

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
WALK_JS = """
(function () {
  var p = scene.find('Spieler');
  if (!p) return JSON.stringify({ found: false });
  node.setStr(p, 'script',
     'innen_player.js{eye=Spieler.Kamera,torch=Spieler.Lampe,walkSpeed=2.2,' +
     'eyeHeight=1.62,torchOn=true,autoWalk=true};walk_probe.js{every=0.25}');
  var q = node.getPos(p);
  return JSON.stringify({ found: true, start: [q[0], q[1], q[2]],
                          script: node.getStr(p, 'script') });
})();
"""

PROBE = re.compile(r"PROBE t=([-\d.]+) p=([-\d.]+),([-\d.]+),([-\d.]+) "
                   r"v=([-\d.]+),([-\d.]+),([-\d.]+) g=(\d)")


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
    shutil.rmtree(proj, ignore_errors=True)
    os.makedirs(proj + "/scenes")
    shutil.copytree("projects/Untitled/assets", proj + "/assets")
    with open(proj + "/project.daidalos", "w") as f:
        f.write("daidalos-project 1\nname INNEN walk\nengine 0.2.0\n")
    with open(proj + "/boot.cfg", "w") as f:
        # 60 Hz, Talos, and the level as the start scene. No window is opened
        # by --headless, so width and height only matter to the log.
        f.write("daidalos-boot 1\nscene scenes/innen_walk.daidalos\n"
                "title INNEN walk\nwidth 640\nheight 360\ntick_hz 60\n")

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
        with open("examples/scripts/innen_m0.js") as f:
            built = b.js(f.read())
        check(built.get("ok") is True, "the level script failed: %r" % built.get("error"))
        if not built.get("ok"):
            return 1
        info = json.loads(built["result"])
        check(info.get("script") is True,
              "this build cannot put a behaviour on a node, so there is no player")
        check(info.get("collider") is True,
              "this build has no collider.shape, so the player is a box, not a capsule")

        answer = b.js(WALK_JS)
        check(answer.get("ok") is True, "could not switch the player to autoWalk: %r"
              % answer.get("error"))
        walk = json.loads(answer["result"]) if answer.get("ok") else {}
        check(walk.get("found") is True, "there is no node called Spieler in the level")
        start = walk.get("start")
        saved = b.js("editor.save('%s/scenes/innen_walk.daidalos')" % proj)
        check(saved.get("ok") is True, "the scene could not be saved: %r" % saved.get("error"))
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

    frames = int(args.seconds * 60)
    run = subprocess.run(["build/daidalos_runtime", proj, "--headless", str(frames)],
                         capture_output=True, text=True, timeout=600)
    out = run.stdout + run.stderr
    samples = []
    for m in PROBE.finditer(out):
        samples.append({
            "t": float(m.group(1)),
            "p": [float(m.group(2)), float(m.group(3)), float(m.group(4))],
            "v": [float(m.group(5)), float(m.group(6)), float(m.group(7))],
            "g": int(m.group(8)),
        })

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
    # Through the SECOND door as well: the hallway's far door is at z = -9.86,
    # so anything past it is inside the hall. Two doorways walked through is
    # what makes this a level and not a room.
    check(min(zs) < -10.4,
          "the player never reached the hall: the furthest it got was z=%.2f"
          % min(zs))
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
    if not args.keep:
        shutil.rmtree(proj, ignore_errors=True)

    print("ok: %d checks, %d failures" % (PASS + FAIL, FAIL))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
