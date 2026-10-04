// mgmp_spawntest.h -- a DEVELOPER EXPERIMENT (ui.dev_tools only, single player only): make, replace and remove battle units
// with the game's own functions, so the host-authoritative resync can later repair "a unit of another kind at this index" and
// "a unit one peer has and the other has not" instead of halting. The functions and their arguments are read from the
// disassembly (RESEARCH-spawn-destroy-20261003.md); none had been called live before this. What happened is logged under
// SPAWNTEST, and every unit the test touched is followed for the next few turn boundaries -- the point of the experiment
// is to learn whether the game stays consistent afterwards (the battle list, the turn order, the brain, the board).
//
// A button only REQUESTS an operation. It runs at the next turn boundary (h_NextTurn, on the game thread, before the
// original), the same place the board sync applies the host's board -- never from the panel's frame, which may be drawn
// in the middle of anything.
#pragma once

#include <cstdint>

namespace mgmp {

enum class SpawnTestOp : int {
    None = 0,
    Transform,      // an enemy becomes another enemy's kind, in place (sub_1408D3C20)
    Spawn,          // a copy of an enemy's kind is made on a free tile next to it (sub_1407ACF70 + Move + Face + turn order)
    RemoveSilent,   // an enemy is taken off the board with TacticsObject::Remove only: no death, no drop, no deathrattle
    RemoveGameWay,  // the game's DeleteObject sequence: Die + OnCorpsePop + Remove (death events DO run)
    CorruptStats,   // LAYER 2 EXPERIMENT: one of THIS peer's own player cats gets its seven stats and max hp changed, then the game's own recompute is run on it (does it undo that?);
                    // in a session the host's board is armed to write its stats back and the cat is watched for four boundaries
};

void        spawntest_request(SpawnTestOp op);          // any thread; replaces a request not yet run
SpawnTestOp spawntest_pending();
const char* spawntest_op_name(SpawnTestOp op);
const char* spawntest_last_result();                     // one line for the panel

// --- used by the host-authoritative board sync (mgmp_lockstep: board_publish / board_apply) -- the two calls the live test above proved (2026-10-03) ---

// The unit's DEFINITION name, the string the game's spawn / transform functions take: *(Character+0x240)+0x88. NOT Character+0x248, which is the display key
// ('ENEMY_LEAPER_NAME') and made the game stop with "No Character Named ...". False when it cannot be read or does not look like a definition name.
bool unit_definition_name(const void* chr, char* out, unsigned cap);
bool unit_is_champion(const void* chr);

// Replace `old` by a unit of definition `def` in place (the game's own transform, sub_1408D3C20: same tile and facing, its place in the turn order, the other units' references
// moved over, the old unit removed from the board). Game thread, at a turn boundary only. Returns the new Character, or null with `why` saying what was refused. A name is handed to the
// game only after it passed the same check unit_definition_name applies: an unknown name is fatal there.
void* unit_transform_to(void* old, const char* def, const char*& why);

// The unit's board object is marked removed (TacticsObject::Remove ran: off the board, but the Character may stay in the battle list for a boundary or two).
bool unit_is_removed(const void* chr);
// A pickup (coin, scrap, ...): the authored type name starts with "PICKUP_". A unit may stand on the same tile as one, so it does not make a tile "taken".
bool unit_is_pickup(const void* chr);
// The board's size, read off a unit.
bool unit_board_size(const void* chr, int32_t& w, int32_t& h);
// Make a unit of definition `def` and put it on tile (x, y): the game's own summon sequence (spawn, Move, Face, recompute, start, turn order). Null with `why` when refused. `like` is any live
// unit of the battle (its world and facing are borrowed), `tc` the TurnControl. Game thread, turn boundary only.
// `keys` (or null): {initiative key A (+0x954), key B (+0x958), initiative base (+0x5DC)} written BEFORE the unit joins the turn order, which is sorted on them.
void* unit_spawn_at(void* tc, const void* like, const char* def, int32_t x, int32_t y, const int32_t* keys, const char*& why);
// Take a unit off the board silently (TacticsObject::Remove only: no death, no drop, no deathrattle).
bool unit_remove_silent(void* chr);
// The game's own DeleteObject sequence (killers list emptied, Die, OnCorpsePop, Remove): the unit is dead and off the board when this returns, its death events have run.
bool unit_remove_game_way(void* chr);

// h_NextTurn, before the original. Runs the pending request (if any) and reports on the units earlier requests touched.
void spawntest_turn_boundary(void* turn_control);

} // namespace mgmp
