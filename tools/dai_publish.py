#!/usr/bin/env python3
"""Write the update manifest for a release.

    tools/dai_publish.py 0.2.0 https://jarvis.fleitec.com/dai/ \
        build/daidalos_editor.exe shaders.pack > dai_version.json

Hashing the files by hand and pasting them into JSON is how a manifest ends up
promising a hash the file does not have - and the updater, correctly, then
refuses to install anything. So the manifest is generated from the files that
are actually being shipped, never typed.

The output matches include/dai_update.h and the shape the JARVIS helper already
uses, so one habit covers both.
"""

import hashlib
import json
import os
import sys


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 16), b""):
            h.update(block)
    return h.hexdigest()


def main(argv):
    if len(argv) < 4:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    version, base_url = argv[1], argv[2]
    if not base_url.endswith("/"):
        base_url += "/"

    files = {}
    total = 0
    for path in argv[3:]:
        name = os.path.basename(path)
        # The updater refuses names with a separator in them; catching it here
        # means finding out at publish time instead of on a user's machine.
        if name != path.strip("./") and os.sep in name:
            print("bad name: %s" % name, file=sys.stderr)
            return 1
        size = os.path.getsize(path)
        files[name] = {"sha256": sha256(path), "size": size}
        total += size
        print("  %-32s %10d bytes  %s" % (name, size, files[name]["sha256"][:16]),
              file=sys.stderr)

    manifest = {"version": version, "base_url": base_url, "files": files, "size": total}
    print(json.dumps(manifest, indent=2))
    print("\n%d file(s), %.1f MB total" % (len(files), total / 1048576.0), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
