"""Find the DIRECT CALL SITES of a function, by scanning for E8 rel32.

WHY THIS EXISTS (2026-09-23). A hardware watchpoint on LevelUpScreen+0xA0 caught only
mgmp's own write, because the game sets that field before the screen object exists. A static
scan for stores to +0xA0 then found 0x385A90 -- a 184-byte function that takes THE CAT as its
first argument and does the whole job (it reads the old subject, then installs the new one;
the raw field write mgmp does today reaches the module's own logic but not the picture, the
stat numbers or the option roll). So the question is now "who calls it": that call site is
the draw -- "which cat levels" -- and it also shows what the third argument is.

READ-ONLY: opens the exe, scans bytes, prints RVAs. It never runs the game.

Usage: find_callers.py <Mewgenics.exe> <target-rva> [name]
"""
import struct
import sys


def sections(d):
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if d[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("not a PE image")
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    optsz = struct.unpack_from("<H", d, pe + 20)[0]
    out = []
    for i in range(nsec):
        o = pe + 24 + optsz + i * 40
        name = d[o:o + 8].rstrip(b"\0").decode("latin1")
        vsz, va, rsz, ro = struct.unpack_from("<IIII", d, o + 8)
        out.append((name, va, vsz, ro, rsz))
    return out


def main():
    exe = sys.argv[1]
    target = int(sys.argv[2], 0)
    name = sys.argv[3] if len(sys.argv) > 3 else ""

    d = open(exe, "rb").read()
    secs = sections(d)

    def rva_of(foff):
        for _n, va, _vsz, ro, rsz in secs:
            if ro <= foff < ro + rsz:
                return va + (foff - ro)
        return None

    def file_of(rva):
        for _n, va, _vsz, ro, rsz in secs:
            if va <= rva < va + rsz:
                return ro + (rva - va)
        return None

    print("target 0x%X %s" % (target, name))
    text = None
    for n, va, vsz, ro, rsz in secs:
        if n == ".text":
            text = (va, ro, rsz)
    if not text:
        raise SystemExit("no .text")
    tva, tro, trsz = text
    blob = d[tro:tro + trsz]

    hits = []
    for i in range(len(blob) - 5):
        if blob[i] != 0xE8:
            continue
        rel = struct.unpack_from("<i", blob, i + 1)[0]
        site = tva + i
        if site + 5 + rel == target:
            hits.append(site)

    print("direct E8 call sites: %d" % len(hits))
    for site in hits:
        # 24 bytes of context before the call, so the argument setup is visible.
        fo = file_of(site)
        ctx = d[fo - 24:fo + 5] if fo and fo >= 24 else b""
        print("  call at RVA 0x%06X  before: %s" % (site, " ".join("%02X" % b for b in ctx)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
