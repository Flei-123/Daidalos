#!/usr/bin/env python3
"""layout_guard - read the layout dumps and fail on the ones that are bugs.

AGENTENPLAN.md M3. tools/modeling_shot.cpp writes a <shot>.layout.json next to
every picture it takes: every string of that frame with the box it went into
and the clip it sat under, plus the problems include/dai_ui_layout.inl found.
This turns them into a pass or a fail.

The three kinds are NOT equally bad, and pretending they are is how a check
gets ignored:

  CUT_OFF   the ink runs past the clip. Nobody decided this; the panel simply
            ends mid-glyph and the reader cannot tell he is missing anything.
            This is "Block.Beve" from the gauntlet round. ALWAYS an error.

  OVERLAP   two strings on the same pixels under the same clip. ALWAYS an
            error - there is no layout in which that is intended.

  CLIPPED   the panel shortened its own label to an ellipsis. This one is a
            judgement call: a hierarchy row that shows "Wall.Front.D..." in a
            174 px tree is behaving correctly, while an inspector label that
            reads "Pos..." has lost the word. So it is a NOTE by default and an
            error under --strict, and --max-clipped sets how many are tolerable
            before the layout is called crowded.

    tools/layout_guard.py .gauntlet-shots
    tools/layout_guard.py /tmp/shots --strict
    tools/layout_guard.py .gauntlet-shots --max-clipped 4

Exit 0 = clean, 1 = at least one error.
"""

import argparse
import glob
import json
import os
import sys


def load(path):
    try:
        with open(path) as f:
            return json.load(f)
    except Exception as e:
        return {"_error": str(e)}


def main():
    ap = argparse.ArgumentParser(description="check UI layout dumps")
    ap.add_argument("directory")
    ap.add_argument("--strict", action="store_true",
                    help="a shortened label counts as an error, not a note")
    ap.add_argument("--max-clipped", type=int, default=-1,
                    help="fail if more than this many labels were shortened (-1 = no limit)")
    ap.add_argument("--newer-than", default="",
                    help="only dumps newer than this file")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(args.directory, "*.layout.json")))
    if args.newer_than:
        if not os.path.exists(args.newer_than):
            print("layout_guard: marker %s does not exist" % args.newer_than)
            return 1
        cutoff = os.path.getmtime(args.newer_than)
        files = [f for f in files if os.path.getmtime(f) >= cutoff]

    if not files:
        print("layout_guard: no *.layout.json in %s - the shot tool did not write any"
              % args.directory)
        return 1

    errors, notes = [], []
    total_texts = 0
    clipped_total = 0

    for path in files:
        name = os.path.basename(path)[:-len(".layout.json")]
        d = load(path)
        if "_error" in d:
            errors.append("UNREADABLE %s: %s" % (name, d["_error"]))
            continue
        total_texts += d.get("counts", {}).get("texts", 0)
        for issue in d.get("issues", []):
            kind = issue.get("kind")
            text = issue.get("text", "")
            r = issue.get("rect", [0, 0, 0, 0])
            c = issue.get("clip", [0, 0, 0, 0])
            where = "%s at %.0f,%.0f" % (name, r[0], r[1])
            if kind == "CUT_OFF":
                errors.append('CUT_OFF %s: "%s" is %.0f px wide and runs %.0f px past its clip '
                              "(box ends at %.0f)" % (where, text, r[2], issue.get("overrun", 0),
                                                      c[0] + c[2]))
            elif kind == "OVERLAP":
                errors.append('OVERLAP %s: "%s" and "%s" share %.0fx%.0f px'
                              % (where, text, issue.get("other", "?"), r[2], r[3]))
            elif kind == "CLIPPED":
                clipped_total += 1
                msg = ('CLIPPED %s: "%s" was shortened to fit %.0f px'
                       % (where, text, c[2]))
                (errors if args.strict else notes).append(msg)

    if not args.quiet:
        print("layout_guard: %d dump(s), %d strings, %d shortened label(s)"
              % (len(files), total_texts, clipped_total))
        for n in notes:
            print("   note: " + n)

    if args.max_clipped >= 0 and clipped_total > args.max_clipped:
        errors.append("TOO CROWDED %d labels were shortened, at most %d are tolerated "
                      "- the panel is narrower than its content at this resolution"
                      % (clipped_total, args.max_clipped))

    if errors:
        print("")
        print("layout_guard: %d ERROR(S)" % len(errors))
        for e in errors:
            print("   !! " + e)
        return 1

    print("layout_guard: ok - nothing cut off, nothing overlapping in %d frame(s)" % len(files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
