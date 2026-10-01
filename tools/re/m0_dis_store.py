# Disassemble around the one store that installs the battle list pointer:
#   0x1401B3600  mov qword ptr [rbx+0x1f90], rdi
# and report the containing function, its size, and its callers, so the site can
# be placed in the code that builds a battle.
import struct, sys
import capstone, pefile

EXE = r"D:\software\steam\Steam\steamapps\common\Mewgenics\Mewgenics.exe"
BASE = 0x140000000
SITE = 0x1401B3600

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

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True

f = fn_of(SITE)
print("site 0x%X is in fn 0x%X..0x%X (size %d)" % (SITE, f[0], f[1], f[1] - f[0]))
print("the destructor I found earlier, 0x1401B3DD0, is in fn 0x%X..0x%X"
      % fn_of(0x1401B3DD0))

# callers of the containing function
o = f[0] - TEXT_VA
size = f[1] - f[0]
callers = []
for k in range(len(TEXT) - 5):
    if TEXT[k] != 0xE8: continue
    tgt = TEXT_VA + k + 5 + struct.unpack_from('<i', TEXT, k + 1)[0]
    if tgt == f[0]:
        callers.append(TEXT_VA + k)
print("callers of fn 0x%X: %d" % (f[0], len(callers)))
for c in callers[:10]:
    cf = fn_of(c)
    print("   call at 0x%X  from fn 0x%X +0x%X" % (c, cf[0], c - cf[0]))

start = int(sys.argv[1], 16) if len(sys.argv) > 1 else SITE - 0x120
stop  = int(sys.argv[2], 16) if len(sys.argv) > 2 else SITE + 0x160
print("\n=== 0x%X .. 0x%X ===" % (start, stop))
for ins in md.disasm(TEXT[start - TEXT_VA: stop - TEXT_VA], start):
    mark = "   <== STORE" if ins.address == SITE else ""
    print("  0x%X  %-11s %s%s" % (ins.address, ins.mnemonic, ins.op_str, mark))
