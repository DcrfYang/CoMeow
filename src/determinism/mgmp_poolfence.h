// mgmp_poolfence.h -- the random picks from the AnyUnlocked ability pool (and its kin), on a stream both peers derive (0.1.75).
//
// WHY. Room QQA9 (2026-10-23): the quest antenna replaced a spell with a random one of the AnyUnlocked pool. The pool was the same on both
// peers (in a battle the unlock answers are the host's), but the one draw came from the shared stream at a point a multi-hit spell's per-hit
// procs had moved differently on each peer (RNG research R001) -- 'Anoint' here, 'RemoteDetonator' there, and the next cast halted the battle.
// The antenna has its own hook (T_AlliesScramble, mgmp_hooks.cpp). The research (调研任务列表/结果/批次08.md, 16, 27) lists the other functions
// that pick from the same pools inside an action; mgmp_rngframe already fences the Scrambled status (R075) and ConjureBonusAbility (R106).
//
// WHAT. Each function below is wrapped: in a room, in a live fight, the shared stream is saved, set from (battle, turn, the acting unit, the
// action's number, the n-th such pick in it, the function) -- lockstep_stream_for_pick -- the function runs, and the shared stream is put back.
// Both peers then pick the same entry whatever the stream had been through; the rest of the action draws as before. Draws inside are left out
// of the RNG ledger, as mgmp_rngframe's are. Outside a room or a fight the functions run untouched.
#pragma once

#include <cstdint>

namespace mgmp {

// After MinHook is initialised (hooks_install): verifies each entry's first bytes and installs the wrappers. Returns how many went in.
int poolfence_install(uintptr_t base);

} // namespace mgmp
