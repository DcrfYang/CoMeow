# recover_symbols.py -- rebuild glaiel:: function names in a FRESH database.
#
# Run inside IDA (MCP py_exec_file, or headless idat -A -S).
#
# THE POINT: the names are in the BINARY, not in the .i64. A game update
# replaces the database, and the design notes' symbol-recovery section described the
# method in prose only -- so after the first update it had to be redone by
# hand. This is that method, executed. After an update: open the new .i64, run
# this, then gen_sigs.py.
#
# TWO CHANNELS, neither trusted alone.
#
#   1. ASSERT __FUNCSIG__ STRINGS. The release build ships asserts, so .rdata
#      holds full C++ signatures. The string is referenced from inside the
#      function it names.
#
#   2. _Func_impl_no_alloc VFTABLES. The mangled symbol embeds the ENCLOSING
#      function's name, and that function is the only code xref to the vftable
#      which is not one of the vftable's own slots.
#
# INLINING IS THE FAILURE MODE OF BOTH, and it is not theoretical: measured on
# this build, channel 2 attributed Ability::QueueEnableSuperArmor to the
# 7831-byte Ability::trigger, because the tiny queue helper was inlined into it
# and its lambda came along. Channel 1 has the same disease -- ByteStream::read
# asserts appear inside every function that inlined it.
#
# THE DISCRIMINATOR IS FAN-OUT. A real name lands in exactly one function; an
# inlined utility lands in many. So when a function carries several candidate
# names the one with the SMALLEST fan-out is the specific one and wins, a name
# whose fan-out exceeds INLINE_FANOUT may never name anything, and an exact tie
# is left alone rather than guessed.
#
# OUTLINING IS THE THIRD FAILURE MODE, and fan-out does NOT catch it, because
# the wrong attribution is unique. MSVC splits a cold chunk out of a function
# into its own callee, and the chunk keeps the parent's lambda scope: measured
# here, `LevelUpScreen::select_option` landed on 0x140386610, a 738-byte callee
# of the real select_option at 0x1403835D0 -- which is the one that reads
# LevelUpScreen+160, the subject CatData*, eighteen times.
#
# SO THESE NAMES ARE NAVIGATIONAL, NOT AUTHORITATIVE. Nothing the mod hooks is
# resolved from them -- mgmp_addresses.h is pinned by unique byte signature and
# cross-checked against the shipped PE. Treat a name here as a lead to verify,
# the way every offset in the design notes carries two independent readings.

import os, re, collections
import ida_bytes, ida_funcs, ida_name, idautils, idc

DRY_RUN       = os.environ.get("MGMP_DRY_RUN", "") not in ("", "0")
INLINE_FANOUT = 3      # a name reaching more functions than this is inlined
NAME_LAMBDAS  = True   # also name each lambda body as <enclosing>__inner_N

SHORT = idc.get_inf_attr(idc.INF_SHORT_DN)


def _root():
    env = os.environ.get("MGMP_ROOT")
    if env:
        return os.path.abspath(env)
    here = globals().get("__file__")
    if here:                                  # <root>/tools/ida/recover_symbols.py
        return os.path.abspath(os.path.join(os.path.dirname(here), "..", ".."))
    idb = idc.get_idb_path()
    if idb:
        return os.path.dirname(os.path.abspath(idb))
    raise RuntimeError("cannot locate the mod directory -- set MGMP_ROOT")


REPORT = os.path.join(_root(), "tools", "ida", "symbols_report.txt")


# ------------------------------------------------------------------ channel 1

SIG = re.compile(r"__cdecl\s+(glaiel::[\w:~]+)\s*\(")


def channel_asserts():
    """func_ea -> set(name). The assert string sits inside its own function."""
    out = collections.defaultdict(set)
    n = 0
    for s in idautils.Strings():
        try:
            v = str(s)
        except Exception:
            continue
        if "__cdecl glaiel::" not in v:
            continue
        n += 1
        m = SIG.search(v)
        if not m:
            continue
        for x in idautils.DataRefsTo(s.ea):
            f = ida_funcs.get_func(x)
            if f:
                out[f.start_ea].add(m.group(1))
    return out, n


# ------------------------------------------------------------------ channel 2

LAM = re.compile(r"`(.+?)'::`\d+'::")


def _vftable_slots(ea, n=8):
    slots = []
    for i in range(n):
        f = ida_funcs.get_func(ida_bytes.get_qword(ea + 8 * i))
        slots.append(f.start_ea if f else None)
    return slots


