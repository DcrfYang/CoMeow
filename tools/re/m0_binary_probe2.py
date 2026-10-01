# M0 probe 3 -- validate the scanner, then use the right instrument.
#
# Lesson from probe 2: call-site counts of 0 for push_back and for the accessor
# are suspicious, and a template member function is INLINED almost everywhere,
# so counting E8 calls is the wrong instrument. Before trusting any of it, the
# E8 scanner is run against targets whose call sites the mod already knows.
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
print(".text va 0x%X size %d" % (text_va, len(text)))

def calls_to(target):
    out = []
    for k in range(len(text) - 5):
        if text[k] != 0xE8:
            continue
        rel = int.from_bytes(text[k+1:k+5], 'little', signed=True)
        if text_va + k + 5 + rel == target:
            out.append(text_va + k)
    return out

print("\n=== scanner self-test: call sites of targets the notes already name ===")
known = {
    0x140391050: 'MapScreen::EnterNode (1 code caller: a std::function _Do_call)',
    0x140E2D8B0: 'SaveSelection::ContinueSlot',
    0x1409A9D80: 'ApplicationBase::FrameBegin (called all over)',
    0x1401183E0: 'Character::FindAbility',
    0x14094B550: 'RollChance',
    0x140382A00: 'LevelUpScreen::select_option',
    0x140937F30: 'WorldEvent option commit',
    0x140047FC0: 'CustomVector<T*>::push_back',
    0x14004A550: 'character-list accessor',
}
for addr, what in known.items():
    c = calls_to(addr)
    print("  0x%X  %-58s  %d call site(s)" % (addr, what, len(c)))

# The bytes at push_back, so the signature question is answerable.
off = 0x140047FC0 - text_va
print("\n=== bytes at 0x140047FC0 (first 48) ===")
print("  " + " ".join("%02X" % b for b in text[off:off+48]))

print("\n=== bytes at 0x14004A550 (the accessor, first 48) ===")
off2 = 0x14004A550 - text_va
print("  " + " ".join("%02X" % b for b in text[off2:off2+48]))

print("\n=== bytes at the two +0x1FA0 sites and the +0x1F90 site (24 bytes each) ===")
for a in (0x140551751, 0x140551777, 0x140E0797C):
    o = a - text_va
    print("  0x%X: %s" % (a, " ".join("%02X" % b for b in text[o-16:o+16])))

# --- scene names referenced from data as well as code ----------------------
print("\n=== scene-name strings: code refs (lea rip) AND absolute pointers ===")
for name in ("ActSelection", "ClassChooser", "StorageItems", "House",
             "SaveSelectionScreen", "Cutscene", "MapScreen"):
    pat = name.encode() + b"\x00"
    for sname, (va, data) in secs.items():
        i = data.find(pat)
        while i != -1:
            sva = va + i
            leas = []
            for k in range(len(text) - 7):
                if text[k] not in (0x48, 0x4C) or text[k+1] != 0x8D:
                    continue
                if (text[k+2] & 0xC7) != 0x05:
                    continue
                disp = int.from_bytes(text[k+3:k+7], 'little', signed=True)
                if text_va + k + 7 + disp == sva:
                    leas.append(text_va + k)
            ptrs = []
            p8 = sva.to_bytes(8, 'little')
            for s2, (v2, d2) in secs.items():
                if s2 in ('.text',):
                    continue
                j = d2.find(p8)
                while j != -1:
                    ptrs.append(v2 + j)
                    j = d2.find(p8, j + 1)
            if leas or ptrs:
                print("  %-20s @0x%X (%s): %d lea %s | %d ptr %s"
                      % (name, sva, sname, len(leas), [hex(x) for x in leas[:3]],
                         len(ptrs), [hex(x) for x in ptrs[:3]]))
            i = data.find(pat, i + 1)
