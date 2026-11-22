#pragma once
// mgmp_impnote.h -- THE IMPORTANT UPDATE NOTICE (2026-11-16, 0.1.234).
//
// Some updates change what a player meets in the game (a feature switched on for them, a rule that changed). Such an update names a note in the
// menu's table (kImportantNotes, ui/mgmp_menu.cpp): a version and its text. The first start of the game after an update that CROSSES that version
// -- the mod that ran before was older than it, the one running now is the same or newer -- shows the note once, on the title screen; several crossed
// notes are shown together, oldest first, divided by "--------". OK writes this mod's version into mgmp.json (ui.last_version), and nothing is shown again.
//
// What "the mod that ran before" was: ui.last_version. The updater keeps the player's own mgmp.json, so an install updated from a build before 0.1.234
// has no such key -- older than every note (it gets them all). A fresh install has the key already: make_folder writes the release's own version into
// the shipped mgmp.json, so a new player is not told about changes to a game they never played. A downgrade or the same version shows nothing (the
// key only ever moves up). An update can also carry a one-time change of a setting with its note (0.1.234: ui.exp_house_coop switched on).
//
// Header only, so the tests can take it without the menu.

#include <cstdlib>

namespace mgmp {

// "a.b.c(.d)" compared numerically: <0, 0, >0. An empty or missing part counts as 0.
inline int impnote_version_cmp(const char* a, const char* b) {
    for (int part = 0; part < 4; ++part) {
        char* ea = nullptr; char* eb = nullptr;
        const long x = (a && *a) ? strtol(a, &ea, 10) : 0;
        const long y = (b && *b) ? strtol(b, &eb, 10) : 0;
        if (x != y) return x < y ? -1 : 1;
        a = (ea && *ea == '.') ? ea + 1 : nullptr;
        b = (eb && *eb == '.') ? eb + 1 : nullptr;
        if (!a && !b) break;
    }
    return 0;
}

// True when going from `prev` (the version that ran before; empty = an install from before ui.last_version existed) to `cur` crosses
// `version`: prev < version <= cur.
inline bool impnote_crosses(const char* prev, const char* cur, const char* version) {
    if (impnote_version_cmp(version, cur) > 0) return false;
    return !(prev && *prev) || impnote_version_cmp(prev, version) < 0;
}

} // namespace mgmp
