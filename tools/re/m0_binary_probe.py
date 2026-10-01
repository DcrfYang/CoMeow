# M0 probe 2 -- locate the code that decides WHO FIGHTS, using only anchors the
# project already trusts plus raw encoding patterns (no disassembler available).
#
#   A. the scene-name strings and their references -- what UI actually exists
#      ("ActSelection" / "ClassChooser" / "StorageItems" / "House")
#   B. call sites of CustomVector<T*>::push_back @ 0x140047FC0 that sit next to a
#      reference to the battle character list at +0x1F90 -- i.e. the appends
#   C. every reference to the battle character list fields (+0x1F90/98/9C/A0)
#   D. call sites of the accessor sub_14004A550 -- who CONSUMES the roster
#
# Everything is reported as an image-base-relative address (base 0x140000000),
# which is the form the design notes and mgmp_addresses.h use.
import sys
import lief

BASE = 0x140000000
EXE = r"D:\software\steam\Steam\steamapps\common\Mewgenics\Mewgenics.exe"

img = lief.parse(EXE)
if img is None:
    print("lief could not parse the exe")
    sys.exit(1)

secs = {}
for s in img.sections:
    try:
        data = bytes(s.content)
    except Exception:
        continue
    secs[s.name] = (s.virtual_address + BASE, data)

text_va, text = secs.get('.text', (None, None))
if text is None:
    print("no .text")
    sys.exit(1)
print("sections: %s" % ", ".join(sorted(secs)))
print(".text: va 0x%X, %d bytes" % (text_va, len(text)))

def find_dword(needle, lo=0, hi=None):
    """All file offsets of the 4-byte little-endian needle."""
    b = needle.to_bytes(4, 'little')
    out = []
    i = text.find(b, lo, hi if hi else len(text))
    while i != -1:
        out.append(i)
        i = text.find(b, i + 1, hi if hi else len(text))
    return out

def func_start(off):
    """Nearest preceding int3 run -- a crude but reliable function boundary for
    MSVC code, good enough to name a candidate for the real disassembler."""
    i = off
    while i > 0 and i > off - 0x6000:
        if text[i-1] == 0xCC and text[i-2] == 0xCC and text[i-3] == 0xCC:
            return text_va + i
        i -= 1
    return 0

def describe(off):
    """Classify the instruction whose disp32 is at `off`, from the bytes before."""
    j = off - 1
    op = text[j]
    modrm = text[j-1] if j > 0 else 0
    rex = 0
    k = j - 2
    if k >= 0 and 0x40 <= text[k] <= 0x4F:
        rex = text[k]
    mod = (modrm >> 6) & 3
    rm = modrm & 7
    if mod != 2 or rm == 4:
        return None                     # not [reg+disp32]
    kind = {0x89: 'mov [base+disp],reg', 0x8B: 'mov reg,[base+disp]',
            0x8D: 'lea reg,[base+disp]', 0xC7: 'mov [base+disp],imm32',
            0x3B: 'cmp reg,[base+disp]',  0x39: 'cmp [base+disp],reg',
            0x01: 'add [base+disp],reg',  0x03: 'add reg,[base+disp]'}.get(op)
    return kind

# --- A. scene names --------------------------------------------------------
print("\n=== A. scene-name strings and who references them ===")
for name in ("ActSelection", "ClassChooser", "StorageItems", "House",
             "SaveSelectionScreen", "Cutscene"):
    pat = name.encode('ascii') + b"\x00"
    hits = []
    for sname, (va, data) in secs.items():
        i = data.find(pat)
        while i != -1:
            hits.append((sname, va + i))
            i = data.find(pat, i + 1)
    if not hits:
        print("  %-20s NOT FOUND" % name)
        continue
    for sname, sva in hits[:3]:
        # lea reg,[rip+disp32] = 48 8D 0D/15/1D/05 + disp32   (also 4C 8D ..)
        refs = []
        for s2name, (tva, tdata) in secs.items():
            if s2name != '.text':
                continue
            for k in range(len(tdata) - 7):
                if tdata[k] not in (0x48, 0x4C):
                    continue
                if tdata[k+1] != 0x8D:
                    continue
                modrm = tdata[k+2]
                if (modrm & 0xC7) != 0x05:      # mod=00, rm=101 -> rip-relative
                    continue
                disp = int.from_bytes(tdata[k+3:k+7], 'little', signed=True)
                if tva + k + 7 + disp == sva:
                    refs.append(tva + k)
        print("  %-20s string @0x%X in %s -- %d lea reference(s)%s"
              % (name, sva, sname, len(refs),
                 (": " + ", ".join("0x%X (fn 0x%X)" % (r, func_start(r - tva))
                                   for r in refs[:4])) if refs else ""))

# --- B/C. the battle character list ---------------------------------------
print("\n=== B/C. the battle character list: +0x1F90 (data 16, count 12, cap 8) ===")
list_offsets = {0x1F90: 'data', 0x1F98: 'cap?', 0x1F9C: 'count', 0x1FA0: 'end/data+16'}
all_sites = {}
for d, label in list_offsets.items():
    for off in find_dword(d):
        kind = describe(off)
        if not kind:
            continue
        all_sites[off] = (d, label, kind)

# call sites of CustomVector<T*>::push_back @ 0x140047FC0
PB = 0x140047FC0
pb_sites = []
for k in range(len(text) - 5):
    if text[k] != 0xE8:
        continue
    rel = int.from_bytes(text[k+1:k+5], 'little', signed=True)
    if text_va + k + 5 + rel == PB:
        pb_sites.append(k)
print("  push_back call sites: %d" % len(pb_sites))

for off in sorted(all_sites):
    d, label, kind = all_sites[off]
    near_pb = [p for p in pb_sites if abs(p - off) <= 96]
    print("  0x%X  +0x%X %-8s %-22s fn 0x%X%s"
          % (text_va + off, d, label, kind, func_start(off),
             "   <-- push_back %d byte(s) away" % (off - near_pb[0]) if near_pb else ""))

# --- D. the accessor ------------------------------------------------------
print("\n=== D. sub_14004A550 (the character-list accessor): who calls it ===")
ACC = 0x14004A550
callers = []
for k in range(len(text) - 5):
    if text[k] != 0xE8:
        continue
    rel = int.from_bytes(text[k+1:k+5], 'little', signed=True)
    if text_va + k + 5 + rel == ACC:
        callers.append(text_va + k)
uniq = sorted(set(callers))
print("  %d call site(s):" % len(uniq))
for a in uniq[:24]:
    print("    0x%X   fn 0x%X" % (a, func_start(a - text_va)))
