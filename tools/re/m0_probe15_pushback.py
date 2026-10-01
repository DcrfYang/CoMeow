# M0 probe 15 -- the append helper, keyed on its DATA FLOW rather than on which
# offsets a function happens to touch.
#
# Probe 14 was still too loose (1188 candidates). "Writes [reg+0xC] and
# [reg+0x10] with the same base register name" is satisfied by any function that
# uses rcx for two different objects in the same body -- which is most of them.
#
# The shape that only a push_back has:
#
#     mov  eax, [rcx+0Ch]      ; count
#     cmp  eax, [rcx+8]        ; ... against capacity
#     jne  have_room
#     ... grow (realloc through the pointer at +0x10) ...
#   have_room:
#     mov  rax, [rcx+10h]      ; data pointer
#     mov  [rax+rdx*8], rsi    ; STORE THROUGH IT, indexed  <-- the marker
#     inc  dword [rcx+0Ch]     ; count++
#
# So the test is a tiny data-flow one, not an offset one:
#   1. some register D is loaded FROM [reg+0x10]
#   2. something is STORED through [D + index*8]
#   3. [reg+0x0C] is read and [reg+0x08] is read in the same body
#
# That is the array-append idiom with an 8-byte element, which is exactly the
# container the battle's cat list is.
import struct
from collections import Counter
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
print("functions: %d" % len(FUNCS))

calls = Counter()
for k in range(len(TEXT) - 5):
    if TEXT[k] == 0xE8:
        calls[TEXT_VA + k + 5 + struct.unpack_from('<i', TEXT, k + 1)[0]] += 1

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
from capstone.x86_const import X86_OP_REG, X86_OP_MEM
REG = X86_OP_REG

def regn(ins, r):
    return ins.reg_name(r) if r else ''

cands = []
for b, e in FUNCS:
    size = e - b
    if size > 0x500 or size < 24: continue
    o = b - TEXT_VA
    body = TEXT[o:o + size]
    if b"\x0c" not in body: continue

    data_regs      = set()     # registers holding the pointer from [base+0x10]
    bases_seen     = set()     # base registers we saw a +0xC or +0x8 access on
    count_read     = set()
    cap_read       = set()
    stored_through = False
    count_inc      = False

    for ins in md.disasm(body, b):
        try: ops = ins.operands
        except capstone.CsError: break
        if not ops: continue
        d = ops[0]

        # loads into a register from [base+disp]
        if d.type == REG and len(ops) > 1 and ops[1].type == capstone.x86.X86_OP_MEM:
            m = ops[1].mem
            dst = regn(ins, d.reg)
            if m.disp == 0x0C: count_read.add(regn(ins, m.base))
            if m.disp == 0x08: cap_read.add(regn(ins, m.base))
            if m.disp == 0x10: data_regs.add(dst)

        # stores to memory
        if d.type == capstone.x86.X86_OP_MEM and (d.access & capstone.CS_AC_WRITE):
            m = d.mem
            base = regn(ins, m.base)
            if base in data_regs and m.index != 0:
                stored_through = True
            if m.disp == 0x0C and ins.mnemonic in ('inc', 'add'):
                count_inc = True
            if m.disp in (0x08, 0x0C):
                bases_seen.add(base)

    if stored_through and count_inc and (count_read & cap_read):
        cands.append((b, size, calls.get(b, 0), sorted(count_read & cap_read)))

cands.sort(key=lambda c: -c[2])
print("push_back-shaped candidates: %d\n" % len(cands))
for b, size, n, regs in cands[:16]:
    print("fn 0x%X  size %-5d  base %-4s  call sites=%d" % (b, size, regs[0] if regs else '?', n))

print("\n=== disassembled ===")
for b, size, n, regs in cands[:5]:
    print("\n--- fn 0x%X (size %d, %d call sites, base %s) ---" % (b, size, n, regs))
    o = b - TEXT_VA
    shown = 0
    for ins in md.disasm(TEXT[o:o + min(size, 0xF0)], b):
        print("   0x%X  %-10s %s" % (ins.address, ins.mnemonic, ins.op_str))
        shown += 1
        if shown >= 30: break
