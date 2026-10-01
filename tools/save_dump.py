#!/usr/bin/env python3
"""Read-only dump of a Mewgenics save: SQLite rows + LZ4-compressed cat blobs.

WHY THIS EXISTS
  The co-op party work needs to know what the SAVE says about a cat -- in
  particular the `on_adventure` status bit -- and it needs a way to check the
  ids the mod reads out of live memory (MewDirector+1468) against the file the
  game wrote. Reading the file is also the only way to tell a "the mod is
  wrong" from a "the game agreed with the mod".

WHERE THE LAYOUT FACTS COME FROM (recorded, because they are not ours)
  * The save is a SQLite 3 database whose page 1 carries plain-text DDL.
    Read directly off our own file on 2026-09-23:
        CREATE TABLE cats          (key INTEGER PRIMARY KEY, data BLOB) STRICT
        CREATE TABLE files         (key TEXT    PRIMARY KEY, data BLOB) STRICT
        CREATE TABLE furniture     (key INTEGER PRIMARY KEY, data BLOB) STRICT
        CREATE TABLE properties    (key TEXT    PRIMARY KEY, data ANY ) STRICT
        CREATE TABLE winning_teams (key INTEGER PRIMARY KEY, data BLOB) STRICT
    and independently documented by p0lymeric/mewgenics_analysis
    (python/0_dump_save.py, python/common/sql_save.py).
  * A `cats` row's `data` is a 4-byte little-endian uncompressed size followed
    by an LZ4 block -- same repo, python/0_dump_save.py.
  * Inside the decompressed cat blob (format version 19 at the time of
    writing), the prefix is:
        u32 version   @0
        u64 entropy   @4
        WString name                 (u64 byte length + UTF-16LE)
        String  nameplate_symbol     (u64 byte length + ASCII)
        u32 sex, u32 sex_dup
        8 bytes of status bitfields
    from imhex_patterns/cat.hexpat and python/common/dumped_save.py of that
    repo. The two files agree, and dumped_save.py ends with
    `assert(house_boss_kills_offset_past + 79 == len(blob))` -- i.e. the whole
    blob is accounted for, which is why this prefix is trusted here rather
    than guessed.

  NOTE the version field is read and PRINTED, not assumed: everything below
  the version is only claimed for version 19.

STATUS BITS (bit numbering is global across the 8 bytes)
    flags0 bit 0 owned, 1 retired, 2 starving, 4 trashed, 5 dead
    flags1 bit 2 died_from_old_age (global 10), 3..7 donated_to_* (11..15)
    flags2 bit 0..1 donated_to_frank / baby_jack (16/17),
           bit 2 died_in_battle (18), bit 3 on_adventure (19)

READ-ONLY. Opens the file it is given with sqlite3 mode=ro and never writes:
point it at a COPY, not at the live save.
"""

import argparse
import json
import sqlite3
import struct
import sys

try:
    import lz4.block
except ImportError:  # pragma: no cover
    sys.exit("this tool needs the `lz4` package: python -m pip install lz4")


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def u64(b, o):
    return struct.unpack_from("<Q", b, o)[0]


def read_wstring(blob, off):
    """u64 CHARACTER count + UTF-16LE, bounds-checked.

    The count is in CHARACTERS, not bytes -- and that is not a guess: the
    reference parser (p0lymeric/mewgenics_analysis,
    python/common/dumped_save.py) computes
        name_length_bytes = struct.unpack_from('<Q', self.blob, 12)[0] * 2
    i.e. it multiplies by two. Reading it as a byte count is exactly what made
    an earlier version of this tool report a one-character name and then read a
    length of 8719078831429804329 one field later: the whole prefix after the
    name was misaligned. The crash looked like "a malformed row"; it was this.
    """
    n_chars = u64(blob, off)
    if n_chars > (1 << 20):
        raise ValueError("wstring at +%d claims %d characters" % (off, n_chars))
    n = n_chars * 2
    if off + 8 + n > len(blob):
        raise ValueError("wstring at +%d claims %d bytes, blob is %d bytes"
                         % (off, n, len(blob)))
    off += 8
    raw = blob[off:off + n]
    return raw.decode("utf-16-le", "replace"), off + n


