# M0 probe 13 -- find the APPEND helper for the 24-byte container type.
#
# Why this is the right instrument where displacement scanning was not:
# the battle list lives at +0x1F90 of a big object, and code reaches it through
# whatever base register the compiler chose -- very often with the constant
# folded into the base (base=obj+0x1000, disp=0xF90). So the displacement 0x1F90
# is the WRONG key.
#
# The container TYPE is the right key. Its header is 24 bytes:
#     { u32 refcount@0, u32 pad@4, u32 cap@8, u32 count@12, T** data@16 }
# so whatever *helper* appends to such a container must, in its own body, write
# a small constant displacement off its argument register:
#     count  at +0x0C   and   data at +0x10   (and cap at +0x08 when it grows)
# That shape is base-independent, which is exactly what is needed.
#
# Output: small functions that write +0x0C AND +0x10 (append candidates), with
# their call-site counts, so the hot one can be told from the rest.
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
print("functions: %d" % len(FUNCS))

WRITE_MNEM = ('mov', 'movups', 'movaps', 'movdqu', 'movdqa', 'or', 'and', 'add', 'sub', 'inc')

cands = []
scanned = 0
for b, e in FUNCS:
    size = e - b
    if size > 0x300 or size < 16:
        continue
    o = b - text_va
    body = text[o:o+size]
    # cheap prefilter: the body must contain a disp32 0x0C and a disp32 0x10
    if b"\x0c\x00\x00\x00" not in body or b"\x10\x00\x00\x00" not in body:
        continue
    scanned += 1
    writes = Counter()
    disp_writes = {}
    insns = list(md.disasm(body, b))
    for ins in insns:
        try: ops = ins.operands
        except capstone.CsError: continue
        if not ops or ops[0].type != capstone.x86.X86_OP_MEM: continue
        d = ops[0].mem.disp
        if d in (0x8, 0xC, 0x10):
            writes[d] += 1
            disp_writes.setdefault(d, ins)
    if 0xC in writes and 0x10 in writes:
        cands.append((b, size, writes, insns))

print("prefiltered bodies: %d   append-shaped candidates: %d" % (scanned, len(cands)))

def calls_to(target):
    n = 0
    for k in range(0, len(text) - 5):
        if text[k] != 0xE8: continue
        rel = int.from_bytes(text[k+1:k+5], 'little', signed=True)
        if text_va + k + 5 + rel == target:
            n += 1
    return n

print("\n=== append-shaped candidates (writes +0xC and +0x10 off their argument) ===")
for b, size, writes, insns in sorted(cands, key=lambda c: -c[2][0x10])[:12]:
    n = calls_to(b)
    print("\n  fn 0x%X  size %d  writes %s  call sites %d"
          % (b, size, dict(writes), n))
    for ins in insns[:14]:
        print("      0x%X  %-8s %s" % (ins.address, ins.mnemonic, ins.op_str))
