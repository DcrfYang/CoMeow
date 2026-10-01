# Disassemble a function PROPERLY: linearly, from its entry, so every address
# printed is a real instruction boundary.
#
# The lesson this file exists for: the previous window started at site-0x120 and
# produced `mov ebx, 0x1e40` followed by `[rbx+0x1e60]` -- a constant used as a
# pointer, which is impossible. The window was misaligned, and every line after
# it was fiction that still looked plausible. A disassembler will happily do that.
import struct, sys
import capstone, pefile

EXE = r"D:\software\steam\Steam\steamapps\common\Mewgenics\Mewgenics.exe"
BASE = 0x140000000

pe = pefile.PE(EXE, fast_load=True)
secs = {s.Name.rstrip(b'\x00').decode(): s for s in pe.sections}
ts = secs['.text']
TEXT = pe.__data__[ts.PointerToRawData: ts.PointerToRawData + ts.SizeOfRawData]
TEXT_VA = BASE + ts.VirtualAddress
exc = pe.OPTIONAL_HEADER.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']]
def rva_off(rva):
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + max(s.SizeOfRawData, s.Misc_VirtualSize):
            return s.PointerToRawData + (rva - s.VirtualAddress)
raw = pe.__data__[rva_off(exc.VirtualAddress): rva_off(exc.VirtualAddress) + exc.Size]
FUNCS = sorted((b + BASE, e + BASE) for b, e, _ in
               (struct.unpack('<III', raw[i:i+12]) for i in range(0, len(raw) - 11, 12)) if b)

def fn_of(va):
    for b, e in FUNCS:
        if b <= va < e: return (b, e)
    return None

def callers_of(target):
    out = []
    for k in range(len(TEXT) - 5):
        if TEXT[k] != 0xE8: continue
        if TEXT_VA + k + 5 + struct.unpack_from('<i', TEXT, k + 1)[0] == target:
            out.append(TEXT_VA + k)
    return out

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True

def sweep(va, limit=0x2000):
    f = fn_of(va)
    start = f[0]
    return start, list(md.disasm(TEXT[start - TEXT_VA: start - TEXT_VA + min(f[1] - start, limit)], start))

SITE = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x1401B3600
AROUND = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x60

start, insns = sweep(SITE)
print("fn 0x%X .. 0x%X   (%d bytes, %d instructions swept)"
      % (start, fn_of(SITE)[1], fn_of(SITE)[1] - start, len(insns)))

print("\n=== first 20 instructions (what kind of function is this) ===")
for ins in insns[:20]:
    print("  0x%X  %-11s %s" % (ins.address, ins.mnemonic, ins.op_str))

print("\n=== around 0x%X (from the aligned sweep) ===" % SITE)
for ins in insns:
    if SITE - AROUND <= ins.address <= SITE + AROUND:
        mark = "   <== THE STORE" if ins.address == SITE else ""
        print("  0x%X  %-11s %s%s" % (ins.address, ins.mnemonic, ins.op_str, mark))

# the writer of the value being stored: what is in the source register?
src = None
for ins in insns:
    if ins.address == SITE:
        src = ins.op_str.split(', ')[-1]
        idx = insns.index(ins)
        break
if src:
    print("\n=== where '%s' came from (last 12 writes to it before the store) ===" % src)
    n = 0
    for ins in reversed(insns[:idx]):
        if ins.op_str.startswith(src + ',') or ins.op_str.startswith(src + ' '):
            print("  0x%X  %-11s %s" % (ins.address, ins.mnemonic, ins.op_str))
            n += 1
            if n >= 12: break

print("\n=== callers of this function ===")
for c in callers_of(start)[:8]:
    cf = fn_of(c)
    print("  0x%X  from fn 0x%X +0x%X" % (c, cf[0], c - cf[0]))
