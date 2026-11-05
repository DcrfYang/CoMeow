// mgmp_completion_rule.h -- the class completion marks in a room (0.1.147). Pure: no game memory, no network.
//
// The game decides the SOLO-CAT and the MONOCLASS marks of a cleared chapter (GlobalProgressionData 0x1401C96B0) from the departed list (MewDirector +0x5C8, the
// party as it left the House): exactly one cat is a solo-cat run; two or more cats of one class are a monoclass run. In a room that list holds the HOST's cats whatever
// the player brought, so the marks followed the host's party. The room's rule instead, over the cats EVERY player present brought (0.1.148: the solo-cat mark
// needs one class too):
//   * every player brought exactly one cat AND all the cats are of one class -> the solo-cat mark for everybody,
//   * otherwise, all the cats are of one class (some player brought two or more) -> the monoclass mark for everybody,
//   * otherwise (the cats are of different classes, whatever their number)   -> neither mark for anybody.
// catsync_completion_begin puts a departed list into the director that makes the game's own check come out so: one cat for "solo", the same cat twice for "monoclass",
// two cats of different classes for "neither" (at most two entries: they fit the list's capacity, nothing is allocated).
#pragma once
#include <cstdint>
#include <cstring>

namespace mgmp {

struct CompletionCat { uint8_t seat; char cls[32]; };
enum class CompletionMark { Solo, Mono, None };

// `cats`: the cats the players present brought (n >= 1), `me`: this player's seat. a, b: the indices of the (one or two) cats the departed list is to hold.
inline CompletionMark completion_rule(const CompletionCat* cats, uint32_t n, uint8_t me, uint32_t& a, uint32_t& b) {
    a = b = 0;
    if (!cats || !n) return CompletionMark::None;
    for (uint32_t i = 1; i < n; ++i)             // cats of different classes: no mark of either kind
        if (strcmp(cats[i].cls, cats[0].cls) != 0) { a = 0; b = i; return CompletionMark::None; }
    unsigned per_seat[256] = {};
    bool solo = true;
    for (uint32_t i = 0; i < n; ++i) ++per_seat[cats[i].seat];
    for (unsigned s = 0; s < 256; ++s) if (per_seat[s] > 1) solo = false;
    if (solo) {                                  // every seat that brought cats brought exactly one, and all are of one class
        for (uint32_t i = 0; i < n; ++i) if (cats[i].seat == me) { a = b = i; break; }
        return CompletionMark::Solo;
    }
    return CompletionMark::Mono;                 // a = b = 0: the same cat twice
}

} // namespace mgmp
