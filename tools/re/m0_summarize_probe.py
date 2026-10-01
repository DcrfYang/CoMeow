# Summarise a PROBE log: keep only the hits that describe a REAL container, then
# attribute each distinct write site.
#
# WHY THE FILTER IS NOT OPTIONAL. A watchpoint only watches an ADDRESS. After the
# battle ends the vector object is freed and the allocator hands the same bytes to
# somebody else, so every later write to that memory traps -- and the probe keeps
# dutifully printing whatever is there, which by then is another object's data:
#
#   hit 788 [vec.data] count=6357087 cap=7471222 data=0x20900690070   <- not a container
#
# Those rows are noise, and there were 136 of them. They are also the reason the
# probe needs a sanity check before arming (NOT a reason to trust the noise).
#
# A row is kept only if it could be this container: cap in (0, 4096], count <= cap,
# data != 0. That is exactly the predicate a container of Character* satisfies and
# recycled memory almost never does.
#
# usage: m0_summarize_probe.py <logfile> [base_hex]
import re, sys, struct
from collections import Counter, defaultdict
import capstone, pefile

EXE = r"D:\software\steam\Steam\steamapps\common\Mewgenics\Mewgenics.exe"

def load_pdata():
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

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True

def writer_of(hexdump, rip):
    toks = hexdump.split()
    if len(toks) < 4: return None
    code = bytes(int(t, 16) for t in toks)
    for ins in md.disasm(code, rip - 16):
        if ins.address + ins.size == rip:
            return (ins.address, ins.mnemonic, ins.op_str)
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
    base = base or 0x140000000

    HIT = re.compile(r'PROBE\s+hit\s+(\d+)\s+\[([^\]]+)\]\s+rip=0x([0-9A-Fa-f]+)\s+count=(\d+) cap=(\d+) data=0x([0-9A-Fa-f]+)')
    groups = defaultdict(lambda: {'n': 0, 'sane': 0, 'counts': Counter(),
                                  'writer': None, 'stack': None, 'bytes': None,
                                  'ids': []})
    cur = None
    for l in lines:
        m = HIT.search(l)
        if m:
            slot, rip, cnt, cap, data = m.group(2), int(m.group(3), 16), int(m.group(4)), int(m.group(5)), int(m.group(6), 16)
            cur = (slot, rip)
            g = groups[cur]
            g['n'] += 1
            if 0 < cap <= 4096 and cnt <= cap and data:
                g['sane'] += 1
                g['counts'][cnt] += 1
                if len(g['ids']) < 6: g['ids'].append(m.group(1))
            continue
        m = re.search(r'bytes @rip-16:\s*(.*)$', l)
        if m and cur:
            if groups[cur]['bytes'] is None:
                groups[cur]['bytes'] = m.group(1).strip()
            continue
        m = re.search(r'stack:\s*(.*)$', l)
        if m and cur:
            if groups[cur]['stack'] is None:
                groups[cur]['stack'] = [int(t, 16) for t in m.group(1).split()]
            continue

    print("log  : %s" % path)
    print("base : 0x%X\n" % base)
    tot = sum(g['n'] for g in groups.values())
    sane = sum(g['sane'] for g in groups.values())
    print("%d hit(s) total, %d of them describing a plausible container "
          "(the rest are the address being recycled after the battle)\n" % (tot, sane))

    for (slot, rip), g in sorted(groups.items(), key=lambda kv: -kv[1]['sane']):
        rva = rip - base
        f = func_of(rva)
        print("[%s] rip rva 0x%-7X  hits %-5d sane %-5d  counts %s" %
              (slot, rva, g['n'], g['sane'],
               dict(sorted(g['counts'].items())[:8]) if g['counts'] else '{}'))
        if g['sane'] == 0:
            continue
        if f:
            print("      in fn rva 0x%X..0x%X (+0x%X, %d bytes)" % (f[0], f[1], rva - f[0], f[1] - f[0]))
        else:
            print("      in fn <not inside .pdata -- likely not the game module>")
        if g['bytes']:
            w = writer_of(g['bytes'], rip)
            if w:
                print("      writer  0x%X (rva 0x%X)  %-10s %s" % (w[0], w[0] - base, w[1], w[2]))
        if g['stack']:
            print("      first stack:")
            for i, s in enumerate(g['stack'][:6]):
                srva = s - base
                sf = func_of(srva)
                if sf:
                    print("         [%d] rva 0x%-7X fn rva 0x%X +0x%X" % (i, srva, sf[0], srva - sf[0]))
                else:
                    print("         [%d] 0x%X (outside the game module)" % (i, srva))
        print()

if __name__ == '__main__':
    if len(sys.argv) < 2: print(__doc__); sys.exit(1)
    main()
