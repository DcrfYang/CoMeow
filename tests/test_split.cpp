// Pins mgmp_split.h -- who controls which cats when there are more than two
// players. In-game this needs three or four live instances AND a battle with the
// right number of human cats, so the arithmetic is pinned here and the live run
// is left to prove only what needs a game.
//
// The property that actually matters is not any single row: it is that across
// all players the ranges TILE the human cats exactly -- no cat claimed twice, no
// cat claimed by nobody. Those are the two failures CONTROL exists to catch, and
// they have opposite symptoms (a desync on the first turn versus a battle that
// stalls forever). The exhaustive sweep at the bottom is the real test; the
// hand-written rows above it are there so a wrong change says WHICH case broke.

#include "mgmp_split.h"

#include <cstdio>
#include <cstring>

using namespace mgmp;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) { printf("FAIL: %s\n", what); ++failures; }
}

static void expect(uint32_t humans, uint32_t peers, uint32_t pos,
                   uint32_t start, uint32_t count) {
    SplitRange r = split_for(humans, peers, pos);
    if (r.start != start || r.count != count) {
        printf("FAIL: split_for(humans=%u, peers=%u, pos=%u) = {%u,%u}, expected {%u,%u}\n",
               humans, peers, pos, r.start, r.count, start, count);
        ++failures;
    }
}

