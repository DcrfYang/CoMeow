#pragma once
// mgmp_catbrief -- the few facts about a cat that the room panel shows, and the
// class table they are read against. No dependencies, so the wire format, the
// parser and the tests can all include it.
//
// WHY A BRIEF AND NOT THE CAT. A cat's full image (mgmp_catsync) is kilobytes and
// its meaning is the game's; a peer's panel needs a name, a level, a health value,
// a class and enough colour to tell one cat from another. Everything here is read
// out of the image the GAME's serializer produced, by mgmp_catblob.h, and is
// cosmetic: nothing branches on it.

#include <cstdint>

namespace mgmp {

constexpr uint32_t kBriefNameMax = 40;   // UTF-8 bytes including the NUL
constexpr uint32_t kBriefCats    = 8;    // a party is four; leave room for familiars

// CatData stores current health as this while the cat is unhurt: campaign_stats.hp of a
// cat that has never been damaged reads 1073741823 (0x3FFFFFFF) in every save inspected.
constexpr int32_t kBriefFullHp = 0x3FFFFFFF;

struct CatBrief {
    char    name[kBriefNameMax] = {};
    int32_t level  = 0;
    int32_t hp     = kBriefFullHp;   // kBriefFullHp = unhurt
    int32_t maxhp  = 0;              // the game's own figure when catsync could ask it, else a floor; 0 = not known
    uint8_t klass  = 255;            // index into kClassKeys, 255 = unknown
    int32_t fur    = -1;             // heritable palette row  (mgmp_catpalette.h)
    int32_t coat   = -1;             // collar palette row, -1 = none
    int32_t tex    = -1;             // fur texture sprite index (pattern)
};

// CatData's collar string names the class. Order is wire format.
constexpr const char* kClassKeys[] = {
    "Fighter", "Hunter", "Mage", "Medic", "Tank", "Thief", "Colorless",
    "Monk", "Butcher", "Druid", "Tinkerer", "Necromancer", "Psychic", "Jester",
};
constexpr uint32_t kClassCount = sizeof(kClassKeys) / sizeof(kClassKeys[0]);

// data/text/combined.csv, zh-cn column.
constexpr const char* kClassNamesZh[] = {
    "\xE6\x88\x98" "\xE5\xA3\xAB",   // 0
    "\xE7\x8C\x8E" "\xE6\x89\x8B",   // 1
    "\xE6\xB3\x95" "\xE5\xB8\x88",   // 2
    "\xE7\x89\xA7" "\xE5\xB8\x88",   // 3
    "\xE5\x9D\xA6" "\xE5\x85\x8B",   // 4
    "\xE7\x9B\x97" "\xE8\xB4\xBC",   // 5
    "\xE6\x97\xA0" "\xE8\x81\x8C" "\xE4\xB8\x9A",   // 6
    "\xE6\xAD\xA6" "\xE5\x83\xA7",   // 7
    "\xE5\xB1\xA0" "\xE5\xA4\xAB",   // 8
    "\xE5\xBE\xB7" "\xE9\xB2\x81" "\xE4\xBC\x8A",   // 9
    "\xE5\xB7\xA5" "\xE7\xA8\x8B" "\xE5\xB8\x88",   // 10
    "\xE6\xAD\xBB" "\xE7\x81\xB5" "\xE6\xB3\x95" "\xE5\xB8\x88",   // 11
    "\xE5\xBC\x82" "\xE8\x83\xBD" "\xE8\x80\x85",   // 12
    "\xE5\xB0\x8F" "\xE4\xB8\x91",   // 13
};

// The constitution each class adds -- classes.gon `stat_mods { con N }`, same order as
// kClassKeys. Health is four per point of the CON the cat FIGHTS with, i.e. its own plus this
// (measured 2026-09-30: a Medic whose image says CON 5 shows 28/28 on the game's own gear
// screen = (5 + 2) x 4; a Druid at CON 6 has 16 = (6 - 2) x 4).
constexpr int kClassConMod[] = { 0, -1, -1, 2, 4, -1, 0, 0, 3, -2, 0, 2, -1, 0 };

// The class palette rows from classes.gon `graphics { palette N }` -- used when a cat's
// own collar palette index is missing.
constexpr int kClassPalette[] = { 54, 50, 55, 52, 51, 53, -1, 66, 64, 65, 63, 68, 62, -1 };

inline uint8_t brief_class_from_key(const char* key) {
    if (!key) return 255;
    for (uint32_t i = 0; i < kClassCount; ++i) {
        const char* a = key; const char* b = kClassKeys[i];
        while (*a && *a == *b) { ++a; ++b; }
        if (!*a && !*b) return (uint8_t)i;
    }
    return 255;
}

inline const char* brief_class_name(uint8_t k) { return k < kClassCount ? kClassNamesZh[k] : "\xE6\x9C\xAA\xE7\x9F\xA5"; }

} // namespace mgmp
