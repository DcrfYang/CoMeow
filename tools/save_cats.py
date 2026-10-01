"""save_cats.py -- what cats a save file holds, without touching it.

WHY (2026-09-23). The 4+4 work wants to know whether a run whose party grew past
five cats still survives the way out (FEASIBILITY §10.3). The answer needs a save
with more than four cats in it, and the baseline fixture used for every other test
has exactly four: the House offered four, and the ids in memory -- 2da 24d 1fe 2b3,
read by mgmp's own read-only catbox probe -- are those same four. Earlier sessions of
this project have seen eight distinct ids, so some save on this machine holds more.
Rather than guess which one, list them all.

The save is a plain SQLite database (measured 2026-09-23 from the bytes: the header
is "SQLite format 3", and the schema is five tables, one of them
`cats (key INTEGER PRIMARY KEY, data BLOB) STRICT`). The cat ids are therefore the
table's keys and no blob decoding is involved. Cat NAMES would need LZ4 (the
community's rewritten cat layout says each blob is LZ4 with a u32 length in front)
and are deliberately not attempted here -- this tool answers "how many cats, and
which ids", which is the question being asked.

READ-ONLY BY CONSTRUCTION: the file is copied to a temp directory first and opened
there, so the game's own save is never locked, never written, and never even opened
for reading from its real location.

Usage:
    python save_cats.py <save or glob> [more saves or globs...]

Used on 2026-09-23 as:
    python tools/save_cats.py "...\\saves\\*.sav" "...\\saves\\mgmp_backups\\*"
"""
import glob
import os
import shutil
import sqlite3
import sys
import tempfile


def cat_ids(path):
    """The keys of the cats table, or raises. Never opens `path` itself."""
    tmp = os.path.join(tempfile.gettempdir(), "save_cats_probe.sav")
    shutil.copy2(path, tmp)
    con = sqlite3.connect(tmp)
    try:
        ids = [r[0] for r in con.execute("select key from cats")]
        tables = [r[0] for r in con.execute(
            "select name from sqlite_master where type='table'")]
    finally:
        con.close()
    return ids, tables


def main():
    pats = sys.argv[1:]
    if not pats:
        print(__doc__)
        return 1

    files = []
    for p in pats:
        hits = sorted(glob.glob(p))
        files.extend(hits if hits else [p])

    for path in files:
        if not os.path.exists(path):
            print("%-56s MISSING" % path)
            continue
        try:
            ids, tables = cat_ids(path)
        except Exception as exc:                       # noqa: BLE001 -- report, never die
            print("%-56s NOT A SAVE (%s)" % (path, exc))
            continue
        where = os.path.basename(os.path.dirname(path))
        name = "%s/%s" % (where, os.path.basename(path)) if where else path
        print("%-56s %2d cat(s)  %-14s [%s]"
              % (name, len(ids), " ".join("%x" % i for i in sorted(ids)),
                 " ".join(tables)))

    print()
    print("For reference: the run's party read out of memory on 2026-09-23 was")
    print("2da 24d 1fe 2b3 -- so any save showing MORE than those four is a")
    print("candidate for the past-five-cats test.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
