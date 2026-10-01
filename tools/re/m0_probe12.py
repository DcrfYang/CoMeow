# M0 probe 12 -- all occurrences, and the one function that addresses the list.
#
# Two loose ends from probe 11:
#   (a) probe 11 checked only the FIRST occurrences of each scene name. "House"
#       exists three times in .rdata and probe 3 found its reference on the
#       SECOND. So: scan every occurrence.
#   (b) exactly one `lea reg,[base+0x1F90]` exists (0x1401B4156, calling
#       0x140058910). That single site is the thread to pull for M0-2, so this
#       prints its function, the surrounding instructions and its callers.
import lief, capstone, pefile, struct
from bisect import bisect_right

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

def riprefs(target):
    out = []
    for j in range(len(text) - 7):
        if text[j] not in (0x48, 0x4C) or text[j+1] != 0x8D or (text[j+2] & 0xC7) != 0x05:
            continue
        disp = int.from_bytes(text[j+3:j+7], 'little', signed=True)
        if text_va + j + 7 + disp == target:
            out.append(text_va + j)
    return out

print("=== B. every occurrence of every scene name ===")
for name in ("ActSelection", "ClassChooser", "StorageItems", "House",
             "SaveSelectionScreen", "Cutscene", "MapScreen"):
    pat = name.encode() + b"\x00"
    total_refs, fns = 0, set()
    per = []
    for sname, (sva, sdata) in secs.items():
        k = sdata.find(pat)
        while k != -1:
            s = sva + k
            refs = riprefs(s)
            total_refs += len(refs)
            for r in refs:
                f = func_of(r)
                if f: fns.add(f[0])
            per.append("0x%X:%d" % (s, len(refs)))
            k = sdata.find(pat, k + 1)
    print("  %-20s occurrences=%s  total refs=%d  fns=%s"
          % (name, ",".join(per[:4]), total_refs, ["0x%X" % x for x in sorted(fns)[:5]]))

print("\n=== A. the one function that takes the address of +0x1F90 ===")
SITE = 0x1401B4156
f = func_of(SITE)
print("  site 0x%X lies in function %s" % (SITE, ("0x%X..0x%X" % f) if f else "?"))
if f:
    b, e = f
    print("  function size: %d bytes" % (e - b))
    insns = list(md.disasm(text[b-text_va: e-text_va], b))
    for ins in insns:
        if SITE - 64 <= ins.address <= SITE + 64:
            mark = "  <== lea of the battle list" if ins.address == SITE - 2 else ""
            print("     0x%X  %-8s %s%s" % (ins.address, ins.mnemonic, ins.op_str, mark))
    calls = set()
    for ins in insns:
        if ins.mnemonic == 'call' and ins.op_str.startswith('0x'):
            calls.add(int(ins.op_str, 16))
    print("  calls: %s" % ["0x%X" % c for c in sorted(calls)][:12])

    # who calls THIS function
    callers = []
    for k in range(len(text) - 5):
        if text[k] != 0xE8: continue
        rel = int.from_bytes(text[k+1:k+5], 'little', signed=True)
        if text_va + k + 5 + rel == b:
            callers.append(text_va + k)
    print("  called from %d site(s): %s" % (len(callers), ["0x%X" % c for c in callers[:10]]))
    cfs = sorted({func_of(c)[0] for c in callers if func_of(c)})
    print("  caller function(s): %s" % ["0x%X" % c for c in cfs[:10]])
