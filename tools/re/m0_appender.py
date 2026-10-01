# The two functions the probe named, disassembled from their ENTRIES, plus their
# call sites -- because "who appends to the roster" is answered by the callers of
# the appender, and the initial fill is one of them.
#
#   fn 0x96B49A  installs the vector pointer into the holder and resets count to 0
#   fn 0x96B52B  increments count  <- the append
#
# Both were found by a hardware watchpoint on the count field, not by guessing at
# displacements, so they are known-good addresses rather than candidates.
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

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True

def callers_of(target):
    out = []
    for k in range(len(TEXT) - 5):
        if TEXT[k] != 0xE8: continue
        if TEXT_VA + k + 5 + struct.unpack_from('<i', TEXT, k + 1)[0] == target:
            out.append(TEXT_VA + k)
    return out

def show(va, label, limit=0x120):
    f = fn_of(va)
    print("=" * 78)
    print("%s : fn 0x%X .. 0x%X   (size %d)" % (label, f[0], f[1], f[1] - f[0]))
    print("=" * 78)
    n = 0
    for ins in md.disasm(TEXT[f[0] - TEXT_VA: f[0] - TEXT_VA + min(f[1] - f[0], limit)], f[0]):
        print("   0x%X  %-11s %s" % (ins.address, ins.mnemonic, ins.op_str))
        n += 1
    if f[1] - f[0] > limit:
        print("   ... (%d more bytes)" % (f[1] - f[0] - limit))

    cs = callers_of(f[0])
    print("\n   call sites: %d" % len(cs))
    seen = {}
    for c in cs:
        cf = fn_of(c)
        key = (cf[0], cf[1] - cf[0])
        seen.setdefault(key, []).append(c)
    for (fb, fsz), lst in sorted(seen.items(), key=lambda kv: -len(kv[1])):
        print("     fn 0x%X (size %d): %d call(s) at %s"
              % (fb, fsz, len(lst), ', '.join('0x%X' % x for x in lst[:6])))

show(0x14096B49A, "fn 0x96B49A -- installs the pointer / resets count")
print()
show(0x14096B52B, "fn 0x96B52B -- increments count  (the append)")
