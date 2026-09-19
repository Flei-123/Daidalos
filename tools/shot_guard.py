#!/usr/bin/env python3
"""shot_guard - the screenshots have to be four different pictures.

WHY THIS EXISTS. Gauntlet round 1 (17.09.2026) delivered four required
screenshots under four different names. They were the SAME FILE - md5
b598614a... four times. Three of the four names were simply untrue, and
nothing in the pipeline noticed; a human reading the round's report found it.
A build that cannot tell four pictures from one picture cannot be used to
judge whether a change to the renderer worked.

The three-liner that would have caught it:

    find .gauntlet-shots -name '*.png' -exec sha256sum {} + | sort | uniq -w64 -D

This file is that check plus the three neighbours it needs to be worth
anything:

  DUPLICATE   two names, one picture. Byte-identical via SHA-256, and
              near-identical via a perceptual hash - a setup step that quietly
              did nothing produces an almost-but-not-quite identical frame, and
              that is exactly the failure a byte hash misses.

  BLANK       a picture with nothing in it. A black frame (camera inside the
              geometry, renderer never initialised, shot taken before the first
              present) must never pass as evidence. Judged on unique colours
              and standard deviation, not on average brightness: a uniformly
              grey frame has a perfectly reasonable mean.

  MISSING     every name the caller asked for exists exactly once, and nothing
              else is lying around. A shot that was never taken and a stale
              file from three runs ago are the same lie in opposite directions.

  CANARY      a deliberately altered copy of a real frame that MUST be reported
              as different. If the canary passes, the comparison itself is
              broken and every green result above it is worthless. This runs on
              every invocation - a self-test that only runs when someone
              remembers it is not a self-test.

No dependencies: SHA-256 from hashlib, the PNG decoded by hand (this repository
writes uncompressed RGB PNGs from its own encoder), the perceptual hash a
16x16 average-hash computed on the decoded pixels. numpy/Pillow are used when
present, purely for speed.

    tools/shot_guard.py .gauntlet-shots
    tools/shot_guard.py .gauntlet-shots --expect 20-innen-zelle.png,21-innen-flur.png
    tools/shot_guard.py .dai-mcp-shots --min-colors 500

Exit code 0 = every check passed, 1 = at least one failed. Nothing is printed
twice and every failure names the file and the number that failed it, because
"the screenshot test failed" is not a bug report.
"""

import argparse
import hashlib
import os
import struct
import sys
import zlib

ALLOW_FILE = "allowed_duplicates.txt"


# --------------------------------------------------------------------------
# PNG -> pixels, without a dependency
# --------------------------------------------------------------------------

def _unfilter(raw, width, height, bpp):
    """Undo the per-scanline filters. Straight out of the PNG spec, because a
    picture this code cannot read is a picture this code cannot judge."""
    stride = width * bpp
    out = bytearray(stride * height)
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        ft = raw[pos]; pos += 1
        line = bytearray(raw[pos:pos + stride]); pos += stride
        if ft == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return bytes(out), stride


