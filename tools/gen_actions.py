#!/usr/bin/env python3
"""Generates api/ACTIONS (the `manifest 1` action manifest of Daidalos) from the TOOLS table of tools/dai_mcp.py.

The MCP server is the one place that defines what an agent can do with Daidalos; the manifest is a generated view
of it (same format as openplan/model/ACTIONS and logiclab/api/ACTIONS, readable by the OrientOS action bus), so the
two cannot drift. Levels, undo links and the type mapping are the only things decided here.

    python3 tools/gen_actions.py            print the manifest
    python3 tools/gen_actions.py --write    write api/ACTIONS
    python3 tools/check_actions.py          CI: api/ACTIONS is current, lints clean
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import dai_mcp  # noqa: E402  (importing it starts nothing; main() is guarded)

# read: looks only. write: changes the scene, the files or the running runtime. Nothing here is `critical`
# (every change is inside the repository / /tmp or an undoable editor step).
LEVEL = {
    "state": "read", "api": "read", "docs": "read", "get": "read", "assets": "read", "scene": "read",
    "shot": "read", "tests": "read",
    "add": "write", "set": "write", "remove": "write", "undo": "write", "js": "write", "save": "write",
    "build": "write", "restart": "write",
}
# the editor undo stack reverses these: "one bridge call was one step"
UNDO = {"add": "undo", "set": "undo", "remove": "undo", "undo": "undo"}
TYPES = {"string": "string", "integer": "int", "boolean": "bool", "number": "string", "array": "string",
         "object": "string"}
RETURNS = {"shot": [("picture", "string", "The render as a picture (the PNG is kept in .dai-mcp-shots/)")]}


def one_line(text, limit=260):
    t = re.sub(r"\s+", " ", text).replace('"', "'").strip()
    if len(t) > limit:
        cut = t[:limit].rsplit(" ", 1)[0]
        t = cut.rstrip(".,;:") + " ..."
    return t


def arg_doc(name, spec):
    d = one_line(spec.get("description", ""), 160)
    t = spec.get("type", "string")
    if t == "number":
        d = (d + " " if d else "") + "(a decimal number)"
    elif t == "array":
        item = spec.get("items", {}).get("type", "string")
        n = spec.get("minItems")
        d = (d + " " if d else "") + "(a JSON array of %s%s)" % (item + "s", " [%d values]" % n if n else "")
    elif t == "object":
        d = (d + " " if d else "") + "(a JSON object)"
    if "enum" in spec:
        d = (d + " " if d else "") + "(one of: %s)" % ", ".join(spec["enum"])
    return d or name


def generate():
    out = [
        "manifest 1",
        "app daidalos",
        'title "Daidalos editor / runtime, driven through the MCP bridge (docs/MCP_SERVER.md)"',
        "# api 1.0 -- GENERATED from tools/dai_mcp.py by tools/gen_actions.py. Do not edit: change the TOOLS table,",
        "# run `python3 tools/gen_actions.py --write` and commit both. tools/check_actions.py fails when they differ.",
        "# These actions are the MCP tools of tools/dai_mcp.py (JARVIS config.json: mcp_servers.daidalos); the",
        "# editor socket behind them is include/dai_bridge_host.inl (one JSON object per line, docs/BRIDGE.md).",
        "# SemVer: a new tool or optional argument is a minor step, a removed or changed one a major step.",
    ]
    for tool in dai_mcp.TOOLS:
        name = tool["name"]
        if name not in LEVEL:
            sys.exit("gen_actions: tool '%s' has no level in LEVEL -- decide read or write" % name)
        out.append("")
        out.append('action daidalos.%s %s "%s"' % (name, LEVEL[name], one_line(tool["description"])))
        schema = tool.get("inputSchema", {})
        required = set(schema.get("required", []))
        for pname, spec in schema.get("properties", {}).items():
            out.append('  arg %s %s %s "%s"' % (pname, TYPES[spec.get("type", "string")],
                                                  "required" if pname in required else "optional", arg_doc(pname, spec)))
        out.append('  returns text string "The tool\'s text answer"')
        for r in RETURNS.get(name, []):
            out.append('  returns %s %s "%s"' % r)
        if name in UNDO:
            out.append("  undo daidalos.%s" % UNDO[name])
    return "\n".join(out) + "\n"


if __name__ == "__main__":
    text = generate()
    if "--write" in sys.argv:
        os.makedirs(os.path.join(ROOT, "api"), exist_ok=True)
        open(os.path.join(ROOT, "api", "ACTIONS"), "w", encoding="utf8").write(text)
        print("wrote api/ACTIONS (%d tools)" % len(dai_mcp.TOOLS))
    else:
        sys.stdout.write(text)
