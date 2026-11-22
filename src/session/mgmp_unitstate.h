// mgmp_unitstate.h -- THE DEEP STATE OF A BATTLE UNIT (0.1.173, proto 113; research: RESEARCH-battle-sync-20261104.md).
//
// The turn hash covered a unit's hit points, tile, death and elements, and the host's board its numbers (hp, tile, turn-order keys, faction, mana).
// Everything else a unit carries -- its statuses and passives, its abilities' state, its gear's durability, its stats -- could part between two games
// and stay apart until it changed a roll or a decision (a status on one game only, an ability one game had refreshed ...). This module reads ALL of it:
//
//   passives    every live component of the unit whose class hierarchy holds glaiel::Passive (1414; a status also holds 1838): its type id, its
//               stacks (+0x5C), its flags (+0x70/+0x74/+0x78), its class name (= the name the game's factories create it by). Removed ones (+0x75) are left out.
//   abilities   move / attack / bonus / every spell slot: the GON name and the fields both games agree on (+0x34..+0x6C, +0xC8..+0x100; +0x30 is
//               the source item's per-save id and +0x7C/+0x8C/+0xAC/+0x104 are leftover bytes -- not state).
//   gear        a cat's five item slots: name, durability (+0x48), aux (+0x4C) (the slot's id +0x00 is per save).
//   numbers     a list of Character fields (faction, base and current stats, initiative base, mana, move points).
//
// The host keeps every unit's record of each boundary; a unit's 32-bit digest rides on its board row (BoardUnit.deep), so a client finds a difference
// at the next boundary, asks for that unit's record (MSG_USTATEREQ, answered on the host's receive thread) and puts its own copy right with the game's
// own functions: a status or passive it lacks is created by the factory the game uses (ApplyStatus 0x140483120 / AddPassive 0x1404CCB20), one it
// holds alone is removed (Passive::Remove 0x140767FA0), stacks, ability fields, gear durability and the listed numbers are written. Grep USYNC.
//
// The three factories are hooked as well (T_ApplyStatus, T_AddPassive, T_PassiveRemove): in a room's battle every status and passive given or
// taken is logged with its unit and the game's call site (USTATUS) -- where a difference started is in the log of the game that has it.
#pragma once
#include <cstdint>

