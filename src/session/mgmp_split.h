#pragma once
// mgmp_split -- who controls which cats, as arithmetic.
//
// Extracted for the same reason td_decide and barrier_decide are: exercising it
// in the game needs three or four live instances and a battle with the right
// number of human cats, which is an expensive way to find out that a division
// rounded the wrong way. Here it is a table.
//
// THE RULE. The battle's human-driven cats are handed out in roster order,
// contiguously, one run per player, earliest positions first. Every player gets
// floor(humans/P), and the first (humans % P) players get one extra -- so the
// remainder lands on the host and the players nearest it, which is the same
// preference the two-player rule had when it gave the host the odd cat.
//
//   4 cats, 2 players  ->  2 / 2
//   3 cats, 2 players  ->  2 / 1        (the old (humans+1)/2, unchanged)
//   4 cats, 3 players  ->  2 / 1 / 1
//   4 cats, 4 players  ->  1 / 1 / 1 / 1
//   2 cats, 4 players  ->  1 / 1 / 0 / 0
//
// POSITION, NOT PEER ID. `pos` is an index into the sorted membership list.
// The two are the same until somebody disconnects, after which ids have a gap
// and positions do not. Deriving the split from ids would silently shift every
// later player's cats the moment an earlier one dropped -- and shifting cats
// mid-run means two players briefly claim the same one, which is a desync on
// its first turn.
//
// CONTIGUOUS, NOT INTERLEAVED. Adjacent cats per player make the roster log
// readable by eye, which is how every control bug so far has actually been
// spotted. Interleaving would spread one player's cats across the whole list
// for no gain.
//
// The result is still cross-checked on the wire: every player publishes what it
// claims in CONTROL, and the tally must show each human cat claimed by exactly
// one player. This function being right is not what makes the split safe; it is
// what makes it agree without negotiating.

#include <cstdint>

namespace mgmp {

struct SplitRange {
    uint32_t start = 0;   // index into the HUMAN cats, in roster order
    uint32_t count = 0;   // how many of them are this peer's
};

// `humans` is how many cats a human brain drives in this battle, `peers` how
// many players are in the session, `pos` this peer's position in the sorted
// membership list. A peers of 0 is treated as 1, and a pos past the end is
// clamped to claiming nothing -- both are "we have not been told the membership
// yet", where claiming nothing is the safe answer and claiming everything is
// the one that has two people driving one cat.
inline SplitRange split_for(uint32_t humans, uint32_t peers, uint32_t pos) {
    SplitRange r;
    if (peers == 0) peers = 1;
    if (pos >= peers) return r;              // start 0, count 0

    const uint32_t base  = humans / peers;
    const uint32_t extra = humans % peers;

    // Everyone before us took base, plus one each for the first `extra` of them.
    r.start = pos * base + (pos < extra ? pos : extra);
    r.count = base + (pos < extra ? 1u : 0u);
    return r;
}

// True when cat number `nth_human` (0-based, in roster order) belongs to us.
inline bool split_owns(const SplitRange& r, uint32_t nth_human) {
    return nth_human >= r.start && nth_human < r.start + r.count;
}

// Once setup/parent ownership is available, extra units must never redistribute
// known cats. Unresolved friendly units belong to host; resolved summons follow
// their parent even when that produces unequal team sizes. No notes means the
// caller should use the legacy positional/fingerprint split instead.
inline bool split_by_owner(const bool* human, const uint8_t* owner,
                           const bool* known, uint32_t count, uint32_t pos,
                           bool* local) {
    bool any = false;
    for (uint32_t i = 0; i < count; ++i) any |= human[i] && known[i];
    if (!any) return false;
    for (uint32_t i = 0; i < count; ++i)
        local[i] = human[i] && (known[i] ? owner[i] : 0u) == pos;
    return true;
}

// --- WHO a human cat IS, when position cannot say ----------------------------
//
// The split walks the roster in order, so it is only as good as the order: two
// peers holding the same cats in a different arrangement hand every human cat to
// the other peer, and each side greys the bar for what it thinks the peer owns.
// Measured 2026-09-21: `ROSTER SIZE 31 cats` against `32`, `18 of 31 cat(s)
// differ` with the same hp values on adjacent indices, then `ours` on one peer
// and `a PEER owns it` on the other for the same cat.
//
// The order cannot be trusted, but the cats can be IDENTIFIED. `fingerprint` is
// a hash of what a cat IS -- its ability slots by authored GON name, from
// mgmp_ability, which is stable within a battle and identical on both peers
// because the names are authored data. Sorting the human cats by it gives both
// peers the same sequence without either one knowing where the other put them.
//
// TIES GO TO THE INDEX, deliberately: two cats with identical fingerprints are
// interchangeable as far as this rule can tell, and a stable tiebreak is still
// better than an unstable one. A zero fingerprint means "could not read that
// cat", and the caller must fall back rather than sort on it -- see
// split_order_by_fp.
inline uint32_t split_order_by_fp(const uint32_t* fp, const bool* human,
                                  uint32_t count, uint8_t* out) {
    // Insertion sort: at most a few dozen entries, once per battle, and it keeps
    // this header free of <algorithm> the same way the rest of the file is.
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (!human[i]) continue;
        uint32_t k = n;
        while (k > 0) {
            const uint8_t prev = out[k - 1];
            const bool after = fp[prev] < fp[i] ||
                               (fp[prev] == fp[i] && prev < (uint8_t)i);
            if (after) break;
            out[k] = prev;
            --k;
        }
        out[k] = (uint8_t)i;
        ++n;
    }
    return n;
}

