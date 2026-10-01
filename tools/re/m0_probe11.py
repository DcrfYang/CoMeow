# M0 probe 11 -- probe 10 rewritten, because probe 10's byte indexing was wrong
# (it reported 0 sites for patterns probes 5-7 had already found, including the
# calibration string). The lesson from this project applies to my own tools too:
# a scan that finds nothing must be shown to be able to find something.
#
# Encoding facts used, written out so the indexing is checkable:
#   lea reg,[base+disp32]   = [REX] 8D ModRM dimm32      ModRM.mod=10 (0x80 bit set)
#   ModRM.rm = base; rm 4 = SIB follows, rm 5 = rip-relative
#   rip-relative form       = [REX] 8D ModRM dimm32, ModRM.mod=00, rm=101 (0x05)
import lief, capstone, pefile, struct
from bisect import bisect_right
from collections import Counter

BASE = 0x140000000
EXE  = r"D:\software\steam\Steam\steamapps\common\Mewgenics\Mewgenics.exe"
img  = lief.parse(EXE)
secs = {}
for s in img.sections:
    try: secs[s.name] = (s.virtual_address + BASE, bytes(s.content))
    except Exception: pass
text_va, text = secs['.text']
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True

pe = pefile.PE(EXE, fast_load=True)
exc = pe.OPTIONAL_HEADER.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']]
def rva_off(rva):
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + max(s.SizeOfRawData, s.Misc_VirtualSize):
            return s.PointerToRawData + (rva - s.VirtualAddress)
raw = pe.__data__[rva_off(exc.VirtualAddress): rva_off(exc.VirtualAddress) + exc.Size]
FUNCS = sorted((b + BASE, e + BASE) for b, e, _ in
               (struct.unpack('<III', raw[i:i+12]) for i in range(0, len(raw) - 11, 12)) if b)
STARTS = [f[0] for f in FUNCS]
def func_of(va):
    i = bisect_right(STARTS, va) - 1
    if i < 0: return None
    b, e = FUNCS[i]
    return (b, e) if b <= va < e else None
def disas_from(va, span):
    f = func_of(va)
    if not f: return []
    b, e = f
    o = va - text_va
    return list(md.disasm(text[o: o + min(span, e - va)], va))

def dword(o): return int.from_bytes(text[o:o+4], 'little')

def lea_disp32_sites(disp):
    """Every [REX] 8D /r dimm32 whose displacement is `disp` and whose base is a
    plain register. Returns [(site_va, kind)]."""
    pat = disp.to_bytes(4, 'little')
    out = []
    i = text.find(pat)
    while i != -1:
        if i >= 2:
            modrm, op = text[i-1], text[i-2]
            if op == 0x8D and (modrm >> 6) & 3 == 2 and (modrm & 7) not in (4, 5):
                out.append(text_va + i - 2)
        if i >= 1:
            modrm, op = text[i-1], text[i-2] if i >= 2 else 0
        i = text.find(pat, i + 1)
    return out

def ripref_sites(target_va):
    """lea reg,[rip+disp32] sites that resolve to target_va (the calibration
    pattern: this is how probe 3 found MapScreen's reference)."""
    out = []
    for j in range(len(text) - 7):
        if text[j] not in (0x48, 0x4C): continue
        if text[j+1] != 0x8D: continue
        if (text[j+2] & 0xC7) != 0x05: continue
        disp = int.from_bytes(text[j+3:j+7], 'little', signed=True)
        if text_va + j + 7 + disp == target_va:
            out.append(text_va + j)
    return out

# --- calibration FIRST: the instrument must be able to find a known thing ----
print("=== calibration: MapScreen's RTTI string (probe 3 found exactly 1 ref) ===")
cal = None
for sname, (sva, sdata) in secs.items():
    k = sdata.find(b"MapScreen\x00")
    if k != -1:
        cal = sva + k
        print("  string @0x%X, refs=%d" % (cal, len(ripref_sites(cal))))
        break
if cal is None:
    raise SystemExit("calibration string missing -- scan is broken")

# --- A. address-of the battle list, and what is called with it --------------
print("\n=== A. lea (address of) +0x1F90, and the callee ===")
sites = lea_disp32_sites(0x1F90)
print("  lea sites: %d  %s" % (len(sites), ["0x%X" % s for s in sites[:6]]))
targets = Counter()
for s in sites:
    for ins in disas_from(s, 24):
        if ins.mnemonic == 'call' and ins.op_str.startswith('0x'):
            targets[int(ins.op_str, 16)] += 1
            break
print("  distinct callee(s): %d -> %s" % (len(targets), ["0x%X" % t for t in targets]))

for tgt, n in targets.most_common(10):
    body = disas_from(tgt, 0x1400)
    small = []
    for ins in body:
        try: ops = ins.operands
        except capstone.CsError: continue
        if ops and ops[0].type == capstone.x86.X86_OP_MEM and 0 <= ops[0].mem.disp <= 0x30:
            if ins.mnemonic.startswith(('mov', 'add', 'or', 'and', 'sub')):
                small.append("+%X:%s" % (ops[0].mem.disp, ins.mnemonic))
    print("   0x%X used %d time(s), %d insn(s); arg writes: %s"
          % (tgt, n, len(body), dict(list(Counter(small).items())[:10])))

# --- B. scene names, attributed -------------------------------------------
print("\n=== B. scene-name references (attributed to .pdata functions) ===")
for name in ("ActSelection", "ClassChooser", "StorageItems", "House",
             "SaveSelectionScreen", "Cutscene"):
    for sname, (sva, sdata) in secs.items():
        k = sdata.find(name.encode() + b"\x00")
        if k == -1: continue
        s = sva + k
        refs = ripref_sites(s)
        fns = sorted({func_of(r)[0] for r in refs if func_of(r)})
        print("  %-20s @0x%X refs=%d fns=%s" % (name, s, len(refs), ["0x%X" % x for x in fns[:5]]))
        break
