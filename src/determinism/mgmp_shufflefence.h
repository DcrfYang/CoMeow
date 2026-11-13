#pragma once
// A SHUFFLE OF BATTLE UNITS STARTS FROM THE SAME ORDER ON EVERY PEER (0.1.217, sim 58; room H22P 11-09 23:54, battle 138c287e: a FleshGolem halted the battle at once).
//
// sub_140086F00(vec* {u32 cap, u32 count, T** data}, rng*) is a Fisher-Yates shuffle with the stream it is given (the shared one, TLS+0x178). The FleshGolem builder
// (`catdata fleshgolem`, 0x1407A2BA0) collects the summoner's living allied cats from the game's LIVE character list, shuffles them (0x1407A3715, 0x1407A386B) and
// then takes one spell from each in that order, and passives until two are taken. The live list's ORDER is each game's own -- a unit the board sync made or a reused
// address puts entries elsewhere on a client -- so the same allies came out of the shuffle in another order: the host's golem had Revive / PlayDead / Rebuke, both clients'
// Rebuke / Rebirth / Marked (and the passive picks drew a different number of times: the stream before the golem's load differed).
//
// In a room's battle, a vector whose every element is a unit of the roster is put in roster order (the numbering every peer shares) before the game shuffles it: the same
// draws then give the same result on every peer. Any other vector is left as the game made it. Hooked at its rva with a check of its first bytes (the function's code is a
// template the linker folded nowhere else; it has no unique signature).
#include <cstdint>

namespace mgmp {
int shufflefence_install(uintptr_t base);
}
