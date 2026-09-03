"""Sweep every FlexVoice 2.0 speech parameter and measure what it does.

Mindmaker documented a range for none of these. The .tav files give one value
each and the engine accepts whatever you write, including values that make it
render silence, clip, or stop producing audio at all -- so the only way to know
where a control's 0% and 100% should sit is to render across the range and
measure the result.

Each render runs as a separate process with a timeout, because a bad value can
wedge the engine, and a wedged engine cannot be recovered in-process.

Usage:
    python probe/sweep.py [--data DIR] [--tav FILE] [--out DIR] [--steps N]
"""

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RENDER = os.path.join(ROOT, "build", "probe", "fv2_render.exe")

# The scalar keys of a .tav, with the range currently declared in
# src/fv2_speaker.cpp. The sweep is what justifies those numbers.
PARAMS = [
    "volume", "speechRate", "defaultPitch", "pitchMin", "pitchMax",
    "pitchRate", "intonationLevel", "headsize", "tilt", "richness",
    "smoothness", "fricationRate", "plosiveRate", "singingPitchRate",
    "speedWPM", "volumeSmoothWindow",
]

TEXT = ("The quick brown fox jumps over the lazy dog. "
        "She sells sea shells by the sea shore.")

LINE = re.compile(
    r"samples=(\d+) seconds=([\d.]+) ms=(\d+) rtf=([\d.-]+) "
    r"peak=(\d+) rms=([\d.-]+) dbfs=([\d.-]+)")


def render(data, tav, extra, out=None, timeout=60):
    """Returns a dict of measurements, or a string describing the failure."""
    cmd = [RENDER, "--data", data, "--tav", tav, "--text", TEXT]
    cmd += extra
    if out:
        cmd += ["--out", out]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return "WEDGED"
    if p.returncode != 0:
        return "CRASH(%d)" % p.returncode
    m = LINE.search(p.stdout)
    if not m:
        return "NO-OUTPUT"
    return {
        "samples": int(m.group(1)),
        "seconds": float(m.group(2)),
        "ms": int(m.group(3)),
        "peak": int(m.group(5)),
        "rms": float(m.group(6)),
        "dbfs": float(m.group(7)),
    }


def fmt(r, base):
    if isinstance(r, str):
        return "%-12s" % r
    clip = " CLIP" if r["peak"] >= 32700 else ""
    silent = " SILENT" if r["peak"] < 200 else ""
    rel = ""
    if base and not isinstance(base, str) and base["seconds"] > 0:
        rel = " %+5.0f%%dur" % ((r["seconds"] / base["seconds"] - 1) * 100)
    return "%6.3fs peak=%5d %6.1fdB%s%s%s" % (
        r["seconds"], r["peak"], r["dbfs"], rel, clip, silent)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=os.path.join(ROOT, "engine", "Data"))
    ap.add_argument("--tav", default=None)
    ap.add_argument("--out", default=None, help="write a WAV per step here")
    ap.add_argument("--steps", type=int, default=5)
    ap.add_argument("--only", default=None, help="sweep just this parameter")
    args = ap.parse_args()

    tav = args.tav or os.path.join(args.data, "Voices", "Julie.tav")
    if not os.path.exists(RENDER):
        sys.exit("build fv2_render first: probe\\build_probe.bat")
    if args.out:
        os.makedirs(args.out, exist_ok=True)

    print("data = %s" % args.data)
    print("tav  = %s" % tav)
    print()

    base = render(args.data, tav, [])
    print("baseline (the voice as Mindmaker shipped it): %s" % fmt(base, None))
    print()

    pcts = [i * 100.0 / (args.steps - 1) for i in range(args.steps)]
    names = [args.only] if args.only else PARAMS

    header = "%-20s" % "parameter" + "".join("%-34s" % ("%g%%" % p) for p in pcts)
    print(header)
    print("-" * len(header))

    problems = []
    for name in names:
        row = "%-20s" % name
        for pct in pcts:
            out = None
            if args.out:
                out = os.path.join(args.out, "%s-%03d.wav" % (name, int(round(pct))))
            r = render(args.data, tav, ["--pct", "%s=%g" % (name, pct)], out)
            if isinstance(r, str):
                problems.append("%s at %g%%: %s" % (name, pct, r))
            elif r["peak"] >= 32700:
                problems.append("%s at %g%%: clipping (peak %d)" % (name, pct, r["peak"]))
            elif r["peak"] < 200:
                problems.append("%s at %g%%: silent" % (name, pct))
            row += "%-34s" % fmt(r, base)
        print(row)

    print()
    if problems:
        print("problems found -- these bound the usable range:")
        for p in problems:
            print("   ", p)
    else:
        print("no crashes, no clipping, no silence anywhere in the declared ranges")


if __name__ == "__main__":
    main()
