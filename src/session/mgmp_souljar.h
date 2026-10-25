// mgmp_souljar.h -- THE SOUL JAR IN A ROOM (2026-10-22: "the client crashed right after the host's Soul Jar went off").
//
// The item, in the game's data (items/trinkets.gon, abilities/item_abilities.gon):
//   - SoulJar: passive `DropSoulJarOnDeath SoulJar_Full`. When the cat's body is destroyed, the lambda at 0x140070F90 takes the jar off the cat
//     (0x1400CA510, slot 4), builds SoulJar_Full with aux = the dead cat's id (CatData+0xC48, truncated to 32 bits) and puts it into the global
//     inventory (Inventory::insert_item, called at 0x1400712C3) -- of WHICHEVER GAME RUNS THE CODE.
//   - SoulJar_Full (aux_is_catid): ability tk_SoulJar spawns PlayerCat_Shade with `catdata item_aux_catid`. SpawnAbility (0x1407A2BA0, the branch at
//     0x1407A3C2A) finds the item (0x1400475C0: the root ability's +0x30 item uid on its character), then copies CatDatabase::get(registry, aux)
//     (0x1400D7220) with the CatData copy constructor 0x1400D7900 -- whose first read, `mov rax,[rdx]` at 0x1400D791B, is of the lookup's answer
//     and has NO null check (the tooltip's {aux_cat_name}, 0x1407C2F10, does check and says "unknown cat").
//
// In a room every peer runs both halves for every cat, each against ITS OWN save:
//   1. a cat's death put a SoulJar_Full into every player's inventory, not only its owner's;
//   2. a summon looked the aux id up in every peer's own save. The id belongs to the jar owner's save (an ordinary id, or a session id of the run
//      it was filled in): on another peer it is no cat at all (a null read -> the crash) or another cat (a silent desync).
//
// What this module does, in a room:
//   1. the jar a dying cat drops goes into its OWNER's inventory only (the cat's session id names the owner); the cat itself loses the jar on every
//      peer alike, as before;
//   2. at every battle build (the party stage) each player looks at its own cats' equipped SoulJar_Full, takes the cat each one holds from its own
//      save, and sends that cat's image (MSG_SOULCAT, serialized by the game). Every peer -- the owner too -- builds a CatData from those bytes,
//      and a summon copies THAT: the same ghost everywhere. A jar whose cat the owner's own save does not hold, and a lookup nobody published for,
//      summons a copy of the caster instead of reading a null pointer.
// Outside a room only the null check is added (the game's own answer is used whenever there is one).
//
// Log tag: SOULJAR (every drop, publication, arrival and summon, on every peer).
#pragma once

#include <cstdint>

namespace mgmp {

struct SoulCatMsg;

// From hooks_install, before the hooks are live: checks the three call sites this module keys on (a drifted game build turns it off, loudly).
void souljar_set_base(uintptr_t base);

// T_AbilityItem (0x1400475C0, the item an ability comes from), after the original: the summon's own call notes the caster and the item for
// the lookup that follows. Every other caller is ignored.
void souljar_note_item(void* ability, void* item, uintptr_t ret);

// T_CatById (0x1400D7220, CatDatabase::get): the summon's lookup is answered here. True = `out` is the answer (the original may have been
// called through `original` to find it); false = not the summon's call, run the original.
bool souljar_cat_lookup(void* registry, uint64_t id, uintptr_t ret, void* (*original)(void*, uint64_t), void*& out);

// T_InvInsert: true = this insert is the soul jar a dying cat dropped and the cat is another player's -- do not put it into this inventory.
bool souljar_drop_skip(void* item, int32_t loc, uintptr_t ret);

// The battle build's party stage has run: this player's jars publish the cats they hold.
void souljar_on_party_built();

// MSG_SOULCAT from the jar's owner.
void souljar_on_message(uint8_t from, const SoulCatMsg& m);

// A session starts: what the last one's jars held is forgotten (0.1.83: a jar whose owner fails to publish must not summon an old room's cat).
void souljar_reset();

} // namespace mgmp