namespace mgmp {

constexpr uint32_t kUsPassives  = 64;
constexpr uint32_t kUsAbilities = 16;
constexpr uint32_t kUsNameLen   = 48;
constexpr uint32_t kUsAbilInts  = 29;   // 14 from +0x34..+0x6C, 15 from +0xC8..+0x100
// THE ABILITY'S TILE LIST IS NOT STATE (0.1.183). +0xE0..+0xEF (record ints 20..23) is {u32 cap, u32 count, iVec2* data}: the tiles a special_tagged_tiles range
// (ChaosSwitchSides, SummonBear...) or a custom AOE was built from, rebuilt by the game from the GON and the level whenever Ability::can_cast sees a new battle
// generation (+0x6F0). Its data is a HEAP POINTER of the process that holds it: reading it made the digest differ on every peer, and the repair wrote the HOST's
// pointer into this machine's ability -- the next rebuild reallocated a foreign pointer and the game died in the heap (2026-11-04, Rifthead fight, C0000005 in ntdll).
// These ints are always 0 in the record; the repair never writes them.
constexpr uint32_t kUsAbilTileFirst = 20, kUsAbilTileLast = 23;
constexpr uint32_t kUsItems     = 5;
constexpr uint32_t kUsCharInts  = 27;   // 0.1.176 (proto 114): + max hp 0x4BC; 0.1.190 (proto 115): + the hp bonus base 0x5EC

// `data` (0.1.190): the value of the passive's GON node (Passive+0x68 -> a node of kind 1/2/5, its string +0x68) -- ReplaceBasicAttack's attack, a random pool's pick.
// Two passives of one class with other data are different passives (KD8E test: a Needlecat the board made rolled 'ReplaceBasicAttack BasicRanged' from its random
// passive pool, the host's 'BasicButcherMelee' -- the class matched, and the passive put its own attack back after every repair).
struct UsPassive { uint16_t type = 0; uint8_t status = 0; int32_t stacks = 0; uint32_t f70 = 0, f74 = 0, f78 = 0; char name[kUsNameLen] = {}; char data[kUsNameLen] = {}; };
struct UsAbility { uint8_t slot = 0; char name[kUsNameLen] = {}; int32_t f[kUsAbilInts] = {}; };
struct UsItem    { char name[kUsNameLen] = {}; int32_t dur = 0, aux = 0; };
struct UnitDeep {
    uint8_t   ok = 0;          // read at all
    uint8_t   np = 0, na = 0;
    uint8_t   items = 0;       // the unit is a cat with item slots
    UsPassive p[kUsPassives];
    UsAbility a[kUsAbilities];
    UsItem    it[kUsItems];
    int32_t   c[kUsCharInts] = {};
};

// The Character offsets of UnitDeep::c, and which of them a repair writes (the others are the board's to put right, or only compared).
extern const uint16_t kUsCharOff[kUsCharInts];

// GAME THREAD, a unit in the live list only (the reads include virtual getters of its components).
bool unitstate_read(const void* chr, UnitDeep& out);
uint32_t unitstate_digest(const UnitDeep& d);     // never 0 for a read record; 0 = not read

// The wire form of one unit's record (MSG_USTATE payload, type byte included).
uint32_t unitstate_encode(uint8_t* p, uint32_t cap, uint64_t battle, uint32_t turn, uint8_t index, const UnitDeep& d);
bool     unitstate_decode(const uint8_t* p, uint32_t n, uint64_t& battle, uint32_t& turn, uint8_t& index, UnitDeep& d);

// CLIENT, game thread: puts `chr` (roster index `index`) right toward the host's record. `mine` is this game's reading at the same boundary.
// Returns the number of things changed; every change and every difference it cannot repair is logged (USYNC). The shared stream is fenced.
// 0.1.190: a unit whose ability bar is not the host's gets the host's abilities first (by name, the game's own factory); `player_cat` = a player's cat (its cat data's
// spell names are its save's and are not written).
// 0.1.192 `own_cat`: this player's own cat (an item of another name is swapped with this player's backpack).
uint32_t unitstate_repair(uint8_t index, void* chr, const UnitDeep& host, const UnitDeep& mine, bool player_cat, bool own_cat = false);
// 0.1.238 (sim 62), GAME THREAD: every ReplaceBasicAttackWhenCastable of `chr` runs its check now (the game's per-frame check, vtable slot 11) -- called at every boundary
// on every peer before the hash, and by the repair after the board. Returns how many swaps changed.
uint32_t unitstate_settle_castable(void* chr, const char* why);
// 0.1.247, GAME THREAD: `chr` carries a live passive of the game class `name` (its own or an item's; destroyed, removed and ending ones do not count).
bool unitstate_has_passive(const void* chr, const char* name);
// 0.1.242 (sim 64), GAME THREAD: every XIs* passive of `chr` writes its X into its ability now (vtable slot 83) -- before the unit acts and at every boundary, on every peer.
uint32_t unitstate_settle_xvalues(void* chr);
// The log of what differs, without changing anything (used for the units a repair could not settle).
// `cols` names the two sides in the line (0.1.233: the failed-resync diagnosis also compares two readings of this game).
void unitstate_log_diff(uint8_t index, const UnitDeep& host, const UnitDeep& mine, const char* when, const char* cols = "host|here");

// The hooks (mgmp_hooks): what the game gave or took, for the log. `name` is the factory's name argument (read before the call), `ra` the caller's rva.
void unitstate_note_apply(bool status, const char* name, const void* target, int32_t stacks, const void* source, uint32_t ra, const void* made);
void unitstate_note_remove(const void* passive, uint32_t ra);
void unitstate_battle_reset();    // a new battle: the log caps start again

// The game's own calls, through the hooks' originals (mgmp_hooks). Null when a hook is not live.
void* hooks_call_apply_status(void* name, void* target, int32_t stacks);
void* hooks_call_add_passive(void* name, void* target, int32_t stacks);
bool  hooks_call_passive_remove(void* passive);

// DEV: a deliberate difference on this game only (harness `usdev status Poison 3` ...). See the .cpp.
bool unitstate_dev(const char* cmd);

} // namespace mgmp
