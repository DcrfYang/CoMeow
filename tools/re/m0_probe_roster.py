# M0 probe 5 -- follow the thread from the three sites that reference the battle
# character list at +0x1F90.
#
# Structural OFFSETS survive a game update; addresses do not. Probe 4 proved the
# prose addresses in the design notes are stale (they were written for an older build)
# while the offsets and the generated table are current. So the search is done
# entirely in offset space, and only the *results* are addresses.
#
#   what it prints:
#     - the call targets near each +0x1F90 site (the container operations)
#     - the head of each target, to tell a vector helper from real code
#     - the call sites of a chosen target (now that addresses are trustworthy)
#     - the same treatment for the RUN's cat list at MewDirector+0x5B8
import lief

BASE = 0x140000000
EXE  = r"D:\software\steam\Steam\steamapps\common\Mewgenics\Mewgenics.exe"
img  = lief.parse(EXE)
secs = {}
for s in img.sections:
    try:
        secs[s.name] = (s.virtual_address + BASE, bytes(s.content))
    except Exception:
        pass
text_va, text = secs['.text']

def hexs(va, n=32):
    o = va - text_va
    if o < 0 or o + n > len(text): return "(oob)"
    return " ".join("%02X" % b for b in text[o:o+n])

def calls_in(va, before=48, after=64):
    """E8 rel32 call sites in a window around va, as (site, target)."""
    out = []
    lo = max(0, va - text_va - before)
    hi = min(len(text) - 5, va - text_va + after)
    for k in range(lo, hi):
        if text[k] != 0xE8: continue
        rel = int.from_bytes(text[k+1:k+5], 'little', signed=True)
        out.append((text_va + k, text_va + k + 5 + rel))
    return out

def calls_to(target, cap=40):
    out = []
    for k in range(len(text) - 5):
        if text[k] != 0xE8: continue
        rel = int.from_bytes(text[k+1:k+5], 'little', signed=True)
        if text_va + k + 5 + rel == target:
            out.append(text_va + k)
            if len(out) >= cap: break
    return out

print("=== the three +0x1F90 references, and what they call ===")
sites = [0x140551751, 0x140551777, 0x140E0797C]
targets = {}
for s in sites:
    print("\n  0x%X  bytes: %s" % (s, hexs(s - 24, 48)))
    for site, tgt in calls_in(s):
        print("     call 0x%X -> 0x%X   head: %s" % (site, tgt, hexs(tgt, 24)))
        targets[tgt] = site

print("\n=== is the 0x140E07970 wrapper's target a container helper? ===")
for tgt in sorted(targets):
    cs = calls_to(tgt)
    print("  0x%X: %d call site(s) total, head %s" % (tgt, len(cs), hexs(tgt, 16)))
    for c in cs[:6]:
        print("        called from 0x%X" % c)

# --- the RUN's cat list: MewDirector+0x5B8 (1464 = {cap@5B8? count@5BC, data@5C0})
print("\n=== the run cat list at +0x5B8: lea sites and what they call ===")
pat = bytes.fromhex("B8050000")           # 0x5B8 as disp32
found = []
i = text.find(pat)
while i != -1:
    found.append(i)
    i = text.find(pat, i + 1)
print("  disp32 0x5B8 occurrences: %d" % len(found))

lea_sites = []
for off in found:
    j = off - 2                                  # expect: 48 8D 8? B8 05 00 00
    if j < 0: continue
    if text[j] in (0x48, 0x4C) and text[j+1] == 0x8D and (text[j+2] & 0xC7) == 0x85:
        lea_sites.append(text_va + j)
print("  of which lea reg,[reg+0x5B8]: %d" % len(lea_sites))
for s in lea_sites[:12]:
    cs = calls_in(s, 8, 40)
    print("    0x%X  calls: %s" % (s, ["0x%X" % t for _, t in cs[:3]]))
