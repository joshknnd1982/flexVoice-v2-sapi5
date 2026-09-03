"""Read the real vtable layout out of FlexVoice_2_00_010.dll.

The DLL exports its vftables (??_7Class@@6B...), and it exports the member
functions those tables point at. So the exact virtual-function ORDER of a class
-- which the mangled names alone do not tell you -- can be recovered by reading
the table and mapping each slot back to an export RVA.

That order is what a client subclass of IWaveOutputSite has to reproduce
exactly; get it wrong and the engine calls the wrong slot.
"""

import struct
import sys


def load(path):
    with open(path, "rb") as f:
        return f.read()


def pe_info(d):
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    assert d[pe:pe + 4] == b"PE\0\0", "not a PE"
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    optsz = struct.unpack_from("<H", d, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", d, opt)[0]
    assert magic == 0x10B, "expected PE32"
    image_base = struct.unpack_from("<I", d, opt + 28)[0]
    # Export directory is data directory 0; its offset differs by magic.
    dd = opt + 96
    exp_rva, exp_size = struct.unpack_from("<II", d, dd)
    secs = []
    sec0 = opt + optsz
    for i in range(nsec):
        off = sec0 + i * 40
        name = d[off:off + 8].rstrip(b"\0").decode("latin1")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", d, off + 8)
        secs.append((name, vaddr, vsize, rawptr, rawsize))
    return image_base, exp_rva, secs


def rva_to_off(secs, rva):
    for _, vaddr, vsize, rawptr, rawsize in secs:
        if vaddr <= rva < vaddr + max(vsize, rawsize):
            return rawptr + (rva - vaddr)
    return None


def exports(d, secs, exp_rva):
    off = rva_to_off(secs, exp_rva)
    nfunc, nname = struct.unpack_from("<II", d, off + 20)
    afunc, aname, aord = struct.unpack_from("<III", d, off + 28)
    fo, no, oo = (rva_to_off(secs, afunc), rva_to_off(secs, aname),
                  rva_to_off(secs, aord))
    by_rva = {}
    by_name = {}
    for i in range(nname):
        nrva = struct.unpack_from("<I", d, no + 4 * i)[0]
        so = rva_to_off(secs, nrva)
        end = d.index(b"\0", so)
        name = d[so:end].decode("latin1")
        ordinal = struct.unpack_from("<H", d, oo + 2 * i)[0]
        frva = struct.unpack_from("<I", d, fo + 4 * ordinal)[0]
        by_rva.setdefault(frva, []).append(name)
        by_name[name] = frva
    return by_rva, by_name


def dump_vtable(d, secs, base, by_rva, by_name, sym, count=24):
    if sym not in by_name:
        print("  (no such export: %s)" % sym)
        return
    rva = by_name[sym]
    off = rva_to_off(secs, rva)
    print("\n=== %s" % sym)
    print("    vtable rva=0x%08x" % rva)
    for i in range(count):
        va = struct.unpack_from("<I", d, off + 4 * i)[0]
        if va == 0:
            print("    [%2d] 0x00000000  <null - end>" % i)
            break
        slot_rva = va - base
        # A vtable slot must point into a code section.
        if rva_to_off(secs, slot_rva) is None:
            print("    [%2d] 0x%08x  <not in image - end of table>" % (i, va))
            break
        names = by_rva.get(slot_rva)
        if names:
            print("    [%2d] rva=0x%06x  %s" % (i, slot_rva, names[0]))
        else:
            # Unexported thunk or internal function: still a real slot.
            print("    [%2d] rva=0x%06x  (not exported)" % (i, slot_rva))


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "engine/FlexVoice_2_00_010.dll"
    d = load(path)
    base, exp_rva, secs = pe_info(d)
    by_rva, by_name = exports(d, secs, exp_rva)
    print("image base 0x%08x, %d exported rvas" % (base, len(by_rva)))

    wanted = [s for s in by_name if s.startswith("??_7") and
              ("OutputSite" in s or "Notif" in s or "OutputFormat" in s)]
    for sym in sorted(wanted):
        dump_vtable(d, secs, base, by_rva, by_name, sym)


if __name__ == "__main__":
    main()
