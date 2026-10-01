# M0 probe 6 -- corrected ModRM decoding.
#
# probe 5's "0 lea sites for +0x5B8" was a bug in MY instrument, not a finding:
# for mod=10 (disp32), rm=101 means rip-relative and rm=100 means "SIB follows".
# Every OTHER rm value is a base register -- which is the ordinary
# `lea rcx,[rax+5B8h]` shape (ModRM 0x88), and probe 5 had demanded rm=101.
#
# So this pass re-derives both scans with the rule written out properly:
#   mod=10, rm in {0,1,2,3,5,6,7}  -> [base+disp32]      (base = rm, mapped)
#   mod=10, rm=4                   -> SIB [+disp32]      (base from the SIB byte)
import lief

BASE = 0x140000000
EXE  = r"D:\software\steam\Steam\steamapps\common\Mewgenics\Mewgenics.exe"
img  = lief.parse(EXE)
secs = {}
for s in img.sections:
    try: secs[s.name] = (s.virtual_address + BASE, bytes(s.content))
    except Exception: pass
text_va, text = secs['.text']

REGS = ['rax','rcx','rdx','rbx','rsp','rbp','rsi','rdi',
        'r8','r9','r10','r11','r12','r13','r14','r15']

def decode_disp(off):
    """At `off` sits a 4-byte displacement. Look backwards for a REX + opcode +
    ModRM whose disp32 is at off. Returns (kind, base_reg) or None."""
    for back in (2, 3):                       # rex-less, or with REX
        j = off - back                        # opcode index
        if j < 1: continue
        op   = text[j]
        modrm = text[j+1]
        rex  = text[j-1] if back == 3 else 0
        if back == 3 and not (0x40 <= rex <= 0x4F): continue
        if op not in (0x89, 0x8B, 0x8D, 0xC7, 0x3B, 0x39, 0x01, 0x03): continue
        mod = (modrm >> 6) & 3
        rm  = modrm & 7
        if mod != 2: continue
        if rm == 4:                           # SIB: base is the SIB's base field
            sib = text[j+2] if back == 2 else text[j+2]
            # SIB sits right after ModRM; with disp32 present it is at j+2
            sib_reg = (text[j+2] & 7)
            if sib_reg == 5 and (text[j+2] >> 6) == 0:
                return ({0x8D:'lea rip-rel', 0x8B:'mov reg,[rip]'}.get(op), 'rip')
            return ({0x8D:'lea', 0x8B:'mov', 0x89:'mov!', 0xC7:'mov [.]imm',
                     0x3B:'cmp', 0x39:'cmp!', 0x01:'add!', 0x03:'add'}.get(op),
                    REGS[((text[j+2] >> 0) & 7) + (8 if (text[j+2] >> 6) == 1 else 0)])
        if rm == 5:                           # rip-relative
            return ({0x8D:'lea rip-rel', 0x8B:'mov reg,[rip]'}.get(op), 'rip')
        base = REGS[rm + (8 if rex and (rex & 1) else 0)]
        kind = {0x8D:'lea', 0x8B:'mov', 0x89:'mov!', 0xC7:'mov [.],imm32',
                0x3B:'cmp', 0x39:'cmp!', 0x01:'add!', 0x03:'add'}.get(op)
        return (kind, base)
    return None

def scan(disp, label):
    pat = disp.to_bytes(4, 'little')
    hits = []
    i = text.find(pat)
    while i != -1:
        d = decode_disp(i)
        if d: hits.append((text_va + i, d[0], d[1]))
        i = text.find(pat, i + 1)
    print("\n=== +0x%X (%s): %d decoded reference(s) ===" % (disp, label, len(hits)))
    for va, kind, base in hits[:28]:
        print("   0x%X  %-14s base=%s" % (va, kind, base))
    return hits

scan(0x1F90, 'battle character list header')
scan(0x1F9C, 'battle character list count')
scan(0x1FA0, 'battle character list data ptr')
scan(0x5B8,  'run cat list (MewDirector+1464)')

def calls_in(va, before=8, after=40):
    out = []
    lo = max(0, va - text_va - before); hi = min(len(text) - 5, va - text_va + after)
    for k in range(lo, hi):
        if text[k] != 0xE8: continue
        rel = int.from_bytes(text[k+1:k+5], 'little', signed=True)
        out.append((text_va + k, text_va + k + 5 + rel))
    return out

print("\n=== what the lea sites call (container operations) ===")
for disp, name in ((0x1F90, 'battle list'), (0x5B8, 'run cat list')):
    pat = disp.to_bytes(4, 'little')
    i = text.find(pat)
    n = 0
    while i != -1 and n < 8:
        d = decode_disp(i)
        if d and d[0] == 'lea':
            va = text_va + i
            cs = calls_in(va)
            print("  +0x%X lea at 0x%X -> calls %s" % (disp, va, ["0x%X" % t for _, t in cs[:2]]))
            n += 1
        i = text.find(pat, i + 1)