def read_string(blob, off):
    n = u64(blob, off)
    if off + 8 + n > len(blob):
        raise ValueError("string at +%d claims %d bytes, blob is %d bytes"
                         % (off, n, len(blob)))
    off += 8
    raw = blob[off:off + n]
    return raw.decode("utf-8", "replace"), off + n


# (name, which flag byte, bit within it) -- bit index is the global one from
# the hexpat comments, converted to per-byte offsets here.
FLAGS = [
    ("owned", 0, 0), ("retired", 0, 1), ("starving", 0, 2), ("unknown_f0_3", 0, 3),
    ("trashed", 0, 4), ("dead", 0, 5), ("unknown_f0_6", 0, 6), ("unknown_f0_7", 0, 7),
    ("unknown_f1_0", 1, 0), ("unknown_f1_1", 1, 1), ("died_from_old_age", 1, 2),
    ("donated_to_dr_beanies", 1, 3), ("donated_to_butch", 1, 4), ("donated_to_tink", 1, 5),
    ("donated_to_tracy", 1, 6), ("donated_to_organ_grinder", 1, 7),
    ("donated_to_frank", 2, 0), ("donated_to_baby_jack", 2, 1),
    ("died_in_battle", 2, 2), ("on_adventure", 2, 3),
]


def parse_cat(blob):
    """Return the blob's prefix as a dict, or {'error': ...} if it is not the
    shape this file documents. Never raises on a malformed blob: a dump that
    dies on one bad row cannot be used to prove anything about the other rows."""
    if len(blob) < 20:
        return {"error": "blob shorter than the documented prefix (%d bytes)" % len(blob)}

    version = u32(blob, 0)
    entropy = u64(blob, 4)

    if version != 19:
        # Still worth reporting the version: it is the first thing to check
        # when the offsets below stop making sense.
        return {"version": version, "entropy": entropy,
                "error": "version %d is not the 19 this tool's offsets are for" % version}

    name, off = read_wstring(blob, 12)
    nameplate, off = read_string(blob, off)

    sex = u32(blob, off)
    sex_dup = u32(blob, off + 4)
    flags = blob[off + 8:off + 16]

    return {
        "version": version,
        "entropy": entropy,
        "name": name,
        "nameplate_symbol": nameplate,
        "sex": sex,
        "sex_dup": sex_dup,
        "flags_raw": flags.hex(),
        "flags": {n: (flags[byte] >> bit) & 1 for n, byte, bit in FLAGS},
        "flags_offset": off + 8,
    }


# --- the import file the mod reads ------------------------------------------
#
# WHY A FILE, AND WHY THIS SHAPE. The 4+4 shape needs each player's OWN cats
# inside the shared run, so the peer that does not own them has to be handed
# them. The end state is that the mod reads a save in-process; until that
# exists this is the smallest honest bridge, and it is small because the mod
# already has the receiving half: `mgmp_catsync` moves exactly these bytes over
# the wire and deserializes them into a fresh cat with the game's own reader.
#
# Layout, little endian:
#     char magic[8]   "MGMPCAT1"
#     u32  count
#     count x { u64 id; u32 size; u8 data[size] }
#
# `data` is the DECOMPRESSED cat record, byte for byte as it comes out of the
# save's LZ4 block. Nothing is re-encoded, which is the property that keeps the
# mod's side a plain deserialize with no private format to stay in step with.
EXPORT_MAGIC = b"MGMPCAT1"


