#!/usr/bin/env python3
"""Resolve a MewUI-style signatures.json against a PE image, offline.

WHY THIS EXISTS
  Two questions keep coming back and neither needs the game running:

  1. "The game patched -- how much has to be re-derived?" Our own resolver
     (mgmp_sigscan) answers it at runtime and prints a banner; this answers it
     without starting anything, and against an arbitrary file.
  2. "Is a third party's signature still good on OUR build?" Mewgenics mods
     found in the wild (MewUI, CatManager, mewgenics_analysis) each pin
     addresses for the build THEY had. Ours is a different build -- verified
     2026-09-23: ours is
         4127cd6a792ae528bca6f65a8873dd61789591937d87656c2b586a5e30eb77ea
     while MewUI's analysis was done on another one. Running their pattern here
     says whether it survives the difference, which is a stronger statement
     than either side can make alone.

SIGNATURE GRAMMAR (MewUI's re_tools/mew_ui_api_signatures.json)
  {"symbols": {"NAME": {"kind": "rva",
                        "resolver": "rip_relative_rva",
                        "pattern": "48 8D 0D ?? ?? ?? ?? E8 ...",
                        "displacement_offset": 19,     // where the disp32 sits
                        "instruction_end_offset": 23,  // disp32 is relative to this
                        "match_offset": 0,             // optional
                        "search_sections": [".text"]}}} // optional

  A plain-address signature has no resolver (or resolver "absolute"), and then
  the match's RVA is the answer.

  The rule that makes any of this safe is the one mgmp_sigscan.h states: a
  signature must match EXACTLY ONCE. This tool refuses to resolve anything that
  matches more than once, rather than picking the first hit.

READ-ONLY: opens the exe with "rb" and never writes to it.
"""

import argparse
import json
import struct
import sys


