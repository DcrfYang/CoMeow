"""Find the code that STORES a pointer to +0xA0 -- the level-up screen's subject field.

WHY THIS EXISTS (2026-09-23). The runtime watchpoint on LevelUpScreen+0xA0 fired exactly
once, and the probe marked that hit's first caller "[other]" -- i.e. mgmp's own re-point
write. The game's own setting of that field never appeared, because it happens BEFORE the
screen object exists, and a watchpoint can only be armed on an address that exists. So the
initial write cannot be seen at runtime until the address is known, which means the static
question is the right one now:

    which instructions store a pointer to [reg+0xA0]?

The answer is a short list, and it is expected to contain the LevelUpScreen constructor --
whose CALLER is the draw ("which cat levels"), which is the thing the per-peer split needs.

READ-ONLY. It opens the exe, scans bytes, prints RVAs. It never runs the game.

Usage: scan_store_a0.py <Mewgenics.exe> [lo_rva] [hi_rva]
       defaults to the level-up cluster, 0x300000..0x400000 -- the range the notes place
       LevelUpScreen's code in (0x3786F9 and 0x383645 both read the subject).
"""
import struct
import sys
import re


def main():
    exe = sys.argv[1]
    lo = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x300000
    hi = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x400000

    d = open(exe, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if d[pe:pe + 4] != b"PE\0\0":
        print("not a PE image")
        return 1
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    optsz = struct.unpack_from("<H", d, pe + 20)[0]
    base = struct.unpack_from("<Q", d, pe + 24 + 24)[0]

    secs = []
    for i in range(nsec):
        o = pe + 24 + optsz + i * 40
        name = d[o:o + 8].rstrip(b"\0").decode("latin1")
        vsz, va, rsz, ro = struct.unpack_from("<IIII", d, o + 8)
        secs.append((name, va, vsz, ro, rsz))
    print("IMAGE BASE 0x%X, %d section(s): %s"
          % (base, nsec, ", ".join("%s(va=%x,raw=%x)" % (n, va, ro) for n, va, _, ro, _ in secs)))

    def rva_of(foff):
        for _n, va, _vsz, ro, rsz in secs:
            if ro <= foff < ro + rsz:
                return va + (foff - ro)
        return None

    # mov qword ptr [reg+disp32], reg  ==  48 89 /r with mod=10 and disp32 == 0xA0
    #   ModRM mod=10 (0x80..0xBF) and little-endian 0xA0 00 00 00 after it.
    pat = re.compile(rb"\x48\x89[\x80-\xbf]\xa0\x00\x00\x00")
    hits = []
    for m in pat.finditer(d):
        r = rva_of(m.start())
        if r is None:
            continue
        if lo <= r <= hi:
            hits.append((r, m.start()))

    print("stores to +0xA0 in 0x%X..0x%X: %d" % (lo, hi, len(hits)))
    for r, foff in hits:
        reg = d[foff + 2]
        src = "rax rcx rdx rbx rsp rbp rsi rdi r8..r15".split()
        # ModRM 0x80|reg<<3|rm: the SOURCE register is the `reg` field.
        srcname = src[(reg >> 3) & 7] if reg < 0xC0 else "r8.."
        print("  RVA 0x%06X  (file 0x%06X)  stores %s" % (r, foff, srcname))
    return 0


if __name__ == "__main__":
    sys.exit(main())
