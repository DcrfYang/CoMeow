# M0 probe 14 -- find the APPEND helper for the 24-byte container, done right.
#
# Probe 13 was wrong, and the mistake is worth recording: it prefiltered function
# bodies on the four-byte pattern `0C 00 00 00` / `10 00 00 00`, i.e. it assumed
# the displacement would be encoded as a disp32. MSVC emits a ONE-BYTE
# displacement for every offset below 0x80 whenever the base register allows it
# (`inc dword ptr [rcx+0Ch]` is `FF 41 0C`), so the prefilter threw away
# essentially every real candidate and left 4 false positives.
#
# Correct key, in two parts:
#   1. the WRITE (operand access flag, not a mnemonic guess), and
#   2. the displacement +0x0C for the count, with +0x10 (data) or +0x08 (cap)
#      written in the same function off the SAME base register.
# That is base-register independent, which is the property that matters: the
# call site's own addressing (folded constants, whatever base it picked) is not
# part of the test.
#
# Reports each candidate with its call-site count and its first instructions.
import re, struct
from collections import Counter
import capstone, pefile

EXE = r"D:\software\steam\Steam\steamapps\common\Mewgenics\Mewgenics.exe"
BASE = 0x140000000

pe = pefile.PE(EXE, fast_load=True)
sections = {s.Name.rstrip(b'\x00').decode(): s for s in pe.sections}
text_s = sections['.text']
TEXT = pe.__data__[text_s.PointerToRawData: text_s.PointerToRawData + text_s.SizeOfRawData]
TEXT_VA = BASE + text_s.VirtualAddress

exc = pe.OPTIONAL_HEADER.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']]
def rva_off(rva):
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + max(s.SizeOfRawData, s.Misc_VirtualSize):
            return s.PointerToRawData + (rva - s.VirtualAddress)
raw = pe.__data__[rva_off(exc.VirtualAddress): rva_off(exc.VirtualAddress) + exc.Size]
FUNCS = sorted((b + BASE, e + BASE) for b, e, _ in
               (struct.unpack('<III', raw[i:i+12]) for i in range(0, len(raw) - 11, 12)) if b)
print("functions: %d" % len(FUNCS))

# --- one pass for call-site counts ------------------------------------------
calls = Counter()
for k in range(len(TEXT) - 5):
    if TEXT[k] == 0xE8:
        calls[TEXT_VA + k + 5 + struct.unpack_from('<i', TEXT, k + 1)[0]] += 1
print("call sites indexed: %d distinct targets" % len(calls))

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True

def mem_writes(body, va):
    """[(disp, base_reg_name, mnemonic, address)] for writes to [base+disp]."""
    out = []
    for ins in md.disasm(body, va):
        try: ops = ins.operands
        except capstone.CsError: break
        if len(ops) < 1 or ops[0].type != capstone.x86.X86_OP_MEM: continue
        if not (ops[0].access & capstone.CS_AC_WRITE): continue
        base = ops[0].mem.base
        if base == capstone.x86.X86_REG_INVALID: continue
        out.append((ops[0].mem.disp, ins.reg_name(base) if base else '', ins, ops))
    return out

cands = []
scanned = 0
for b, e in FUNCS:
    size = e - b
    if size > 0x400 or size < 16: continue
    o = b - TEXT_VA
    body = TEXT[o:o + size]
    if b"\x0c" not in body: continue          # cheap: the count displacement byte
    scanned += 1
    ws = mem_writes(body, b)
    if not ws: continue
    # group by base register: the three fields must be off ONE base
    by_reg = {}
    for disp, reg, ins, ops in ws:
        by_reg.setdefault(reg, []).append(disp)
    for reg, disps in by_reg.items():
        if 0xC in disps and (0x10 in disps or 0x8 in disps):
            cands.append((b, size, reg, sorted(set(disps)), calls.get(b, 0)))

print("scanned bodies with a 0x0C byte: %d   candidates: %d\n" % (scanned, len(cands)))

cands.sort(key=lambda c: -c[4])
for b, size, reg, disps, n in cands[:14]:
    print("fn 0x%X  size %-5d base=%-4s disps=%s  call sites=%d"
          % (b, size, reg, [hex(d) for d in disps], n))

print("\n=== the top candidates, disassembled ===")
for b, size, reg, disps, n in cands[:4]:
    print("\n--- fn 0x%X (size %d, base %s, %d call sites) ---" % (b, size, reg, n))
    o = b - TEXT_VA
    shown = 0
    for ins in md.disasm(TEXT[o:o + min(size, 0x100)], b):
        print("   0x%X  %-10s %s" % (ins.address, ins.mnemonic, ins.op_str))
        shown += 1
        if shown >= 26: break
