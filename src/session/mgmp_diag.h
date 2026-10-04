// mgmp_diag.h -- the diagnostics that let ONE uploaded log name the cause of a desync (proto 80).
//
// The log of a halt used to say WHERE two peers differed (a turn, a unit, a field) but not WHY when the cause was inside an action: the shared random stream drifts and the only evidence was two different
// numbers on a RESEED line. Three records close that, each sent to the other peer and compared there, so both logs (and the one uploaded copy, through the log tail the peers share on a mismatch) carry both sides:
//
//   * the RNG LEDGER  -- per action: how many draws the shared stream served, from which call sites, and the order. A difference names the site that drew a different number of times.
//   * the DEEP digest -- per turn boundary, per unit: the values the state hash does not cover (stats, speed, turn-order keys). Taken before the host's board can repair them, so a repaired value is still reported.
//   * the ENVIRONMENT -- who each peer is: game executable, mod build, ruleset, OS, the switches that change what is simulated. A desync between two builds is then visible in the log's first lines.
//
// Nothing here changes a battle and nothing halts: every difference is logged, counted in SUMMARY-SYNC and followed by the shared log tail.
#pragma once

#include <cstdint>
#include "mgmp_proto.h"

namespace mgmp {

// Game thread, at every action boundary (next to unlockq_flush): take the draws since the last one, keep them, send them, compare with the peer's for the same flush number.
void rngl_flush(uint32_t turn, uint32_t action);
void rngl_on_peer(uint8_t from, const RnglMsg& m);
// `mismatched`: the draws inside the game's apply-action calls differ (a real divergence); `loose`: the ones between actions differ (an owner's preview, or the AI deciding differently).
void rngl_stats(uint32_t& compared, uint32_t& mismatched, uint32_t& loose);

// One unit's derived values, read by the caller (mgmp_lockstep owns the offsets).
struct DeepRow {
    bool    ok = false;
    int32_t speed = 0, key_a = 0, key_b = 0, init_base = 0, maxhp = 0, bonus = 0;
    int32_t stat[7] = {};
};
// Game thread, at the turn boundary, before the board is applied.
void deep_publish(uint64_t battle, uint32_t turn, const DeepRow* rows, uint32_t n);
void deep_on_peer(uint8_t from, const DeepMsg& m);
void deep_stats(uint32_t& compared, uint32_t& mismatched);

// The line that says who this peer is (also the one logged locally). Returns the text length.
size_t diag_environment_text(char* out, size_t cap);

} // namespace mgmp