def read_sections(path):
    """Return (image_base, [(name, va, vsize, raw_off, raw_size, characteristics)])."""
    with open(path, "rb") as f:
        data = f.read()

    if data[:2] != b"MZ":
        sys.exit("%s is not a PE image (no MZ)" % path)
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        sys.exit("%s has no PE signature at 0x%x" % (path, e_lfanew))

    coff = e_lfanew + 4
    n_sections = struct.unpack_from("<H", data, coff + 2)[0]
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic != 0x20B:
        sys.exit("expected PE32+ (0x20B), got 0x%X" % magic)
    image_base = struct.unpack_from("<Q", data, opt + 24)[0]

    secs = []
    first = opt + opt_size
    for i in range(n_sections):
        off = first + i * 40
        name = data[off:off + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, va, raw_size, raw_off = struct.unpack_from("<IIII", data, off + 8)
        chars = struct.unpack_from("<I", data, off + 36)[0]
        secs.append((name, va, vsize, raw_off, raw_size, chars))

    return image_base, secs, data


def compile_pattern(text):
    """'48 8D 0D ?? ?? ?? ??' -> (pattern bytes, mask bytes); mask 0 means wildcard.

    The first token is NOT a length prefix. An earlier version of this file
    guessed that it was (because some signature files carry one) and silently
    dropped the leading byte of every pattern -- which shifted every resolved
    displacement by one and produced a negative RVA that looked like a real
    answer. MewUI's patterns are pure hex; nothing is stripped.
    """
    toks = text.split()
    pat, mask = bytearray(), bytearray()
    for t in toks:
        if t in ("?", "??"):
            pat.append(0)
            mask.append(0)
        else:
            if len(t) > 2:
                t = t[:2]                   # tolerate running the tokens together
            pat.append(int(t, 16))
            mask.append(0xFF)
    return bytes(pat), bytes(mask)


def find_all(buf, pat, mask):
    """Plain scan. Called once per symbol per section; patterns are short and
    the sections are a few MB, which is fast enough to not need a heuristic."""
    out = []
    n, m = len(buf), len(pat)
    if m == 0 or m > n:
        return out
    first = pat[0]
    i = 0
    while True:
        i = buf.find(bytes([first]), i)
        if i < 0 or i + m > n:
            return out
        ok = True
        for j in range(1, m):
            if mask[j] and buf[i + j] != pat[j]:
                ok = False
                break
        if ok:
            out.append(i)
        i += 1


def resolve(sym, image_base, secs, data):
    pat, mask = compile_pattern(sym["pattern"])
    want = [s.strip().lower() for s in sym.get("search_sections", [".text"])]
    hits = []

    for name, va, vsize, raw_off, raw_size, _chars in secs:
        if name.lower() not in want:
            continue
        buf = data[raw_off:raw_off + raw_size]
        for pos in find_all(buf, pat, mask):
            rva = va + pos + int(sym.get("match_offset", 0))
            if sym.get("resolver") == "rip_relative_rva":
                disp_off = int(sym["displacement_offset"])
                end_off = int(sym["instruction_end_offset"])
                disp = struct.unpack_from("<i", buf, pos + disp_off)[0]
                rva = va + pos + end_off + disp
            hits.append((name, rva))

    return hits


# --- dumping raw bytes at an RVA (added 2026-09-23, for the departure path) ---
#
# WHY: mgmp_listprobe mode 2 named the functions that write the run's cat list when
# the player leaves the House -- 0xAE145 and 0xAE6EA, with the Depart_Sign handler
# 0x1F8ED3 above them. The next question is what they TAKE and what they DO, i.e.
# their shape. Part of that is answerable from the file on disk: the prologue says
# which argument registers a function was handed, and a window of bytes can be
# searched for direct calls and for the party vector's displacements.
#
# It is a reader, not a disassembler, and everything it prints beyond the hex is
# labelled as a heuristic. Its value is that it needs no running game and no debugger.

def rva_to_file_off(secs, rva):
    for name, va, vsize, raw_off, raw_size, _chars in secs:
        if va <= rva < va + max(vsize, raw_size):
            return raw_off + (rva - va), name
    return None, None


def read_pdata(secs, data):
    """The (start, end) RVA of every function the image declares, from .pdata.

    WHY THIS AND NOT A PROLOGUE SCAN: a window of bytes at some RVA cannot say where
    the function that contains it BEGINS -- reading backwards for something that looks
    like a prologue is a guess, and the departure path is exactly where a wrong guess
    would be expensive. The image already carries the answer: .pdata is the exception
    directory, one 12-byte RUNTIME_FUNCTION per function, holding its start and end
    RVA. This project has watched several functions from the outside (the party-vector
    watchpoints return addresses, which land INSIDE their callers), so this is what
    turns "the code near 0xAE6EA" into "the function 0xAE6xx..0xAE7yy".
    """
    for name, va, vsize, raw_off, raw_size, _chars in secs:
        if name != ".pdata":
            continue
        buf = data[raw_off:raw_off + raw_size]
        out = []
        for i in range(0, len(buf) - 11, 12):
            start, end, _unwind = struct.unpack_from("<III", buf, i)
            if start and end > start:
                out.append((start, end))
        return out
    return []


# Displacements that mean something in this project: the run's party vector is
# {cap, count, data} at MewDirector+1464/+1468/+1472 -- measured 2026-09-23 by
# watching exactly those fields (see mgmp_addresses.h's kDir_* and the probe log).
INTERESTING_DISP = (
    (0x5B8, "party.cap   (MewDirector+1464)"),
    (0x5BC, "party.count (MewDirector+1468)"),
    (0x5C0, "party.data  (MewDirector+1472)"),
)


def dump_rvas(secs, data, rvas, window):
    for rva in rvas:
        off, sec = rva_to_file_off(secs, rva)
        if off is None:
            print("RVA 0x%X: not inside any section" % rva)
            continue
        buf = data[off:off + window]
        print("=" * 78)
        print("RVA 0x%X  section %s  file offset 0x%X  (%d bytes)"
              % (rva, sec, off, len(buf)))
        for i in range(0, len(buf), 16):
            chunk = buf[i:i + 16]
            print("  +%02X  %-47s  %s"
                  % (i, " ".join("%02X" % b for b in chunk),
                     "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)))

        # Argument registers being spilled in the first instruction or two: the classic
        # prologue names how many arguments a function was handed (x64 fastcall passes
        # them in rcx, rdx, r8, r9).
        for reg, name in ((b"\x48\x89\x4c\x24", "rcx (arg0)"),
                          (b"\x48\x89\x54\x24", "rdx (arg1)"),
                          (b"\x4c\x89\x44\x24", "r8  (arg2)"),
                          (b"\x4c\x89\x4c\x24", "r9  (arg3)")):
            if buf[4:8] == reg or buf[0:4] == reg:
                print("   prologue: stores %s" % name)

        # Direct calls: E8 rel32, target = end of the instruction + rel. Reported only
        # when the target lands inside .text, which is what keeps a stray E8 in data
        # from filling the output.
        for i in range(len(buf) - 5):
            if buf[i] == 0xE8:
                rel = struct.unpack_from("<i", buf, i + 1)[0]
                tgt = rva + i + 5 + rel
                if 0x1000 <= tgt < 0xE40000:
                    print("   call  +%02X -> 0x%X" % (i, tgt))

        # The party vector's displacements as raw 4-byte patterns. A hit is a byte
        # pattern, not proof of an instruction -- read it next to the hex above.
        for want, why in INTERESTING_DISP:
            pat = struct.pack("<I", want)
            idx = buf.find(pat)
            while idx >= 0:
                print("   disp  +%02X contains 0x%X  (%s)" % (idx, want, why))
                idx = buf.find(pat, idx + 1)
        print()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("signatures", nargs="?", default="",
                    help="a signatures.json in the MewUI format (not needed with --dump)")
    ap.add_argument("--only", default="", help="substring filter on symbol names")
    ap.add_argument("--show", type=int, default=0,
                    help="print this many resolved symbols (0 = summary only)")
    ap.add_argument("--dump", default="",
                    help="comma-separated RVAs (hex) to dump as raw bytes, with the call "
                         "targets and party-vector displacements found in the window")
    ap.add_argument("--func", default="",
                    help="comma-separated RVAs (hex) that are INSIDE functions: report "
                         "each function's exact extent from .pdata and dump its entry")
    ap.add_argument("--window", type=int, default=96, help="bytes per RVA")
    args = ap.parse_args()

    image_base, secs, data = read_sections(args.exe)
    exe_secs = ", ".join("%s(va=%x size=%x)" % (n, va, vsz) for n, va, vsz, _, _, _ in secs)
    print("IMAGE BASE: 0x%X" % image_base)
    print("SECTIONS:   %s" % exe_secs)
    print()

    if args.dump:
        rvas = [int(t.strip().lower().replace("0x", ""), 16)
                for t in args.dump.split(",") if t.strip()]
        dump_rvas(secs, data, rvas, args.window)
        return

    if args.func:
        rvas = [int(t.strip().lower().replace("0x", ""), 16)
                for t in args.func.split(",") if t.strip()]
        funcs = read_pdata(secs, data)
        print(".pdata declares %d function(s)" % len(funcs))
        for rva in rvas:
            hit = [f for f in funcs if f[0] <= rva < f[1]]
            if not hit:
                print("RVA 0x%X: no .pdata entry contains it" % rva)
                continue
            start, end = hit[0]
            print("FUNCTION 0x%X..0x%X (%u bytes) -- contains 0x%X" % (start, end, end - start, rva))
            dump_rvas(secs, data, [start], args.window)
        return

    with open(args.signatures, "r", encoding="utf-8") as f:
        doc = json.load(f)
    syms = doc.get("symbols", doc)

    unique = ambiguous = unresolved = 0
    shown = 0
    for key in sorted(syms):
        sym = syms[key]
        if "pattern" not in sym:
            continue
        if args.only and args.only.lower() not in key.lower():
            continue

        hits = resolve(sym, image_base, secs, data)
        if len(hits) == 1:
            unique += 1
            if args.show and shown < args.show:
                shown += 1
                print("  UNIQUE  %-42s rva=0x%08X  va=0x%X" % (key, hits[0][1], image_base + hits[0][1]))
        elif len(hits) > 1:
            ambiguous += 1
            print("  AMBIGUOUS %-40s %d matches: %s"
                  % (key, len(hits), " ".join("0x%08X" % r for _, r in hits[:6])))
        else:
            unresolved += 1
            print("  NONE      %-42s (pattern not in this image)" % key)

    print()
    print("SUMMARY: %d unique, %d ambiguous, %d not found" % (unique, ambiguous, unresolved))
    print("NOTE: only 'unique' counts as resolved -- the rule mgmp_sigscan.h states.")


if __name__ == "__main__":
    main()
