# Turn a PROBE log into an attribution.
#
# The probe logs, for every write to the battle's cat list:
#   * the RIP that FOLLOWED the write (a data breakpoint traps after the
#     instruction), and 32 bytes of code around it, and
#   * up to eight stack qwords that point into the game's image.
#
# This resolves all of it offline:
#   * the writer instruction itself -- disassemble from rip-16 and take the
#     instruction that ENDS exactly at rip. That is the store.
#   * the containing function, from .pdata (55,712 exact [begin,end) ranges).
#   * every stack value, also as function + offset.
#
# usage: m0_resolve_probe.py <logfile> [base_in_hex]
import re, sys, glob, struct
import capstone, pefile

EXE = r"D:\software\steam\Steam\steamapps\common\Mewgenics\Mewgenics.exe"
DEFAULT_BASE = 0x140000000

def load_pdata():
    """Function ranges as RVAs, NOT as 0x140000000-based addresses.

    The first version kept them at the image base and compared them against
    RUNTIME addresses, so every lookup missed by the whole ASLR slide and every
    hit reported "not inside .pdata" -- including ones that plainly were. The
    slide is a constant, which is exactly why it is easy to forget.
    """
    pe = pefile.PE(EXE, fast_load=True)
    exc = pe.OPTIONAL_HEADER.DATA_DIRECTORY[
            pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']]
    def rva_off(rva):
        for s in pe.sections:
            if s.VirtualAddress <= rva < s.VirtualAddress + max(s.SizeOfRawData, s.Misc_VirtualSize):
                return s.PointerToRawData + (rva - s.VirtualAddress)
    raw = pe.__data__[rva_off(exc.VirtualAddress): rva_off(exc.VirtualAddress) + exc.Size]
    fns = sorted((b, e) for b, e, _ in
                 (struct.unpack('<III', raw[i:i+12]) for i in range(0, len(raw) - 11, 12)) if b)
    pe.close()
    return fns

FUNCS = load_pdata()

def func_of(rva):
    lo, hi = 0, len(FUNCS) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        b, e = FUNCS[mid]
        if rva < b:      hi = mid - 1
        elif rva >= e:   lo = mid + 1
        else:            return b, e
    return None

def exe_bytes(va, n):
    """Read `n` bytes of the exe at a VA, from the file itself."""
    rva = va - DEFAULT_BASE
    return PE_DATA.get(rva, b"\x00" * n)[:n]

def load_image():
    pe = pefile.PE(EXE, fast_load=True)
    data = pe.__data__
    out = {}
    for s in pe.sections:
        out[s.VirtualAddress] = (data[s.PointerToRawData: s.PointerToRawData + s.SizeOfRawData],
                                 s.VirtualAddress)
    pe.close()
    return out

SECT = load_image()
def section_at(rva):
    for va, (blob, _) in SECT.items():
        if va <= rva < va + len(blob):
            return blob, va
    return None, None

class PEView:
    def __getitem__(self, rva):
        blob, base = section_at(rva)
        if blob is None: return b"\x00"
        off = rva - base
        return blob[off:]

PE_DATA = PEView()

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True

def writer_instruction(hexdump, rip, rip_minus_16):
    """Disassemble the dumped bytes; report the instruction ending at rip."""
    toks = hexdump.split()
    if len(toks) < 4: return None
    code = bytes(int(t, 16) for t in toks)
    for ins in md.disasm(code, rip_minus_16):
        if ins.address + ins.size == rip:
            return "0x%X  %-10s %s" % (ins.address, ins.mnemonic, ins.op_str)
        if ins.address >= rip:
            break
    return None

def main():
    path = sys.argv[1]
    base = int(sys.argv[2], 16) if len(sys.argv) > 2 else None
    lines = open(path, encoding='utf-8', errors='replace').read().splitlines()

    if base is None:
        for l in lines:
            m = re.search(r'base\s*:\s*([0-9A-Fa-f]{8,})', l)
            if m: base = int(m.group(1), 16); break
    base = base or DEFAULT_BASE
    print("log   : %s" % path)
    print("base  : 0x%X   (rva = address - base)\n" % base)

    hits = []
    cur = None
    for l in lines:
        m = re.search(r'PROBE\s+hit\s+(\d+)\s+\[([^\]]+)\]\s+rip=0x([0-9A-Fa-f]+)\s+count=(\d+) cap=(\d+) data=0x([0-9A-Fa-f]+) vec=0x([0-9A-Fa-f]+) holder\[\+1F90\]=0x([0-9A-Fa-f]+)', l)
        if m:
            cur = {'n': int(m.group(1)), 'slot': m.group(2), 'rip': int(m.group(3), 16),
                   'count': int(m.group(4)), 'cap': int(m.group(5)),
                   'data': int(m.group(6), 16), 'vec': int(m.group(7), 16)}
            hits.append(cur)
            continue
        m = re.search(r'bytes @rip-16:\s*(.*)$', l)
        if m and cur is not None:
            cur['bytes'] = m.group(1).strip()
            continue
        m = re.search(r'stack:\s*(.*)$', l)
        if m and cur is not None:
            cur['stack'] = [int(t, 16) for t in m.group(1).split()]

    if not hits:
        print("no PROBE hits in this log (the instrument is armed but nothing has written the list)")
        return

    print("%d hit(s)\n" % len(hits))
    for h in hits:
        rip = h['rip']
        rva = rip - base
        f = func_of(rva)
        print("hit %d  slot=%s  count=%u cap=%u" % (h['n'], h['slot'], h['count'], h['cap']))
        print("   rip      rva 0x%X" % rva)
        if f:
            print("   in fn    rva 0x%X..0x%X  (+0x%X, %d bytes)"
                  % (f[0], f[1], rva - f[0], f[1] - f[0]))
        else:
            print("   in fn    <not inside .pdata>")
        if 'bytes' in h:
            w = writer_instruction(h['bytes'], rip, rip - 16)
            print("   writer   %s" % (w if w else "(could not decode -- bytes: %s)" % h['bytes']))
        for i, s in enumerate(h.get('stack', [])[:8]):
            srva = s - base
            sf = func_of(srva)
            if sf:
                print("   stack%-2d  rva 0x%-8X fn rva 0x%X +0x%X" % (i, srva, sf[0], srva - sf[0]))
            else:
                print("   stack%-2d  0x%X  (outside .text / not a function)" % (i, srva))
        print()

if __name__ == '__main__':
    if len(sys.argv) < 2:
        cands = sorted(glob.glob('mgmp_*_2*.log')   # logs in the current directory)
        print("usage: m0_resolve_probe.py <logfile>\navailable:\n  " + "\n  ".join(cands[-4:]))
        sys.exit(1)
    main()
