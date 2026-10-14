// mgmp_rngfix.h -- fixes from the RNG research of 2026-10-11 (调研任务列表/结果/总结.md, 635 functions), and the markers for the ones not done yet.
//
// DONE here (each hook is thin, in mgmp_hooks.cpp; the work is in mgmp_rngfix.cpp):
//   R430 R431 R434-R440  event keywords that drew from the shared stream without the reset every other T_Ev* has   -> rngfix_event_keyword
//   R369 (P0)            DelayedTrigger's PER-FRAME target re-pick (inline xoshiro on the shared stream)          -> rngfix_delayed_target (fence)
//   R102                 Character late_update's per-frame AI-intent display pick                                -> rngfix_pick_index (fence)
//   R027 (P0)            the battle build applies global_passives of THIS PEER'S run backpack to the whole battle -> rngfix_battle_backpack (ignored in a room)
//   R373 R386 R388 (P0)  the "decision-time window": slot 52 OnAnyCharacterMakesDecision runs after UpdateDecision on a stream the owner's
//                        aim preview moved                                                                     -> lockstep_reseed_decision (mgmp_lockstep.cpp)
//   (not in the research) ModelingClay's weapon change, drawn when the throw lands                      -> rngfix_clay
//   R270                 the level's next WAVE, spawned mid-battle the frame no kill_required enemy is left
//                        (e.g. desert111: 4 more bounty hunters) -- from a stream of its own, the shared one put back -> rngfix_wave
//
// NOT DONE -- grep the tree for "TODO(RNG-" to find every open spot; the RNGL ledger lines name the research item of a differing site
// (rngfix_site_tag), so a desync log points straight at the entry in 批次NN.md:
//   TODO(RNG-FRAME)  FIRST STAGE DONE 2026-10-11 (0.1.22, mgmp_rngframe): 215 passive handlers that fire during an action or per frame run on
//                    per-event streams. Still open: 局内随机效果排查.csv group C, ability closures (R195), tiles. Originally:
//                    the frame-driven class (总结.md 2.1, 26 items): several sequences inside ONE action advance with the animation and each draws
//                    from the shared stream, so their interleaving (frame timing) decides who gets which number. Source: R195 reaction closures pushed
//                    into the unit's own queue [unit+0x78] by 0x1402A8A60; also R001 per-damage-instance procs, R261 projectiles, R266/R269 knockback,
//                    R268 dice. Fix shape: give each sequence its own stream seeded from (battle, turn, action, unit, n-th closure) and put the shared
//                    stream back around it. mgmp_rng's kIsolated (SpawnThingOnDamage only) is the one point done so far.
//   TODO(RNG-R268)   Dicer's dice redraw their pips on the `roll` animation event; DicerBrain (slot 31, 0x140160AEC) picks its D6 ability from them.
//   TODO(RNG-R013)   AOE landing particle delays draw through an inline helper (0x14094F1F0, return 0x140097EFD): invisible to the ledger.
//   TODO(RNG-R438)   scramble_basic_attack builds its candidates from THIS peer's unlocked classes (outside a battle the save answers).
//   TODO(RNG-R027)   the backpack's global passives are ignored in a room; the faithful version is the host sending the item names with the build.
//   TODO(RNG-R440)   spawn_unit_next_fight is reset at entry now; the robust version is the host sending MewDirector+0x610 with the level.
#pragma once

#include <cstdint>

namespace mgmp {

// The event keyword handlers (R430 ...): resets the shared stream (unlocks_event_roll) with a line naming the keyword.
void rngfix_event_keyword(const char* rid, const char* keyword);

// R369: `orig` is DelayedTrigger's target computation (this, out) -> out. Called for the per-frame (late_update) call site only.
typedef void* (__fastcall* fn_rngfix_2)(void*, void*);
void* rngfix_delayed_target(void* self, void* out, fn_rngfix_2 orig);

// R102: `orig` is the random-element helper (out, vector, stream). Called for Character late_update's call only.
typedef void* (__fastcall* fn_rngfix_3)(void*, void*, void*);
void* rngfix_pick_index(void* out, void* vec, void* stream, fn_rngfix_3 orig);

// R027: `vec` is the vector the battle build's backpack read returned ({u32, u32 count, Item** data}); in a room its count is set to 0.
void rngfix_battle_backpack(void* vec);

// R270: the extra-spawn build stage called by Level's per-frame update (outside the build window): the level's next wave. `orig` is the stage.
// `wave` is the stage's own second argument (the wave whose objects it spawns) -- it must reach the original unchanged.
typedef void* (__fastcall* fn_rngfix_wave)(void*, int32_t);
void* rngfix_wave(void* level, int32_t wave, fn_rngfix_wave orig);

// ModelingClay (found 2026-10-11 from a halt; the research had missed it): `orig` is the passive's after-cast handler (this, ...). In a room the
// weapon it turns into is drawn from a stream of (battle, the cat, how many times this cat's clay changed in this battle), the shared one put back.
typedef void* (__fastcall* fn_rngfix_4)(void*, void*, void*, void*);
void* rngfix_clay(void* self, void* a, void* b, void* c, fn_rngfix_4 orig);

// "R001/P1" for a draw site (return RVA, as the ledger keys it) that the research flagged; nullptr otherwise.
const char* rngfix_site_tag(uint32_t rva);

} // namespace mgmp
