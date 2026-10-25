// mgmp_gains.h -- WHAT A CAT GAINS IN A BATTLE GOES TO ITS OWNER (2026-10-24 health check).
//
// Every peer runs every cat's abilities and passives, and the game puts what they produce into THIS game's own inventory (the global at
// 0x1413C7560). In a room that gave every player a copy of every item another player's cat found, and every coin it picked up -- measured in
// the 10-22 logs: the host's Hunter's FindItemFromPool put an item into both players' backpacks five times in one run (and the first two were not
// even the same item: each game rolls from its own save's item pool).
//
// What this module does, in a room's battle:
//   - ITEMS. The functions that put an item into the inventory for a cat are wrapped, and while one runs the cat it works for is noted:
//       - the ability effects, through their per-effect wrappers (rcx = the character the effect is on, r9 = the caster, or the 5th argument's
//         ability +0x10): FindItem, FindItemFromPool, StealEquipment for the CASTER; DuplicateRandomEquippedItem, DestroyEquipmentAndAttachParasite,
//         GiveBoundItemToTarget for the TARGET;
//       - the passives and battle-end effects (the cat is the passive's +0x38): RerollItemsOnBattleEnd, FindExtraItemFromPoolOnBattleEnd,
//         ReturnBoundItemOnBattleEnd, ReclaimItemOnBreak; Regurge's trinket swap (the cat in rdx); RockyArmorPassive (wrapped by mgmp_rngframe,
//         whose current passive is read);
//     h_InvInsert asks gains_insert_skip: the item of a player cat another player owns is not put into this inventory (its owner's game puts it into
//     its own). An enemy's, a summon's and this player's own cats' gains are left as the game does them.
//   - COINS. Character::GainCoins (0x140114E30, every coin a character gains or pays: pickups, GainCoins effects, ability costs) adds to this
//     game's inventory for a player-side character. For a cat another player owns it still runs whole (animations, the passives it triggers), and
//     the inventory's coins are put back afterwards.
// Outside a room, or outside a battle (no roster), nothing changes. Log tag: GAINS.
#pragma once

#include <cstdint>

namespace mgmp {

// After MH_Initialize: checks each target's first bytes and wraps it. Returns how many went live.
int  gains_install(uintptr_t base);

// h_InvInsert: true = this item is another player's cat's -- do not put it into this inventory.
bool gains_insert_skip(void* item, int32_t loc, uintptr_t ret);

} // namespace mgmp