int main() {
    // --- two players: must reproduce the old (humans+1)/2 rule exactly -------
    //
    // This is the compatibility row. The two-player split shipped and was
    // measured working over 37 turns across two battles; if a generalisation
    // changes it, the generalisation is wrong.
    expect(4, 2, 0, 0, 2);   expect(4, 2, 1, 2, 2);
    expect(3, 2, 0, 0, 2);   expect(3, 2, 1, 2, 1);   // host takes the odd one
    expect(2, 2, 0, 0, 1);   expect(2, 2, 1, 1, 1);
    expect(1, 2, 0, 0, 1);   expect(1, 2, 1, 1, 0);
    for (uint32_t h = 0; h <= 16; ++h)
        check(split_for(h, 2, 0).count == (h + 1) / 2,
              "two-player host share still equals (humans+1)/2");

    // --- three players ------------------------------------------------------
    expect(4, 3, 0, 0, 2);   expect(4, 3, 1, 2, 1);   expect(4, 3, 2, 3, 1);
    expect(3, 3, 0, 0, 1);   expect(3, 3, 1, 1, 1);   expect(3, 3, 2, 2, 1);
    expect(5, 3, 0, 0, 2);   expect(5, 3, 1, 2, 2);   expect(5, 3, 2, 4, 1);

    // --- four players, the stated goal: one cat each -------------------------
    for (uint32_t p = 0; p < 4; ++p) expect(4, 4, p, p, 1);

    // Fewer cats than players: the late positions get nothing rather than
    // sharing. A player with no cat watches; two players sharing one cat is the
    // failure this avoids.
    expect(2, 4, 0, 0, 1);   expect(2, 4, 1, 1, 1);
    expect(2, 4, 2, 2, 0);   expect(2, 4, 3, 2, 0);

    // --- degenerate inputs --------------------------------------------------
    expect(0, 4, 0, 0, 0);                  // no human cats: nobody claims one
    expect(4, 0, 0, 0, 4);                  // peers=0 is treated as 1, we take all
    expect(4, 2, 7, 0, 0);                  // pos past the end claims NOTHING
    check(split_for(4, 2, 7).count == 0,
          "an out-of-range position claims nothing rather than everything");

    // --- split_owns agrees with the range -----------------------------------
    {
        SplitRange r = split_for(4, 3, 0);   // {0,2}
        check( split_owns(r, 0), "owns first");
        check( split_owns(r, 1), "owns second");
        check(!split_owns(r, 2), "does not own third");
        SplitRange empty = split_for(2, 4, 3);
        check(!split_owns(empty, 0), "an empty range owns nothing");
    }

    // --- THE PROPERTY: the ranges tile the humans exactly --------------------
    //
    // For every plausible battle, every human cat is claimed by exactly one
    // player. This is the invariant CONTROL verifies on the wire; if it fails
    // here it would fail there as a halt or a permanent stall.
    for (uint32_t peers = 1; peers <= 8; ++peers) {
        for (uint32_t humans = 0; humans <= 24; ++humans) {
            uint32_t claims[32] = {};
            uint32_t total = 0;
            for (uint32_t pos = 0; pos < peers; ++pos) {
                SplitRange r = split_for(humans, peers, pos);
                total += r.count;
                for (uint32_t h = 0; h < humans; ++h)
                    if (split_owns(r, h)) ++claims[h];
            }
            if (total != humans) {
                printf("FAIL: humans=%u peers=%u -- shares total %u\n",
                       humans, peers, total);
                ++failures;
            }
            for (uint32_t h = 0; h < humans; ++h) {
                if (claims[h] == 1) continue;
                printf("FAIL: humans=%u peers=%u -- cat %u claimed %u time(s)\n",
                       humans, peers, h, claims[h]);
                ++failures;
                break;
            }
            // Fairness: nobody carries two more cats than anybody else.
            uint32_t lo = 0xFFFFFFFFu, hi = 0;
            for (uint32_t pos = 0; pos < peers; ++pos) {
                uint32_t c = split_for(humans, peers, pos).count;
                if (c < lo) lo = c;
                if (c > hi) hi = c;
            }
            if (hi - lo > 1) {
                printf("FAIL: humans=%u peers=%u -- shares differ by %u\n",
                       humans, peers, hi - lo);
                ++failures;
            }
            // The remainder goes to the EARLIEST positions, host first.
            for (uint32_t pos = 1; pos < peers; ++pos) {
                if (split_for(humans, peers, pos - 1).count >=
                    split_for(humans, peers, pos).count) continue;
                printf("FAIL: humans=%u peers=%u -- position %u got more than %u\n",
                       humans, peers, pos, pos - 1);
                ++failures;
                break;
            }
            // Ranges are contiguous and ascending, which is what makes the
            // roster log readable and what split_owns assumes.
            uint32_t expect_start = 0;
            for (uint32_t pos = 0; pos < peers; ++pos) {
                SplitRange r = split_for(humans, peers, pos);
                if (r.start != expect_start) {
                    printf("FAIL: humans=%u peers=%u pos=%u -- start %u, expected %u\n",
                           humans, peers, pos, r.start, expect_start);
                    ++failures;
                    break;
                }
                expect_start += r.count;
            }
        }
    }

    // --- the numbering: what a mid-battle ARRIVAL may do to it ---------------
    //
    // The roster snapshot pins an index per character, and adoption is the only
    // operation allowed to extend it. Every failure below is a renumbering: an
    // entry admitted twice, out of order, or past the end gives one cat a
    // different index on each peer -- and two peers that disagree about an index
    // disagree about who owns that cat for the rest of the fight.
    //
    // Added 2026-09-21 with the summon fix: a player-brain summon had no index at
    // all, so it fell outside the ownership model and both peers drove it.
    {
        const void* a = (const void*)0x1000;
        const void* b = (const void*)0x2000;
        const void* c = (const void*)0x3000;
        const void* d = (const void*)0x4000;

        const void* known[2] = { a, b };
        const void* live[4]  = { a, b, c, d };
        uint8_t     out[8]   = {};

        uint32_t n = adopt_new(known, 2, live, 4, out, 8);
        check(n == 2, "two arrivals are adopted");
        check(n == 2 && out[0] == 2 && out[1] == 3,
              "arrivals keep LIVE order -- adoption must never re-sort the roster");

        // A pointer the live list repeats is one cat, not two.
        const void* dup[3] = { a, c, c };
        n = adopt_new(known, 2, dup, 3, out, 8);
        check(n == 1 && out[0] == 1, "a pointer repeated in the live list adopts once");

        // Nothing new: the live list's own order is not evidence of an arrival.
        const void* same[2] = { b, a };
        n = adopt_new(known, 2, same, 2, out, 8);
        check(n == 0, "a live list of known entries adopts nothing");

        // Nulls are holes in the live list, not entries.
        const void* holes[4] = { nullptr, c, nullptr, a };
        n = adopt_new(known, 2, holes, 4, out, 8);
        check(n == 1 && out[0] == 1, "a null live entry is not an arrival");

        // Capacity: as many as fit, earliest arrivals first.
        n = adopt_new(known, 2, live, 4, out, 1);
        check(n == 1 && out[0] == 2, "a full roster adopts as many as fit, in order");

        // A death moves an index; an arrival must not inherit that shift. `a` is
        // gone from the live list and `c` sits where it was -- by index c would
        // look like a, and by pointer it is plainly new.
        const void* shrunk[2] = { b, c };
        n = adopt_new(known, 2, shrunk, 2, out, 8);
        check(n == 1 && out[0] == 1,
              "a death and an arrival in the same list still adopts only the arrival");
    }

    // --- identity, when the roster order cannot be trusted -------------------
    //
    // Two peers that arranged the same cats differently must still hand out the
    // same cats. The sequence is the FINGERPRINTS', not the roster's; ties fall
    // back to the index, which is at least stable even when it cannot be
    // meaningful.
    {
        bool     human[16] = {};
        uint32_t fp[16]    = {};
        human[2] = human[5] = human[7] = human[9] = true;
        fp[2] = 0x40;  fp[5] = 0x10;  fp[7] = 0x30;  fp[9] = 0x20;

        uint8_t  order[16] = {};
        const uint32_t n = split_order_by_fp(fp, human, 16, order);
        check(n == 4, "four human cats, four entries");
        check(n == 4 && order[0] == 5 && order[1] == 9 && order[2] == 7 && order[3] == 2,
              "ordered by fingerprint ascending, not by index");

        // The SAME four cats, placed at different indices -- the permutation that
        // broke the split in play. The fingerprint sequence has to come out
        // identical, because that is what both peers then split over.
        bool     human2[16] = {};
        uint32_t fp2[16]    = {};
        human2[0] = human2[1] = human2[2] = human2[3] = true;
        fp2[0] = 0x30;  fp2[1] = 0x40;  fp2[2] = 0x20;  fp2[3] = 0x10;

        uint8_t  order2[16] = {};
        const uint32_t n2 = split_order_by_fp(fp2, human2, 16, order2);
        check(n2 == 4, "the permuted roster also yields four");
        bool same_sequence = (n == n2);
        for (uint32_t k = 0; same_sequence && k < n; ++k)
            if (fp[order[k]] != fp2[order2[k]]) same_sequence = false;
        check(same_sequence,
              "a permuted roster hands out the same CATS in the same ORDER");

        // Ties: equal fingerprints keep index order rather than scrambling.
        bool     human3[8] = {};
        uint32_t fp3[8]    = {};
        human3[1] = human3[3] = human3[5] = true;
        fp3[1] = fp3[3] = fp3[5] = 7;
        uint8_t order3[8] = {};
        check(split_order_by_fp(fp3, human3, 8, order3) == 3 &&
              order3[0] == 1 && order3[1] == 3 && order3[2] == 5,
              "equal fingerprints fall back to index order");
    }

    // --- telling a permutation from a real divergence ------------------------
    //
    // This is what CONTROL could not answer before: a permutation is a split the
    // identity can reconcile, a different set is run state that has already gone.
    {
        const uint32_t a[2] = { 0x11, 0x22 };
        const uint32_t b[2] = { 0x11, 0x22 };
        const uint32_t c[2] = { 0x22, 0x11 };
        const uint32_t d[2] = { 0x11, 0x33 };
        check(fp_relation(a, 2, b, 2) == FP_SAME,      "identical lists read as the same");
        check(fp_relation(a, 2, c, 2) == FP_PERMUTED,  "the same values reordered read as PERMUTED");
        check(fp_relation(a, 2, d, 2) == FP_DIFFERENT, "a changed value reads as DIFFERENT");
        check(fp_relation(a, 2, a, 1) == FP_DIFFERENT, "a different length reads as DIFFERENT");
    }

    // Eight setup cats + multiple friendly summons: explicit ownership must
    // survive every combination of known parent links and unresolved units.
    // Permute the roster as well; do not assume host cats are the first four.
    for (unsigned shift = 0; shift < 12; ++shift) {
        for (unsigned mask = 0; mask < 16; ++mask) {
            bool human[12]{}, known[12]{}, local[2][12]{};
            uint8_t owner[12]{};
            for (unsigned cat = 0; cat < 11; ++cat) {
                const unsigned i = (cat + shift) % 12;
                human[i] = true;
                known[i] = cat < 8 || ((mask >> (cat - 8)) & 1);
                owner[i] = cat < 4 ? 0 : 1; // Summons all follow client when resolved.
            }
            for (unsigned pos = 0; pos < 2; ++pos)
                check(split_by_owner(human, owner, known, 12, pos, local[pos]),
                      "partial ownership uses notes, never equal ranges");
            for (unsigned cat = 0; cat < 12; ++cat) {
                const unsigned i = (cat + shift) % 12;
                const unsigned expected = cat < 4 || !known[i] ? 0 : 1;
                check(unsigned(local[0][i]) + unsigned(local[1][i]) == unsigned(human[i]),
                      "every friendly unit has exactly one controller; AI has none");
                if (human[i]) check(local[expected][i], "known cats and summons retain their owner");
            }
        }
    }
    {
        bool human[2] = {true, true}, known[2]{}, local[2] = {true, false};
        uint8_t owner[2]{};
        check(!split_by_owner(human, owner, known, 2, 0, local), "no ownership keeps legacy fallback");
        check(local[0] && !local[1], "fallback does not overwrite the caller's result");
    }

    if (failures) { printf("test_split: FAILED -- %d failure(s)\n", failures); return 1; }
    printf("test_split: PASSED -- 0 failure(s)\n");
    return 0;
}