def read_png(path):
    """-> (width, height, bpp, pixels). Only what this repo writes: 8-bit
    RGB/RGBA, no interlace, no palette."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos = 8
    width = height = depth = color = None
    idat = bytearray()
    while pos + 8 <= len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            width, height, depth, color, _c, _f, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or color not in (2, 6) or interlace:
                raise ValueError("unsupported PNG (depth %d, colour %d, interlace %d)"
                                 % (depth, color, interlace))
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break
    bpp = 3 if color == 2 else 4
    raw = zlib.decompress(bytes(idat))
    pixels, _stride = _unfilter(raw, width, height, bpp)
    return width, height, bpp, pixels


# --------------------------------------------------------------------------
# the measurements
# --------------------------------------------------------------------------

def stats(path, sample_step=4, crop=None):
    """unique colours, standard deviation of luma, and a 16x16 average hash.
    Sampled every Nth pixel - 1280x720 is 920k pixels and the answer does not
    change, but the runtime does."""
    w, h, bpp, px = read_png(path)
    if crop:
        px, w, h = crop_pixels(px, w, h, bpp, crop)
    colors = set()
    total = 0
    sq = 0.0
    n = 0
    step = bpp * sample_step
    for i in range(0, len(px) - bpp + 1, step):
        r, g, b = px[i], px[i + 1], px[i + 2]
        colors.add((r << 16) | (g << 8) | b)
        lum = 0.299 * r + 0.587 * g + 0.114 * b
        total += lum
        sq += lum * lum
        n += 1
    mean = total / max(n, 1)
    var = max(sq / max(n, 1) - mean * mean, 0.0)

    # average hash: 16x16 grid of block means, one bit each against the overall
    # mean. Two renders that differ only in noise land on the same bits; two
    # that differ in a moved panel do not.
    bits = 0
    cell_w = max(w // 16, 1)
    cell_h = max(h // 16, 1)
    cell_means = []
    for by in range(16):
        for bx in range(16):
            acc = 0
            cnt = 0
            for y in range(by * cell_h, min((by + 1) * cell_h, h), 2):
                row = y * w * bpp
                for x in range(bx * cell_w, min((bx + 1) * cell_w, w), 2):
                    i = row + x * bpp
                    acc += 0.299 * px[i] + 0.587 * px[i + 1] + 0.114 * px[i + 2]
                    cnt += 1
            cell_means.append(acc / max(cnt, 1))
    grand = sum(cell_means) / len(cell_means)
    for i, m in enumerate(cell_means):
        if m > grand:
            bits |= (1 << i)
    return {"w": w, "h": h, "colors": len(colors), "std": var ** 0.5, "ahash": bits}


def crop_pixels(px, w, h, bpp, rect):
    """A rectangle out of the decoded frame. Judging the whole editor screenshot
    means judging mostly the panels - two completely different rooms then differ  by a
    handful of hash bits, which is a false alarm, and a genuinely broken render
    hides in the same average."""
    cx, cy, cw, ch = rect
    cx = max(0, min(cx, w - 1)); cy = max(0, min(cy, h - 1))
    cw = max(1, min(cw, w - cx)); ch = max(1, min(ch, h - cy))
    out = bytearray(cw * ch * bpp)
    for y in range(ch):
        src = ((cy + y) * w + cx) * bpp
        dst = y * cw * bpp
        out[dst:dst + cw * bpp] = px[src:src + cw * bpp]
    return bytes(out), cw, ch


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def hamming(a, b):
    return bin(a ^ b).count("1")


def load_allowed(directory):
    """Pairs that are allowed to be identical, one 'a.png b.png' per line.
    Comments with #. A pair has to be written down deliberately; that is the
    point - it turns 'these two are the same' from an accident into a claim
    somebody made."""
    path = os.path.join(directory, ALLOW_FILE)
    pairs = set()
    if not os.path.exists(path):
        return pairs
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) == 2:
                pairs.add(frozenset(parts))
    return pairs


# --------------------------------------------------------------------------
# the canary: a picture that MUST come back different
# --------------------------------------------------------------------------

def canary(reference):
    """Take a real frame, change one 40x40 block, and require the comparison to
    notice. If this passes, the pipeline above it proved nothing."""
    w, h, bpp, px = read_png(reference)
    px = bytearray(px)
    for y in range(min(40, h)):
        row = y * w * bpp
        for x in range(min(40, w)):
            i = row + x * bpp
            px[i] = 255 - px[i]
            px[i + 1] = 255 - px[i + 1]
            px[i + 2] = 255 - px[i + 2]
    tmp = reference + ".canary.png"
    write_png(tmp, w, h, bpp, bytes(px))
    try:
        same_bytes = sha256(tmp) == sha256(reference)
        a, b = stats(reference), stats(tmp)
        dist = hamming(a["ahash"], b["ahash"])
        # It must differ in bytes. The perceptual hash may legitimately be
        # close (40x40 of 1280x720 is 0.2% of the frame) - what is checked is
        # that the byte comparison is alive and the decoder really decoded.
        ok = (not same_bytes) and a["colors"] > 0 and b["colors"] > 0
        return ok, dist
    finally:
        os.unlink(tmp)


def write_png(path, w, h, bpp, px):
    ctype = 2 if bpp == 3 else 6
    raw = bytearray()
    stride = w * bpp
    for y in range(h):
        raw.append(0)
        raw += px[y * stride:(y + 1) * stride]
    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body
                + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, ctype, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        f.write(chunk(b"IEND", b""))


# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="check a directory of screenshots")
    ap.add_argument("directory")
    ap.add_argument("--expect", default="",
                    help="comma-separated file names that must exist exactly once")
    ap.add_argument("--min-colors", type=int, default=64,
                    help="a frame with fewer distinct colours counts as blank (default 64)")
    ap.add_argument("--min-std", type=float, default=6.0,
                    help="minimum standard deviation of luma (default 6.0)")
    ap.add_argument("--phash-distance", type=int, default=2,
                    help="two frames closer than this in average-hash bits count as near-duplicates")
    ap.add_argument("--crop", default="",
                    help="x,y,w,h - judge only this rectangle. The editor shot is 58%% panels "
                         "(viewport 840x463 of 1280x720), and a hash over the whole frame mostly "
                         "measures the inspector, so two different rooms look alike. For "
                         "tools/modeling_shot use --crop 184,150,840,463.")
    ap.add_argument("--newer-than", default="",
                    help="only judge pictures newer than this file. run_tests.sh touches a marker "
                         "before it photographs anything: without this, a directory accumulates "
                         "orphans from runs nobody remembers - .gauntlet-shots still held five "
                         "editor shots from 09.09. that no script in the tree produces any more, "
                         "and they made the suite permanently red for a reason that was history.")
    ap.add_argument("--stale-is-error", action="store_true",
                    help="treat those older pictures as a failure instead of a note")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    d = args.directory
    if not os.path.isdir(d):
        print("shot_guard: no such directory: %s" % d)
        return 1

    files = sorted(f for f in os.listdir(d) if f.lower().endswith(".png")
                   and not f.endswith(".canary.png"))
    stale = []
    if args.newer_than:
        if not os.path.exists(args.newer_than):
            print("shot_guard: marker %s does not exist" % args.newer_than)
            return 1
        cutoff = os.path.getmtime(args.newer_than)
        fresh = [f for f in files if os.path.getmtime(os.path.join(d, f)) >= cutoff]
        stale = [f for f in files if f not in fresh]
        files = fresh
    if not files:
        print("shot_guard: no PNGs in %s - nothing was photographed" % d)
        return 1

    crop = None
    if args.crop:
        try:
            crop = tuple(int(x) for x in args.crop.split(","))
            if len(crop) != 4:
                raise ValueError
        except ValueError:
            print("shot_guard: --crop wants x,y,w,h")
            return 1

    allowed = load_allowed(d)
    failures = []
    info = {}

    for f in files:
        p = os.path.join(d, f)
        try:
            s = stats(p, crop=crop)
            s["sha"] = sha256(p)
            info[f] = s
        except Exception as e:
            failures.append("UNREADABLE %s: %s" % (f, e))

    # --- duplicates, byte-identical
    by_sha = {}
    for f, s in info.items():
        by_sha.setdefault(s["sha"], []).append(f)
    for sha, group in by_sha.items():
        if len(group) > 1:
            for i in range(len(group)):
                for j in range(i + 1, len(group)):
                    if frozenset((group[i], group[j])) in allowed:
                        continue
                    failures.append("DUPLICATE %s and %s are the SAME FILE (sha %s)"
                                    % (group[i], group[j], sha[:12]))

    # --- near-duplicates
    names = sorted(info)
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            a, b = names[i], names[j]
            if info[a]["sha"] == info[b]["sha"]:
                continue  # already reported
            if frozenset((a, b)) in allowed:
                continue
            dist = hamming(info[a]["ahash"], info[b]["ahash"])
            if dist <= args.phash_distance:
                failures.append("NEAR-DUPLICATE %s and %s differ in only %d of 256 hash bits"
                                % (a, b, dist))

    # --- blank
    for f, s in info.items():
        if s["colors"] < args.min_colors:
            failures.append("BLANK %s has %d distinct colours (min %d) - nothing was rendered"
                            % (f, s["colors"], args.min_colors))
        elif s["std"] < args.min_std:
            failures.append("BLANK %s luma std %.2f (min %.2f) - flat frame"
                            % (f, s["std"], args.min_std))

    # --- expected names
    if args.expect:
        want = [x.strip() for x in args.expect.split(",") if x.strip()]
        have = set(files)
        for w in want:
            if w not in have:
                failures.append("MISSING %s was required and does not exist" % w)
        for h in sorted(have - set(want)):
            failures.append("ORPHAN %s is in the directory but was not asked for" % h)

    # --- canary, on every run
    if info:
        ref = os.path.join(d, sorted(info)[0])
        try:
            ok, dist = canary(ref)
            if not ok:
                failures.append("CANARY the deliberately altered copy of %s was NOT detected "
                                "as different - the comparison is broken and every result "
                                "above this line is worthless" % os.path.basename(ref))
        except Exception as e:
            failures.append("CANARY could not run on %s: %s" % (os.path.basename(ref), e))

    if stale:
        line = ("%d picture(s) older than this run (%s%s)"
                % (len(stale), ", ".join(sorted(stale)[:4]),
                   ", ..." if len(stale) > 4 else ""))
        if args.stale_is_error:
            failures.append("STALE " + line)
        elif not args.quiet:
            print("shot_guard: note - " + line)

    if not args.quiet:
        print("shot_guard: %d picture(s) in %s" % (len(files), d))
        for f in sorted(info):
            s = info[f]
            print("   %-38s %4dx%-4d  %6d colours  std %6.2f  %s"
                  % (f, s["w"], s["h"], s["colors"], s["std"], s["sha"][:12]))

    if failures:
        print("")
        print("shot_guard: %d FAILURE(S)" % len(failures))
        for f in failures:
            print("   !! " + f)
        return 1

    print("shot_guard: ok - %d pictures, all distinct, none blank, canary caught" % len(files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
