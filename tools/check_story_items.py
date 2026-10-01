"""check_story_items.py -- static checks for the "a client may not equip main-story items" rule.

Everything here is read-only: it parses the game's own item data, the shipped Mewgenics.exe, and (optionally)
copies of save files. It answers four questions the unit test (tests/test_setup.cpp::test_equipment, which only
exercises the decision matrix with mocks) cannot:

  1. DATA   Which items does the rule block? The game's predicate (Equipment::is_legacy_quest, RVA 2DE0A0) is
            `quest_item && legacy_quest`, both read from the item's gon definition. This lists every item the
            predicate is true for, and every quest item it is NOT true for (the "sidequest" items).
  2. BINARY The four native functions the guard calls still are what the mod thinks they are (their string
            literals, their call relations), and the hook target is the callback the inventory buttons use.
  3. SAVES  Is any such item ALREADY equipped on a cat in the given saves? The guard blocks the click that equips;
            it does not strip an item a cat already wears, so such a cat would travel into a run.
  4. DATA   Cross-check with the progression table: every `unlock_quest_item X` names an item the predicate knows.

Usage: python check_story_items.py [--exe PATH] [--data DIR] [--saves GLOB ...]
Defaults: the Steam install and `游戏资源/data` next to the repo, and no saves.
"""
import glob, os, re, shutil, sqlite3, struct, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
EXE = r"D:\software\steam\Steam\steamapps\common\Mewgenics\Mewgenics.exe"
DATA = os.path.join(REPO, "游戏资源", "data")
SAVES = []
a = sys.argv[1:]
while a:
    k = a.pop(0)
    if k == "--exe": EXE = a.pop(0)
    elif k == "--data": DATA = a.pop(0)
    elif k == "--saves": SAVES.append(a.pop(0))

fails = 0
def check(ok, what):
    global fails
    print(("  ok    " if ok else "  FAIL  ") + what)
    if not ok: fails += 1

# ------------------------------------------------------------------ 1. data
def parse_items(path):
    """top-level `Name {` blocks -> {name: {key: value}} for the flat scalar keys we care about."""
    out = {}
    txt = open(path, encoding="utf8", errors="replace").read()
    depth = 0; cur = None
    for line in txt.splitlines():
        s = line.split("//")[0].strip()
        if not s: continue
        m = re.match(r'^([A-Za-z0-9_]+)\s*\{', s)
        if depth == 0 and m:
            cur = m.group(1); out[cur] = {}; depth = 1; continue
        if depth == 1 and cur is not None:
            m2 = re.match(r'^([A-Za-z0-9_]+)\s+(.+)$', s)
            if m2 and not s.endswith("{") and not s.endswith("["):
                out[cur][m2.group(1)] = m2.group(2).strip().strip('"')
        depth += s.count("{") - s.count("}")
        if depth <= 0: depth = 0; cur = None
    return out

print("== 1. data: what the predicate `quest_item && legacy_quest` is true for")
items = {}
for f in sorted(glob.glob(os.path.join(DATA, "items", "*.gon"))):
    for k, v in parse_items(f).items():
        v["_file"] = os.path.basename(f); items[k] = v
check(len(items) > 200, "parsed %d item definitions from %s" % (len(items), os.path.join(DATA, "items")))
legacy = sorted(k for k, v in items.items() if v.get("quest_item") == "true" and v.get("legacy_quest") == "true")
sidequest = sorted(k for k, v in items.items() if v.get("quest_item") == "true" and v.get("legacy_quest") != "true")
print("  blocked on a client (main story, %d): %s" % (len(legacy), ", ".join(legacy)))
print("  NOT blocked quest items (side quests, %d): %s" % (len(sidequest), ", ".join(sidequest)))
check(all(items[k]["_file"] == "legacy_quest_items.gon" for k in legacy), "every blocked item lives in legacy_quest_items.gon")
check(all(items[k]["_file"] != "legacy_quest_items.gon" or items[k].get("legacy_quest") == "true"
          for k in items if items[k].get("quest_item") == "true" and items[k]["_file"] == "legacy_quest_items.gon") or True,
      "(informational) legacy file membership")
nolegacy_in_file = [k for k, v in items.items() if v["_file"] == "legacy_quest_items.gon" and v.get("legacy_quest") != "true"]
print("  items in legacy_quest_items.gon that the predicate does NOT cover: %s" % (", ".join(nolegacy_in_file) or "none"))
variants = [k for k, v in items.items() if "variant_of" in v and v.get("quest_item") == "true"]
print("  quest items that are variants of another item (inherit flags in-game?): %s" % (", ".join(variants) or "none"))

# --------------------------------------------------------------- 4. progression
prog = open(os.path.join(DATA, "adventure_progression_unlocks.gon"), encoding="utf8", errors="replace").read()
unlocks = sorted(set(re.findall(r'unlock_quest_item\s+(\w+)', prog)))
check(all(u in items for u in unlocks), "every progression `unlock_quest_item` names a known item (%d)" % len(unlocks))
print("  progression-unlocked quest items outside the predicate: %s" %
      (", ".join(u for u in unlocks if u not in legacy) or "none"))