def channel_vftables():
    """func_ea -> set(name), plus lambda bodies recovered from slot 2."""
    out = collections.defaultdict(set)
    bodies = {}
    seen = 0
    for ea, sym in idautils.Names():
        if not sym.startswith("??_7?$_Func_impl_no_alloc@"):
            continue
        if "glaiel@@" not in sym:
            continue
        d = ida_name.demangle_name(sym, SHORT)
        if not d:
            continue
        m = LAM.search(d)
        if not m:
            continue
        seen += 1
        encl = m.group(1).split("(")[0].strip()
        slots = _vftable_slots(ea)
        own = set(s for s in slots if s is not None)
        cands = set()
        for x in list(idautils.DataRefsTo(ea)) + list(idautils.CodeRefsTo(ea, 0)):
            f = ida_funcs.get_func(x)
            if f and f.start_ea not in own:
                cands.add(f.start_ea)
        if len(cands) == 1:
            out[list(cands)[0]].add(encl)
        if NAME_LAMBDAS and len(slots) > 2 and slots[2]:   # slot 2 is _Do_call
            bodies.setdefault(slots[2], encl)
    return out, bodies, seen


# --------------------------------------------------------------------- merge

def pick(cands, fanout):
    """Fewest hosts wins: the specific name, never the inlined utility."""
    usable = [c for c in cands if fanout[c] <= INLINE_FANOUT]
    if not usable:
        return None
    usable.sort(key=lambda c: (fanout[c], c))
    if len(usable) > 1 and fanout[usable[0]] == fanout[usable[1]]:
        return None                       # undecidable -- do not guess
    return usable[0]


def sanitize(n):
    return re.sub(r"[^A-Za-z0-9_:~]", "_", n)


def main():
    ch1, nstr = channel_asserts()
    ch2, bodies, nvft = channel_vftables()
    print("channel 1: %d assert signatures -> %d functions" % (nstr, len(ch1)))
    print("channel 2: %d vftables -> %d functions, %d lambda bodies"
          % (nvft, len(ch2), len(bodies)))

    fan = collections.Counter()
    for src in (ch1, ch2):
        for ea, names in src.items():
            for n in names:
                fan[n] += 1

    decided, skipped = {}, []
    for ea in set(ch1) | set(ch2):
        chosen = pick(ch1.get(ea, set()), fan) or pick(ch2.get(ea, set()), fan)
        if chosen:
            decided[ea] = chosen
        else:
            skipped.append((ea, sorted(ch1.get(ea, set()) | ch2.get(ea, set()))))

    print("decided: %d functions, ambiguous (left alone): %d"
          % (len(decided), len(skipped)))

    applied = kept = 0
    lines = []
    for ea, nm in sorted(decided.items()):
        cur = ida_name.get_ea_name(ea) or ""
        if cur and not cur.startswith(("sub_", "nullsub_", "j_", "unknown_")):
            kept += 1
            lines.append("KEPT   %#x  %-46s (already %s)" % (ea, nm, cur))
            continue
        lines.append("NAME   %#x  %s" % (ea, nm))
        if not DRY_RUN:
            ida_name.set_name(ea, sanitize(nm),
                              ida_name.SN_NOCHECK | ida_name.SN_FORCE)
        applied += 1

    nlam = 0
    if NAME_LAMBDAS:
        used = collections.Counter()
        for body, encl in sorted(bodies.items()):
            if body in decided:
                continue
            cur = ida_name.get_ea_name(body) or ""
            if cur and not cur.startswith(("sub_", "nullsub_", "j_")):
                continue
            i = used[encl]
            used[encl] += 1
            nm = "%s__inner_%d" % (encl, i)
            lines.append("LAMBDA %#x  %s" % (body, nm))
            if not DRY_RUN:
                ida_name.set_name(body, sanitize(nm),
                                  ida_name.SN_NOCHECK | ida_name.SN_FORCE)
            nlam += 1

    print("applied %d names, %d already named, %d lambda bodies%s"
          % (applied, kept, nlam, "  [DRY RUN]" if DRY_RUN else ""))

    with open(REPORT, "w", encoding="utf-8") as f:
        f.write("channel1 strings=%d funcs=%d\nchannel2 vftables=%d funcs=%d\n"
                % (nstr, len(ch1), nvft, len(ch2)))
        f.write("decided=%d applied=%d lambdas=%d ambiguous=%d\n\n"
                % (len(decided), applied, nlam, len(skipped)))
        f.write("--- ambiguous, left alone ---\n")
        for ea, names in skipped:
            f.write("  %#x  %s\n" % (ea, names))
        f.write("\n--- actions ---\n")
        f.write("\n".join(lines))
    print("report ->", REPORT)


main()
