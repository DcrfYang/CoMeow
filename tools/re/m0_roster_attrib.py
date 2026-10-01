# M0 probe 9 -- attribute the battle character list (+0x1F90) to real functions.
#
# Why the previous scans failed: a raw displacement scan cannot tell one
# structure's +0x1F90 from another's, and disassembling from a hit in the middle
# of an instruction produces garbage. Two instruments fix both:
#
#   1. .pdata (the PE exception directory) lists EVERY function's [begin, end)
#      RVA. So an address can be attributed to a function exactly, with no
#      heuristics and no IDA.
#   2. capstone, started at the FUNCTION START and walked forward, so every
#      instruction boundary is correct -- and each instruction's memory operand
#      says whether it is a READ or a WRITE of the field.
#
# Layout being hunted (mgmp_lockstep.cpp's own constants, so build-current):
#   header 0x1F90 { refcount@1F90, pad@1F94, cap@1F98, count@1F9C, data@1FA0 }
#   chain  TurnControl+0x18 -> +0x08 -> +0x20 -> +0x1F90
import lief, capstone, struct
from bisect import bisect_right

BASE = 0x140000000
EXE  = r"D:\software\steam\Steam\steamapps\common\Mewgenics\Mewgenics.exe"

img = lief.parse(EXE)
secs = {}
for s in img.sections:
    try: secs[s.name] = (s.virtual_address + BASE, bytes(s.content))
    except Exception: pass
text_va, text = secs['.text']

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True

# --- .pdata: every function boundary in the image ---------------------------
def load_pdata():
    try:
        import pefile
    except ImportError:
        return []
    pe = pefile.PE(EXE, fast_load=True)
    dirs = pe.OPTIONAL_HEADER.DATA_DIRECTORY
    exc = dirs[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']]
    if exc.VirtualAddress == 0:
        return []
    # map RVA -> file offset via section headers
    def rva_off(rva):
        for s in pe.sections:
            if s.VirtualAddress <= rva < s.VirtualAddress + max(s.SizeOfRawData, s.Misc_VirtualSize):
                return s.PointerToRawData + (rva - s.VirtualAddress)
        return None
    off = rva_off(exc.VirtualAddress)
    data = pe.__data__[off: off + exc.Size]
    out = []
    for i in range(0, len(data) - 11, 12):
        begin, end, _ = struct.unpack('<III', data[i:i+12])
        if begin:
            out.append((begin + BASE, end + BASE))
    out.sort()
    pe.close()
    return out

FUNCS = load_pdata()
FUNC_STARTS = [f[0] for f in FUNCS]
print("functions in .pdata: %d" % len(FUNCS))

def func_of(va):
    i = bisect_right(FUNC_STARTS, va) - 1
    if i < 0: return None
    b, e = FUNCS[i]
    return (b, e) if b <= va < e else None

def disas_func(va, limit=6000):
    """Disassemble from the function start that contains `va`, stopping when the
    function's .pdata end is passed."""
    f = func_of(va)
    if not f: return []
    b, e = f
    o = b - text_va
    out = []
    for insn in md.disasm(text[o: o + min(0x8000, e - b)], b):
        out.append(insn)
        if len(out) >= limit: break
    return out

def mem_write_of(insn, disp):
    """True if some operand is a memory WRITE at [reg+disp]."""
    try:
        ops = insn.operands
    except capstone.CsError:
        return None
    if not ops: return None
    dst = ops[0]
    if dst.type != capstone.x86.X86_OP_MEM: return None
    if dst.mem.disp != disp: return None
    if insn.mnemonic.startswith('mov') or insn.mnemonic in ('movups','movdqu','movaps',
                                                            'movdqa','lea','add','sub','or','and'):
        return 'write' if insn.mnemonic != 'lea' else 'addr'
    return None

def mem_read_of(insn, disp):
    try:
        ops = insn.operands
    except capstone.CsError:
        return None
    for op in ops[1:] + (ops[:1] if ops else []):
        if op.type == capstone.x86.X86_OP_MEM and op.mem.disp == disp:
            return 'mem'
    return None

FIELDS = {0x1F90: 'refcount', 0x1F94: 'pad', 0x1F98: 'cap', 0x1F9C: 'count', 0x1FA0: 'data'}

def scan_fields(disps, label, require_writes=True):
    print("\n=== %s: %s ===" % (label, ", ".join("+0x%X=%s" % (d, FIELDS.get(d, '?')) for d in disps)))
    # candidate functions: any function whose bytes contain one of the disp32s
    cand = set()
    for d in disps:
        pat = d.to_bytes(4, 'little')
        i = text.find(pat)
        while i != -1:
            f = func_of(text_va + i)
            if f: cand.add(f[0])
            i = text.find(pat, i + 1)
    print("  candidate function(s): %d" % len(cand))

    rows = []
    for fstart in sorted(cand):
        write_hits, read_hits, calls = [], [], []
        for insn in disas_func(fstart):
            for d in disps:
                w = mem_write_of(insn, d)
                if w == 'write': write_hits.append((insn.address, d, insn.mnemonic, insn.op_str))
                elif w == 'addr': write_hits.append((insn.address, d, 'lea', insn.op_str))
                elif mem_read_of(insn, d): read_hits.append((insn.address, d, insn.mnemonic, insn.op_str))
            if insn.mnemonic == 'call' and insn.op_str.startswith('0x'):
                try: calls.append(int(insn.op_str, 16))
                except ValueError: pass
        if require_writes and not write_hits:
            continue
        rows.append((fstart, write_hits, read_hits, calls))

    print("  function(s) with a WRITE/lea: %d" % len(rows))
    for fstart, w, r, calls in rows[:20]:
        print("\n  fn 0x%X   (%d write/lea, %d read)  calls: %s"
              % (fstart, len(w), len(r), ["0x%X" % c for c in calls[:6]]))
        for a, d, m, o in w[:8]:
            print("      0x%X  +0x%X %-8s %s %s" % (a, d, FIELDS.get(d, '?'), m, o))
    return rows

scan_fields([0x1F90, 0x1F98, 0x1F9C, 0x1FA0], "battle character list header + writes")
