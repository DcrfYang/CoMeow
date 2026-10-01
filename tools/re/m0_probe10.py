# M0 probe 10 -- who BUILDS the battle character list.
#
# probe 9's result is the clue: the only function that writes the list header
# fields is 0x1401B2130, zeroing all 24 bytes -- a constructor/clear. Nothing
# writes count or data through a direct [reg+0x1F9C]/[reg+0x1FA0] store, which
# means the append goes through a helper call whose argument is the vector's
# ADDRESS, taken with `lea`. probe 5 already saw that shape:
#     lea rcx,[rbp+1F90h] ; call <helper>
#
# So: collect every lea-address-of-+0x1F90 and the call that follows it, then
# classify each distinct target by what IT writes (count at +0x0C, data at
# +0x10 relative to its argument = the append; +0x00/+0x08 = the clear).
#
# section B does the same attribution job for the scene-name strings, which is
# M0-1's code-level half: does a "pick your cats" screen exist at all.
import lief, capstone, struct
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

import pefile
pe = pefile.PE(EXE, fast_load=True)
exc = pe.OPTIONAL_HEADER.DATA_DIRECTORY[
        pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']]
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
def disas(va, span=0x6000):
    f = func_of(va)
    if not f: return []
    b, e = f
    return list(md.disasm(text[b-text_va: b-text_va + min(span, e-b)], b))

# --- A. lea &list ; call helper -------------------------------------------
print("=== A. 'lea reg,[base+0x1F90]' sites and the call that follows ===")
pat = (0x1F90).to_bytes(4, 'little')
lea_sites = []
i = text.find(pat)
while i != -1:
    for back in (3, 4):
        j = i - back
        if j < 1: continue
        rex_ok = (0x40 <= text[j] <= 0x4F) if back == 4 else True
        op = text[j+1] if back == 4 else text[j]
        modrm = text[j+2] if back == 4 else text[j+1]
        if not rex_ok or op != 0x8D: continue
        if (modrm >> 6) & 3 != 2: continue
        if (modrm & 7) in (4, 5): continue
        lea_sites.append((text_va + j, modrm & 7))
        break
    i = text.find(pat, i + 1)
print("  %d site(s)" % len(lea_sites))

def call_after(va, window=32):
    b = func_of(va)
    insns = disas(va, span=window)
    for k, ins in enumerate(insns):
        if ins.address >= va and ins.mnemonic == 'call' and ins.op_str.startswith('0x'):
            return int(ins.op_str, 16), ins.address
        if ins.address > va + window: break
    return None, None

targets = Counter()
for va, base in lea_sites:
    tgt, at = call_after(va)
    if tgt: targets[tgt] += 1
print("  distinct callee(s): %d" % len(targets))
for tgt, n in targets.most_common(12):
    insns = disas(tgt, span=0x1200)
    writes = []
    for ins in insns:
        try: ops = ins.operands
        except capstone.CsError: continue
        if not ops: continue
        d = ops[0]
        if d.type == capstone.x86.X86_OP_MEM and ins.mnemonic.startswith(('mov','or','and','add','sub')):
            if 0 <= d.mem.disp <= 0x28:
                writes.append("+0x%X:%s" % (d.mem.disp, ins.mnemonic))
    sizes = Counter(writes)
    print("   0x%X  used %d time(s)  writes to arg: %s"
          % (tgt, n, dict(list(sizes.items())[:8])))

# --- B. scene-name references, attributed to functions ---------------------
print("\n=== B. scene-name strings: references, attributed (M0-1) ===")
for name in ("ActSelection", "ClassChooser", "StorageItems", "House",
             "SaveSelectionScreen", "Cutscene", "MapScreen"):
    pat = name.encode() + b"\x00"
    for sname, (sva, sdata) in secs.items():
        k = sdata.find(pat)
        while k != -1:
            target = sva + k
            refs = []
            for m in range(len(text) - 7):
                # lea reg,[rip+disp32]
                for pre in (2, 3, 4):
                    j = m - pre
                    if j < 0: continue
                    if pre == 4 and not (0x40 <= text[j] <= 0x4F): continue
                    op = text[j+1] if pre == 4 else text[j]
                    modrm = text[j+2] if pre == 4 else text[j+1]
                    if op != 0x8D or (modrm & 0xC7) != 0x05: continue
                    disp = int.from_bytes(text[j+pre:j+pre+4], 'little', signed=True)
                    site = text_va + j
                    if site + 7 + disp == target:
                        refs.append(site)
            fns = sorted({func_of(r)[0] for r in refs if func_of(r)})
            print("  %-20s @0x%X refs=%d fns=%s"
                  % (name, target, len(refs), ["0x%X" % x for x in fns[:5]]))
            k = sdata.find(pat, k + 1)
            break        # first occurrence is enough for this calibration
