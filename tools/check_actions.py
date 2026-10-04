#!/usr/bin/env python3
"""CI check of the Daidalos action manifest (api/ACTIONS).

  1. api/ACTIONS equals what tools/gen_actions.py generates from tools/dai_mcp.py (no hand edits, no drift)
  2. the manifest lints clean (tools/manifest_lint.py, a copy of Firn tools/manifest-lint/manifest_lint.py)
  3. every MCP tool is exactly one action and the other way round
  4. when a Firn checkout is around ($FIRN or /root/jarvis/projects/u_DiS4in7esMF1/firn): the copy of the linter
     is the same file as Firn's (a differing copy is reported, not fatal -- sync it)
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import dai_mcp  # noqa: E402
import gen_actions  # noqa: E402
import manifest_lint  # noqa: E402

bad = 0
path = os.path.join(ROOT, "api", "ACTIONS")
have = open(path, encoding="utf8").read()
if have != gen_actions.generate():
    print("FAIL api/ACTIONS differs from the generated one -- run: python3 tools/gen_actions.py --write")
    bad = 1
else:
    print("ok   api/ACTIONS is what gen_actions.py generates from tools/dai_mcp.py")

r = manifest_lint.lint(have, "api/ACTIONS")
for w in r.warnings:
    print("warning: " + w)
if r.errors:
    for e in r.errors:
        print("FAIL " + e)
    bad = 1
else:
    print("ok   manifest lints clean (%d actions)" % len(r.actions))

tools = sorted(t["name"] for t in dai_mcp.TOOLS)
acts = sorted(a["name"].split(".", 1)[1] for a in r.actions)
if tools != acts:
    print("FAIL tools and actions differ: %s" % sorted(set(tools) ^ set(acts)))
    bad = 1
else:
    print("ok   %d MCP tools == %d actions" % (len(tools), len(acts)))

firn = os.environ.get("FIRN", "/root/jarvis/projects/u_DiS4in7esMF1/firn")
ref = os.path.join(firn, "tools", "manifest-lint", "manifest_lint.py")
if os.path.exists(ref):
    if open(ref, encoding="utf8").read() == open(os.path.join(HERE, "manifest_lint.py"), encoding="utf8").read():
        print("ok   tools/manifest_lint.py is Firn's linter")
    else:
        print("note tools/manifest_lint.py differs from %s -- sync it" % ref)
else:
    print("note no Firn checkout: cannot compare the linter copy")
sys.exit(bad)
