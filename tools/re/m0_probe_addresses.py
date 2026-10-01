# M0 probe 4 -- settle the address space before doing anything else.
#
# The mod's own rule: verify against Mewgenics.exe, not against the notes. Two
# byte sequences are written out in full in the project's documentation, so they
# are ground truth for "is this the build the notes describe, and what is the
# VA->file-offset mapping":
#
#   MapScreen::EnterNode prologue (mgmp_follow.h):
#       mov eax, 178h ; mov rcx, gs:58h ; mov rdx,[rcx] ; movups xmm0,[rdi+118h]
#   save_adventure / ContinueAdventure 21-byte prologues (the design notes), which
#   differ only in one displacement.
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

def find_all(pat, label):
    hits = []
    i = text.find(pat)
    while i != -1:
        hits.append(text_va + i)
        i = text.find(pat, i + 1)
    print("  %-46s %d hit(s): %s" % (label, len(hits), [hex(h) for h in hits[:6]]))
    return hits

def at(va, n=24):
    o = va - text_va
    if o < 0 or o + n > len(text):
        return "(out of .text)"
    return " ".join("%02X" % b for b in text[o:o+n])

print("=== ground-truth byte patterns from the project's own documentation ===")

# EnterNode: B8 78 01 00 00           mov eax,178h
#            65 48 8B 0C 25 58 00 00 00   mov rcx, gs:[58h]
ent = bytes.fromhex("B878010000" "65488B0C2558000000")
find_all(ent, "EnterNode prologue (mov eax,178h + gs:58h)")

# save_adventure, documented 21 bytes
sa = bytes.fromhex("488BC4488958205556574154415541564157488DA8" "68FEFF")
find_all(sa, "save_adventure prologue (48 8B C4 ... 68 FE FF)")

# ContinueAdventure, same 21 bytes but the last displacement differs
ca = bytes.fromhex("488BC4488958205556574154415541564157488DA8" "D8FCFF")
find_all(ca, "ContinueAdventure prologue (... D8 FC FF)")

print("\n=== what the notes claim vs what is at those addresses ===")
claims = {
    0x140391050: "notes: MapScreen::EnterNode",
    0x1403B9CE0: "notes: MewDirector::save_adventure",
    0x1403BB120: "notes: MewDirector::ContinueAdventure",
    0x140137B70: "notes: Brain::GetChoice",
    0x140047FC0: "notes: CustomVector<T*>::push_back",
    0x14004A550: "notes: character-list accessor",
    0x1409A9D80: "notes: ApplicationBase::FrameBegin",
}
for va, what in claims.items():
    print("  0x%X %-40s -> %s" % (va, what, at(va)))

print("\n=== runtime-resolved addresses from the live hook log, minus the module base ===")
# base 00007FF618C70000 in that run
runtime = {
    0x140001000 + 0x391C20: "log: [+] ENTERNODE",
    0x140001000 + 0x138590: "log: [+] CHOICE (Brain::GetChoice)",
    0x140001000 + 0x29627870 - 0x29627870 + 0x29627870: "skip",
}
for va, what in list(runtime.items())[:2]:
    print("  0x%X %-34s -> %s" % (va, what, at(va)))

# The hook log's RVA for FrameBegin was 00007FF619627870 -> RVA 0x29627870? no:
# 0x19627870-0x18C70000 = 0x9B7870. Print both candidates.
for va, what in ((BASE + 0x9B7870, "log: [+] FRAME (base+0x9B7870)"),):
    print("  0x%X %-34s -> %s" % (va, what, at(va)))
