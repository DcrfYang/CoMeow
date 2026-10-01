"""Find a class's vtable from one known virtual function, then find the code that installs it.

WHY THIS EXISTS (2026-09-23). The per-peer level-up still draws the DRAWN cat on the panel.
Three instruments have already said where it is NOT: the screen object holds the cat at
nothing besides +0xA0; the run object does not hold it in its first 0x800 bytes; and the
queue getter 0x52AF0 is never called on that peer. What is left is the moment the screen is
BUILT -- the copy the panel displays is made then, which is also why replacing the subject
pointer afterwards cannot reach it (the module's own read follows, the picture does not).

There is no symbol for the constructor, but there is a handle on the class: LVLUPD is
`LevelUpScreen::update`, virtual slot 9. So the class's vtable is the table in .rdata whose
entry 9 is that function, and the constructor is the code that stores that table's address
into a fresh object (`lea reg,[rip+vt]; mov [rcx],reg`). Its caller is the thing that opens a
level-up screen -- i.e. the draw, and the argument it passes is what the panel is built from.

READ-ONLY: opens the exe, scans bytes, prints RVAs.

Usage: find_vtable_ctor.py <Mewgenics.exe> <known-fn-rva> <slot>
       e.g. 0x383210 9
"""
import struct
import sys


def sections(d):
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    optsz = struct.unpack_from("<H", d, pe + 20)[0]
    base = struct.unpack_from("<Q", d, pe + 24 + 24)[0]
    out = []
    for i in range(nsec):
        o = pe + 24 + optsz + i * 40
        name = d[o:o + 8].rstrip(b"\0").decode("latin1")
        vsz, va, rsz, ro = struct.unpack_from("<IIII", d, o + 8)
        out.append((name, va, vsz, ro, rsz))
    return base, out


def main():
    exe = sys.argv[1]
    fn_rva = int(sys.argv[2], 0)
    slot = int(sys.argv[3], 0)
    d = open(exe, "rb").read()
    base, secs = sections(d)
    print("image base 0x%X, target fn rva 0x%X at vtable slot %d" % (base, fn_rva, slot))

    want = base + fn_rva
    # 1. every qword in .rdata (and .data) equal to the function's address
    cands = []
    for name, va, vsz, ro, rsz in secs:
        if name not in (".rdata", "_RDATA", ".data"):
            continue
        blob = d[ro:ro + rsz]
        for i in range(0, len(blob) - 7, 8):
            if struct.unpack_from("<Q", blob, i)[0] != want:
                continue
            entry_va = va + i
            vt_va = entry_va - slot * 8
            if vt_va < va:
                continue
            cands.append((vt_va, entry_va, name))
    print("qword hits for that address: %d" % len(cands))
    for vt_va, entry_va, name in cands[:20]:
        print("  entry at rva 0x%X (%s)  => candidate vtable rva 0x%X" % (entry_va, name, vt_va))

    # 2. code that lea's that vtable (RIP-relative): 48 8D modrm disp32, modrm rm=101
    tva = tro = trsz = None
    for name, va, vsz, ro, rsz in secs:
        if name == ".text":
            tva, tro, trsz = va, ro, rsz
    blob = d[tro:tro + trsz]
    print("--- lea sites for each candidate vtable ---")
    for vt_va, _entry_va, _name in cands[:20]:
        shown = 0
        for i in range(len(blob) - 7):
            if blob[i] != 0x48 or blob[i + 1] != 0x8D:
                continue
            modrm = blob[i + 2]
            if (modrm & 0xC7) != 0x05:          # mod=00, rm=101 => RIP-relative
                continue
            disp = struct.unpack_from("<i", blob, i + 3)[0]
            site = tva + i
            if site + 7 + disp != vt_va:
                continue
            ctx = d[tro + i - 12:tro + i + 12]
            print("  vt 0x%X  lea at rva 0x%06X   before/after: %s"
                  % (vt_va, site, " ".join("%02X" % b for b in ctx)))
            shown += 1
            if shown >= 6:
                break
    return 0


if __name__ == "__main__":
    sys.exit(main())