// How the two peers' claimed human cats relate to each other.
//
// This is the question the CONTROL cross-check could not answer before: an
// index-based tally can see that everyone claimed something, but not whether the
// two sides are describing the SAME cats. Permuted is a different problem from
// different -- one is a split that can be derived from identity, the other is a
// run state that has already diverged -- and they need different answers.
enum FpRelation {
    FP_SAME,        // identical multisets: the peers hold the same cats
    FP_PERMUTED,    // the same cats, different order
    FP_DIFFERENT,   // not the same cats at all
};

inline FpRelation fp_relation(const uint32_t* a, uint32_t na,
                              const uint32_t* b, uint32_t nb) {
    const uint32_t kCap = 32;                 // ControlMsg carries at most this many
    if (na > kCap || nb > kCap || na != nb) return FP_DIFFERENT;

    uint32_t sa[kCap], sb[kCap];
    for (uint32_t i = 0; i < na; ++i) sa[i] = a[i];
    for (uint32_t i = 0; i < nb; ++i) sb[i] = b[i];
    for (uint32_t i = 1; i < na; ++i) {       // insertion sort, same reasoning
        const uint32_t v = sa[i];
        uint32_t k = i;
        while (k > 0 && sa[k - 1] > v) { sa[k] = sa[k - 1]; --k; }
        sa[k] = v;
    }
    for (uint32_t i = 1; i < nb; ++i) {
        const uint32_t v = sb[i];
        uint32_t k = i;
        while (k > 0 && sb[k - 1] > v) { sb[k] = sb[k - 1]; --k; }
        sb[k] = v;
    }

    bool same_order = true;
    for (uint32_t i = 0; i < na; ++i) {
        if (sa[i] != sb[i]) return FP_DIFFERENT;
        if (a[i] != b[i]) same_order = false;
    }
    return same_order ? FP_SAME : FP_PERMUTED;
}

// --- the numbering, and the one operation that may extend it -----------------
//
// The battle roster is snapshotted once, at the first turn boundary, and every
// index the wire carries refers to that snapshot (see mgmp_lockstep.h for why a
// live lookup is not an option: the turn order is reshuffled every round, so the
// index IS the identity).
//
// That pin survives exactly one kind of change. A DEATH removes an entry and
// shifts every later one, but membership is tracked by POINTER
// (snapshot_membership), so a departure moves nothing that matters. An ARRIVAL
// -- a summon -- can simply take the next free index and disturb nobody, which
// is what this function has to get exactly right: admitting one twice, or out of
// order, renumbers live cats, and two peers that renumber differently disagree
// about who owns what for the rest of the fight.
//
// So: live entries that are not in `known`, in LIVE order, nothing admitted
// twice even if the live list itself repeats a pointer, and never past `cap`.
// `out` receives LIVE indices; the caller decides which roster index each takes.
inline uint32_t adopt_new(const void* const* known, uint32_t known_n,
                          const void* const* live,  uint32_t live_n,
                          uint8_t* out, uint32_t cap) {
    uint32_t n = 0;
    for (uint32_t j = 0; j < live_n; ++j) {
        const void* c = live[j];
        if (!c) continue;

        bool seen = false;
        for (uint32_t k = 0; k < known_n && !seen; ++k)
            if (known[k] && known[k] == c) seen = true;
        for (uint32_t k = 0; k < n && !seen; ++k)          // a repeated pointer in
            if (live[out[k]] == c) seen = true;            // the live list itself

        if (seen) continue;
        if (n >= cap) break;
        out[n++] = (uint8_t)j;
    }
    return n;
}

} // namespace mgmp
