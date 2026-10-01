# Identify the class of the object the mod calls "the holder":
#   read its vtable out of the live process, rebase it to an RVA, then find the
#   code that STORES that vtable -- which is the class's constructor.
#
# That matters because the constructor is the one moment the object's address is
# handed to somebody (rcx), i.e. the earliest possible place to arm a watchpoint
# on a field of a per-battle object. If the holder's class turns out to be the big
# 6609-byte object at 0x1401B2130, the whole race disappears.
#
# usage: m0_ident_holder.py <pid> <holder_hex> <base_hex>
import ctypes, struct, sys
import capstone, pefile

k32 = ctypes.WinDLL('kernel32', use_last_error=True)

def rd(h, addr, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t(0)
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)) \
       or got.value != n:
        raise OSError('read 0x%X failed (%d)' % (addr, ctypes.get_last_error()))
    return buf.raw

EXE = r"D:\software\steam\Steam\steamapps\common\Mewgenics\Mewgenics.exe"
IMAGEBASE = 0x140000000

pe = pefile.PE(EXE, fast_load=True)
secs = {s.Name.rstrip(b'\x00').decode(): s for s in pe.sections}
ts = secs['.text']
TEXT = pe.__data__[ts.PointerToRawData: ts.PointerToRawData + ts.SizeOfRawData]
TEXT_VA = IMAGEBASE + ts.VirtualAddress
exc = pe.OPTIONAL_HEADER.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']]
def rva_off(rva):
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + max(s.SizeOfRawData, s.Misc_VirtualSize):
            return s.PointerToRawData + (rva - s.VirtualAddress)
raw = pe.__data__[rva_off(exc.VirtualAddress): rva_off(exc.VirtualAddress) + exc.Size]
FUNCS = sorted((b + IMAGEBASE, e + IMAGEBASE) for b, e, _ in
               (struct.unpack('<III', raw[i:i+12]) for i in range(0, len(raw) - 11, 12)) if b)
def fn_of(va):
    for b, e in FUNCS:
        if b <= va < e: return (b, e)
    return None

def rva_name(rva):
    """Which section is this RVA in, and what does the name table say?"""
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + max(s.SizeOfRawData, s.Misc_VirtualSize):
            return s.Name.rstrip(b'\x00').decode()
    return '?'

def rtti_of_vtable(vt_rva):
    """vtable[-8] -> COL -> TypeDescriptor -> mangled name."""
    col_rva = vt_rva - 8
    blob = pe.get_data(col_rva, 0x18)
    if len(blob) < 0x18: return None
    sig, off, cd_off, td_rva = struct.unpack('<IIII', blob[:16])
    if sig != 1: return None
    td = pe.get_data(td_rva, 0x18)
    if len(td) < 0x18: return None
    name_ptr, name_rva = struct.unpack('<II', td[0x10:0x18])
    if not name_rva: return None
    raw_name = pe.get_data(name_rva, 512).split(b'\x00')[0]
    try: return raw_name.decode('ascii', 'replace')
    except Exception: return repr(raw_name)

def main():
    pid   = int(sys.argv[1])
    holder = int(sys.argv[2], 16)
    base  = int(sys.argv[3], 16)
    h = k32.OpenProcess(0x0010 | 0x0400, False, pid)
    if not h: raise OSError('OpenProcess failed %d' % ctypes.get_last_error())

    vt_abs = struct.unpack('<Q', rd(h, holder, 8))[0]
    vt_rva = vt_abs - base
    print("holder 0x%X  vtable 0x%X  -> rva 0x%X  (section %s)"
          % (holder, vt_abs, vt_rva, rva_name(vt_rva)))
    name = rtti_of_vtable(vt_rva)
    print("RTTI  : %s" % (name if name else '(no valid COL / not a polymorphic class)'))

    print("\nfields (first 12 qwords):")
    for i in range(12):
        v = struct.unpack('<Q', rd(h, holder + i * 8, 8))[0]
        print("   +0x%03X 0x%X" % (i * 8, v))
    v = struct.unpack('<Q', rd(h, holder + 0x1F90, 8))[0]
    print("   +0x1F90 0x%X   <-- the cat vector pointer" % v)

    # who STORES this vtable?  lea rax,[rip+disp] followed (within a few
    # instructions) by a store of that register.
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
    print("\n=== code that stores this vtable (the constructor) ===")
    target_abs = IMAGEBASE + vt_rva
    hits = []
    for b, e in FUNCS:
        if e - b > 0x8000: continue
        for ins in md.disasm(TEXT[b - TEXT_VA: b - TEXT_VA + (e - b)], b):
            if ins.mnemonic != 'lea': continue
            try: ops = ins.operands
            except capstone.CsError: break
            if len(ops) < 2 or ops[1].type != capstone.x86.X86_OP_MEM: continue
            mem = ops[1].mem
            if mem.base != capstone.x86.X86_REG_RIP: continue
            if ins.address + ins.size + mem.disp == target_abs:
                hits.append((ins.address, b, ins.reg_name(ops[0].reg)))
    for a, f, reg in hits[:12]:
        print("   lea %-4s at 0x%X   in fn 0x%X  (fn size %d)" % (reg, a, f, fn_of(a)[1] - f))
    if not hits:
        print("   none -- the vtable is never stored by a lea (it may be copied, or come from .rdata data)")

if __name__ == '__main__':
    if len(sys.argv) < 4:
        print(__doc__); sys.exit(1)
    main()
