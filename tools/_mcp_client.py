#!/usr/bin/env python3
"""Kleiner Stdio-MCP-Client fuer dai_mcp.py.

Aufruf:  ./tools/_mcp_client.py calls.json [out_dir]
calls.json = Liste von {"tool": "...", "args": {...}} ODER {"raw_js": "..."}.
Bilder (image content) werden als PNG in out_dir gespeichert, Text wird gedruckt.
"""
import base64, json, os, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class MCP:
    def __init__(self):
        self.p = subprocess.Popen([sys.executable, os.path.join(ROOT, "tools", "dai_mcp.py")],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, cwd=ROOT, text=True, bufsize=1)
        self.id = 0
        self.rpc("initialize", {"protocolVersion": "2024-11-05", "capabilities": {},
                                "clientInfo": {"name": "jarvis", "version": "1"}})

    def rpc(self, method, params=None):
        self.id += 1
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.id,
                                       "method": method, "params": params or {}}) + "\n")
        self.p.stdin.flush()
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("MCP-Server beendet")
            try:
                msg = json.loads(line)
            except Exception:
                continue
            if msg.get("id") == self.id:
                return msg

    def call(self, tool, args):
        return self.rpc("tools/call", {"name": tool, "arguments": args})

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=10)
        except Exception:
            self.p.kill()


def main():
    calls = json.load(open(sys.argv[1]))
    out = sys.argv[2] if len(sys.argv) > 2 else "/tmp/daishots"
    os.makedirs(out, exist_ok=True)
    m = MCP()
    n = 0
    for c in calls:
        t0 = time.time()
        r = m.call(c["tool"], c.get("args", {}))
        n += 1
        res = r.get("result") or {}
        err = r.get("error")
        print("=" * 70)
        print("#%d %s %s  (%.1fs)" % (n, c["tool"], json.dumps(c.get("args", {}))[:160], time.time() - t0))
        if err:
            print("ERROR:", json.dumps(err)[:2000])
            continue
        for part in res.get("content", []):
            if part.get("type") == "text":
                txt = part["text"]
                lim = c.get("limit", 4000)
                print(txt[:lim] + ("\n...[gekuerzt, %d Zeichen]" % len(txt) if len(txt) > lim else ""))
            elif part.get("type") == "image":
                fn = os.path.join(out, c.get("save", "shot_%02d.png" % n))
                open(fn, "wb").write(base64.b64decode(part["data"]))
                print("BILD ->", fn)
    m.close()


if __name__ == "__main__":
    main()
