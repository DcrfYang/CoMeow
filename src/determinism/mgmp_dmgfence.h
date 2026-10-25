// mgmp_dmgfence.h -- EVERY DAMAGE INSTANCE ROLLS FROM A STREAM OF ITS OWN (RNG research R001, 2026-10-24).
//
// Character's damage resolution (R001, 0x140110120: rcx = the character taking it, rdx = the damage instance, whose first word is its source
// object and that object's +0x98 the attacker) rolls on the shared stream: hit/dodge (0x14011038B), crit (0x140110818) and the chance effects the
// instance carries (0x14011090D, 0x140110EBC, 0x140111252, 0x140111512), and the on-hit callbacks it dispatches roll there too. A multi-hit spell,
// a volley, reactions and knockbacks resolve their damage frame by frame, so two peers can resolve the same set of instances in a DIFFERENT ORDER
// -- and then each instance reads another position of the shared stream. In the 10-23/10-24 logs the draw ledger's only differences were these
// sites (8 rooms, most of them halted afterwards).
//
// So each call is fenced, the way mgmp_rngframe fences the passive handlers: the shared stream is set to a seed made of the battle, the current
// action (lockstep_reseed_key: the number the stream was last reseeded with, the same on every peer at the same point), the attacker's and the
// target's creation keys and how many times this attacker has hit this target in this action -- and put back afterwards. Whatever the order, an
// instance rolls the same numbers on every peer; draws inside the fence are not entered into the draw ledger.
// Only in a room's battle; alone, the game's own stream is used. Log tag: DMGFENCE.
#pragma once

#include <cstdint>

namespace mgmp {

int dmgfence_install(uintptr_t base);

} // namespace mgmp
