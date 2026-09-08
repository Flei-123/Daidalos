#!/usr/bin/env python3
"""Talk to a running Daidalos editor over the Jarvis bridge.

The bridge is the local TCP socket in include/dai_bridge_host.inl: one JSON
object per line, 127.0.0.1 only, and OFF unless the editor was started with a
port:

    DAI_BRIDGE_PORT=8181 ./build/editor_demo
    DAI_SHADER_DIR=shaders ./build/modeling_shot --serve 60 --bridge 8181 /tmp

Then, from anywhere on the same machine:

    tools/daibridge.py ping
    tools/daibridge.py js "var b = editor.add('Box'); node.setVec(b,'transform.scale',2,3,1); b"
    tools/daibridge.py js -f examples/scripts/innen_room.js
    tools/daibridge.py scene --name Box
    tools/daibridge.py shot /tmp/room.png --eye 5,3,-7 --target 0,1,0 --fov 52
    tools/daibridge.py save projects/INNEN/scenes/room.daiscene
    tools/daibridge.py undo
    tools/daibridge.py raw '{"cmd":"ping"}'

Every command prints the answer as JSON and exits non-zero when the editor
answered `"ok": false`, so it can be used from a shell script without parsing
prose. `js` prints only the `result` unless --json is given, because the value
of the last expression is what a caller of a REPL wants.

There is no authentication and there is not going to be any: the socket is on
the loopback address, it is off by default, and anything that can open it can
already run programs as this user. What there IS, is that it says in the
editor's console that it is open - a modelling socket nobody can see is the
thing to be afraid of, not this one.
"""

import argparse
import json
import socket
import sys


class Bridge(object):
    """One connection. Line in, line out, in the order they were sent."""

    def __init__(self, port, host="127.0.0.1", timeout=30.0):
        self.sock = socket.create_connection((host, port), timeout)
        self.sock.settimeout(timeout)
        self.file = self.sock.makefile("rwb")

    def ask(self, message):
        self.file.write((json.dumps(message) + "\n").encode("utf-8"))
        self.file.flush()
        line = self.file.readline()
        if not line:
            raise IOError("the bridge closed the connection")
        return json.loads(line.decode("utf-8"))

    # -- the commands, one method each -------------------------------------
    def ping(self):
        return self.ask({"cmd": "ping"})

    def js(self, code):
        return self.ask({"cmd": "eval", "code": code})

    def scene(self):
        return self.ask({"cmd": "scene"})

    def save(self, path):
        return self.ask({"cmd": "save", "path": path})

    def shot(self, path, eye=None, target=None, fov=None):
        msg = {"cmd": "shot", "path": path}
        if eye is not None:
            msg["eye"] = list(eye)
        if target is not None:
            msg["target"] = list(target)
        if fov is not None:
            msg["fov"] = float(fov)
        return self.ask(msg)

    def undo(self):
        return self.ask({"cmd": "undo"})

    def redo(self):
        return self.ask({"cmd": "redo"})

    def quit(self):
        return self.ask({"cmd": "quit"})

    def node(self, name):
        """The first node called `name`, or None. Handy in scripts."""
        for n in self.scene().get("nodes", []):
            if n.get("name") == name:
                return n
        return None

    def close(self):
        try:
            self.file.close()
        finally:
            self.sock.close()


def vec(text):
    parts = [p for p in text.replace(" ", "").split(",") if p]
    if len(parts) != 3:
        raise argparse.ArgumentTypeError("expected three numbers, e.g. 5,3,-7")
    return [float(p) for p in parts]


def main(argv):
    ap = argparse.ArgumentParser(description="talk to a running Daidalos editor")
    ap.add_argument("--port", type=int, default=8181)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--json", action="store_true",
                    help="print the whole answer, not just the interesting part")
    sub = ap.add_subparsers(dest="cmd")

    sub.add_parser("ping")
    p_js = sub.add_parser("js", help="run JavaScript in the editor's script context")
    p_js.add_argument("code", nargs="?", default=None)
    p_js.add_argument("-f", "--file", default=None, help="read the code from a file")
    p_scene = sub.add_parser("scene", help="the document, as JSON")
    p_scene.add_argument("--name", default=None, help="only the node with this name")
    p_save = sub.add_parser("save")
    p_save.add_argument("path")
    p_shot = sub.add_parser("shot")
    p_shot.add_argument("path")
    p_shot.add_argument("--eye", type=vec, default=None)
    p_shot.add_argument("--target", type=vec, default=None)
    p_shot.add_argument("--fov", type=float, default=None)
    sub.add_parser("undo")
    sub.add_parser("redo")
    sub.add_parser("quit")
    p_raw = sub.add_parser("raw", help="send one line of JSON as it stands")
    p_raw.add_argument("message")

    args = ap.parse_args(argv)
    if not args.cmd:
        ap.print_help()
        return 2

    try:
        b = Bridge(args.port, args.host, args.timeout)
    except (socket.error, OSError) as e:
        sys.stderr.write("no bridge on %s:%d (%s)\n"
                         "start the editor with DAI_BRIDGE_PORT=%d\n"
                         % (args.host, args.port, e, args.port))
        return 1

    try:
        if args.cmd == "ping":
            answer = b.ping()
        elif args.cmd == "js":
            code = args.code
            if args.file:
                with open(args.file, "r") as f:
                    code = f.read()
            if code is None:
                sys.stderr.write("js needs either CODE or -f FILE\n")
                return 2
            answer = b.js(code)
        elif args.cmd == "scene":
            answer = b.scene()
            if args.name is not None and answer.get("ok"):
                answer = {"ok": True,
                          "nodes": [n for n in answer.get("nodes", [])
                                    if n.get("name") == args.name]}
        elif args.cmd == "save":
            answer = b.save(args.path)
        elif args.cmd == "shot":
            answer = b.shot(args.path, args.eye, args.target, args.fov)
        elif args.cmd == "undo":
            answer = b.undo()
        elif args.cmd == "redo":
            answer = b.redo()
        elif args.cmd == "quit":
            answer = b.quit()
        else:
            answer = b.ask(json.loads(args.message))
    finally:
        b.close()

    if args.cmd == "js" and not args.json and answer.get("ok"):
        print(answer.get("result", ""))
    else:
        print(json.dumps(answer, indent=2, sort_keys=True))
    return 0 if answer.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
