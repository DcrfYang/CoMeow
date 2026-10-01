# M0 probe 8 -- what does a RUN actually write into a save?
#
# The event-idempotence design needs a policy per persistent key, and the first
# input to that policy is the truth about which keys move during play. The
# game's own backup snapshots are a free A/B capture: the same save at two
# moments, hours or days apart, with a run played in between.
#
# Everything here is read-only.
import sqlite3, os, glob

# the first Steam profile folder under the game's save root (set DIR by hand if you have several)
_ROOT = os.path.join(os.environ.get("APPDATA", ""), "Glaiel Games", "Mewgenics")
DIR = os.path.join((glob.glob(os.path.join(_ROOT, "*")) or [_ROOT])[0], "saves")
CUR = os.path.join(DIR, "steamcampaign01.sav")
BAK = sorted(glob.glob(os.path.join(DIR, "backups", "steamcampaign01_*.savbackup")))

def props(path):
    con = sqlite3.connect("file:%s?mode=ro" % path.replace("\\", "/"), uri=True)
    try:
        return {k: (t, d) for k, t, d in
                con.execute("SELECT key, typeof(data), data FROM properties")}
    finally:
        con.close()

def files(path):
    """key -> (hash, size). The HASH matters: comparing sizes alone would report
    a blob that changed without changing length -- which is exactly what most
    progression writes look like (a flag flipped inside a 2 KB blob)."""
    import hashlib
    con = sqlite3.connect("file:%s?mode=ro" % path.replace("\\", "/"), uri=True)
    try:
        return {k: (hashlib.md5(d).hexdigest()[:10], len(d))
                for k, d in con.execute("SELECT key, data FROM files")}
    finally:
        con.close()

def cats(path):
    con = sqlite3.connect("file:%s?mode=ro" % path.replace("\\", "/"), uri=True)
    try:
        return con.execute("SELECT count(*), min(key), max(key) FROM cats").fetchone()
    finally:
        con.close()

print("backups found: %d" % len(BAK))
for b in BAK:
    print("   %s  %d bytes" % (os.path.basename(b), os.path.getsize(b)))

if not BAK:
    raise SystemExit("no backups to diff against")

# The TWO NEWEST backups, not the oldest and newest: the first one is a 32 KB
# empty save, so diffing against it would report every key as "added" and say
# nothing about what a run writes. The newest pair brackets a week of real play
# (2026-09-13 -> 2026-09-20, the co-op testing included).
old, new = BAK[-2], BAK[-1]
print("\n=== diff: %s  ->  %s ===" % (os.path.basename(old), os.path.basename(new)))
a, b = props(old), props(new)
added   = sorted(set(b) - set(a))
removed = sorted(set(a) - set(b))
changed = sorted(k for k in set(a) & set(b) if a[k] != b[k])
print("  properties: %d -> %d   added %d, removed %d, CHANGED %d"
      % (len(a), len(b), len(added), len(removed), len(changed)))
print("  added:   %s" % (added[:20] if added else "-"))
print("  removed: %s" % (removed[:20] if removed else "-"))
print("  changed:")
for k in changed[:40]:
    an, ad = a[k]
    bn, bd = b[k]
    def show(t, d):
        if t == 'text':   return repr(d)[:28]
        if d is None:     return 'NULL'
        return "%s(%d bytes)" % (t, len(d)) if isinstance(d, (bytes, bytearray)) else str(d)
    print("     %-52s %-14s -> %s" % (k, show(an, ad), show(bn, bd)))
print("  (%d more not shown)" % max(0, len(changed) - 40))

print("\n=== files table sizes ===")
fa, fb = files(old), files(new)
for k in sorted(set(fa) | set(fb)):
    ha, la = fa.get(k, ('-', 0))
    hb, lb = fb.get(k, ('-', 0))
    mark = "  <== CONTENT CHANGED" if ha != hb else ""
    print("  %-22s %7d B  %s -> %s%s" % (k, la, ha, hb, mark))

print("\n=== cats ===")
for label, path in (("old backup", old), ("new backup", new), ("live save", CUR)):
    c, lo, hi = cats(path)
    print("  %-12s count=%4d  id range %d..%d" % (label, c, lo, hi))
