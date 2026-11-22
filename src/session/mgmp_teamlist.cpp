// mgmp_teamlist.cpp -- see mgmp_teamlist.h.
#include "mgmp_teamlist.h"

#include <algorithm>

namespace mgmp {
namespace {

struct Site { uint32_t rva; TeamUse use; bool build_only; const char* what; };

// The return addresses of the calls of 0x1403B2F30 (the call instruction + 5), Mewgenics build 6a9c51e5.
const Site kSites[] = {
    { 0x923862, TeamUse::All, false, "party_damage" },
    { 0x923AAE, TeamUse::All, false, "party_damage (varying)" },
    { 0x923DCB, TeamUse::All, false, "party_damage (percent)" },
    { 0x927FA9, TeamUse::All, false, "party_status_next_fight" },
    { 0x92A55B, TeamUse::All, false, "party_gain_disorder_from_pool" },
    { 0x92A5DB, TeamUse::All, false, "party_gain_disorder_from_pool (but the subject)" },
    { 0x92CADC, TeamUse::All, false, "party_permanent_stats" },
    { 0x92CB5C, TeamUse::All, false, "party_permanent_stats_exclude_self" },
    { 0x92CBEB, TeamUse::All, false, "party_injury" },
    { 0x92D73B, TeamUse::All, false, "party_heal_injury" },
    { 0x92DD8B, TeamUse::All, false, "party_heal_disorder" },
    { 0x92F0CD, TeamUse::All, false, "party_heal" },
    { 0x92F463, TeamUse::All, false, "party_heal (percent)" },
    { 0x93271B, TeamUse::All, false, "party_random_mutation_from_set" },
    { 0x93321B, TeamUse::All, false, "party_random_mutation" },
    { 0x9347A9, TeamUse::All, false, "make_old all" },
    { 0x3B6F94, TeamUse::All, false, "the battle start's ChanceToAmbush roll" },
    { 0x3B3190, TeamUse::All, true,  "the battle build's global passives of equipped items" },
    { 0x92B48F, TeamUse::Own, false, "level_up all" },
    // 0.1.246: the adventure start (the chapter map's set-off lambda 0x1403B10B0) gives each party cat's passives' bonus_items (Survivalist: FoodMedium x6 +
    // WaterBottle_Full / FoodBig x4) through 0x1400D3450. A client's cats are familiars by then: its Survivalist gave nothing, and a host cat's would land in
    // both backpacks. Own: each player's own cats, into that player's own backpack.
    { 0x3B1165, TeamUse::Own, false, "the adventure start's bonus_items (Survivalist food and water)" },
    // 0.1.164: The Creator (the Infinite's last boss) clones "the team" at the battle start: TheCreator_SpawnCloneTeam spawns PlayerCat_FinalBossClone on each of the
    // level's 4 FinalBossCloneSpot tiles (shuffled) with `catdata teamclone` -- spawn's handler (sub_1407A2BA0) asks this list at 0x1407A39B9, drops the cats already
    // cloned and draws one of the rest (PickRandom, the battle stream: the battle-start window keeps it the same on every peer). With ALL the draw is from every
    // player's cats, sorted by id -- the same list on every peer, so the same cat at the same spot everywhere. One clone per spot: 9 spots in a room since 0.1.165 (mgmp_hooks: creator_spots_extend).
    { 0x7A39BE, TeamUse::All, false, "The Creator's clone team (catdata teamclone)" },
};

bool has(const uint64_t* v, uint32_t n, uint64_t id) {
    for (uint32_t i = 0; i < n; ++i) if (v[i] == id) return true;
    return false;
}

} // namespace

TeamUse teamlist_use(uint32_t rva, bool in_build, const char** what) {
    for (const Site& s : kSites) {
        if (s.rva != rva) continue;
        if (s.build_only && !in_build) break;
        if (what) *what = s.what;
        return s.use;
    }
    if (what) *what = nullptr;
    return TeamUse::Off;
}

uint32_t teamlist_compose(TeamUse use, const uint64_t* party, uint32_t party_n, const uint64_t* fam, uint32_t fam_n,
                          uint8_t self, TeamOwnerFn owner, uint64_t* out, uint32_t cap) {
    if (use == TeamUse::Off || !out || !cap) return 0;
    if (use == TeamUse::Own && !owner) return 0;
    uint32_t n = 0;
    for (int pass = 0; pass < 2; ++pass) {
        const uint64_t* src = pass ? fam : party;
        const uint32_t cnt = pass ? fam_n : party_n;
        for (uint32_t i = 0; src && i < cnt && n < cap; ++i) {
            const uint64_t id = src[i];
            if (!id || has(out, n, id)) continue;
            if (use == TeamUse::Own) {
                uint8_t pos = 0xFF;
                if (!owner(id, pos) || pos != self) continue;
            }
            out[n++] = id;
        }
    }
    std::sort(out, out + n);
    return n;
}

bool teamlist_insert(int32_t loc, int64_t arg, uint8_t self, TeamOwnerFn owner, int32_t& new_loc, bool& skip) {
    skip = false; new_loc = loc;
    if (loc != 3 || !arg || !owner) return false;
    uint8_t pos = 0xFF;
    if (!owner((uint64_t)arg, pos)) return false;          // not a cat of the room's roster: the game's own way
    if (pos == self) { new_loc = 0; return true; }         // this player's own cat: into this player's backpack, as the party's cats' items go
    skip = true;                                           // another player's cat: its owner's game keeps the item
    return true;
}

uint32_t teamlist_creator_spots(const int32_t* xy, uint32_t n, int32_t* add_xy, uint32_t cap) {
    if (!xy || !add_xy || n != 4 || cap < 5) return 0;
    // the leftmost on screen is the one with the smallest x; the line is (x0+i, y0-i)
    uint32_t first = 0;
    for (uint32_t i = 1; i < 4; ++i) if (xy[2 * i] < xy[2 * first]) first = i;
    const int32_t x0 = xy[2 * first], y0 = xy[2 * first + 1];
    for (int32_t k = 0; k < 4; ++k) {
        bool found = false;
        for (uint32_t i = 0; i < 4 && !found; ++i) found = xy[2 * i] == x0 + k && xy[2 * i + 1] == y0 - k;
        if (!found) return 0;
    }
    for (int32_t k = 0; k < 4; ++k) { add_xy[2 * k] = x0 + k - 1; add_xy[2 * k + 1] = y0 - k; }   // down-left of each
    add_xy[8] = x0 + 3; add_xy[9] = y0 - 4;                                                      // down-right of the rightmost
    return 5;
}

} // namespace mgmp