def export_cats(con, ids, path, remap=None):
    remap = remap or {}
    cur = con.cursor()
    rows = []
    for key, data in cur.execute("SELECT key, data FROM cats ORDER BY key"):
        if ids and key not in ids:
            continue
        size = u32(data, 0)
        try:
            raw = lz4.block.decompress(data[4:], uncompressed_size=size)
        except Exception as e:  # noqa: BLE001
            print("export: skipping 0x%x (LZ4: %s)" % (key, e))
            continue

        # DOES THE BLOB CARRY THE ID AS WELL? Two sources disagree and only the file
        # can settle it: mgmp_catsync.h says "CatData+0x00 is a u64 id, serialized
        # right after that version tag", while the ImHex pattern names +4 `entropy`.
        # So the value at +4 is PRINTED next to the key, and it is patched only when
        # it provably equals the key -- never because it was assumed to.
        raw = bytearray(raw)
        new_id = remap.get(key, key)
        inner_note = "no room for the field"
        if len(raw) >= 12:
            inner = struct.unpack_from("<Q", raw, 4)[0]
            if inner == key:
                if new_id != key:
                    struct.pack_into("<Q", raw, 4, new_id)
                    inner_note = "inner id == key, patched %x -> %x" % (key, new_id)
                else:
                    inner_note = "inner id == key"
            else:
                inner_note = "inner +4 is 0x%x (NOT the key) -- left alone" % inner

        rows.append((new_id, bytes(raw)))
        print("  id 0x%x -> 0x%x  %d bytes  [%s]" % (key, new_id, len(raw), inner_note))

    with open(path, "wb") as f:
        f.write(EXPORT_MAGIC)
        f.write(struct.pack("<I", len(rows)))
        for key, raw in rows:
            f.write(struct.pack("<Q", key))
            f.write(struct.pack("<I", len(raw)))
            f.write(raw)

    # 8 magic + 4 count, then 8 id + 4 size per record. (An earlier line said 16
    # for the header and reported 4 bytes more than the file it had just written;
    # the file is the truth, and this is the arithmetic that matches it.)
    total = 8 + 4 + sum(8 + 4 + len(r) for _, r in rows)
    print("EXPORT: %d cat(s), %d bytes -> %s" % (len(rows), total, path))
    print("EXPORTED IDS: %s" % " ".join("%x" % k for k, _ in rows))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("save", help="path to a COPY of a .sav (never the live one)")
    ap.add_argument("--ids", default="",
                    help="comma-separated cat ids to highlight (hex, as the mod logs them)")
    ap.add_argument("--limit", type=int, default=0, help="only dump the first N cats")
    ap.add_argument("--flags-only", action="store_true",
                    help="one line per cat: key, version, owner flags")
    ap.add_argument("--export", default="",
                    help="write the chosen cats' CatData blobs to this file, in the "
                         "format the mod's importer reads (see export_cats)")
    ap.add_argument("--remap-ids", default="",
                    help="old:new pairs in hex, comma separated. Gives the exported cats "
                         "ids the receiving side's registry does NOT already have -- which "
                         "is the only way to exercise the mod's 'create a cat I have never "
                         "seen' path (two peers loading one save share all 671 ids, so an "
                         "unmodified export is just an overwrite)")
    ap.add_argument("--export-ids", default="",
                    help="which cats to export (hex, comma separated). Empty = every "
                         "cat carrying the on_adventure flag, i.e. the ones in a run")
    args = ap.parse_args()

    want = set()
    for tok in args.ids.split(","):
        tok = tok.strip()
        if tok:
            want.add(int(tok, 16))

    # This console is code page 936 and a cat name can contain anything; a
    # UnicodeEncodeError mid-dump is the same failure as the crash above, one
    # layer out.
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001 - cosmetic only
        pass

    # mode=ro: the file cannot be modified through this connection even by
    # accident, which is the promise this tool makes.
    con = sqlite3.connect("file:%s?mode=ro" % args.save.replace("\\", "/"), uri=True)
    cur = con.cursor()

    tables = [r[0] for r in cur.execute(
        "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name")]
    print("TABLES:", ", ".join(tables))

    keys = [r[0] for r in cur.execute("SELECT key FROM cats ORDER BY key")]
    # 671 keys spell out to a wall of numbers that buries every other line. The
    # range and the count are what a reader needs; the ids in question are
    # printed in full just below.
    print("cats rows: %d  key range %x..%x" % (len(keys), min(keys), max(keys)))

    if want:
        missing = sorted(want - set(keys))
        print("IDS ASKED FOR (from the mod's memory read): %s"
              % " ".join("%x" % k for k in sorted(want)))
        print("              present in the save: %s"
              % " ".join("%x" % k for k in sorted(want & set(keys))))
        if missing:
            print("              NOT in the save:     %s"
                  % " ".join("%x" % k for k in missing))

    rows = cur.execute("SELECT key, data FROM cats ORDER BY key")
    n = 0
    on_adventure = []
    versions = {}
    malformed = 0
    for key, data in rows:
        n += 1
        if args.limit and n > args.limit:
            break
        size = u32(data, 0)
        try:
            raw = lz4.block.decompress(data[4:], uncompressed_size=size)
        except Exception as e:  # noqa: BLE001 - a bad row must not end the dump
            print("%20x  LZ4 FAILED: %s" % (key, e))
            continue
        try:
            info = parse_cat(raw)
        except Exception as e:  # noqa: BLE001 - same rule for a malformed prefix
            info = {"error": "malformed prefix: %s" % e}
            malformed += 1
        versions[info.get("version")] = versions.get(info.get("version"), 0) + 1
        if info.get("flags", {}).get("on_adventure"):
            on_adventure.append(key)

        if args.flags_only:
            print("%20x  v%-3s  %s" % (key, info.get("version"),
                                       "ON ADVENTURE" if info.get("flags", {}).get("on_adventure")
                                       else ""))
            continue

        print("=" * 78)
        print("cat 0x%x  (blob %d -> %d bytes)" % (key, len(data), len(raw)))
        if "error" in info:
            print("   !! %s" % info["error"])
            print("   raw prefix: %s" % raw[:32].hex())
            continue
        print("   version        %d" % info["version"])
        print("   entropy        0x%016x" % info["entropy"])
        print("   name           %r" % info["name"])
        print("   nameplate      %r" % info["nameplate_symbol"])
        print("   sex            %d (dup %d)" % (info["sex"], info["sex_dup"]))
        print("   flags raw      %s  @+%d" % (info["flags_raw"], info["flags_offset"]))
        setb = [k for k, v in info["flags"].items() if v]
        print("   flags set      %s" % (", ".join(setb) if setb else "(none)"))

    print("=" * 78)
    print("format versions seen: %s"
          % ", ".join("%s x%d" % (v, c) for v, c
                      in sorted(versions.items(), key=lambda kv: str(kv[0]))))
    print("malformed rows: %d" % malformed)
    print("ON ADVENTURE: %s" % (" ".join("%x" % k for k in on_adventure) or "(none)"))

    if args.export:
        want_export = set()
        for tok in args.export_ids.split(","):
            tok = tok.strip().lower().replace("0x", "")
            if tok:
                want_export.add(int(tok, 16))
        if not want_export:
            # Default to the game's own answer to "which cats are in this run":
            # the on_adventure flag, verified 2026-09-23 to select exactly the
            # four ids the mod reads out of MewDirector+1468.
            want_export = set(on_adventure)
            print("export: no --export-ids given, using the on_adventure cats")
        remap = {}
        for pair in args.remap_ids.split(","):
            pair = pair.strip()
            if not pair:
                continue
            old_s, _, new_s = pair.partition(":")
            old = int(old_s.strip().lower().replace("0x", ""), 16)
            new = int(new_s.strip().lower().replace("0x", ""), 16)
            if not old or not new:
                sys.exit("--remap-ids: bad pair %r (want old:new in hex)" % pair)
            remap[old] = new
        if remap:
            print("export: remapping %d id(s)" % len(remap))
        export_cats(con, want_export, args.export, remap)

    # properties: keys only. The RNG seed and the adventure state live here, and
    # the KEY LIST alone is safe to print (the values are not, they are the
    # player's save).
    try:
        pkeys = [r[0] for r in cur.execute("SELECT key FROM properties ORDER BY key")]
        print("properties keys: %d" % len(pkeys))
        # Only the keys this project has a reason to look at: the adventure flag
        # the party work cares about, the RNG seed, and the save's own identity.
        interesting = [k for k in pkeys if any(w in k for w in (
            "adventure", "seed", "steamid", "version", "scum", "timer"))]
        print("properties of interest: %s" % ", ".join(interesting))
    except sqlite3.Error as e:
        print("properties: %s" % e)

    con.close()


if __name__ == "__main__":
    main()
