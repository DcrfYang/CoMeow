// mgmp_balance.h -- multiplayer balance: how much tougher the enemies are in a room (2026-10-01).
//
// In a multiplayer room every ENEMY character starts with kEnemyScale times the health, the maximum health
// and the armor its definition gives it. "Enemy" is decided by the faction the definition names (the same
// faction the game uses to decide who fights whom). Allies, neutral objects (rocks, walls, bombs), third
// parties and a player's own cats are untouched.
//
// WHERE: the definition loader (T_LoadChar) -- the one place a character's numbers come in. A boss with several
// phases is several definitions (ThrobbingKing -> ThrobbingKing2, TheCreator -> TheDestroyer -> TheChild ...),
// each built through the same loader when its turn comes, so every phase is scaled with no extra code. The
// copies of enemies that a battle makes later (summons, split slimes, spawn-on-death) come through the same
// loader and are scaled too; the user said those MAY stay as they are, and telling them from the spawn of a
// phase change would need more hooks than the rule is worth.
//
// WHAT, in the character's own terms (see T_LoadChar in mgmp_addresses.h):
//   maximum health = 4 * constitution + kChr_HpBonus   -> the bonus is chosen so the maximum is exactly
//                                                         kEnemyScale times what the definition said
//   kChr_HpBonusBase                                    -> the value the bonus is rebuilt from whenever
//                                                         passives change; scaled alike, or the first
//                                                         rebuild would undo the doubling
//   current health                                      -> only when the definition names an initial_health
//                                                         (otherwise the game fills it to the maximum itself)
//   armor (kChr_Shield)                                 -> times kEnemyScale
//
// Every peer applies the same rule to the same definition, so the lockstep state stays identical; the protocol
// version is bumped so a peer without the rule cannot join one with it.
#pragma once

#include <cstdint>

namespace mgmp {

constexpr int32_t kEnemyScale = 2;

// Is a faction value (the game's enum) an enemy faction: enemies 2, cavemen 8, sabertooths 9, mammoths 10,
// kaiju1 11, kaiju2 12.
bool balance_is_enemy_faction(int32_t faction);

// Scale a character the definition loader has just filled. `hp_before` is its current health from BEFORE the
// loader ran: the loader only writes health for an initial_health definition, and a character that already
// had its health (a reload in place) must not be doubled a second time. True when anything changed.
bool balance_scale_enemy(void* chr, int32_t hp_before);

// THE RULESET both peers must share (2026-10-01): everything that changes what a battle computes but is not in the
// game's data -- the enemy multiplier and the test aid. Folded into the handshake identity, so two builds (or two
// configs) with different rules refuse each other at the door instead of halting at turn 0.
uint64_t balance_ruleset_id();

} // namespace mgmp
