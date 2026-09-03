"""Find how FVWrapper.dll -- Mindmaker's own SAPI4 layer for FlexVoice 2.0 --
calls into the engine, and read the constants it passes.

The v2 Language enum's values are not recoverable from the export table and the
engine DLL contains no language-name strings. But FVWrapper.dll is a working
client of createEngine(IOutputSite*, const Speaker&, Language), so the language
value it pushes is the value that works.

Approach: resolve the import thunk for the symbol, find every
`call dword ptr [thunk]` (FF 15 imm32) in the image, and print the instruction
bytes leading up to it, where the arguments were pushed.
"""

import struct
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0] if "\\" in __file__ else ".")


def load(path):
    with open(path, "rb") as f:
        return f.read()


def pe(d):
    off = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, off + 6)[0]
    optsz = struct.unpack_from("<H", d, off + 20)[0]
    opt = off + 24
    base = struct.unpack_from("<I", d, opt + 28)[0]
    imp_rva, imp_size = struct.unpack_from("<II", d, opt + 96 + 8)
    secs = []
    sec0 = opt + optsz
    for i in range(nsec):
        o = sec0 + i * 40
        name = d[o:o + 8].rstrip(b"\0").decode("latin1")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", d, o + 8)
        secs.append((name, vaddr, vsize, rawptr, rawsize))
    return base, imp_rva, secs


def r2o(secs, rva):
    for _, vaddr, vsize, rawptr, rawsize in secs:
        if vaddr <= rva < vaddr + max(vsize, rawsize):
            return rawptr + (rva - vaddr)
    return None


def cstr(d, off):
    end = d.index(b"\0", off)
    return d[off:end].decode("latin1")


def imports(d, base, imp_rva, secs):
    """Return {symbol_name: iat_rva}."""
    out = {}
    o = r2o(secs, imp_rva)
    while True:
        oft, tstamp, fwd, name_rva, first = struct.unpack_from("<IIIII", d, o)
        if name_rva == 0:
            break
        dll = cstr(d, r2o(secs, name_rva))
        lookup = oft if oft else first
        lo, io = r2o(secs, lookup), first
        i = 0
        while True:
            ent = struct.unpack_from("<I", d, lo + 4 * i)[0]
            if ent == 0:
                break
            if not (ent & 0x80000000):
                nm = cstr(d, r2o(secs, ent) + 2)
                out[nm] = io + 4 * i
            i += 1
        o += 20
    return out


def main():
    path = sys.argv[1]
    want = sys.argv[2] if len(sys.argv) > 2 else "createEngine"
    d = load(path)
    base, imp_rva, secs = pe(d)
    imps = imports(d, base, imp_rva, secs)

    hits = [(n, r) for n, r in imps.items() if want in n]
    if not hits:
        print("no import matching %r in %s" % (want, path))
        print("imports from the engine DLL:")
        for n in sorted(imps):
            if "MM_TTSAPI" in n:
                print("   ", n)
        return

    text = [s for s in secs if s[0] == ".text"][0]
    _, tvaddr, tvsize, trawptr, trawsize = text
    body = d[trawptr:trawptr + max(tvsize, trawsize)]

    for name, iat_rva in hits:
        va = base + iat_rva
        pat = b"\xFF\x15" + struct.pack("<I", va)
        print("\n=== %s" % name)
        print("    IAT rva=0x%06x  va=0x%08x" % (iat_rva, va))
        n = 0
        start = 0
        while True:
            i = body.find(pat, start)
            if i < 0:
                break
            start = i + 1
            n += 1
            ctx = body[max(0, i - 48):i]
            print("    call at .text+0x%06x; %d bytes before:" % (i, len(ctx)))
            print("      " + " ".join("%02x" % b for b in ctx))
            # push imm32 = 68 xx xx xx xx ; push imm8 = 6A xx
            pushes = []
            j = 0
            while j < len(ctx):
                if ctx[j] == 0x68 and j + 5 <= len(ctx):
                    pushes.append(("push imm32", struct.unpack_from("<i", ctx, j + 1)[0]))
                    j += 5
                elif ctx[j] == 0x6A and j + 2 <= len(ctx):
                    pushes.append(("push imm8", struct.unpack_from("<b", ctx, j + 1)[0]))
                    j += 2
                else:
                    j += 1
            for kind, val in pushes:
                print("        %-11s %d (0x%x)" % (kind, val, val & 0xFFFFFFFF))
        if n == 0:
            print("    (no direct call sites found)")


if __name__ == "__main__":
    main()
