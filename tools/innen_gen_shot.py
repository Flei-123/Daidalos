#!/usr/bin/env python3
"""Photographs of a GENERATED floor - one plan view and two from eye height.

    tools/innen_gen_shot.py [--seed 7] [--rooms 14] [--out .gauntlet-shots]

The camera is not typed in: the plan comes back from
`examples/scripts/innen_gen.js`, and the plan view is framed around the
bounding box of whatever the seed happened to build. A generator whose floor
grows in a different direction next week still gets photographed.

  27-innen-gen-plan.png   the whole floor from above, ceilings off
  28-innen-gen-raum.png   inside the deepest room the generator reached
  29-innen-gen-tuer.png   the phone box's doorway, from the room behind it

The scene is saved as well, so the editor can open exactly what was shot.
"""

import argparse
import json
import os
import subprocess
import sys
import socket as pysocket
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from daibridge import Bridge, read_sources          # noqa: E402

LIB = "examples/scripts/innen_lib.js"
GEN = "examples/scripts/innen_gen.js"


def wait_for_port(port, proc, seconds=40.0):
    deadline = time.time() + seconds
    while time.time() < deadline:
        if proc.poll() is not None:
            return False
        try:
            s = pysocket.create_connection(("127.0.0.1", port), 0.5)
            s.close()
            return True
        except (pysocket.error, OSError):
            time.sleep(0.2)
    return False


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--rooms", type=int, default=14)
    ap.add_argument("--out", default=".gauntlet-shots")
    ap.add_argument("--port", type=int, default=8399)
    ap.add_argument("--width", type=int, default=1600)
    ap.add_argument("--height", type=int, default=900)
    ap.add_argument("--prefix", default="")
    ap.add_argument("--scene", default="projects/Untitled/scenes/innen_gen.daiscene")
    args = ap.parse_args(argv[1:])

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    if not os.path.exists("build/modeling_shot"):
        print("innen_gen_shot: build/modeling_shot missing")
        return 2
    if not os.path.isdir(args.out):
        os.makedirs(args.out)

    env = dict(os.environ)
    env.setdefault("DAI_SHADER_DIR", "shaders")
    env["DAI_BRIDGE_PORT"] = str(args.port)
    proc = subprocess.Popen(["build/modeling_shot", "/tmp",
                             str(args.width), str(args.height),
                             "--serve", "300", "--bridge", str(args.port)],
                            env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    rc = 0
    try:
        if not wait_for_port(args.port, proc):
            print("innen_gen_shot: the host never opened the bridge")
            return 2
        b = Bridge(args.port, timeout=180.0)
        head = "var GEN = { seed: %d, rooms: %d };\n" % (args.seed, args.rooms)
        answer = b.js(head + read_sources([LIB, GEN]))
        if not answer.get("ok"):
            print("innen_gen_shot: the generator failed: %r" % answer.get("error"))
            return 1
        plan = json.loads(answer["result"])
        rooms = plan["rooms"]

        # The floor's own bounding box, so the plan view frames what was built.
        x0 = min(r["foot"][0] for r in rooms)
        x1 = max(r["foot"][1] for r in rooms)
        z0 = min(r["foot"][2] for r in rooms)
        z1 = max(r["foot"][3] for r in rooms)
        cx, cz = (x0 + x1) / 2.0, (z0 + z1) / 2.0
        reach = max(x1 - x0, z1 - z0)

        def shot(name, eye, target, fov):
            r = b.shot(os.path.join(args.out, args.prefix + name),
                       eye=eye, target=target, fov=fov)
            return bool(r.get("ok"))

        # Ceilings off for the plan view only, and back on before the save:
        # the file on disk has to be the level, not the photograph.
        b.js("var n = editor.count(); for (var i = 0; i < n; i++) {"
             " var id = editor.at(i);"
             " if (node.getStr(id, 'node.name').indexOf('.Ceiling') > 0)"
             "  node.setNum(id, 'renderer.enabled', 0); } 'off'")
        ok = shot("27-innen-gen-plan.png",
                  [cx + reach * 0.35, reach * 0.95, cz + reach * 0.45],
                  [cx, 0.6, cz], 52)
        b.js("var n = editor.count(); for (var i = 0; i < n; i++) {"
             " var id = editor.at(i);"
             " if (node.getStr(id, 'node.name').indexOf('.Ceiling') > 0)"
             "  node.setNum(id, 'renderer.enabled', 1); } 'on'")
        rc |= 0 if ok else 1

        # The deepest room the growth reached, from a corner of it at eye
        # height, looking at its middle.
        deep = max(rooms, key=lambda r: r["depth"])
        dx, dz = deep["centre"]
        dw, dd = deep["size"][0], deep["size"][1]
        ok = shot("28-innen-gen-raum.png",
                  [dx - dw * 0.35, 1.62, dz + dd * 0.38], [dx, 1.35, dz - dd * 0.4], 62)
        rc |= 0 if ok else 1

        # The phone box's own doorway, seen from the room the generator put in
        # front of it - the first door of the game, generated.
        ok = shot("29-innen-gen-tuer.png",
                  [0.0, 1.60, rooms[0]["foot"][2] - 2.4], [0.0, 1.30, 0.4], 58)
        rc |= 0 if ok else 1

        if args.scene:
            b.save(os.path.abspath(args.scene))
        b.close()
        print("seed %d: %d rooms, %.1f x %.1f m, %d doorways, deepest zone %d"
              % (args.seed, len(rooms), x1 - x0, z1 - z0, len(plan["edges"]),
                 max(r["depth"] for r in rooms)))
        for name in ("27-innen-gen-plan.png", "28-innen-gen-raum.png",
                     "29-innen-gen-tuer.png"):
            print(os.path.join(args.out, args.prefix + name))
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
