// mgmp_rngframe.h -- the per-event stream fence over the battle's frame-driven passive handlers (2026-10-11, TODO(RNG-FRAME), first stage).
//
// WHY. A passive that fires while an action is still playing out -- after a cast, on a hit, on a death, when a projectile arrives -- or on the
// frame a condition comes true (late_update) draws from the shared stream at a moment set by the animation, and other things in the same action
// draw at moments of their own. Which comes first is frame timing, so each peer can hand the same draws to different effects. ModelingClay's
// weapon change halted a room that way (rngfix_clay). The second-pass study (调研任务列表/结果/局内随机效果排查.md) found 237 such handlers.
//
// WHAT. Every handler in mgmp_rngframe_table.inc (generated from that study; groups A "during an action" and B "per frame", 215 after the
// automatic argument checks) is wrapped. In a room, in a battle, the wrapper saves the shared stream, runs the handler on a stream of its own
// -- seeded from (battle, turn, the owner unit's creation key, the handler, n) -- and puts the shared stream back. n counts this unit's earlier
// calls of this handler in this turn that actually drew; per-frame handlers use n = 0 (their call count follows the frame rate). The result then
// depends on which events happened, not on their order in the frame. Draws inside a fence are left out of the RNG ledger (mgmp_rng).
//
// NOT COVERED YET (TODO(RNG-FRAME)): the handlers of 局内随机效果排查.csv group C (timing not labelled), ability closures pushed into a unit's
// queue (R195 and kin), tiles, and anything that reaches the stream through a std::function the static study could not follow.
#pragma once

#include <cstdint>

namespace mgmp {

// After MinHook is initialised (hooks_install): verifies each table entry's first bytes and installs the wrappers. Returns how many went in.
int rngframe_install(uintptr_t base);

} // namespace mgmp
