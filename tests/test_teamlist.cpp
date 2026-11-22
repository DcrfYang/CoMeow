// test_teamlist.cpp -- mgmp_teamlist: which callers of "the party" get which list, how the list is composed, loc 3 of insert_item.
#include "mgmp_teamlist.h"
#include <cstdio>
#include <cstring>

using namespace mgmp;
static int fails = 0, checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++fails; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

// seat = the byte above the low 24 bits of a session id: 0x70.. seat 0, 0x71.. seat 1, 0x72.. seat 2; anything else is not a room cat
static bool owner(uint64_t id, uint8_t& pos) {
    if ((id >> 28) != 7) return false;
    pos = (uint8_t)((id >> 24) & 0xF);
    return true;
}

int main() {
    const char* what = nullptr;
    // the sites
    CHECK(teamlist_use(0x92F0CD, false, &what) == TeamUse::All && what && strstr(what, "party_heal"));
    CHECK(teamlist_use(0x923862, false, nullptr) == TeamUse::All);
    CHECK(teamlist_use(0x92B48F, false, &what) == TeamUse::Own && strstr(what, "level_up"));
    CHECK(teamlist_use(0x3B1165, false, &what) == TeamUse::Own && strstr(what, "bonus_items"));   // 0.1.246: Survivalist at the adventure start
    CHECK(teamlist_use(0x3B6F94, false, nullptr) == TeamUse::All);              // the ambush roll, at any node entry
    CHECK(teamlist_use(0x3B3190, true, nullptr) == TeamUse::All);               // the global passives -- inside the battle build
    CHECK(teamlist_use(0x3B3190, false, &what) == TeamUse::Off && !what);       // ... and the same function elsewhere (shop, settlement): left alone
    CHECK(teamlist_use(0x1C9835, false, nullptr) == TeamUse::Off);              // the class marks have their own fix (0.1.131)
    CHECK(teamlist_use(0x7A39BE, false, &what) == TeamUse::All && what && strstr(what, "Creator"));   // 0.1.164: the Creator's clone team draws from every player's cats
    CHECK(teamlist_use(0x7A39BE, true, nullptr) == TeamUse::All);               // ... at the battle start, inside the build or not
    CHECK(teamlist_use(0, false, nullptr) == TeamUse::Off);

    // the whole team: host 3 (party, in the game's order) + the client 4 (familiars), the same sorted list whatever the order and the promotion
    const uint64_t host_party[] = { 0x70000002, 0x70000001, 0x70000003 };
    const uint64_t fam[] = { 0x71000003, 0x71000001, 0x71000002, 0x71000004 };
    uint64_t out[16] = {};
    uint32_t n = teamlist_compose(TeamUse::All, host_party, 3, fam, 4, 0, owner, out, 16);
    CHECK(n == 7);
    const uint64_t want[] = { 0x70000001, 0x70000002, 0x70000003, 0x71000001, 0x71000002, 0x71000003, 0x71000004 };
    CHECK(n == 7 && !memcmp(out, want, sizeof(want)));
    // a promoted party (the game moved one familiar into the party): the same list, nothing twice
    const uint64_t promoted[] = { 0x70000002, 0x70000001, 0x70000003, 0x71000003 };
    const uint64_t fam_left[] = { 0x71000001, 0x71000002, 0x71000004, 0x71000003 };
    uint64_t out2[16] = {};
    CHECK(teamlist_compose(TeamUse::All, promoted, 4, fam_left, 4, 1, owner, out2, 16) == 7 && !memcmp(out2, want, sizeof(want)));
    // the client's view (the shared layout) gives the host's view's list
    uint64_t out3[16] = {};
    CHECK(teamlist_compose(TeamUse::All, host_party, 3, fam, 4, 1, owner, out3, 16) == 7 && !memcmp(out3, out, 7 * 8));

    // this player's own: the host its three, the client its four, a third seat none
    uint64_t own[16] = {};
    n = teamlist_compose(TeamUse::Own, host_party, 3, fam, 4, 0, owner, own, 16);
    CHECK(n == 3 && own[0] == 0x70000001 && own[2] == 0x70000003);
    n = teamlist_compose(TeamUse::Own, host_party, 3, fam, 4, 1, owner, own, 16);
    CHECK(n == 4 && own[0] == 0x71000001 && own[3] == 0x71000004);
    CHECK(teamlist_compose(TeamUse::Own, host_party, 3, fam, 4, 2, owner, own, 16) == 0);
    CHECK(teamlist_compose(TeamUse::Own, host_party, 3, fam, 4, 0, nullptr, own, 16) == 0);    // no owner lookup: nothing
    // zeros and empty lists, the cap
    const uint64_t with_zero[] = { 0, 0x70000001, 0 };
    CHECK(teamlist_compose(TeamUse::All, with_zero, 3, nullptr, 0, 0, owner, out, 16) == 1);
    CHECK(teamlist_compose(TeamUse::All, nullptr, 0, nullptr, 0, 0, owner, out, 16) == 0);
    CHECK(teamlist_compose(TeamUse::All, host_party, 3, fam, 4, 0, owner, out, 5) == 5);
    CHECK(teamlist_compose(TeamUse::Off, host_party, 3, fam, 4, 0, owner, out, 16) == 0);

    // The Creator's clone spots: the vanilla line in any order -> down-left of each, down-right of the rightmost
    const int32_t spots[] = { 6, 5, 4, 7, 7, 4, 5, 6 };
    int32_t add[10] = {};
    CHECK(teamlist_creator_spots(spots, 4, add, 5) == 5);
    const int32_t want_add[] = { 3, 7, 4, 6, 5, 5, 6, 4, 7, 3 };
    CHECK(!memcmp(add, want_add, sizeof(want_add)));
    const int32_t shifted[] = { 1, 2, 2, 1, 0, 3, 3, 0 };                      // relative to another origin: the same shape
    CHECK(teamlist_creator_spots(shifted, 4, add, 5) == 5 && add[0] == -1 && add[1] == 3 && add[8] == 3 && add[9] == -1);
    const int32_t nine[] = { 4, 7, 5, 6, 6, 5, 7, 4, 3, 7, 4, 6, 5, 5, 6, 4, 7, 3 };
    CHECK(teamlist_creator_spots(nine, 9, add, 5) == 0);                      // already extended
    const int32_t bent[] = { 4, 7, 5, 6, 6, 5, 7, 5 };
    CHECK(teamlist_creator_spots(bent, 4, add, 5) == 0);                      // not a line
    const int32_t dup[] = { 4, 7, 4, 7, 6, 5, 7, 4 };
    CHECK(teamlist_creator_spots(dup, 4, add, 5) == 0);
    CHECK(teamlist_creator_spots(spots, 3, add, 5) == 0 && teamlist_creator_spots(spots, 4, add, 4) == 0 && teamlist_creator_spots(nullptr, 4, add, 5) == 0);

    // insert_item loc 3
    int32_t loc = -1; bool skip = true;
    CHECK(teamlist_insert(3, 0x71000002, 1, owner, loc, skip) && !skip && loc == 0);         // the client's own cat: its backpack
    CHECK(teamlist_insert(3, 0x70000001, 1, owner, loc, skip) && skip);                      // the host's cat, on the client: not here
    CHECK(teamlist_insert(3, 0x70000001, 0, owner, loc, skip) && !skip && loc == 0);         // the host's own
    CHECK(!teamlist_insert(3, 0x123, 0, owner, loc, skip) && !skip && loc == 3);              // not a room cat: the game's way
    CHECK(!teamlist_insert(0, 0x71000002, 1, owner, loc, skip) && loc == 0 && !skip);         // other locations untouched
    CHECK(!teamlist_insert(3, 0, 1, owner, loc, skip));

    std::printf("test_teamlist: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
