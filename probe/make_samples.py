"""Render the sample set: every voice, every parameter, and the awkward inputs.

These are the evidence behind the claims in the README. Each file is rendered
by the same code path the shipping wrapper uses -- a generated .tav handed to
Speaker::load -- so a sample that sounds right means the product is right.

    python probe/make_samples.py [--out samples] [--data engine/Data]
"""

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RENDER = os.path.join(ROOT, "build_x86", "Release", "fv2_render.exe")
if not os.path.exists(RENDER):
    RENDER = os.path.join(ROOT, "build", "probe", "fv2_render.exe")

# name, .tav file, the measured level trim from src/fv2_voices.hpp
# The shipping trims from src/fv2_voices.hpp: each voice at -17 dBFS RMS, with
# the host's limiter handling the peaks.
VOICES = [
    ("Julie",  "Julie.tav",  7.47),
    ("Bill",   "Bill.tav",   4.17),
    ("Jill",   "Jill.tav",   2.72),
    ("Julius", "Julius.tav", 5.69),
    ("Kit",    "Kit.tav",    5.62),
]

# Only the parameters the engine responds to; see fv2_params.inc.
PARAMS = [
    "volume", "speechRate", "defaultPitch", "pitchMin", "pitchMax", "pitchRate",
    "intonationLevel", "headsize", "richness", "smoothness", "fricationRate",
    "plosiveRate", "volumeSmoothWindow",
]

PANGRAM = ("The quick brown fox jumps over the lazy dog. "
           "She sells sea shells by the sea shore.")

# What was actually thrown at the engine while working out what breaks it.
ROBUSTNESS = [
    ("numerals",        "Chapter 5. The year 1984. It costs 3.50 dollars."),
    ("bare-digits",     "1234567890"),
    ("punctuation",     "Hello, world! Really? Yes -- absolutely; of course."),
    ("symbols",         "Tom & Jerry, 100% done, a < b > c, x | y."),
    ("bracket-command", "[:rate 200] this is spoken, not obeyed."),
    ("backslash",       "A backslash \\ is spoken, not an escape."),
    ("mixed-case",      "NASA and ASCII and Wi-Fi and iPhone."),
    ("long-word",       "supercalifragilisticexpialidocious"),
    ("accented",        "cafe naive resume Zoe"),
    ("single-letter",   "a"),
]

LINE = re.compile(r"samples=(\d+) seconds=([\d.]+) ms=(\d+) rtf=([\d.-]+) "
                  r"peak=(\d+) rms=([\d.-]+) dbfs=([\d.-]+)")


def render(data, tav, out, text, extra=None, timeout=90):
    cmd = [RENDER, "--data", data, "--tav", tav, "--text", text, "--out", out]
    cmd += extra or []
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None
    if p.returncode != 0:
        return None
    m = LINE.search(p.stdout)
    return m.groups() if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "samples"))
    ap.add_argument("--data", default=os.path.join(ROOT, "engine", "Data"))
    args = ap.parse_args()

    if not os.path.exists(RENDER):
        sys.exit("build fv2_render first (build_all.bat)")

    voices_dir = os.path.join(args.data, "Voices")
    julie = os.path.join(voices_dir, "Julie.tav")

    index = []
    made = 0
    failed = []

    # --- 1. the voices -----------------------------------------------------
    d = os.path.join(args.out, "01-voices")
    os.makedirs(d, exist_ok=True)
    print("voices")
    for i, (name, tav, trim) in enumerate(VOICES):
        out = os.path.join(d, "%02d-%s.wav" % (i + 1, name))
        r = render(args.data, os.path.join(voices_dir, tav), out, PANGRAM,
                   ["--set", "volume=%g" % trim])
        if r:
            print("  %-8s %ss  peak %s  %s dBFS" % (name, r[1], r[4], r[6]))
            index.append(("01-voices/%02d-%s.wav" % (i + 1, name),
                          "%s, at its measured level trim" % name))
            made += 1
        else:
            failed.append(name)

    # --- 2. every parameter across its range -------------------------------
    d = os.path.join(args.out, "02-parameters")
    os.makedirs(d, exist_ok=True)
    print("\nparameters (Julie, 0 / 25 / 50 / 75 / 100 percent)")
    for p in PARAMS:
        line = "  %-20s" % p
        for pct in (0, 25, 50, 75, 100):
            out = os.path.join(d, "%s-%03d.wav" % (p, pct))
            r = render(args.data, julie, out, PANGRAM, ["--pct", "%s=%d" % (p, pct)])
            line += (" %5ss" % r[1]) if r else "  FAIL"
            if r:
                index.append(("02-parameters/%s-%03d.wav" % (p, pct),
                              "%s at %d%%" % (p, pct)))
                made += 1
            else:
                failed.append("%s@%d" % (p, pct))
        print(line)

    # --- 3. the engine-level multipliers -----------------------------------
    d = os.path.join(args.out, "03-rate-and-volume")
    os.makedirs(d, exist_ok=True)
    print("\nengine rate and volume multipliers")
    for r_ in (0.25, 0.5, 1.0, 2.0, 4.0):
        out = os.path.join(d, "rate-%gx.wav" % r_)
        res = render(args.data, julie, out, PANGRAM, ["--rate", str(r_)])
        if res:
            print("  rate %-5g %ss" % (r_, res[1]))
            index.append(("03-rate-and-volume/rate-%gx.wav" % r_,
                          "engine speech rate multiplier %gx" % r_))
            made += 1
    for v in (0.25, 0.5, 1.0):
        out = os.path.join(d, "volume-%gx.wav" % v)
        res = render(args.data, julie, out, PANGRAM, ["--volume", str(v)])
        if res:
            print("  volume %-3g peak %s" % (v, res[4]))
            index.append(("03-rate-and-volume/volume-%gx.wav" % v,
                          "engine volume multiplier %gx" % v))
            made += 1

    # --- 4. inputs that had to be checked ----------------------------------
    d = os.path.join(args.out, "04-robustness")
    os.makedirs(d, exist_ok=True)
    print("\nrobustness")
    for name, text in ROBUSTNESS:
        out = os.path.join(d, "%s.wav" % name)
        r = render(args.data, julie, out, text, ["--set", "volume=7.47"])
        if r:
            print("  %-16s %ss  %s" % (name, r[1], text[:44]))
            index.append(("04-robustness/%s.wav" % name, text))
            made += 1
        else:
            failed.append(name)

    # --- an index, so the folder explains itself ---------------------------
    with open(os.path.join(args.out, "README.md"), "w", encoding="utf-8") as f:
        f.write("# FlexVoice 2 sample renders\n\n")
        f.write("Every file here was rendered through the same path the shipping\n")
        f.write("wrapper uses: parameters written into a generated `.tav`, handed to\n")
        f.write("the engine's `Speaker::load`. 16 kHz, 16-bit, mono.\n\n")
        f.write("Text, unless noted: *%s*\n\n" % PANGRAM)
        f.write("| File | What it is |\n|---|---|\n")
        for path, desc in index:
            f.write("| `%s` | %s |\n" % (path, desc.replace("|", "\\|")))
        f.write("\nRegenerate with `python probe/make_samples.py`.\n")

    print("\n%d files written to %s" % (made, args.out))
    if failed:
        print("FAILED: %s" % ", ".join(failed))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