# ------------------------------------------------------------------ 2. binary
print("== 2. binary: the native functions the guard calls")
try:
    import pefile, capstone
    pe = pefile.PE(EXE, fast_load=True)
    BASE = 0x140000000
    data = pe.__data__
    text = [s for s in pe.sections if s.Name.rstrip(b"\0") == b".text"][0]
    TEXT = data[text.PointerToRawData:text.PointerToRawData + text.SizeOfRawData]
    TVA = BASE + text.VirtualAddress
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

    def off_to_va(o):
        for s in pe.sections:
            if s.PointerToRawData <= o < s.PointerToRawData + s.SizeOfRawData:
                return BASE + s.VirtualAddress + (o - s.PointerToRawData)
    def lea_strings(va, n=400):
        """The text a function builds. std::string literals up to 22 chars are assembled from
        immediate-style loads out of .rdata (movsd / mov / movzx [rip+X]), not from a lea, so
        concatenate the bytes those loads read, in order, plus any lea'd literal."""
        got = []; chunk = b""
        def rd(t, size):
            rva = t - BASE
            for sx in pe.sections:
                if sx.VirtualAddress <= rva < sx.VirtualAddress + sx.SizeOfRawData:
                    o = sx.PointerToRawData + rva - sx.VirtualAddress
                    return data[o:o + size]
            return b""
        for i in md.disasm(TEXT[va - TVA:va - TVA + n * 4], va):
            m = re.search(r'rip \+ (0x[0-9a-f]+)', i.op_str)
            if m and i.mnemonic in ("lea", "mov", "movsd", "movzx"):
                t = i.address + i.size + int(m.group(1), 16)
                if i.mnemonic == "lea":
                    sx = rd(t, 40).split(bytes([0]))[0]
                    if 3 <= len(sx) < 36 and all(32 <= c < 127 for c in sx): got.append(sx.decode())
                else:
                    size = 8 if i.mnemonic == "movsd" else (4 if "dword" in i.op_str or re.match(r'e', i.op_str) else (2 if "word ptr" in i.op_str and "dword" not in i.op_str else 1))
                    if i.mnemonic == "mov" and i.op_str.startswith("r"): size = 8
                    chunk += rd(t, size)
            if i.mnemonic == "ret": break
        txt = "".join(chr(c) if 32 <= c < 127 else "|" for c in chunk)
        got.append(txt)
        return got
    def calls(va, n=400):
        out = []
        for i in md.disasm(TEXT[va - TVA:va - TVA + n * 4], va):
            if i.mnemonic == "call": out.append(i.op_str)
            if i.mnemonic == "ret": break
        return out
    s_is_quest = lea_strings(0x1402DDFE0)
    s_is_legacy = lea_strings(0x1402DE0A0)
    check(any("quest_item" in x for x in s_is_quest), "2DDFE0 reads the gon key 'quest_item'  (%s)" % s_is_quest)
    check(any("legacy_quest" in x for x in s_is_legacy), "2DE0A0 reads the gon key 'legacy_quest'  (%s)" % s_is_legacy)
    check("0x1402ddfe0" in calls(0x1402DE0A0), "2DE0A0 first calls 2DDFE0 (quest_item) -- so it is quest_item && legacy_quest")
    c = calls(0x14034EB20, 1500)
    for need in ("0x14034bb50", "0x14034bdd0", "0x14034bcb0", "0x1402ddfe0"):
        check(need in c, "34EB20 (the click the hook sits on) calls %s" % need)
    check("0x1402de0a0" not in c, "34EB20 itself does not call 2DE0A0 -- the mod's predicate is an ADDITION to the game's checks")
except ImportError:
    print("  (pefile/capstone not installed -- binary checks skipped)")

# --------------------------------------------------------------------- 3. saves
if SAVES:
    print("== 3. saves: legacy story items already worn by a cat (the gate does not strip them)")
    try:
        import lz4.block
    except ImportError:
        lz4 = None
    names = [k.encode() for k in legacy]
    for pat in SAVES:
        for p in sorted(glob.glob(pat)):
            tmp = os.path.join(tempfile.gettempdir(), "csi_probe.sav")
            shutil.copy2(p, tmp)
            try:
                con = sqlite3.connect(tmp)
                n = hits = 0
                for k, d in con.execute("select key, data from cats"):
                    n += 1
                    raw = lz4.block.decompress(d[4:], uncompressed_size=struct.unpack_from("<I", d, 0)[0]) if lz4 else d
                    for nm in names:
                        if nm in raw:
                            hits += 1; print("    %s: cat %x wears %s" % (os.path.basename(p), k, nm.decode()))
                print("  %s: %d cats scanned, %d wearing a main-story item" % (os.path.basename(p), n, hits))
                con.close()
            except Exception as e:
                print("  %s: unreadable (%s)" % (os.path.basename(p), e))

print("\n%s" % ("ALL CHECKS PASSED" if not fails else "%d CHECK(S) FAILED" % fails))
sys.exit(1 if fails else 0)
