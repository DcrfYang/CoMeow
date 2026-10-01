# M0 probe 7 -- corroborate the +0x5B8 lead.
#
# A raw displacement scan cannot tell one object's +0x5B8 from another's, so the
# only way to trust a hit is to show the SAME base register being used for other
# offsets this project already knows belong to MewDirector:
#
#   +0x590 (1424)  the run history object   (mgmp_runhist)
#   +0x5A8 (1448)  the pedigree pointer     (mgmp_catsync notes)
#   +0x668 (1640)  the weather list         (WorldEvent::add_weather)
#   +0x5B8 (1464)  the run's cat list {cap@5B8, count@5BC, data@5C0}  <-- the target
import lief
from collections import Counter

BASE = 0x140000000
img  = lief.parse(r"D:\software\steam\Steam\steamapps\common\Mewgenics\Mewgenics.exe")
secs = {}
for s in img.sections:
    try: secs[s.name] = (s.virtual_address + BASE, bytes(s.content))
    except Exception: pass
text_va, text = secs['.text']

def hexs(va, n=40):
    o = va - text_va
    return " ".join("%02X" % b for b in text[o:o+n]) if 0 <= o < len(text)-n else "(oob)"

def calls_to(target):
    out = []
    for k in range(len(text) - 5):
        if text[k] != 0xE8: continue
        rel = int.from_bytes(text[k+1:k+5], 'little', signed=True)
        if text_va + k + 5 + rel == target: out.append(text_va + k)
    return out

def disp_leas(disp):
    """lea reg,[reg+disp] sites, by proper ModRM (mod=10, rm not 4/5)."""
    pat = disp.to_bytes(4, 'little')
    out = []
    i = text.find(pat)
    while i != -1:
        for back in (2, 3):
            j = i - back
            if j < 1: continue
            op, modrm = text[j], text[j+1]
            rex = text[j-1] if back == 3 else 0
            if back == 3 and not (0x40 <= rex <= 0x4F): continue
            if op != 0x8D: continue
            if (modrm >> 6) & 3 != 2: continue
            rm = modrm & 7
            if rm in (4, 5): continue
            out.append((text_va + j, rm + (8 if rex and (rex & 1) else 0)))
            break
        i = text.find(pat, i + 1)
    return out

print("=== the run cat list (+0x5B8): who is the base, and where else does it appear ===")
targets = disp_leas(0x5B8)
print("  %d lea site(s) for +0x5B8" % len(targets))
KNOWN = {0x590: 'run history 1424', 0x5A8: 'pedigree 1448',
         0x5BC: 'cat-list count', 0x5C0: 'cat-list data', 0x668: 'weather list 1640'}
corrob = 0
for va, base in targets[:14]:
    window = text[va - text_va - 32: va - text_va + 96]
    seen = []
    for d, what in KNOWN.items():
        if d.to_bytes(4, 'little') in window:
            seen.append(what)
    if seen: corrob += 1
    print("  0x%X base=r%d  nearby known offsets: %s" % (va, base, ", ".join(seen) if seen else "-"))
print("  sites corroborated as MewDirector: %d/%d shown" % (corrob, min(14, len(targets))))

print("\n=== the function every +0x5B8 site calls: 0x140942DA0 ===")
print("  head: %s" % hexs(0x140942DA0, 48))
cs = calls_to(0x140942DA0)
print("  %d call site(s)" % len(cs))
c = Counter()
for a in cs:
    c[a >> 12] += 1                      # cluster by 4 KB page
print("  clusters (page -> count), largest first: %s" % c.most_common(6))
print("  first 8 sites: %s" % ["0x%X" % a for a in cs[:8]])

print("\n=== cross-check: which of those callers also touch +0x1F90 (battle list) ===")
lst = set()
for a in cs:
    w = text[a - text_va - 64: a - text_va + 8]
    if (0x1F90).to_bytes(4, 'little') in w: lst.add(a)
print("  %d caller(s) mention +0x1F90 within 64 bytes: %s"
      % (len(lst), ["0x%X" % a for a in sorted(lst)[:6]]))
