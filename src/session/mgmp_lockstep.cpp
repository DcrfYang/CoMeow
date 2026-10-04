// mgmp_lockstep.cpp -- see mgmp_lockstep.h for the design.

#include "mgmp_lockstep.h"
#include "mgmp_spawntest.h"   // unit_definition_name / unit_transform_to: the board sync repairs a unit of another kind
#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_barrier.h"
#include "mgmp_battleid.h"
#include "mgmp_hashring.h"
#include "mgmp_net.h"
#include "mgmp_catsync.h"
#include "mgmp_page.h"
#include "mgmp_abandon.h"
#include "mgmp_room.h"
#include "mgmp_catview.h"
#include "mgmp_cursor.h"
#include "mgmp_roster.h"   // roster_on_party -- the party exchange lands here
#include "mgmp_overlay.h"
#include "mgmp_invsync.h"
#include "mgmp_aim.h"
#include "mgmp_nodehash.h"
#include "mgmp_runhist.h"
#include "mgmp_follow.h"
#include "mgmp_unlocks.h"
#include "mgmp_diag.h"
#include "mgmp_chat.h"
#include "mgmp_choice.h"
#include "mgmp_savefile.h"
#include "mgmp_checkpoint.h"
#include "mgmp_setup.h"
#include "mgmp_leave.h"
#include "mgmp_session.h"
#include "mgmp_ability.h"
#include "mgmp_turnaction.h"
#include "mgmp_config.h"
#include "mgmp_tuning.h"
#include "mgmp_mem.h"
#include "mgmp_split.h"
#include "mgmp_derived.h"
#include "mgmp_rtti.h"
#include "mgmp_rng.h"
#include "mgmp_listprobe.h"    // handed the TurnControl* the RE probe needs
#include "mgmp_log.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>

namespace mgmp {
namespace {

// This counts ENTRIES IN THE BATTLE'S CHARACTER LIST, which is mostly scenery.
// Measured over two real battles: 45 NoBrain against 8 PlayerBrain, 6
// GenericBrain and 2 PatternBrain. A boss arena logged 45 entries rising to 46
// mid-fight, and on screen that was four cats and one boss -- the other forty
// were props. So this number tracks ARENA SIZE, not how many things can act,
// and sizing it against "a boss plus its summons" is the wrong model: a fully
// dressed 10x10 grid is already 100 tiles before a single combatant.
//
// It is therefore set to the hard protocol limit rather than to a guess with
// margin. Roster indices travel as uint8_t in ACTION and CONTROL and kNoCat is
// 0xFF, so 254 is the largest index that can exist on the wire; anything under
// that is an arbitrary line waiting to be crossed by a bigger room. The cost is
// about 2 KB of static arrays.
//
// 32 was the old value. Over it the snapshot bailed, which meant no roster, no
// control split, no actions on the wire and no hashes -- both peers quietly
// played their own boss fight and the session summary still said "0 desync(s)".
//
// ControlMsg::cats[32] is unrelated: it carries HUMAN cats only, of which there
// are four.
constexpr uint32_t kMaxCats    = 254;

// One BATTLE's worth of decisions, not one turn's.
//
// 64 was enough while the only thing this queue ever held was the handful of
// decisions in flight around a turn boundary. It is not enough now that a peer
// joining mid-fight is sent the whole battle's history at once: a 12-turn fight
// with four human cats replays far past 64, and pend_push drops the overflow --
// which would leave the joiner short of exactly the decisions it needs, having
// said so in one line that is easy to miss.
//
// Sized to match the catch-up log it has to be able to receive, so the two
// cannot disagree.
constexpr uint32_t kMaxSentLog = 512;
constexpr uint32_t kMaxPending = kMaxSentLog;
constexpr uint8_t  kNoCat      = 0xFF;

// Character list, reached from TurnControl. See the header for provenance.
constexpr uintptr_t kTC_Scene      = 0x18;
constexpr uintptr_t kScene_Sub     = 0x08;
constexpr uintptr_t kSub_Holder    = 0x20;
constexpr uintptr_t kHolder_List   = 0x1F90;
constexpr uintptr_t kList_Count    = 12;
constexpr uintptr_t kList_Data     = 16;

// Brain+0x38 is the owning Character (Hex-Rays types it as such in
// Brain::UpdateDecision); Character+0x68 is the reverse edge, the Brain*
// TurnControl::update reads to call UpdateDecision. TurnControl+0x60 is the
// decision queue's element count.
constexpr uintptr_t kBrain_Owner   = 0x38;
constexpr uintptr_t kChar_Brain    = 0x68;
constexpr uintptr_t kTC_QueueCount = 0x60;

// --- Character fields, for the state hash (recovered 2026-08-24) ------------
//
// Character+0x60 points at a TacticsObject and TacticsObject+0x98 points back
// at the Character. That pair is not a guess to be trusted: the snapshot walks
// it in BOTH directions per cat and refuses to hash a cat whose pointer does
// not round trip. Provenance, one independent reading each:
//
//   +0x60  TacticsObject*  AbilityHealthThreshold::late_update reads
//                          *(char+96)+96 -- TacticsObject's "removed" flag, the
//                          guard at the top of both TacticsObject::Move and
//                          ::ReceiveDamage. Character::get_standing_tile_elements
//                          reads *(char+96)+99, another TacticsObject bool.
//   +0x388 iVec2D facing   Character::Face's only durable write (this+113).
//   +0x4B0 int32  HP       Character::Die zeroes +0x4B0 as a QWORD -- i.e. HP
//                          and shield in one store; ReceiveDamage__inner_0
//                          touches it 21 times; EndTurn and RunAway read it as
//                          a dword.
//   +0x4B4 int32  shield   Character::GainShield writes this+301, and it is the
//                          same field AbilityHealthThreshold adds to HP when
//                          the ability's GON says `count_shield`. Two unrelated
//                          functions, one offset.
//   +0x4BC int32  max HP   the deval base passed for `threshold` /
//                          `threshold_min` / `threshold_max`, so a GON "50%"
//                          means 50% of this; recompute_stats reads it 4x.
//   +0x4C2 bool   dead     Character::Die's entry guard, set by Die and
//                          OnCorpsePop, read by BeginTurn, EndTurn, DoAction,
//                          RoundUpkeep, GainShield and update.
//
// TacticsObject+0x48 is the current tile: ::Move copies the old value to +0x50
// and writes the destination here, and ::ReceiveDamage passes it as the tile
// argument to the splash walk.
constexpr uintptr_t kChar_TObj     = 0x60;
constexpr uintptr_t kChar_Ident    = 0x958;   // the random tie-break drawn when the character is created (kChar_InitB below): one value per unit, fixed for its life -- what tells two objects at the SAME ADDRESS apart
constexpr uintptr_t kChar_Facing   = 0x388;
constexpr uintptr_t kChar_HP       = 0x4B0;
constexpr uintptr_t kChar_Shield   = 0x4B4;
constexpr uintptr_t kChar_MaxHP    = 0x4BC;
constexpr uintptr_t kChar_Dead     = 0x4C2;
constexpr uintptr_t kTObj_Tile     = 0x48;
constexpr uintptr_t kTObj_Owner    = 0x98;

// glaiel::Character::get_affecting_elements(Character*, ElementList* out).
// Returns `out`. See the kCalls entry for why this is worth calling: it is the
// value Character::receive_damage_passives branches on, and its tile half is
// the only damage input that lives on the GRID rather than on the character --
// so it is the one thing a per-character state hash structurally cannot see.
typedef void* (__fastcall *fn_affecting_elements)(const void* chr, uint32_t* out);

// --- per-cat simulation state ----------------------------------------------
//
// Packed deliberately: this is hashed byte-for-byte, so it must contain no
// padding and no pointer. A heap address in here would differ between two
// processes by construction and desync every single turn -- the exact way a
// state hash goes from "detects divergence" to "manufactures it".
#pragma pack(push, 1)
struct CatState {
    int32_t hp     = 0, shield = 0, maxhp = 0;
    int32_t tx     = 0, ty     = 0;
    int32_t fx     = 0, fy     = 0;
    uint32_t e0    = 0, e1     = 0;   // ElementList, or 0/0 if we could not read it
    uint8_t dead   = 0;
    uint8_t linked = 0;   // the TacticsObject round trip held for this cat
    uint8_t elems  = 0;   // the ElementList above is real, not a fallback zero
    uint8_t in_battle = 1; // still in the live character list; see read_cat_state
    // Whether the fields above were read at all. Only meaningful in the copy
    // build_hash keeps for the desync dump: a row that could not be read is a
    // fact worth sending, and a peer that reads a cat the other one cannot is
    // itself a difference the diff should name rather than skip over.
    uint8_t readable = 0;
};
#pragma pack(pop)

struct State {
    bool active = false;

    // Null unless the prologue verified against the pinned build. Every use is
    // null-checked; a build drift turns the element hash off rather than
    // calling into whatever moved there.
    fn_affecting_elements affecting_elements = nullptr;

    const void* cats[kMaxCats] = {};
    uint32_t    cat_key[kMaxCats] = {};      // kChar_Ident of each roster entry when it was taken (a pointer alone is not an identity: see entry_is)
    bool        cat_key_ok[kMaxCats] = {};
    uint32_t    cat_count      = 0;
    // Units a transform REPLACED on this peer only (the host-authoritative repair, the dev transform): the removed object stays in the live list until the scene flushes it, and
    // must not be adopted as a summon (that made this roster one entry longer than the host's). Identified by address AND creation key, like a roster entry.
    // A unit the board sync MADE this boundary is not in the live list yet: the game adds a spawned character to the list one boundary later (measured 2026-10-03: spawned at turn 3, adopted at
    // turn 4; the client's hash of the boundary it was made in had cat 24 in_battle 0 where the host's had 1, a halt). Such an entry counts as present until the list shows it (or kFreshTurns pass).
    static constexpr uint8_t kFreshTurns = 3;
    uint8_t     fresh_left[kMaxCats] = {};
    static constexpr uint32_t kMaxRetired = 16;
    const void* retired[kMaxRetired] = {};
    uint32_t    retired_key[kMaxRetired] = {};
    uint32_t    retired_n      = 0;
    bool        local_cat[kMaxCats] = {};   // this peer's input decides for it
    bool        human_cat[kMaxCats] = {};   // a human brain drives it at all

    // WHO EDITS WHICH CAT, KEPT PAST THE BATTLE (2026-09-22).
    //
    // The split above is a property of a BATTLE; the rule it expresses -- one cat,
    // one editor -- is needed on the MAP, by mgmp_catsync, which publishes and
    // applies between nodes. lockstep_cat_owner cannot answer there (it requires
    // g.active), so the answer is copied out at the one moment it is certainly
    // known: right after the split is derived below, from the same per-cat ids the
    // level-up probe reads.
    //
    // Only the human roster is in here. A cat that is not (an AI cat, a familiar)
    // has no entry, and both callers read a missing entry as "unknown" and keep
    // their previous behaviour -- the host-authoritative one-way push this module
    // started with. That is the correct answer for a familiar anyway, and it is why
    // this cache does not need a completeness guarantee.
    struct OwnedCat {
        uint64_t save_id = 0;
        bool     mine    = false;
        uint8_t  owner   = kNoPeer; // absolute session position, retained after battle
    };
    OwnedCat owned_cats[kMaxCats] = {};
    uint32_t owned_count = 0;
    bool     owned_valid = false;

    // The battle whose enemies have already been weakened by the test aid -- keyed by
    // battle id, so no reset hook is needed and each battle is weakened exactly once.
    // See weaken_enemies_once.
    uint64_t weakened_battle = 0;

    // Characters may be freed before the upgrade screen; never dereference them there.
    struct LevelCatProbe {
        const void* data = nullptr;
        uint64_t seed = 0;
        uint64_t save_id = 0;
        uint8_t owner = kNoPeer;
        bool readable = false;
    };
    LevelCatProbe level_cats[kMaxCats] = {};
    uint64_t level_probe_node = 0;

    // APPEARED MID-BATTLE. Adopted from the live list at a turn boundary, with
    // the index nobody had, so the snapshot numbering is extended rather than
    // recomputed. See adopt_new_cats: a summon is the one roster change that can
    // be absorbed without renumbering anything.
    bool        summon[kMaxCats] = {};

    // `humans` is the SNAPSHOT's human count and stays that -- it is what CONTROL
    // carries, and verify_control HALTS when the peer's number differs. An
    // adopted human summon therefore must not touch it; it counts here instead,
    // for the log only.
    uint32_t    summon_humans = 0;

    // WHAT EACH HUMAN CAT IS, by ability fingerprint (mgmp_ability), and whether
    // the whole set could be read at all. The split is ordered by this instead of
    // by roster position, so that two peers who arranged the same cats
    // differently still hand out the same cats. A single unreadable one makes the
    // whole ordering unusable -- see where fp_usable is set.
    uint32_t    cat_fp[kMaxCats] = {};
    bool        fp_usable = false;

    // The actor of the last APPLIED action, which is what a summon's ownership is
    // inherited from. Both peers apply the same actions in the same order, so
    // this is the same pointer ON EACH PEER'S OWN HEAP -- i.e. the same cat,
    // which is the only thing that makes an inherited owner agree without a
    // message.
    const void* actor = nullptr;
    bool        snapped        = false;
    const void* snapped_list   = nullptr;   // the vector object we snapshotted
    uint64_t snapshot_battle   = kNoBattle; // identity when that roster was built

    // THE TURN CONTROL, AS SOON AS ANYTHING HANDS IT OVER (2026-09-22).
    //
    // Handed over from the NextTurn hook, ABOVE the session gate, exactly as
    // mgmp_listprobe and mgmp_roster are -- see lockstep_set_turn_control. It is what lets
    // a test aid reach the live character list in a SINGLE-PLAYER run, where no battle is
    // ever snapshotted and g.snapped_list therefore describes a fight that is over.
    void*       tc              = nullptr;

    // --- what the last hash was actually taken over ----------------------
    //
    // Kept so that a desync dump reports the state that WAS hashed rather than
    // the state that happens to be live when the peer's hash lands. Those are
    // not the same instant: a mismatch is noticed either at our own boundary or
    // on arrival, and only the first of those is the moment the numbers were
    // read. Comparing a stale row against a fresh one invents differences.
    CatState hashed[kMaxCats];
    uint32_t hashed_count = 0;
    uint32_t hashed_turn  = 0;
    bool     hashed_valid = false;
    // One dump per divergence in each direction. After the first there is
    // nothing new to learn and the peer is halted anyway.
    bool     dump_sent = false;
    bool     dump_seen = false;

    ActionMsg pending[kMaxPending];
    uint32_t  pend_head = 0, pend_count = 0;

    bool     outstanding = false;   // a local decision is sent but not applied
    uint32_t turn        = 0;

    // Which battle. Turn numbering restarts at 0 in each one, so the turn index
    // alone identifies nothing and a message in flight across a battle boundary
    // would be taken for one about the battle we just entered. See the note
    // above ActionMsg in mgmp_proto.h.
    // The WIRE identity: the node seed both peers read out of MapNode+0x118.
    // Set by lockstep_enter_battle from the map layer, which is the only thing
    // that knows a node was entered. See mgmp_battleid.h.
    BattleTracker<> battles;

    // Every decision taken in the current battle by ANY peer -- ours as we send
    // them, the peer's as they arrive -- for replaying to a peer that joins
    // mid-fight. Cleared at every battle boundary, so it is bounded by one
    // battle rather than by the run.
    ActionMsg sent_log[kMaxSentLog];
    uint32_t  sent_log_n    = 0;
    bool      sent_log_full = false;

    // A LOCAL counter, kept only for the log lines and for "how many battles
    // has this peer played". It is deliberately never compared with a peer's:
    // that is what broke on reconnect.
    uint32_t epoch       = 0;
    uint32_t stale_drops = 0;      // messages dropped this battle as stale
    bool     said_stale  = false;  // ...and whether we have said so yet

    // Whether the cat last polled for a decision is one of ours. Cosmetic only
    // -- it drives the cursor overlay's "whose turn is it" fade and nothing
    // else -- but it lives here because this is the only place that already
    // resolves a Brain back to a roster index. False during an enemy or AI
    // turn, which is correct: that turn belongs to nobody, so no cursor lights.
    bool     local_actor = false;

    // The two directions a counterpart can arrive from; see mgmp_hashring.h.
    //
    // One held ring PER PEER, because with four players the peers are not in
    // lockstep with each other's frame rates any more than with ours: peer 1 may
    // be three turns ahead while peer 2 is one behind, and a single shared ring
    // would interleave their turn numbers and match the wrong pair. `my_hash`
    // stays single -- there is only one of us.
    HashRing<HashMsg> peer_hash[kMaxPeers];
    HashRing<HashMsg> my_hash;
    bool     peer_hash_full_warned[kMaxPeers] = {};

    bool     halted = false;
    // This battle was halted (it stays true after the halt is lifted at the fight's end, for the summary), and the battle whose halt was already lifted (a late HALT message of it is stale).
    bool     halted_in_battle = false;
    uint64_t halt_cleared_for = 0;
    LockstepStats stats;

    // --- the per-turn state trace (net_state_trace) ---
    //
    // The previous boundary's reading of every snapshot cat, so that this
    // boundary can report what MOVED rather than only what IS. Reset with the
    // roster, because a new battle's cat 3 is a different cat.
    CatState prev_state[kMaxCats]{};
    bool     have_prev = false;

    // --- mismatch bookkeeping when halting is off (net_desync_halt = 0) ---
    //
    // Without a halt a divergence can be reported on every remaining turn of
    // the battle, and 45 cats of table each time buries the one line that
    // matters. Dump the table for the first few, then stay terse -- and say
    // loudly when a later turn AGREES again, because that single line is the
    // whole reason the setting exists.
    uint32_t mismatches      = 0;   // this battle
    uint32_t first_mismatch  = 0;   // ...and the turn of the first one
    bool     diverged        = false;
    // the debounce (tune::kDesyncDebounce): the turn of the last state-only mismatch that was let through, and how many were this battle
    uint32_t debounce_turn   = ~0u;
    uint32_t debounced       = 0;
    bool     desync_noticed  = false;   // the upload offer was raised for this battle
    char     halt_reason[192] = {};     // why the battle stopped (this peer's halt or the peer's), for the player-facing notice
    // --- THE RECORD OF ONE BATTLE (2026-10-04): what the BATTLE SUMMARY and the HALT RECORD report. Reset with the battle. ---
    struct BStats {
        uint32_t agreed = 0, mismatched = 0;                                    // turn hashes compared
        uint32_t board_applied = 0, board_differed = 0, board_late = 0;         // the host's boards taken / taken with a repair / not here in time
        uint32_t rewritten = 0, moved = 0, replaced = 0, spawned = 0, trimmed = 0, stuck = 0, bad = 0;   // what the repairs did, summed
        uint32_t turn0_differed = 0;                                            // units the first board repaired: the BUILD itself differed from the host's
        uint32_t audit_cats = 0, audit_differ = 0; bool audit_done = false;     // the pre-battle audit of the player cats
        bool audit_sent = false;
        ULONGLONG t0 = 0;                                                       // GetTickCount64 when the battle was first seen: the ELAPSED time in the records
    } bs;
    // The last turns, newest last: what was hashed and how it turned out. Printed whenever a hash disagrees, so even a log with no peer beside it shows the lead-up.
    struct Trail { uint32_t turn = 0; int32_t actor = -1; uint64_t rng = 0, state = 0; uint8_t verdict = 0; uint8_t board = 0; uint32_t ms = 0; };   // verdict 0 pending 1 agreed 2 MISMATCH
    Trail trail[16]{};
    uint32_t trail_n = 0;
    // LAYER 2, THE EXPERIMENT (dev button 'corrupt my cat's stats'): armed, the board also writes a player's stats and max hp; the cats it touched are WATCHED for a few boundaries
    bool exp_stats = false;
    struct StatWatch { uint32_t cat = 0; int left = 0; } sw[8]{};
    AuditMsg audit_mine{}, audit_peer{};
    bool have_audit_peer = false;

    // Turn hashes that were actually COMPARED against a peer's and agreed,
    // counted for the whole session rather than per battle.
    //
    // Without it the shutdown summary said `0 desync(s)` whether the peers had
    // agreed on sixty turns or had never compared a single one -- the design notes'
    // rule 3, and the exact hole a reconnect left. It is also what decides
    // whether that line is quiet or a warning: a session that compared nothing
    // has to say so, because it is the reading a player takes for a pass.
    uint32_t agreements      = 0;

    uint32_t humans          = 0;   // cats a human brain drives, either peer's
    bool     state_hash_on   = false;

    // The armed debug hit, in points of damage; 0 = not armed. See
    // lockstep_arm_enemy_hit for why this is two steps instead of one.
    int32_t  armed_hit       = 0;
    // Debug hits received from the peer and not yet landed (see debug_hits_pump): FIFO, oldest first.
    static constexpr uint32_t kDebugQueue = 64;
    DebugHitMsg dbg_q[kDebugQueue] = {};
    ULONGLONG   dbg_t[kDebugQueue] = {};
    uint8_t     dbg_from[kDebugQueue] = {};
    bool        dbg_waiting = false;
    uint32_t    dbg_n = 0;
    bool     control_checked = false;
    // One slot per peer id, which is why ids are allocated lowest-free and stay
    // under kMaxPeers. Our own slot is filled in locally when we publish, so the
    // coverage check below can treat every player identically instead of
    // special-casing "us" -- with four players, "the peer" is not a thing.
    ControlMsg peer_control[kMaxPeers]{};
    bool       have_peer_control[kMaxPeers] = {};

    // --- the host's board (proto 61) ---
    uint64_t board_battle   = 0;                // the battle whose board was published / taken over
    uint32_t board_turn     = ~0u;              // ... and the turn boundary of the last one
    BoardMsg board_sent[(kMaxCats + kBoardChunk - 1) / kBoardChunk];   // host: what it sent, for a peer that joins this battle late
    uint32_t board_sent_n   = 0;
    uint64_t board_sent_for = 0;

    // --- the join barrier ---
    uint32_t barrier_waits     = 0;      // GetChoice polls parked in this battle
    bool     barrier_said_open = false;  // ...and whether we have said it lifted

    CRITICAL_SECTION cs;
    bool cs_ready = false;
};

State g;
DerivedHistory g_derived_history;

DerivedFeatures derived_features(uint32_t index) {
    DerivedFeatures f{};
    const auto* c = (const uint8_t*)g.cats[index];
    // RVA 0x11C936 reads the parent's UTF-16 display name at +0x290;
    // 0x11CBEC localizes the authored key at +0x248, 0x11CCC4 stores the name.
    mem_read_std_wstring_utf8(c + 0x290, f.name, sizeof(f.name));
    mem_read_std_string(c + 0x248, f.type, sizeof(f.type));
    f.abilities = g.cat_fp[index];
    mem_read(c + kChar_HP, &f.hp, 4);
    mem_read(c + kChar_MaxHP, &f.maxhp, 4);
    mem_read(c + kChar_Shield, &f.shield, 4);
    return f;
}

// A summon created during battle normally has no CatData of its own.  Some
// class/passive spawns still retain the Character (or CatData) that caused the
// spawn in one of their component objects.  Keep this probe deliberately
// narrow: compare raw pointers only, never interpret an unverified field, and
// require one unique human parent.  Heap addresses differ between peers, but
// the same parent slot is present on both peers, so the resulting owner is
// deterministic without adding a wire message.
struct DerivedParentHint {
    bool     found = false;
    uint32_t parent = 0;
    uint8_t  owner = kNoPeer;
    uint64_t parent_id = 0;
    uintptr_t root_offset = 0;
    uintptr_t field_offset = 0;
    bool     catdata = false;
    unsigned method = 0; // 1 validated parent, 2 unique possessive name, 3 scan, 4 history
    int score = 0;
};

DerivedParentHint find_derived_parent(const void* child, uint32_t child_index, bool history_only = false) {
    DerivedParentHint out{};
    if (!child || child_index >= g.cat_count || !g.human_cat[child_index]) return out;

    struct Candidate {
        const void* chr = nullptr;
        const void* data = nullptr;
        uint64_t id = 0;
        uint8_t owner = kNoPeer;
        uint32_t index = 0;
    } candidates[kMaxCats]{};
    uint32_t candidate_count = 0;
    for (uint32_t i = 0; i < g.cat_count; ++i) {
        if (i == child_index || !g.human_cat[i] || g.summon[i]) continue;
        const void* data = nullptr;
        uint64_t id = 0;
        uint8_t owner = kNoPeer;
        if (!mem_read((const uint8_t*)g.cats[i] + kChar_CatData, &data, sizeof(data)) || !data ||
            !mem_read((const uint8_t*)data + kCatData_SaveId, &id, sizeof(id)) || !id ||
            !lockstep_owner_pos(id, owner)) continue;
        candidates[candidate_count++] = {g.cats[i], data, id, owner, i};
    }
    if (!candidate_count) return out;
    auto choose = [&](uint32_t c, unsigned method, int score) {
        out.found = true;
        out.parent = candidates[c].index;
        out.owner = candidates[c].owner;
        out.parent_id = candidates[c].id;
        out.method = method;
        out.score = score;
    };
    const auto features = derived_features(child_index);
    if (history_only) {
        uint64_t ids[kMaxCats]{};
        for (uint32_t c = 0; c < candidate_count; ++c) ids[c] = candidates[c].id;
        const auto match = g_derived_history.match(features, ids, candidate_count);
        if (match.parent) for (uint32_t c = 0; c < candidate_count; ++c)
            if (ids[c] == match.parent) { choose(c, 4, match.score); break; }
        return out;
    }
    // Native weak Character reference. Assignment at 0x11C0CB/0x11C0E0;
    // validation at 0x11C8F2..0x11C906 and tooltip 0x81DCF2..0x81DD02.
    const void* parent = nullptr;
    uint64_t expected = 0, actual = 0;
    if (mem_read((const uint8_t*)child + 0x468, &parent, sizeof(parent)) && parent &&
        mem_read((const uint8_t*)child + 0x470, &expected, 8)) {
        for (uint32_t c = 0; c < candidate_count; ++c) {
            if (parent == candidates[c].chr &&
                mem_read((const uint8_t*)parent - 8, &actual, 8) && actual == expected) {
                choose(c, 1, 100);
                out.field_offset = 0x468;
                return out;
            }
        }
    }
    uint32_t name_winner = kMaxCats;
    for (uint32_t c = 0; c < candidate_count; ++c) {
        char name[384]{};
        mem_read_std_wstring_utf8((const uint8_t*)candidates[c].chr + 0x290, name, sizeof(name));
        if (!name[0]) mem_read_std_wstring_utf8((const uint8_t*)candidates[c].data + 0x18,
                                              name, sizeof(name));
        if (!derived_named_for(features.name, name)) continue;
        if (name_winner != kMaxCats) { out.method = 2; return out; } // Duplicate names: don't guess.
        name_winner = c;
    }
    if (name_winner != kMaxCats) { choose(name_winner, 2, 100); return out; }


    const void* brain = nullptr;
    const void* tobj = nullptr;
    mem_read((const uint8_t*)child + kChar_Brain, &brain, sizeof(brain));
    mem_read((const uint8_t*)child + kChar_TObj,  &tobj,  sizeof(tobj));
    struct Root { const void* p; uintptr_t span; uintptr_t tag; };
    const Root roots[] = {
        { child, 0x2000, 0 },       // Character and its inline components
        { brain, 0x0800, 0xB000 },  // PlayerBrain-owned state
        { tobj,  0x0400, 0xC000 },  // TacticsObject-owned state
    };

    uint32_t hits[kMaxCats] = {};
    uintptr_t hit_root[kMaxCats] = {};
    uintptr_t hit_field[kMaxCats] = {};
    bool hit_catdata[kMaxCats] = {};
    for (const Root& root : roots) {
        if (!root.p) continue;
        for (uintptr_t off = 0; off + sizeof(uintptr_t) <= root.span; off += sizeof(uintptr_t)) {
            if (root.tag == 0 && off == 0x468) continue; // Native reference already validated.
            uintptr_t value = 0;
            if (!mem_read((const uint8_t*)root.p + off, &value, sizeof(value)) || !value) continue;
            for (uint32_t c = 0; c < candidate_count; ++c) {
                if (value != (uintptr_t)candidates[c].chr && value != (uintptr_t)candidates[c].data)
                    continue;
                ++hits[c];
                hit_root[c] = root.tag;
                hit_field[c] = off;
                hit_catdata[c] = value == (uintptr_t)candidates[c].data;
            }
        }
    }

    uint32_t winner = kMaxCats;
    for (uint32_t c = 0; c < candidate_count; ++c) {
        if (!hits[c]) continue;
        if (winner != kMaxCats && candidates[winner].index != candidates[c].index)
            return out; // Ambiguous: do not guess from a component pointer.
        winner = c;
    }
    if (winner == kMaxCats) return out;
    out.method       = 3;
    out.found        = true;
    out.parent       = candidates[winner].index;
    out.owner        = candidates[winner].owner;
    out.parent_id    = candidates[winner].id;
    out.root_offset  = hit_root[winner];
    out.field_offset = hit_field[winner];
    out.catdata      = hit_catdata[winner];
    return out;
}

struct Guard {
    Guard()  { if (g.cs_ready) EnterCriticalSection(&g.cs); }
    ~Guard() { if (g.cs_ready) LeaveCriticalSection(&g.cs); }
};

// Defined below, next to the desync compare; declared here because the control
// check runs earlier in the file and halting is the right answer to a split
// that does not add up.
void halt(const char* why);
void trail_dump(const char* why);
void log_battle_stats(const char* why, bool halted);

// Defined below with the rest of the split handling; the snapshot calls it as
// its last act, because the peer's CONTROL may already be sitting in the queue
// by the time this side finally has a roster to check it against.
void verify_control();

// --- FNV-1a, the same hash the protocol note specifies ----------------------
uint64_t fnv1a(const void* p, size_t n, uint64_t h = 1469598103934665603ULL) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}


// A cat the game has taken OFF THE BOARD sits at this exact tile. It is a
// game constant, not a stray value -- see the departed-cats section in
// the design notes, where it is also the address that makes get_affecting_elements
// walk the grid out of bounds.
const int kTileOffBoard = -5000;

bool off_board(const CatState& c) {
    return c.linked && c.tx == kTileOffBoard && c.ty == kTileOffBoard;
}

// Wildly-out-of-range values mean the offsets are wrong on this build, not that
// a cat is unusual. Bounds are loose on purpose: they are here to catch reading
// a pointer or a float as an int, not to police game balance.
//
// THE OFF-BOARD SENTINEL IS A PASS, and leaving it out cost a measurement.
// On 2026-09-12 a battle opened with one dead cat already parked at
// (-5000,-5000) while still in the live character list -- so the departed-cat
// path did not cover it -- and the tile bound below rejected it. One cat, and
// the state hash was off for the whole battle while the log blamed "those
// Character offsets do not fit this build". The sentinel is the opposite of
// evidence about offsets: reading it proves the tile was read correctly.
bool plausible(const CatState& c) {
    if (c.maxhp <= 0 || c.maxhp > 100000) return false;
    if (c.hp < -100000 || c.hp > 100000)  return false;
    if (c.shield < -100000 || c.shield > 100000) return false;
    if (off_board(c)) return true;
    if (c.tx < -4096 || c.tx > 4096 || c.ty < -4096 || c.ty > 4096) return false;
    return true;
}

// Kept separate from read_cat_state purely so the __try/__except lives in a
// function with nothing but PODs in it -- MSVC refuses SEH in a frame that
// needs C++ unwinding, and this is the only place in the mod that calls into
// the game from the hash path.
bool read_affecting_elements(const void* chr, uint32_t out[2]) {
    out[0] = out[1] = 0;
    if (!g.affecting_elements || !chr) return false;
    __try {
        g.affecting_elements(chr, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = out[1] = 0;
        return false;
    }
}

// `in_battle` is whether this character is still in the live character list.
//
// It gates the one thing here that is a CALL rather than a read. A character
// the battle has dropped is parked off the board at the (-5000,-5000) sentinel,
// and get_affecting_elements feeds that raw tile straight into a grid walk
// (sub_14082CB30) with no bounds check -- so the result is whatever the heap
// happens to hold. Measured on the same boss fight twice: the host read
// 0x00000400 and the client faulted, then on the rerun the two swapped. Which
// peer "wins" is a coin flip, which is exactly what a state hash must never
// contain.
bool read_cat_state(const void* chr, CatState& out, bool in_battle = true) {
    out = CatState{};
    out.in_battle = in_battle ? 1 : 0;
    if (!chr) return false;

    const uint8_t* c = (const uint8_t*)chr;
    bool ok = mem_read(c + kChar_HP,     &out.hp,     sizeof(out.hp))
           && mem_read(c + kChar_Shield, &out.shield, sizeof(out.shield))
           && mem_read(c + kChar_MaxHP,  &out.maxhp,  sizeof(out.maxhp))
           && mem_read(c + kChar_Dead,   &out.dead,   sizeof(out.dead))
           && mem_read(c + kChar_Facing,     &out.fx, sizeof(out.fx))
           && mem_read(c + kChar_Facing + 4, &out.fy, sizeof(out.fy));
    if (!ok) return false;

    // The tile lives on the TacticsObject, and we only trust it if the object
    // points back at this Character. A cat that fails this still gets hashed --
    // with linked = 0 and a zero tile -- so that a peer where the link holds and
    // a peer where it does not produce different hashes rather than silently
    // agreeing on less state than they think.
    const void* tobj = nullptr;
    if (mem_read(c + kChar_TObj, &tobj, sizeof(tobj)) && tobj) {
        const void* back = nullptr;
        if (mem_read((const uint8_t*)tobj + kTObj_Owner, &back, sizeof(back)) && back == chr) {
            if (mem_read((const uint8_t*)tobj + kTObj_Tile,     &out.tx, sizeof(out.tx)) &&
                mem_read((const uint8_t*)tobj + kTObj_Tile + 4, &out.ty, sizeof(out.ty)))
                out.linked = 1;
        }
    }

    // The elements affecting this cat. Unlike everything above, this is a CALL
    // into the game rather than a read, so it gets its own guard: the pointer is
    // null unless the prologue matched, and the call is wrapped because a
    // Character with a torn equipment list would fault inside the callee where
    // mem_read cannot help us. A cat that fails leaves elems = 0, which hashes
    // differently from a cat that succeeded and read zero -- the same
    // distinction `linked` makes, and for the same reason.
    //
    // OFF-BOARD IS THE SECOND EXCLUSION, and it is the same hazard as a
    // departed cat rather than a milder one. get_affecting_elements feeds the
    // raw tile to a grid walk with no bounds check, so at (-5000,-5000) the
    // result is whatever the heap holds -- measured as one peer reading
    // 0x00000400 while the other faulted, and the two SWAPPING roles on the
    // next run. `in_battle` does not cover it: a cat can be parked off the
    // board and still be in the live character list, which is exactly the
    // battle that exposed this.
    uint32_t el[2] = { 0, 0 };
    if (in_battle && !off_board(out) && read_affecting_elements(chr, el)) {
        out.e0 = el[0];
        out.e1 = el[1];
        out.elems = 1;
    }
    return true;
}

// Dumps every cat's state to the log. Called on a state-only hash mismatch,
// because the hash says only THAT the peers disagree. Two logs each holding
// this table, diffed, say WHICH cat and which field -- which is exactly how the
// one suspected desync so far was settled, by diffing the raw traces rather
// than by staring at hashes.
// Membership, not length.
//
// A turn that APPENDS a summon and REMOVES something else leaves the count
// unchanged, so comparing lengths reports "nothing was summoned or removed"
// while both things happened. That is not hypothetical: a RatBomb boss fight
// does it, and the compensating pair hid a real roster change behind a
// reassuring log line while a snapshot entry that had left the battle went on
// being hashed.
//
// So compare by POINTER. `still_in[i]` answers "is snapshot cat i still in the
// live list", and `appeared` counts live entries that are not in the snapshot.
//
// O(n^2) over at most kMaxCats entries, once per turn boundary -- far cheaper
// than the hash it sits beside.
// A POINTER IS NOT AN IDENTITY (2026-10-03, a turn-18 halt on a state-only mismatch): a unit that leaves the battle frees its Character, and the next unit created -- a summon -- can be handed
// the SAME ADDRESS. By pointer alone the departed roster entry (a dead baby shark, gone since turn 9) came back to life as the new unit, and the two peers, whose allocators had reused
// different addresses, each revived it as a DIFFERENT unit: cat 24 stood on (5,4) here and (4,5) there, and nothing was ever adopted as new. So an entry IS the live object at its address
// only while the object's creation key (kChar_Ident) is still the one it had when the entry was taken; another key at the same address is another unit -- an arrival.
bool read_ident(const void* chr, uint32_t& key) {
    return chr && mem_read((const uint8_t*)chr + kChar_Ident, &key, sizeof(key));
}
void note_ident(uint32_t i) {
    g.cat_key[i] = 0;
    g.cat_key_ok[i] = read_ident(g.cats[i], g.cat_key[i]);
}
bool entry_is(uint32_t j, const void* ptr) {
    if (j >= g.cat_count || g.cats[j] != ptr) return false;
    if (!g.cat_key_ok[j]) return true;                       // no key was readable: the address is all there is
    uint32_t now = 0;
    return !read_ident(ptr, now) || now == g.cat_key[j];
}

// A unit this peer's transform replaced (see g.retired): still in the live list for a while, but neither an arrival nor a roster entry.
bool is_retired(const void* ptr) {
    for (uint32_t k = 0; k < g.retired_n; ++k) {
        if (g.retired[k] != ptr) continue;
        uint32_t now = 0;
        return !read_ident(ptr, now) || now == g.retired_key[k];
    }
    return false;
}
void retire_unit(const void* chr) {
    if (!chr || is_retired(chr)) return;
    if (g.retired_n >= State::kMaxRetired) {          // the oldest goes: it has long since been flushed from the list
        for (uint32_t k = 1; k < g.retired_n; ++k) { g.retired[k - 1] = g.retired[k]; g.retired_key[k - 1] = g.retired_key[k]; }
        --g.retired_n;
    }
    uint32_t key = 0;
    read_ident(chr, key);
    g.retired[g.retired_n] = chr; g.retired_key[g.retired_n] = key; ++g.retired_n;
}

bool snapshot_membership(const void* list, uint32_t& live,
                         bool still_in[kMaxCats], uint32_t& appeared) {
    live = appeared = 0;
    for (uint32_t i = 0; i < kMaxCats; ++i) still_in[i] = false;

    const void* data = nullptr;
    if (!list ||
        !mem_read((const uint8_t*)list + kList_Count, &live, sizeof(live)) ||
        !mem_read((const uint8_t*)list + kList_Data,  &data, sizeof(data)) || !data)
        return false;

    const uint32_t n = live < kMaxCats ? live : kMaxCats;
    const void* cur[kMaxCats] = {};
    for (uint32_t i = 0; i < n; ++i)
        if (!mem_read((const uint8_t*)data + i * sizeof(void*), &cur[i], sizeof(cur[i])))
            return false;

    for (uint32_t i = 0; i < n; ++i) {
        bool found = false;
        for (uint32_t j = 0; j < g.cat_count && !found; ++j) {
            if (cur[i] == g.cats[j] && entry_is(j, cur[i])) { still_in[j] = true; found = true; }
        }
        if (!found && !is_retired(cur[i])) ++appeared;
    }
    for (uint32_t j = 0; j < g.cat_count && j < kMaxCats; ++j) {
        if (!g.fresh_left[j]) continue;
        if (still_in[j]) g.fresh_left[j] = 0;                                   // the list shows it now: an ordinary member from here on
        else if (g.cats[j] && entry_is(j, g.cats[j])) still_in[j] = true;      // made this boundary, not listed yet
    }
    return true;
}

uint32_t snapshot_departed(const bool still_in[kMaxCats]) {
    uint32_t left = 0;
    for (uint32_t j = 0; j < g.cat_count; ++j) if (!still_in[j]) ++left;
    return left;
}

void dump_cat_states(const char* why) {
    log_line("LOCKSTEP", "cat state table (%s):", why);

    // Worked out up front so each row can say whether that cat is still in the
    // battle at all. A departed entry is the one case where the fields below
    // are not trustworthy: our snapshot pointer outlives its membership.
    uint32_t live_now = 0, appeared_now = 0;
    bool still_in[kMaxCats];
    const bool have_membership =
        snapshot_membership(g.snapped_list, live_now, still_in, appeared_now);

    for (uint32_t i = 0; i < g.cat_count; ++i) {
        CatState st{};
        const bool present = !have_membership || still_in[i];
        if (!read_cat_state(g.cats[i], st, present)) {
            log_line("LOCKSTEP", "  cat %2u  <unreadable>", i);
            continue;
        }
        log_line("LOCKSTEP", "  cat %2u  hp=%d/%d shield=%d tile=(%d,%d) face=(%d,%d) elem=%08X:%08X%s%s%s%s",
                 i, st.hp, st.maxhp, st.shield, st.tx, st.ty, st.fx, st.fy,
                 st.e0, st.e1,
                 st.dead ? " DEAD" : "", st.linked ? "" : " [unlinked]",
                 st.elems ? "" : " [no-elem]",
                 (have_membership && !still_in[i]) ? " [GONE -- no longer in the live list]" : "");
        // WHERE A MAX-HP DISAGREEMENT COMES FROM (2026-10-01, the 3-player halt: five human cats' maxhp
        // differed between peers with byte-identical cat images). maxHP = max(1, 4*stat[2] + bonus[0x5E8])
        // (recompute_stats, 0x140102C55), so print the seven stats the Character holds, the bonus term, the
        // two derived fields beside them and the CatData's id -- the next halt then says which input moved.
        if (present) {
            int32_t stat[7] = {}, b_5d8 = 0, b_5dc = 0, b_5e8 = 0, b_5f0 = 0, b_950 = 0, b_954 = 0;
            const uint8_t* c = (const uint8_t*)g.cats[i];
            bool ok = true;
            for (int k = 0; k < 7; ++k) ok = mem_read(c + 0x5BC + k * 4, &stat[k], 4) && ok;
            mem_read(c + 0x5D8, &b_5d8, 4); mem_read(c + 0x5DC, &b_5dc, 4);
            mem_read(c + 0x5E8, &b_5e8, 4); mem_read(c + 0x5F0, &b_5f0, 4);
            mem_read(c + 0x950, &b_950, 4); mem_read(c + 0x954, &b_954, 4);
            const void* cd = nullptr; uint64_t cid = 0;
            if (mem_read(c + kChar_CatData, &cd, sizeof(cd)) && cd) mem_read((const uint8_t*)cd + kCatData_SaveId, &cid, sizeof(cid));
            if (ok)
                log_line("LOCKSTEP", "      stats [%d %d %d %d %d %d %d] bonus 5D8=%d 5DC=%d 5E8=%d 5F0=%d | 950=%d 954=%d | catdata id %016llX",
                         stat[0], stat[1], stat[2], stat[3], stat[4], stat[5], stat[6], b_5d8, b_5dc, b_5e8, b_5f0,
                         b_950, b_954, (unsigned long long)cid);
        }
    }

    // Everything the snapshot does NOT cover: summons, and anything else the
    // battle appended to the character list after it started.
    //
    // These are invisible to the state hash by design -- the snapshot is frozen
    // at battle start so that a summon cannot renumber every cat mid-fight --
    // and each peer's own AI drives its own copies. That is fine while both
    // peers summon the same things at the same turns, and NOTHING CHECKED IT.
    //
    // It matters because a type-6 reaction broadcast walks the live character
    // list, not our snapshot. If the tails differ, a reaction hits a different
    // set of targets and a hashed cat loses a different amount of HP -- with no
    // RNG difference at all, because nothing rolled. That is exactly the shape
    // of the halt this was added for: 45 identical cats and cat 38 two HP
    // apart, in a turn whose only event was a type-6.
    //
    // Printed rather than hashed, for the same reason passive counts are: a
    // summon appearing a frame apart on two peers is not a desync, and a hash
    // would make it one.
    const void* list = g.snapped_list;
    uint32_t    live = 0;
    const void* data = nullptr;
    if (!list ||
        !mem_read((const uint8_t*)list + kList_Count, &live, sizeof(live)) ||
        !mem_read((const uint8_t*)list + kList_Data,  &data, sizeof(data)) || !data) {
        log_line("LOCKSTEP", "  (could not read the live character list)");
        return;
    }

    const uint32_t departed = have_membership ? snapshot_departed(still_in) : 0;

    if (have_membership && appeared_now == 0 && departed == 0) {
        log_line("LOCKSTEP", "  live list is %u and every snapshot entry is still in"
                             " it -- nothing was summoned or removed", live);
        return;
    }

    if (have_membership && (appeared_now != 0 || departed != 0)) {
        log_line("LOCKSTEP", "  ROSTER CHANGED: %u entr(ies) appeared after battle"
                             " start and %u snapshot entr(ies) left the live list"
                             " (live %u, snapshot %u). COMPARE BOTH NUMBERS WITH THE"
                             " PEER'S FIRST: if they differ the divergence is here"
                             " and not in the %u cats above.",
                 appeared_now, departed, live, g.cat_count, g.cat_count);
    }

    if (!have_membership) {
        log_line("LOCKSTEP", "  live list is %u, snapshot was %u (membership could not"
                             " be read -- counts only)", live, g.cat_count);
    }

    // The entries the battle ADDED, found by membership rather than by index.
    // Walking from g.cat_count upward only works while the tail is pure growth:
    // an append paired with a removal leaves the count equal and puts the
    // newcomer somewhere in the middle, where an index scan never looks.
    for (uint32_t i = 0; i < live && i < kMaxCats; ++i) {
        const void* ch = nullptr;
        if (!mem_read((const uint8_t*)data + i * sizeof(void*), &ch, sizeof(ch)) || !ch)
            continue;

        bool in_snapshot = false;
        for (uint32_t j = 0; j < g.cat_count && !in_snapshot; ++j)
            in_snapshot = entry_is(j, ch);
        if (in_snapshot) continue;
        char cls[96];
        strcpy_s(cls, "?");
        const void* brain = nullptr;
        if (mem_read((const uint8_t*)ch + kChar_Brain, &brain, sizeof(brain)) && brain)
            rtti_class_name(brain, cls, sizeof(cls));

        CatState st{};
        if (!read_cat_state(ch, st)) {
            log_line("LOCKSTEP", "  +%2u  <unreadable>  brain=%s", i, cls);
            continue;
        }
        log_line("LOCKSTEP", "  +%2u  hp=%d/%d shield=%d tile=(%d,%d) brain=%s%s",
                 i, st.hp, st.maxhp, st.shield, st.tx, st.ty, cls,
                 st.dead ? " DEAD" : "");
    }
}

// --- the per-turn state trace (net_state_trace) -----------------------------
//
// One line per cat whose state moved since the previous turn boundary.
//
// This exists because the hash and the mismatch dump between them answer only
// two of the three questions a state-only desync raises. The hash says THAT the
// peers disagree; the dump says WHICH FIELD, at the moment it was noticed. What
// neither says is WHEN that field last moved -- and for a difference of a
// couple of HP that is the question the diagnosis turns on:
//
//   * both peers show `cat 38 hp 36->33`, on different turns
//         -> the same effect landed on opposite sides of a turn boundary.
//            A timing artefact of the kind facing already produces; the fix is
//            where the boundary is taken, not what the hash covers.
//   * one peer shows `hp 36->33` and the other `hp 36->31`
//         -> the two simulations really did compute different damage, and the
//            RNG hash agreeing means nothing rolled to cause it.
//
// Diff the two peers' delta lines and both readings are there without another
// run.
//
// Facing USED to be excluded here as known noise, because the aim preview
// rewrote it every frame under a wall-clock gate. That is no longer true --
// lockstep_preview_facing_* puts the preview's write back -- and the exclusion
// turned out to be expensive: facing decides backstab damage, so a divergence
// in it surfaced as three missing HP on turn 49 of a fight whose first 48 turns
// hashed identically. It is traced now.

// Append "<space>" + one formatted field to a fixed buffer, carrying the write
// position in `n`. Truncation is silent and bounded, which is the right answer
// for a diagnostic line: a clipped delta is still a delta, and a cat whose
// every field moved at once is not the interesting case.
void appendf(char* buf, size_t cap, int& n, const char* fmt, ...) {
    if (n < 0 || (size_t)n >= cap - 1) return;
    if (n > 0) { buf[n++] = ' '; buf[n] = 0; }
    va_list ap;
    va_start(ap, fmt);
    int w = _vsnprintf_s(buf + n, cap - n, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (w > 0) n += w;
    else       n = (int)(cap - 1);   // truncated: stop appending
}

void trace_state_deltas() {
    if (!tune::kStateTrace || !g.snapped) return;
    // Gated on the same evidence the hash is. With state_hash_on false at least
    // one cat failed its link or range check, so every reading here is heap luck
    // -- and a trace of heap luck is worse than silence: it prints movement that
    // never happened, in the one place a reader is looking for movement that
    // did. Nothing is lost, either: a zero state hash cannot mismatch.
    if (!g.state_hash_on) return;

    uint32_t live = 0, appeared = 0;
    bool still_in[kMaxCats];
    const bool have_membership =
        snapshot_membership(g.snapped_list, live, still_in, appeared);

    for (uint32_t i = 0; i < g.cat_count; ++i) {
        const bool present = !have_membership || still_in[i];

        CatState now{};
        // A cat that cannot be read at all gets a default-constructed entry, so
        // "became unreadable" shows up as a change like any other rather than
        // silently freezing its last known values.
        read_cat_state(g.cats[i], now, present);

        const CatState& was = g.prev_state[i];
        if (!g.have_prev) { g.prev_state[i] = now; continue; }

        // Field by field, and only the fields the HASH covers -- a trace that
        // reports movement the hash cannot see would send the next reader
        // chasing a difference that never mattered.
        char what[256];
        int  n = 0;
        what[0] = 0;

        if (now.hp     != was.hp)     appendf(what, sizeof(what), n, "hp %d->%d", was.hp, now.hp);
        if (now.shield != was.shield) appendf(what, sizeof(what), n, "shield %d->%d", was.shield, now.shield);
        if (now.maxhp  != was.maxhp)  appendf(what, sizeof(what), n, "maxhp %d->%d", was.maxhp, now.maxhp);
        if (now.tx != was.tx || now.ty != was.ty)
            appendf(what, sizeof(what), n, "tile (%d,%d)->(%d,%d)", was.tx, was.ty, now.tx, now.ty);
        if (now.dead   != was.dead)   appendf(what, sizeof(what), n, "dead %u->%u", was.dead, now.dead);
        if (now.linked != was.linked) appendf(what, sizeof(what), n, "linked %u->%u", was.linked, now.linked);
        if (now.e0 != was.e0 || now.e1 != was.e1)
            appendf(what, sizeof(what), n, "elem %08X:%08X->%08X:%08X", was.e0, was.e1, now.e0, now.e1);
        if (now.elems  != was.elems)  appendf(what, sizeof(what), n, "elem-read %u->%u", was.elems, now.elems);
        if (now.in_battle != was.in_battle)
            appendf(what, sizeof(what), n, "in-battle %u->%u", was.in_battle, now.in_battle);
        // Facing is traced even though the hash does not cover it -- the one
        // exception to the "only hashed fields" rule above, and it is earned.
        // The backstab test reads facing, so it decides damage; the hash cannot
        // cover it because presentation also writes it (CombatAnimation::update
        // turns a cat mid-animation, which is in flight at different points on
        // two machines). Trace-only is the honest middle: a facing difference
        // shows up in a log diff instead of surfacing as three missing HP forty
        // turns later, which is exactly how it surfaced on 2026-08-26.
        if (now.fx != was.fx || now.fy != was.fy)
            appendf(what, sizeof(what), n, "face (%d,%d)->(%d,%d)",
                    was.fx, was.fy, now.fx, now.fy);

        if (n > 0)
            log_line("LOCKSTEP", "turn %u delta: cat %u %s", g.turn, i, what);

        g.prev_state[i] = now;
    }
    g.have_prev = true;
}

// --- cat identity -----------------------------------------------------------

uint8_t cat_index_of(const void* character) {
    if (!character) return kNoCat;
    for (uint32_t i = 0; i < g.cat_count; ++i)
        if (entry_is(i, character)) return (uint8_t)i;
    return kNoCat;
}

// Snapshot the battle's character list. Called once per battle, at the first
// turn boundary -- by then Level::init has run and every starting cat exists.
// Resolve TurnControl -> the battle's character list. Split out from the
// snapshot because it is also the new-battle detector: a different vector
// object means a different battle.
const void* resolve_char_list(void* turn_control) {
    const void* scene  = nullptr;
    const void* sub    = nullptr;
    const void* holder = nullptr;
    const void* list   = nullptr;
    if (!turn_control) return nullptr;
    if (!mem_read((const uint8_t*)turn_control + kTC_Scene, &scene, sizeof(scene)) || !scene) return nullptr;
    if (!mem_read((const uint8_t*)scene + kScene_Sub, &sub, sizeof(sub)) || !sub) return nullptr;
    if (!mem_read((const uint8_t*)sub + kSub_Holder, &holder, sizeof(holder)) || !holder) return nullptr;
    if (!mem_read((const uint8_t*)holder + kHolder_List, &list, sizeof(list)) || !list) return nullptr;
    return list;
}

void snapshot_cats(void* turn_control) {
    if (g.snapped || !turn_control) return;

    const void* list = resolve_char_list(turn_control);
    if (!list) return;

    uint32_t    count = 0;
    const void* data  = nullptr;
    if (!mem_read((const uint8_t*)list + kList_Count, &count, sizeof(count))) return;
    if (!mem_read((const uint8_t*)list + kList_Data, &data, sizeof(data)) || !data) return;
    if (count == 0 || count > kMaxCats) {
        // NOT a warning-and-continue. Without a roster there is no control
        // split, so nothing is sent, nothing is injected and no hash is
        // compared -- the two peers play the same battle independently and
        // every summary line reports success. A boss fight ran that way for
        // four turns and the session still ended "0 desync(s)".
        //
        // A count of 0 is the reverse case: the list is not ready yet, which
        // happens legitimately before a battle is built. That one just returns.
        if (count == 0) return;
        char why[128];
        _snprintf_s(why, sizeof(why), _TRUNCATE,
                    "character list count %u exceeds kMaxCats (%u) -- lockstep "
                    "cannot run this battle", count, kMaxCats);
        log_line("LOCKSTEP", "!! %s", why);
        halt(why);
        return;
    }

    g.cat_count = 0;
    g.retired_n = 0;
    for (uint8_t& f : g.fresh_left) f = 0;
    for (bool& summon : g.summon) summon = false;
    for (uint32_t i = 0; i < count; ++i) {
        const void* c = nullptr;
        if (!mem_read((const uint8_t*)data + i * sizeof(void*), &c, sizeof(c)) || !c) continue;
        g.cats[g.cat_count] = c;
        note_ident(g.cat_count);
        ++g.cat_count;
    }
    g.snapped      = true;
    g.snapped_list = list;
    g.snapshot_battle = g.battles.current;

    // A new roster invalidates the table the last hash was taken over, and
    // re-arms the one-shot desync dump for this battle.
    g.hashed_valid = false;
    g.hashed_count = 0;
    g.dump_sent    = false;
    g.dump_seen    = false;

    // --- who is human -------------------------------------------------
    //
    // First, because the automatic split is derived from it. Only human-driven
    // cats are split between the peers; an AI cat is left to each peer's own
    // brain to re-derive, which is not a compromise but the stronger
    // arrangement -- phase 2B deliberately left 12 of 29 decisions to
    // PatternBrain rather than injecting them, and all 12 matched.
    const Config& cfg = config();
    char bcls[kMaxCats][96];
    g.humans = 0;
    for (uint32_t i = 0; i < g.cat_count; ++i) {
        const void* brain = nullptr;
        strcpy_s(bcls[i], "?");
        if (mem_read((const uint8_t*)g.cats[i] + kChar_Brain, &brain, sizeof(brain)) && brain)
            rtti_class_name(brain, bcls[i], sizeof(bcls[i]));
        g.human_cat[i] = (strstr(bcls[i], tune::kReplayBrains) != nullptr);
        if (g.human_cat[i]) ++g.humans;
    }

    // --- what each human cat IS, so the split need not trust the order -------
    //
    // The roster's order is spawn order and is identical on both peers GIVEN the
    // same run state -- and the run state is exactly what is not guaranteed. Two
    // peers whose parties were picked independently produced the same 32 cats in
    // a different arrangement, every human cat moved to the other peer, and the
    // battle was unplayable from both sides. The fingerprint (an ability-slot
    // hash by authored name, see mgmp_ability) is the same number on both peers
    // whatever order they used, so the split can be derived from the cats
    // themselves.
    //
    // ONE unreadable cat makes the whole ordering unusable rather than partially
    // usable: a zero ties with every other zero and the tiebreak is the index,
    // which is the very thing being avoided.
    for (uint32_t i = 0; i < kMaxCats; ++i) g.cat_fp[i] = 0;
    g.fp_usable = g.humans > 0;
    char fplog[288] = {};
    int  fpoff = 0;
    for (uint32_t i = 0; i < g.cat_count; ++i) {
        if (!g.human_cat[i]) continue;
        g.cat_fp[i] = character_fingerprint(g.cats[i]);
        if (!g.cat_fp[i]) g.fp_usable = false;
        if (fpoff < (int)sizeof(fplog) - 24)
            fpoff += _snprintf_s(fplog + fpoff, sizeof(fplog) - fpoff, _TRUNCATE,
                                 " %u:%08x", i, g.cat_fp[i]);
    }
    if (g.humans)
        log_line("LOCKSTEP", "human cats (index:fingerprint):%s%s", fplog,
                 g.fp_usable ? ""
                             : "   <-- at least one unreadable: the split falls back to"
                               " roster ORDER, which is the fragile thing");

    // --- the control split ---------------------------------------------
    for (uint32_t i = 0; i < kMaxCats; ++i) g.local_cat[i] = false;

    if (cfg.net_control_auto) {
        // Derived identically on both peers rather than negotiated, which is
        // why it needs no message and no waiting: the roster is byte-identical
        // (measured -- 29 cats, same brain class per index, two processes with
        // completely different heap addresses), the human filter is the same
        // string compare, and the rule below is pure arithmetic. Both peers
        // still exchange CONTROL afterwards, but as a check on the result --
        // not as the source of it.
        //
        // Spread the human cats over however many players are in the session,
        // in roster order, host first, with the remainder going to the earliest
        // positions. Four cats over three players is 2/1/1; three over two is
        // 2/1, which is what the two-player rule always did -- the old
        // `(humans+1)/2` is exactly this formula at P=2, so nothing changes for
        // a two-player session.
        //
        // Position, not peer id: see net_peer_pos. And contiguous rather than
        // interleaved, so each player's cats are adjacent in the roster and the
        // log is readable by eye.
        uint32_t P   = net_peer_count();
        uint32_t pos = net_peer_pos();
        if (P == 0) {
            // No PEERS yet. Claim nothing rather than guess: claiming
            // everything would have two peers driving the same cat, and
            // claiming half assumes a player count we have not been told.
            P = 1; pos = 0;
            log_line("LOCKSTEP", "!! no peer list yet -- deferring the split; if "
                                 "this repeats, PEERS never arrived");
        }
        const SplitRange range = split_for(g.humans, P, pos);
        const uint32_t   mine  = range.count;

        // THE ORDER THE RANGE IS APPLIED IN, and the one thing that makes this
        // work across two peers who built their rosters differently: it is the
        // order of the CATS, not of their positions. Same four cats on both sides
        // -- even in a different arrangement -- and `split_order_by_fp` yields
        // the same sequence on both, so the arithmetic below agrees without
        // either peer knowing where the other put them.
        //
        // The fallback is the old positional walk, kept for the case where a
        // fingerprint could not be read. It is NOT equivalent: it is the rule
        // that fails on a permutation, and the log says which one ran.
        // Explicit setup/parent ownership takes precedence over equal ranges.
        // Unknown friendly units fall back to host without moving known cats.
        uint8_t  order[kMaxCats] = {};
        uint8_t  note[kMaxCats]  = {};
        bool     has_note[kMaxCats] = {};
        // The save id of each cat, indexed by the cat's own index. This is the ONE
        // ordering key that is identical on both peers without anyone having to agree
        // on anything: it is read out of the cat itself, at the moment the split runs.
        uint64_t id_of[kMaxCats] = {};
        uint32_t n = 0;
        bool     by_owner = false;

        if (g.fp_usable || tune::kOwnershipSplit) {
            if (g.fp_usable)
                n = split_order_by_fp(g.cat_fp, g.human_cat, g.cat_count, order);
            else
                for (uint32_t i = 0; i < g.cat_count; ++i)
                    if (g.human_cat[i]) order[n++] = (uint8_t)i;

            if (tune::kOwnershipSplit) {
                // WHERE THE OWNERS COME FROM WHEN NOBODY TOLD US (2026-09-23, step B).
                //
                // The 4+4 convention: the run's party holds the four cats the HOST plays,
                // and the familiars carry the other player's (that is what the import
                // puts there, and what the host-authoritative familiar sync keeps equal
                // on both peers). Both peers read the SAME two vectors out of the SAME
                // shared run, so notes derived here need no wire format at all -- which
                // matters, because the order they feed must be identical on both peers
                // or the ranges below hand out different cats.
                //
                // ABSOLUTE POSITIONS: party -> 0 (host), a familiar that is not in the
                // party -> 1 (the second player). Not "mine": two peers who each wrote
                // "mine" would order the list in mirrored ways.
                if (const void** slot = (const void**)addr_of_data(D_MewDirectorPtr)) {
                    const void* md = nullptr;
                    if (mem_read(slot, &md, sizeof(md)) && md) {
                        uint32_t  pn = 0, fn = 0;
                        uintptr_t pd = 0, fd = 0;
                        if (mem_read((const uint8_t*)md + kDir_CatIdCount, &pn, 4) &&
                            mem_read((const uint8_t*)md + kDir_CatIdData,  &pd, sizeof(pd)) &&
                            mem_read((const uint8_t*)md + kDir_CatFamiliars + 4, &fn, 4) &&
                            mem_read((const uint8_t*)md + kDir_CatFamiliars + 8, &fd, sizeof(fd))) {
                            // THESE ARE FALLBACKS, NEVER CORRECTIONS (2026-10-01). The 3-player halt run
                            // showed this block rewriting player 3's cats from their true owner (2) to 1
                            // -- "a familiar that is not in the party belongs to the second player" is a
                            // 2-player rule -- so peer 2 claimed no cat at all. A cat with a note keeps it;
                            // a session cat's owner is in its id (session_cat_owner); only a cat that is
                            // neither falls back to the old 4+4 convention.
                            uint8_t known = 0;
                            for (uint32_t j = 0; j < pn && j < 16; ++j) {
                                uint64_t id = 0;
                                if (mem_read((const uint8_t*)pd + j * 8, &id, 8) && id && !lockstep_owner_pos(id, known)) {
                                    const int so = session_cat_owner(id);
                                    lockstep_note_owner(id, so >= 0 ? (uint8_t)so : 0);
                                }
                            }
                            for (uint32_t j = 0; j < fn && j < 16; ++j) {
                                uint64_t id = 0;
                                if (!mem_read((const uint8_t*)fd + j * 8, &id, 8) || !id) continue;
                                bool in_party = false;
                                for (uint32_t k2 = 0; k2 < pn && !in_party; ++k2) {
                                    uint64_t have = 0;
                                    if (mem_read((const uint8_t*)pd + k2 * 8, &have, 8) && have == id)
                                        in_party = true;
                                }
                                if (!in_party && !lockstep_owner_pos(id, known)) {
                                    const int so = session_cat_owner(id);
                                    lockstep_note_owner(id, so >= 0 ? (uint8_t)so : 1);
                                }
                            }
                        }
                    }
                }

                // Gather high-confidence evidence for EVERY unit before history
                // lookup, so roster enumeration order cannot change the winner.
                DerivedParentHint derived[kMaxCats]{};
                for (uint32_t k = 0; k < n; ++k) {
                    const uint32_t index = order[k];
                    const void* data = nullptr;
                    uint64_t id = 0;
                    uint8_t owner = kNoPeer;
                    if (mem_read((const uint8_t*)g.cats[index] + kChar_CatData, &data, sizeof(data)) &&
                        data && mem_read((const uint8_t*)data + kCatData_SaveId, &id, 8) &&
                        lockstep_owner_pos(id, owner)) continue;
                    derived[index] = find_derived_parent(g.cats[index], index);
                    const auto f = derived_features(index);
                    const auto& hint = derived[index];
                    if (hint.found && (hint.method == 1 || hint.method == 2))
                        g_derived_history.remember(f, hint.parent_id);
                    log_line("DERIVED", "slot=%u name='%s' type='%s' hp=%d/%d shield=%d abilities=%08X "
                             "method=%u parent=%llX history=%u", index, f.name, f.type, f.hp, f.maxhp,
                             f.shield, f.abilities, hint.method, (unsigned long long)hint.parent_id,
                             g_derived_history.count);
                }
                uint32_t noted = 0;
                for (uint32_t k = 0; k < n; ++k) {
                    const void* data = nullptr;
                    uint64_t    id   = 0;
                    if (mem_read((const uint8_t*)g.cats[order[k]] + kChar_CatData, &data,
                                 sizeof(data)) && data &&
                        mem_read((const uint8_t*)data + kCatData_SaveId, &id, sizeof(id)) && id) {
                        id_of[order[k]] = id;
                        if (lockstep_owner_pos(id, note[order[k]])) {
                            has_note[order[k]] = true;
                            ++noted;
                        }
                    }
                    // A class/passive summon can be a PlayerBrain without a
                    // CatData/id.  Before treating it as an unowned extra,
                    // look for a unique parent Character or CatData pointer
                    // in the generated object's component state.
                    if (!has_note[order[k]]) {
                        DerivedParentHint hint = derived[order[k]];
                        // A duplicate possessive name is conflicting evidence,
                        // not permission for a weaker historical guess.
                        if (!hint.found && hint.method != 2)
                            hint = find_derived_parent(g.cats[order[k]], order[k], true);
                        if (hint.found) {
                            note[order[k]] = hint.owner;
                            // The generated unit is not the parent's cat and must not
                            // reuse its sort key.  Reusing parent_id made the ownership
                            // order contain the same id twice (parent + summon); that
                            // leaves the insertion sort dependent on roster position
                            // when the two peers enumerate the generated object at a
                            // slightly different point.  It has no own save id, so keep
                            // it after the real cats in its owner's block.
                            id_of[order[k]] = UINT64_MAX;
                            has_note[order[k]] = true;
                            ++noted;
                            log_line("LOCKSTEP", "derived human cat %u attributed to owner %u "
                                                   "parent slot %u id=%016llX via %s+0x%llX (%s), method=%u score=%d",
                                     (unsigned)order[k], (unsigned)hint.owner, (unsigned)hint.parent,
                                     (unsigned long long)hint.parent_id,
                                     hint.root_offset == 0 ? "Character" :
                                         (hint.root_offset == 0xB000 ? "PlayerBrain" : "TacticsObject"),
                                     (unsigned long long)hint.field_offset,
                                     hint.catdata ? "CatData*" : "Character*", hint.method, hint.score);
                        }
                    }
                }
                const uint32_t unknown = n - noted;
                by_owner = split_by_owner(g.human_cat, note, has_note,
                                          g.cat_count, pos, g.local_cat);
                if (by_owner) {
                    if (unknown) {
                        for (uint32_t k = 0; k < n; ++k) {
                            if (has_note[order[k]]) continue;
                            note[order[k]] = 0;
                            id_of[order[k]] = UINT64_MAX;
                        }
                        log_line("LOCKSTEP", "ownership retained for %u known human cat(s);"
                                             " assigning %u unresolved friendly unit(s) to host",
                                 noted, unknown);
                    }
                    // Stable diagnostic order only; ownership has already been
                    // assigned directly, not via equal-sized sorted ranges.
                    for (uint32_t a = 1; a < n; ++a) {
                        const uint32_t cur = order[a];
                        uint32_t b = a;
                        while (b > 0 && (note[order[b - 1]] > note[cur] ||
                                         (note[order[b - 1]] == note[cur] &&
                                          id_of[order[b - 1]] > id_of[cur]))) {
                            order[b] = order[b - 1];
                            --b;
                        }
                        order[b] = cur;
                    }
                } else {
                    log_line("LOCKSTEP", "ownership order NOT used: %u of %u human cat(s)"
                                         " carry an owner note -- using the fingerprint"
                                         " order instead (identical on both peers either"
                                         " way, which is the only requirement)", noted, n);
                }
            }

            if (!by_owner)
                for (uint32_t k = 0; k < n; ++k)
                    g.local_cat[order[k]] = split_owns(range, k);

            if (by_owner) {
                // The id is printed next to the index so this line can be compared against
                // the peer's directly. An index-only line looks IDENTICAL on two peers whose
                // rosters are permuted -- which is exactly the state that halted the session
                // at 13:59 with two correct ownership tables.
                char txt[320] = {};
                int  off = 0;
                for (uint32_t k = 0; k < n && off < (int)sizeof(txt) - 32; ++k)
                    off += _snprintf_s(txt + off, sizeof(txt) - off, _TRUNCATE,
                                       "%s%u:owner%u:id%llx",
                                       k ? " " : "", order[k], note[order[k]],
                                       (unsigned long long)id_of[order[k]]);
                log_line("LOCKSTEP", "control split assigned by OWNERSHIP: %s -- explicit owners"
                                     " preserved regardless of team sizes",
                         txt);
            }
        } else {
            uint32_t seen = 0;
            for (uint32_t i = 0; i < g.cat_count; ++i) {
                if (!g.human_cat[i]) continue;
                g.local_cat[i] = split_owns(range, seen);
                ++seen;
            }
        }

        if (mine == 0 && g.humans)
            log_line("LOCKSTEP", "!! %u human cat(s) over %u player(s) leaves this "
                                 "peer (position %u) with none -- it will watch "
                                 "this battle", g.humans, P, pos);
    } else {
        for (uint32_t i = 0; i < cfg.net_control_count; ++i) {
            uint8_t idx = cfg.net_control[i];
            if (idx < g.cat_count) g.local_cat[idx] = true;
            else log_line("LOCKSTEP", "!! net_control names cat %u but the battle has %u",
                          (unsigned)idx, g.cat_count);
        }
    }

    uint32_t local = 0;
    for (uint32_t i = 0; i < g.cat_count; ++i) if (g.local_cat[i]) ++local;
    g.stats.cats       = g.cat_count;
    g.stats.local_cats = local;

    // ...and remember it for the map. See State::owned_cats: mgmp_catsync needs this
    // between nodes, where lockstep_cat_owner cannot answer. Read fresh here rather
    // than reusing level_cats, which is only filled when the level-up probe is on.
    g.owned_count = 0;
    g.owned_valid = true;
    for (uint32_t i = 0; i < g.cat_count; ++i) {
        if (!g.human_cat[i]) continue;              // AI cats and familiars: no entry
        const void* data = nullptr;
        uint64_t id = 0;
        if (!mem_read((const uint8_t*)g.cats[i] + kChar_CatData, &data, sizeof(data)) || !data)
            continue;
        if (!mem_read((const uint8_t*)data + kCatData_SaveId, &id, sizeof(id)) || !id)
            continue;
        if (g.owned_count >= kMaxCats) break;
        g.owned_cats[g.owned_count].save_id = id;
        g.owned_cats[g.owned_count].mine    = g.local_cat[i];
        uint8_t owner_pos = kNoPeer;
        if (!lockstep_owner_pos(id, owner_pos)) {
            // Setup IDs already carry an absolute namespace.  The fallback
            // also keeps the two-player legacy path usable between battles.
            if (session_cat_owner(id)>=0) owner_pos = (uint8_t)session_cat_owner(id);
            else if (net_peer_count() == 2)
                owner_pos = g.local_cat[i] ? net_peer_pos() : (net_peer_pos() ? 0 : 1);
        }
        g.owned_cats[g.owned_count].owner   = owner_pos;
        ++g.owned_count;
    }
    log_line("LOCKSTEP", "control split remembered for the map: %u of %u human cat(s) "
                         "named, %u of them mine -- mgmp_catsync publishes and applies"
                         " under this rule until the next run",
             g.owned_count, g.humans, local);

    if (tune::kChoice || tune::kLevelUpProbe) {
        g.level_probe_node = g.battles.current;
        for (auto& cat : g.level_cats) cat = {};
        if (tune::kLevelUpProbe) log_line("CHOICE", "PROBE capture: node=%016llX, %u human cat(s)",
                 (unsigned long long)g.level_probe_node, g.humans);
        for (uint32_t i = 0; i < g.cat_count; ++i) {
            if (!g.human_cat[i]) continue;
            auto& cat = g.level_cats[i];
            const bool ptr_ok = mem_read((const uint8_t*)g.cats[i] + kChar_CatData,
                                         &cat.data, sizeof(cat.data)) && cat.data;
            const bool seed_ok = ptr_ok && mem_read((const uint8_t*)cat.data + kCatData_Seed,
                                                    &cat.seed, sizeof(cat.seed));
            const bool id_ok = ptr_ok && mem_read((const uint8_t*)cat.data + kCatData_SaveId,
                                                  &cat.save_id, sizeof(cat.save_id));
            cat.readable = seed_ok && id_ok;
            if (!cat.readable)
                log_line("CHOICE", "!! cat identity unavailable at slot %u: ptr=%u seed=%u id=%u; upgrades will wait",
                         i, (unsigned)ptr_ok, (unsigned)seed_ok, (unsigned)id_ok);
            if (tune::kLevelUpProbe) log_line("CHOICE", "PROBE capture slot %u %s fp=%08X: Character=0x%llX "
                               "+0x%X -> CatData=0x%llX ptr_ok=%u seed_ok=%u id_ok=%u "
                               "seed=%016llX save_id=%016llX",
                     i, g.local_cat[i] ? "MINE" : "peer", g.cat_fp[i],
                     (unsigned long long)(uintptr_t)g.cats[i], (unsigned)kChar_CatData,
                     (unsigned long long)(uintptr_t)cat.data,
                     (unsigned)ptr_ok, (unsigned)seed_ok, (unsigned)id_ok,
                     (unsigned long long)cat.seed, (unsigned long long)cat.save_id);
        }
    }

    // --- the roster ----------------------------------------------------
    //
    // The one artefact that lets two peers be compared by eye before anything
    // has gone wrong. If the two logs disagree here, nothing downstream is
    // worth debugging. HP and tile are printed alongside so the field offsets
    // can be checked against what is actually on screen -- a state hash built
    // on a wrong offset agrees with itself forever and proves nothing.
    log_line("LOCKSTEP", "battle roster: %u cats, %u human, %u local (%s)",
             g.cat_count, g.humans, local,
             cfg.net_control_auto ? "auto split" : "net_control list");

    // AND THE COMPOSITION, ENTITY BY ENTITY (2026-09-23). The count alone ("22 cats" against
    // "25") says the two peers built different battles but not WHAT differs. With hp/maxhp printed
    // per index, the difference names itself -- measured on the run that halted on rng: one peer
    // had FOUR 3-hp entities where the other had ONE 20-hp entity, i.e. three extra weak spawns,
    // which is the shape of a per-player event or stash leaking into the battle's contents (the
    // open gap recorded at the end of HANDOFF section 4.C). Both lines are read at the same
    // moment -- the roster build -- so they can be compared index by index, and the offsets are
    // the ones the state hash already uses, so a wrong one would show up here first.
    {
        // AND THE CAT'S OWN ID (2026-09-23). The counts and the hp are no longer enough: after
        // the ownership polarity fix the two rosters came down to 25 against 24 -- one entity
        // apart -- and the per-index hp differences are the battle playing out, not the cause.
        // The cause is WHICH entity is present on one side and not the other, and the save id is
        // what names it: an unknown id is an AI spawn, a known one is a cat, and an id that is
        // present at one index on one peer and at a DIFFERENT index on the other is an ordering
        // bug rather than a spawn one. Resolved the same way the ownership table resolves a cat
        // (Character+kChar_CatData -> CatData+kCatData_SaveId), so this line cannot disagree
        // with the split about what a cat is. Four hex digits: the ids differ in their low bits
        // and the line has to stay readable at 25 entries.
        char txt[900] = {};
        int  off = 0;
        for (uint32_t i = 0; i < g.cat_count && off < (int)sizeof(txt) - 32; ++i) {
            int32_t hp = 0, maxhp = 0;
            const uint8_t* c = (const uint8_t*)g.cats[i];
            const bool ok = c && mem_read(c + kChar_HP,    &hp,    sizeof(hp)) &&
                                   mem_read(c + kChar_MaxHP, &maxhp, sizeof(maxhp));
            const void* data = nullptr;
            uint64_t id = 0;
            if (!c || !mem_read(c + kChar_CatData, &data, sizeof(data)) || !data ||
                !mem_read((const uint8_t*)data + kCatData_SaveId, &id, sizeof(id)) || !id)
                id = 0;
            off += _snprintf_s(txt + off, sizeof(txt) - off, _TRUNCATE, "%s%u:%d/%d:%04llx",
                               i ? " " : "", i, ok ? hp : -1, ok ? maxhp : -1,
                               (unsigned long long)(id & 0xFFFF));
        }
        log_line("LOCKSTEP", "roster composition (index:hp/maxhp:savelow16): %s", txt);
    }

    // A cat can already be OUT of the live character list at battle start --
    // dead, or parked off the board at the (-5000,-5000) sentinel. The hash
    // path knows this and stops at membership for such an entry, because every
    // field behind it is heap luck rather than game state. THIS loop used to
    // not know it, and judged a departed cat by the same standard as a live
    // one: the sentinel fails plausible() on tx < -4096, so ONE legitimately
    // departed cat turned the state hash off for the whole battle and blamed
    // "Character offsets do not fit this build" for it.
    //
    // Measured 2026-09-12: a 45-cat fight opened with cat 33 dead at the
    // sentinel, 45/45 linked, 44/45 "in range", state hash off for five turns.
    // Both peers then agreed on rng+queue only -- which the AGREES lines said
    // honestly, and which is exactly the "agreement over unmeasured components"
    // rule 3 warns about.
    //
    // So judge presence the same way the hash does, from the same helper.
    uint32_t live_now = 0, appeared_now = 0;
    bool still_in[kMaxCats];
    const bool have_membership =
        snapshot_membership(g.snapped_list, live_now, still_in, appeared_now);

    uint32_t linked = 0, sane = 0, present_n = 0, departed_n = 0;
    for (uint32_t i = 0; i < g.cat_count; ++i) {
        const char* who = !g.human_cat[i] ? "ai    "
                        : g.local_cat[i]  ? "LOCAL "
                                          : "peer  ";
        const bool present = !have_membership || still_in[i];
        CatState st{};
        bool got = read_cat_state(g.cats[i], st, present);

        if (present) {
            ++present_n;
            if (got && st.linked)     ++linked;
            if (got && plausible(st)) ++sane;
        } else {
            ++departed_n;
        }

        // The entity definition each unit was built from: two peers that built different battles (the roster sizes differ) say which units differ.
        char what[72] = "";
        { const auto df = derived_features(i); _snprintf_s(what, sizeof(what), _TRUNCATE, " type=%s", df.type); }
        if (got)
            log_line("LOCKSTEP", "  cat %2u  %s  hp=%d/%d tile=(%d,%d)%s%s%s%s  brain=%s%s",
                     i, who, st.hp, st.maxhp, st.tx, st.ty,
                     st.dead ? " DEAD" : "",
                     !present  ? " [GONE -- not in the live list]"
                   : off_board(st) ? " [off-board -- elements not read]" : "",
                     (present && !st.linked)     ? " [unlinked]" : "",
                     (present && !plausible(st)) ? " [odd]"      : "", bcls[i], what);
        else
            log_line("LOCKSTEP", "  cat %2u  %s  <state unreadable>%s  brain=%s", i, who,
                     !present ? " [GONE -- not in the live list]" : "", bcls[i]);
    }
    if (departed_n)
        log_line("LOCKSTEP", "  %u of %u snapshot entr%s already out of the live list"
                             " -- hashed by MEMBERSHIP only, and not held against the"
                             " offsets", departed_n, g.cat_count,
                 departed_n == 1 ? "y is" : "ies are");
    if (!have_membership)
        log_line("LOCKSTEP", "  membership unreadable -- every entry judged as present,"
                             " which is the strict reading");

    // --- is the state hash trustworthy on this build? ------------------
    //
    // Gated on evidence rather than on the offsets having been written down
    // correctly. Two failures are caught: the TacticsObject back-pointer not
    // round-tripping (so the tile is being read out of the wrong object), and
    // HP/tile outside any plausible range (so the offsets belong to a different
    // build). Either would have the two peers hashing unrelated bytes -- and
    // since heap layout differs between processes, that is a guaranteed false
    // desync on turn 1, which is strictly worse than no state hash at all.
    //
    // Strict on purpose: every PRESENT cat must pass, not most of them. A
    // partial pass is precisely the ambiguous case, and the roster above marks
    // each failing cat [unlinked] or [odd].
    //
    // Departed entries are excluded from the denominator rather than required
    // to pass, because their fields are not evidence about this build either
    // way -- that is the same judgement the hash makes two hundred lines below,
    // and the two must not disagree about the same cat. A battle where EVERY
    // entry has departed proves nothing, so it does not arm.
    g.state_hash_on = tune::kStateHash && present_n > 0 &&
                      linked == present_n && sane == present_n;
    if (!tune::kStateHash)
        log_line("LOCKSTEP", "state hash disabled by net_state_hash = 0");
    else if (g.state_hash_on)
        log_line("LOCKSTEP", "state hash ON -- all %u live cat(s) linked and in range"
                             " (%u departed, hashed by membership)",
                 present_n, departed_n);
    else
        log_line("LOCKSTEP", "!! state hash OFF -- %u/%u live cats linked, %u/%u in range."
                             " Look for [unlinked] / [odd] in the roster above: those"
                             " Character offsets do not fit this build, and hashing"
                             " them would desync both peers over unrelated bytes",
                 linked, present_n, sane, present_n);

    if (g.humans == 0)
        log_line("LOCKSTEP", "!! no human-driven cats in this battle -- lockstep has"
                             " nothing to split (matched brain '%s')", tune::kReplayBrains);

    // --- publish the split ---------------------------------------------
    //
    // Both peers send their own list, and both check it against the other's.
    // Symmetric rather than host-authoritative because nobody is waiting on it:
    // each side already derived its half locally, so this is purely the check
    // that used to be impossible -- a human cat claimed by NEITHER peer, which
    // otherwise shows up as a battle stalling on that cat's turn with nothing
    // in either log to say why. Making it one-way would put that check in only
    // one of the two logs.
    {
        ControlMsg c{};
        c.battle_id = g.battles.current;
        c.humans = g.humans;
        for (uint32_t i = 0; i < g.cat_count && c.count < 32; ++i)
            if (g.local_cat[i]) {
                c.cats[c.count] = (uint8_t)i;      // where it sits here
                c.fp[c.count]   = g.cat_fp[i];     // and WHAT it is, which is the
                ++c.count;                         // part two peers can agree on
            }
        net_send_control(c);

        // File our own claim in the same table as everyone else's. The coverage
        // check is a tally over all players, and leaving ourselves out of it
        // would make every cat we own look unclaimed.
        uint8_t self = net_self();
        if (self < kMaxPeers) {
            g.peer_control[self]      = c;
            g.have_peer_control[self] = true;
        }
    }

    // The host may already have told us its split before we had a roster to
    // check it against.
    verify_control();
}

// Check the peer's half of the split against ours. Both sides ran the same rule
// over the same roster, so agreement should be a formality -- and when it is
// not, this is the earliest possible warning that the two peers are not in the
// same battle.
//
// Deferred rather than checked on arrival, because each peer reaches its first
// turn boundary whenever it does: CONTROL can land before this side has a
// roster to compare it against, and a check that silently does not run is worse
// than no check at all.
void verify_control() {
    if (g.control_checked || !g.snapped) return;

    // Every member must have spoken for THIS battle before anything can be
    // concluded. With two players that was one message; with four it is three,
    // and checking coverage against a partial set would halt on cats whose
    // owner simply has not reported yet.
    uint8_t ids[kMaxPeers] = {};
    uint8_t count = net_peer_count();
    if (count == 0 || !net_peer_ids(ids, kMaxPeers)) return;

    for (uint8_t i = 0; i < count; ++i) {
        uint8_t id = ids[i];
        if (id >= kMaxPeers) return;
        if (!g.have_peer_control[id]) return;
        // Held until the epochs line up. A peer may have sent its split for a
        // battle we have not reached yet; checking it against THIS battle's
        // roster would compare two different rosters and halt on a difference
        // that is only our own lag.
        if (g.peer_control[id].battle_id != g.battles.current) return;
    }
    g.control_checked = true;

    // Everyone must agree how many human cats there are before their claims
    // mean anything -- a peer counting a different number is looking at a
    // different battle, and its indices are not comparable to ours.
    for (uint8_t i = 0; i < count; ++i) {
        const ControlMsg& c = g.peer_control[ids[i]];
        if (c.humans != g.humans) {
            char why[192];
            _snprintf_s(why, sizeof(why), _TRUNCATE,
                        "roster disagreement: peer %u counted %u human cat(s), we see %u",
                        (unsigned)ids[i], c.humans, g.humans);
            halt(why);
            return;
        }
    }

    // Claim tally. Every human cat must be claimed by EXACTLY ONE player, and
    // both failures are fatal for opposite reasons: claimed twice and two
    // players drive one brain, which desyncs on the first turn it acts; claimed
    // by nobody and it never acts at all, so the battle stalls forever with
    // every peer politely waiting for someone else. The second is the one that
    // used to be invisible, and with more players there are more ways to
    // produce it.
    uint8_t claims[kMaxCats] = {};
    uint8_t owner[kMaxCats];
    for (uint32_t i = 0; i < kMaxCats; ++i) owner[i] = kNoPeer;

    for (uint8_t i = 0; i < count; ++i) {
        const ControlMsg& c = g.peer_control[ids[i]];
        for (uint8_t j = 0; j < c.count; ++j) {
            uint8_t cat = c.cats[j];
            if (cat >= kMaxCats) continue;
            ++claims[cat];
            if (owner[cat] == kNoPeer) owner[cat] = ids[i];
        }
    }

    for (uint32_t i = 0; i < g.cat_count; ++i) {
        if (!g.human_cat[i]) continue;

        // A SUMMON IS NOT CLAIMED ON THE WIRE, BY DESIGN.
        //
        // CONTROL is published once, at the snapshot, and a summon that appeared
        // after it has no entry in anybody's list -- so the tally below would
        // find it "human but claimed by no player" and halt the battle. Its
        // ownership is not negotiated: it is inherited from the actor of the
        // applied action that produced it, which both peers resolve to the same
        // answer from the same action (see adopt_new_cats). Nothing to check
        // here, and a check that fires on correct behaviour would be worse than
        // the silence it replaces.
        if (g.summon[i]) continue;

        if (claims[i] > 1) {
            char why[192];
            _snprintf_s(why, sizeof(why), _TRUNCATE,
                        "cat %u is claimed by %u players -- two people driving one"
                        " brain desyncs on its first turn", i, (unsigned)claims[i]);
            halt(why);
            return;
        }
        if (claims[i] == 0) {
            char why[192];
            _snprintf_s(why, sizeof(why), _TRUNCATE,
                        "cat %u is human but claimed by no player -- its turn"
                        " would stall forever", i);
            halt(why);
            return;
        }
    }

    // --- ARE THE TWO SIDES DESCRIBING THE SAME CATS? -------------------------
    //
    // The tally above counts INDICES, and an index is a POSITION -- which is why
    // it passed on the session that then deadlocked: two peers whose rosters were
    // permutations each claimed two cats, each side read as fully covered, and
    // each was describing different animals. Nothing in `cats` can tell a
    // permutation from a roster that has really diverged; the fingerprint CONTROL
    // now carries is what can, and the two cases need different fixes -- which is
    // the whole reason this check exists.
    bool identity_agrees = true;
    if (g.fp_usable) {
        for (uint8_t i = 0; i < count; ++i) {
            const ControlMsg& c = g.peer_control[ids[i]];
            const uint8_t n = c.count < 32 ? c.count : 32;
            if (!n) continue;

            uint32_t mine[32] = {}, theirs[32] = {};
            for (uint8_t j = 0; j < n; ++j) {
                theirs[j] = c.fp[j];
                const uint8_t cat = c.cats[j];
                mine[j] = cat < kMaxCats ? g.cat_fp[cat] : 0;
            }

            const FpRelation rel = fp_relation(mine, n, theirs, n);
            if (rel == FP_SAME) continue;
            identity_agrees = false;

            char a[160] = {}, b[160] = {};
            int  ao = 0, bo = 0;
            for (uint8_t j = 0; j < n; ++j) {
                if (ao < (int)sizeof(a) - 16)
                    ao += _snprintf_s(a + ao, sizeof(a) - ao, _TRUNCATE, " %u:%08x",
                                      (unsigned)c.cats[j], mine[j]);
                if (bo < (int)sizeof(b) - 16)
                    bo += _snprintf_s(b + bo, sizeof(b) - bo, _TRUNCATE, " %u:%08x",
                                      (unsigned)c.cats[j], theirs[j]);
            }
            log_line("LOCKSTEP", "   peer %u claims (index:fingerprint):%s",
                     (unsigned)ids[i], b);
            log_line("LOCKSTEP", "   this peer has (index:fingerprint):%s", a);

            if (rel == FP_PERMUTED)
                log_line_lvl(LogLevel::Warn, "LOCKSTEP",
                             "!! peer %u and this peer hold the SAME cats in a DIFFERENT "
                             "ORDER. Ownership now agrees anyway -- the split is derived "
                             "from these fingerprints -- but the WIRE is still index-keyed, "
                             "so a decision for one of these cats would land on the wrong "
                             "one. The fix is upstream: make the run's cat list identical "
                             "(or carry identity on the wire).", (unsigned)ids[i]);
            else
                log_line_lvl(LogLevel::Error, "LOCKSTEP",
                             "!! peer %u's claimed cats are NOT the same cats as this "
                             "peer's -- the fingerprints differ as a SET, so the two runs "
                             "have already diverged and no ordering rule can repair it. "
                             "Compare this against the peer's NODEHASH 'cats ... in order' "
                             "line and its CATSYNC 'run cat order' line.",
                             (unsigned)ids[i]);
        }
    }

    for (uint32_t i = 0; i < g.cat_count; ++i) {
        auto& cat = g.level_cats[i];
        cat.owner = identity_agrees && g.human_cat[i] && !g.summon[i] && cat.readable
                  ? owner[i] : kNoPeer;
        if (cat.owner != kNoPeer)
            log_line("CHOICE", "cat owner: save_id=%016llX seed=%016llX slot=%u peer=%u",
                     (unsigned long long)cat.save_id, (unsigned long long)cat.seed,
                     i, (unsigned)cat.owner);
    }

    // Who ended up with what, in one line, because with four players "the split
    // agreed" is no longer enough to picture it.
    char who[192] = {};
    int  off = 0;
    for (uint8_t i = 0; i < count && off < (int)sizeof(who) - 24; ++i) {
        const ControlMsg& c = g.peer_control[ids[i]];
        off += _snprintf_s(who + off, sizeof(who) - off, _TRUNCATE,
                           "%speer %u:%u", i ? "  " : "", (unsigned)ids[i],
                           (unsigned)c.count);
    }
    log_line("LOCKSTEP", "control split agreed across %u player(s): %u human cat(s)"
                         " -- %s", (unsigned)count, g.humans, who);

    // Reaching here is exactly the moment the join barrier opens -- every
    // player has a roster for this battle and they agree about it. Said
    // explicitly, and with the wait, because "the battle started" and "the
    // battle started with everyone in it" are the two things the logs
    // previously could not be used to tell apart.
    if (config().net_join_barrier && !g.barrier_said_open) {
        g.barrier_said_open = true;
        if (g.barrier_waits)
            log_line("LOCKSTEP", "join barrier OPEN for battle %u after %u poll(s)"
                                 " -- all %u player(s) are in this battle",
                     g.epoch, g.barrier_waits, (unsigned)count);
        else
            log_line("LOCKSTEP", "join barrier OPEN for battle %u -- all %u player(s)"
                                 " were already here, nothing was held",
                     g.epoch, (unsigned)count);
    }
}

// --- the join barrier -------------------------------------------------------
//
// Neither peer may decide anything until BOTH have snapshotted this battle's
// roster. Without it, a peer that is not yet in the battle is simply absent
// while the other plays on -- the late-join gap, reproduced 2026-08-24:
//
//   host    -> cat 20 ability slot=1:0 target=(4,3) gon=DefaultMove
//   host    -> cat 20 ability slot=2:0 target=(6,6) gon=BasicMagicShortRanged
//   host    -> cat 20 endturn
//   client  <- cat 20 endturn                     <- the only one it applied
//   client  !! HALT at turn 2: hash mismatch (rng)
//
// The client's turn-2 rng_hash was byte-identical to its own turn 1: its stream
// never advanced, because the move and the spell never ran there.
//
// The queue half of that was fixed by the battle epoch -- pending actions are
// now purged rather than cleared, so nothing is thrown away. This is the other
// half, and it is the one that matters: keeping the actions is no use if the
// peer that missed them has already computed a hash without them. The barrier
// makes "both peers are here" a precondition for anything happening at all,
// rather than something we hope was true.
//
// It costs nothing to wait. Brain::GetChoice is a POLL -- it returns type=1
// ("nothing decided yet") every frame while waiting on a human, and did so for
// 1695 of 1711 calls in one tutorial battle -- so parking a brain is exactly
// the state the game is already in between clicks. No timeout, no blocking
// recv, no frame budget: the same reason the transport needs none.
//
// The condition is already computed for us. verify_control() sets
// control_checked only when this peer has a roster AND the peer's CONTROL for
// the SAME epoch has arrived and agreed -- which is precisely "both of us are
// in this battle, and we concur about who drives what".

// The branches themselves live in mgmp_barrier.h as a pure function, tested by
// tests/test_barrier.cpp; this is only the gather. Same split as td_decide and
// HashRing, and for the same reason -- exercising it in game means arranging
// for two peers to drift apart, which is how the gap was found in the first
// place and is no way to check the edges.
// Collapse "every player has reported" into the two facts barrier_decide takes.
// It stays a two-value question -- have we heard from everyone, and for which
// battle -- so the decision table and its tests are unchanged; what "everyone"
// means is what grew.
//
// The battle reported is ANY member's that differs from ours, because the
// barrier must wait for the peer that is not here yet. Reporting a matching one
// while another peer is elsewhere would open the gate on the strength of a peer
// that is already in step. (With a counter this was "the earliest epoch"; ids
// do not order, so "not ours" is the test, and it is the one that was always
// meant.)
void aggregate_controls(bool& all_in, uint64_t& odd_battle) {
    all_in = false;
    odd_battle = g.battles.current;

    uint8_t ids[kMaxPeers] = {};
    uint8_t count = net_peer_count();
    if (count == 0 || !net_peer_ids(ids, kMaxPeers)) return;

    bool     every = true;
    uint64_t odd   = g.battles.current;
    for (uint8_t i = 0; i < count; ++i) {
        uint8_t id = ids[i];
        if (id >= kMaxPeers || !g.have_peer_control[id]) { every = false; continue; }
        if (g.peer_control[id].battle_id != g.battles.current)
            odd = g.peer_control[id].battle_id;
    }
    all_in = every;
    odd_battle = every ? odd : g.battles.current;
}

const char* barrier_blocking() {
    bool     all_in = false;
    uint64_t peer_battle = 0;
    aggregate_controls(all_in, peer_battle);

    BarrierFacts f;
    f.enabled         = config().net_join_barrier;
    f.control_checked = g.control_checked;
    f.snapped         = g.snapped;
    f.have_peer_ctrl  = all_in;
    f.peer_battle     = peer_battle;
    f.battle          = g.battles.current;
    f.peer_battle_retired = g.battles.is_retired(peer_battle);
    return barrier_decide(f);
}

void barrier_wait_tick(const char* why) {
    if (g.barrier_waits == 0)
        log_line("LOCKSTEP", "join barrier: holding every decision until both peers"
                             " are in battle %u -- %s", g.epoch, why);
    ++g.barrier_waits;
    // ~10 s at 60 fps. Repeated on purpose: a barrier that never opens looks
    // exactly like a hung game, so it has to keep saying which side it is
    // waiting on for as long as it waits.
    if (g.barrier_waits % 600 == 0)
        log_line("LOCKSTEP", "join barrier: still holding after %u poll(s) -- %s",
                 g.barrier_waits, why);
}

// --- battle identity --------------------------------------------------------

// True if the message belongs to a battle we have already LEFT, and must be
// dropped. A message for a battle we have not reached is NOT stale -- it is
// held, which is what lets a peer arrive late without losing what happened
// without it.
//
// Reported once per battle rather than per message: a boundary crossed
// mid-exchange drops a small burst, and one line with a count says everything
// the per-message spam would, without burying the turn hashes around it.
bool stale_battle(uint64_t battle_id, const char* what) {
    if (!g.battles.is_retired(battle_id)) return false;
    ++g.stale_drops;
    if (!g.said_stale) {
        g.said_stale = true;
        log_line("LOCKSTEP", "dropping %s for battle %016llx -- we finished that one"
                             " and are in %016llx now (in flight across the battle"
                             " boundary; expected, and the reason messages carry a"
                             " battle id)",
                 what, (unsigned long long)battle_id,
                 (unsigned long long)g.battles.current);
    }
    return true;
}

// --- the outbound log, for a peer that joins mid-battle ---------------------
//
// Every decision this peer has taken in the CURRENT battle, in order. A peer
// arriving at turn 12 re-enters the same node, so its battle rebuilds
// identically from MapNode+0x118 and its AI re-derives -- but the human
// decisions taken before it arrived exist nowhere except here. Replaying them
// is what lets it fast-forward turns 0..11 instead of playing a different
// battle from turn 0.
//
// They are replayed as ORDINARY ACTION messages: the joining peer's pend_push
// accepts them and pend_take hands them out cat by cat exactly as if they had
// arrived live, so the whole injection path is the one already proven. That is
// why there is no ACTIONLOG message type -- there is nothing for one to say.
void sent_log_push(const ActionMsg& a) {
    if (g.sent_log_n >= kMaxSentLog) {
        if (!g.sent_log_full) {
            g.sent_log_full = true;
            log_line("LOCKSTEP", "!! this battle has run past %u recorded decisions --"
                                 " a peer joining from here cannot be caught up and"
                                 " will be told so rather than desync quietly",
                     kMaxSentLog);
        }
        return;
    }
    g.sent_log[g.sent_log_n++] = a;
}

// --- pending remote actions -------------------------------------------------

void pend_push(const ActionMsg& a) {
    // A decision the PEER took also belongs in the catch-up log. The log has to
    // be the whole battle's history, not this peer's half of it: a joiner
    // rebuilding the fight has to replay what every human did, and a human cat
    // cannot be re-derived the way an AI one can.
    //
    // Logged in ARRIVAL order rather than execution order, which is enough
    // because pend_take resolves per cat and the order within one cat is
    // preserved. Two cats' decisions being swapped relative to each other does
    // not change what either cat does.
    if (a.battle_id == g.battles.current) sent_log_push(a);

    if (g.pend_count >= kMaxPending) {
        log_line("LOCKSTEP", "!! pending queue full -- dropping action for cat %u", a.actor);
        return;
    }
    g.pending[(g.pend_head + g.pend_count) % kMaxPending] = a;
    ++g.pend_count;
}

// Drops queued actions from a battle we have left, keeping this battle's and
// anything already sent for a battle we have NOT reached. Keeping the latter is
// the half that also closes the late-join drop: this queue used to be emptied
// wholesale at the reset, which is what lost a joining peer the actions taken
// before it arrived.
uint32_t pend_purge_retired() {
    // Compacted through a temporary rather than in place. The queue is a RING:
    // once pend_head is non-zero the read cursor wraps, so writing survivors
    // straight to the front would overwrite entries this loop has not read yet.
    // (Only ever called on the game thread, like every other pend_* function,
    // so the buffer needs no guard of its own.)
    static ActionMsg keep[kMaxPending];
    uint32_t kept = 0, dropped = 0;
    for (uint32_t i = 0; i < g.pend_count; ++i) {
        const ActionMsg& a = g.pending[(g.pend_head + i) % kMaxPending];
        if (g.battles.is_retired(a.battle_id)) { ++dropped; continue; }
        keep[kept++] = a;
    }
    for (uint32_t i = 0; i < kept; ++i) g.pending[i] = keep[i];
    g.pend_head  = 0;
    g.pend_count = kept;
    return dropped;
}

// Pops the oldest pending action for `cat`, if any. Actions for different cats
// can legitimately be in flight at once, so this is not a plain FIFO pop.
bool pend_take(uint8_t cat, ActionMsg& out) {
    for (uint32_t i = 0; i < g.pend_count; ++i) {
        uint32_t slot = (g.pend_head + i) % kMaxPending;
        if (g.pending[slot].actor != cat) continue;
        // The battle id is part of the key, not a filter applied afterwards. An
        // action held for the NEXT battle names a cat index that exists in this
        // one too, so matching on the cat alone would inject a decision from
        // the wrong battle -- the same bug battle identity exists to stop, one
        // layer down.
        if (g.pending[slot].battle_id != g.battles.current) continue;
        out = g.pending[slot];
        // Close the gap by shifting the entries behind it forward one.
        for (uint32_t j = i; j + 1 < g.pend_count; ++j) {
            uint32_t a = (g.pend_head + j) % kMaxPending;
            uint32_t b = (g.pend_head + j + 1) % kMaxPending;
            g.pending[a] = g.pending[b];
        }
        --g.pend_count;
        return true;
    }
    return false;
}

// --- hashing ----------------------------------------------------------------

// THE TURN HEARTBEAT (2026-09-23). One stamp per turn, because "the turns stopped" is the
// only reliable way to see the end of a fight from outside: lockstep_in_battle() is false for
// ever once a session has halted, and is_retired() is about battles already behind us, so it
// is never true for the current one. The level-up draw lands a few seconds after the last
// turn, which is what gives a 1.5 s gap its meaning.
static uint64_t g_last_turn_ms = 0;

HashMsg build_hash(void* turn_control) {
    g_last_turn_ms = GetTickCount64();
    HashMsg h{};
    h.battle_id = g.battles.current;
    h.turn  = g.turn;

    // The simulation stream. 32 bytes, and the only RNG state lockstep has to
    // agree on -- TLS+0x198 is presentation and is *meant* to differ (measured:
    // 10x variation between runs C and D while all 44 sim draws stayed
    // byte-identical).
    if (uint64_t* s = rng_global_stream())
        h.rng_hash = fnv1a(s, 32);

    // The decision queue. Cheapest and earliest divergence signal there is:
    // types 6 and 7 exist *because* a passive fired, so a differing proc roll
    // changes the queue population before it changes any visible state, and it
    // costs no serialization of the ~1390 component classes.
    uint32_t depth = 0;
    if (turn_control)
        mem_read((const uint8_t*)turn_control + kTC_QueueCount, &depth, sizeof(depth));
    h.queue_depth = depth;
    h.queue_sig   = (uint32_t)fnv1a(&depth, sizeof(depth));

    // Character state: HP, shield, max HP, tile and the dead flag, in roster
    // order. Only runs when the snapshot's self-check passed -- see there for
    // why a state hash that cannot prove its own offsets is worse than none at
    // all.
    //
    // Status/passive counts are deliberately NOT in here. The Character's
    // passive list at +0xE70 is a lazily rebuilt CACHE with a dirty flag, and
    // anything that enumerates passives -- including a tooltip on one peer and
    // not the other -- can rebuild it. Hashing a cache whose freshness depends
    // on where the mouse is would manufacture desyncs, which is the one failure
    // this hash must not have.
    //
    // FACING IS EXCLUDED FOR THE SAME REASON, and it was measured rather than
    // reasoned. Two peers on different machines halted at turn 5 with identical
    // rng and queue hashes and a state hash differing on exactly one field of
    // one cat: the acting cat's facing, (0,1) against (-1,0), with HP, shield
    // and tile agreeing on all 29 cats. Nothing had rolled or resolved
    // differently -- which is precisely what an identical rng hash means.
    //
    // Character::Face is called from glaiel::Brain::UpdateDecision @ 0x1401377C9
    // under this guard:
    //
    //     cmp   dword ptr [rdi+220h], 2      ; a type-2 decision is cached
    //     jnz   skip
    //     comisd xmm7, qword ptr [rdi+2A8h]  ; the WALL-CLOCK dt timer
    //     jnb   skip                         ; already elapsed -> no Face at all
    //     mov   rdx, [rdi+238h]              ; the pending decision's direction
    //     call  glaiel::Character::Face
    //
    // and the statement immediately above it, under the same two branches, is
    // Brain::DrawAbilityAOE -- a DRAWING call. This whole block is the aim
    // preview: while a decision is held, the cat turns toward where it is being
    // aimed. Whether it turns at all depends on how many frames fit inside the
    // dt window, so a peer at a different frame rate lands on the other side of
    // that `jnb`. Run E measured the same battle at 23,211 frames and 12,230.
    //
    // This never appeared in the loopback test because both instances run on one
    // box at the same frame rate. It is the same class of bug as
    // TimeDelayStatusApplication: wall-clock time reaching something we treat as
    // simulation state.
    //
    // Excluding it is safe rather than merely convenient, and the reason is
    // narrow: every SIMULATION write to facing is made by the action itself --
    // Ability::trigger and Character::DoAction both call Face with the committed
    // direction. The preview write only survives in the gap BETWEEN actions, so
    // dropping it from the hash loses no divergence that an action would not
    // re-establish. Facing is still read and still printed in the halt dump,
    // where it costs nothing and occasionally explains one.
    if (g.state_hash_on) {
        uint64_t sh = 1469598103934665603ULL;

        // Which snapshot entries are still in the battle. A cat the battle has
        // dropped keeps its slot in our frozen snapshot but its object is no
        // longer meaningfully owned, so every field we could read off it is
        // heap luck rather than game state. Hash the MEMBERSHIP -- so two peers
        // that disagree about whether a cat left still diverge -- and then stop,
        // because nothing after that point is trustworthy on either side.
        uint32_t live_h = 0, appeared_h = 0;
        bool still_h[kMaxCats];
        const uint8_t memb =
            snapshot_membership(g.snapped_list, live_h, still_h, appeared_h) ? 1 : 0;
        // Hashed too: a peer that can read membership and one that cannot are
        // computing different things, and that must not pass as agreement.
        sh = fnv1a(&memb, sizeof(memb), sh);

        for (uint32_t i = 0; i < g.cat_count; ++i) {
            const bool present = !memb || still_h[i];
            const uint8_t in_battle = present ? 1 : 0;
            sh = fnv1a(&in_battle, sizeof(in_battle), sh);

            CatState st{};
            uint8_t got = read_cat_state(g.cats[i], st, present) ? 1 : 0;
            sh = fnv1a(&got, sizeof(got), sh);

            // Keep the row for the desync dump. Written here rather than
            // re-read later because HERE is the instant the hash describes;
            // `readable` and `in_battle` ride along because a peer that could
            // read a cat the other could not is a difference in its own right.
            st.in_battle = in_battle;
            st.readable  = got;
            g.hashed[i]  = st;

            // Everything below reads the object itself, which a departed entry
            // no longer backs. Membership above already carries the divergence
            // that matters.
            if (!present) continue;
            // Field by field, skipping fx/fy. Deliberately not a memcpy of a
            // trimmed struct: the next person to add a field should have to
            // decide which side of this line it goes on.
            sh = fnv1a(&st.hp,     sizeof(st.hp),     sh);
            sh = fnv1a(&st.shield, sizeof(st.shield), sh);
            sh = fnv1a(&st.maxhp,  sizeof(st.maxhp),  sh);
            sh = fnv1a(&st.tx,     sizeof(st.tx),     sh);
            sh = fnv1a(&st.ty,     sizeof(st.ty),     sh);
            sh = fnv1a(&st.dead,   sizeof(st.dead),   sh);
            sh = fnv1a(&st.linked, sizeof(st.linked), sh);
            // The elements affecting this cat -- passives, statuses, equipment
            // and, decisively, the TILE it is standing on. Everything else in
            // this hash is a per-character field, so grid state has been
            // invisible to it from the start: two peers whose battlefield is
            // wet in different places agree on every cat until a Fire ability
            // resolves differently, and then disagree by a few HP with an
            // IDENTICAL rng hash, because nothing rolled.
            //
            // This also drags equipment into the hash for the first time, which
            // is the gap that let a mismatched trinket survive to turn 4.
            sh = fnv1a(&st.e0,    sizeof(st.e0),    sh);
            sh = fnv1a(&st.e1,    sizeof(st.e1),    sh);
            sh = fnv1a(&st.elems, sizeof(st.elems), sh);
        }
        h.state_hash = sh;
        g.hashed_count = g.cat_count;
        g.hashed_turn  = h.turn;
        g.hashed_valid = true;
    }
    return h;
}

void halt(const char* why) {
    if (g.halted) return;
    g.halted = true;
    g.halted_in_battle = true;
    g.stats.halted = true;
    ++g.stats.desyncs;
    log_line("LOCKSTEP", "!! HALT at turn %u: %s", g.turn, why);
    strncpy_s(g.halt_reason, sizeof(g.halt_reason), why, _TRUNCATE);
    log_line_lvl(LogLevel::Error, "HALT", "HALT RECORD battle %016llx turn %u (%llu ms into the battle) role=%s sim=%u proto=%u stage '%s': %s", (unsigned long long)g.battles.current, g.turn,
                 (unsigned long long)(g.bs.t0 ? GetTickCount64() - g.bs.t0 : 0), net_role() == NetRole::Host ? "host" : "client", (unsigned)tune::kSimRevision, (unsigned)kProtoVersion, log_stage_last(), why);
    trail_dump("this peer halts");
    log_battle_stats("halted", true);
    lockstep_share_log("this peer halted the battle");

    HaltMsg m{};
    m.turn = g.turn;
    _snprintf_s(m.reason, sizeof(m.reason), _TRUNCATE, "%s", why);
    net_send_halt(m);
}

// --- the desync dump --------------------------------------------------------
//
// Two halves of one idea: put the table this peer hashed on the wire, and print
// a real field-by-field diff when the other peer's arrives. Sent only after a
// hash has already disagreed, so it costs nothing in a healthy session.

void send_state_dump(uint32_t turn) {
    if (g.dump_sent) return;

    if (!g.hashed_valid) {
        log_line("LOCKSTEP", "   (no state dump: the state hash is off, so there "
                             "is no per-cat table to send -- only rng and queue "
                             "were ever compared)");
        g.dump_sent = true;
        return;
    }
    // Refusing here rather than sending the live table is the point. A dump
    // labelled turn N that actually holds turn N+1's numbers would produce a
    // diff full of differences that were never in the hash, and it would look
    // exactly like a real one.
    if (g.hashed_turn != turn) {
        log_line("LOCKSTEP", "   (no state dump: our table is from turn %u and the "
                             "mismatch is at turn %u -- sending it would diff two "
                             "different instants)", g.hashed_turn, turn);
        g.dump_sent = true;
        return;
    }
    if (g.hashed_count == 0 || g.hashed_count > kMaxDumpCats) return;

    StateDumpMsg m{};
    m.battle_id = g.battles.current;
    m.turn      = turn;
    m.count     = g.hashed_count;
    m.stride    = (uint32_t)sizeof(CatState);
    m.size      = m.count * m.stride;
    m.data      = (uint8_t*)g.hashed;
    g.dump_sent = net_send_statedump(m);
}

// Prints one line per cat whose row differs, and nothing at all for the rows
// that agree -- which on a real desync is almost all of them.
void compare_state_dump(const StateDumpMsg& m) {
    if (g.dump_seen) return;

    if (m.stride != sizeof(CatState)) {
        log_line_lvl(LogLevel::Error, "LOCKSTEP",
                     "!! the peer's state dump packs %u bytes per cat and this "
                     "build packs %u -- NOT diffing it. The two peers are running "
                     "different revisions of the record, which is a bigger "
                     "problem than the desync.",
                     m.stride, (unsigned)sizeof(CatState));
        g.dump_seen = true;
        return;
    }
    if (!g.hashed_valid || g.hashed_turn != m.turn) {
        log_line("LOCKSTEP", "   (the peer's turn-%u state dump arrived but our own "
                             "table is from turn %u -- not diffing two different "
                             "instants)", m.turn, g.hashed_turn);
        g.dump_seen = true;
        return;
    }
    g.dump_seen = true;

    if (m.count != g.hashed_count) {
        // Not a reason to stop: the overlap still diffs, and a roster length
        // difference is itself the most useful line in the dump.
        log_line_lvl(LogLevel::Error, "LOCKSTEP",
                     "!! ROSTER SIZE: this peer snapshotted %u cats and the peer "
                     "snapshotted %u. The two are not playing the same battle.",
                     g.hashed_count, m.count);
    }

    const CatState* theirs = (const CatState*)m.data;
    const uint32_t  n = (m.count < g.hashed_count) ? m.count : g.hashed_count;

    log_line("LOCKSTEP", "STATE DIFF at turn %u (this peer | the peer), %u cats "
                         "compared:", m.turn, n);

    uint32_t differ = 0, unrepairable = 0, first_unrep = 0xFFFFu;
    char unrep_why[160] = {};
    char diff_summary[400] = {};
    for (uint32_t i = 0; i < n; ++i) {
        const CatState& a = g.hashed[i];
        const CatState& b = theirs[i];

        // Field by field, and facing is INCLUDED here even though the hash
        // excludes it. That exclusion is about what may halt a run; it is not a
        // reason to hide the field once the run has already halted -- and
        // facing is precisely what explained the 49-turn backstab desync.
        char line[320];
        int  k = 0;
        // _snprintf_s returns -1 on truncation, so the length is re-measured
        // rather than accumulated: a negative return added to `k` would index
        // backwards out of the buffer.
        #define MGMP_DIFF(fmt, fa, fb)                                          \
            if ((fa) != (fb) && k < (int)sizeof(line) - 48) {                    \
                _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, fmt, fa, fb); \
                k = (int)strlen(line);                                           \
            }

        line[0] = 0;
        MGMP_DIFF(" hp %d|%d",        a.hp, b.hp);
        MGMP_DIFF(" shield %d|%d",    a.shield, b.shield);
        MGMP_DIFF(" maxhp %d|%d",     a.maxhp, b.maxhp);
        MGMP_DIFF(" dead %u|%u",      (unsigned)a.dead, (unsigned)b.dead);
        MGMP_DIFF(" elem0 %08X|%08X", a.e0, b.e0);
        MGMP_DIFF(" elem1 %08X|%08X", a.e1, b.e1);
        MGMP_DIFF(" in_battle %u|%u", (unsigned)a.in_battle, (unsigned)b.in_battle);
        MGMP_DIFF(" readable %u|%u",  (unsigned)a.readable,  (unsigned)b.readable);
        MGMP_DIFF(" linked %u|%u",    (unsigned)a.linked,    (unsigned)b.linked);
        MGMP_DIFF(" elems %u|%u",     (unsigned)a.elems,     (unsigned)b.elems);
        #undef MGMP_DIFF

        if ((a.tx != b.tx || a.ty != b.ty) && k < (int)sizeof(line) - 48) {
            _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE,
                        " tile (%d,%d)|(%d,%d)", a.tx, a.ty, b.tx, b.ty);
            k = (int)strlen(line);
        }
        if ((a.fx != b.fx || a.fy != b.fy) && k < (int)sizeof(line) - 48) {
            _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE,
                        " FACE (%d,%d)|(%d,%d) [not hashed]",
                        a.fx, a.fy, b.fx, b.fy);
            k = (int)strlen(line);
        }

        if (!line[0]) continue;
        ++differ;
        log_line_lvl(LogLevel::Error, "LOCKSTEP", "  cat %2u %s", i, line);
        {   // what the host's board puts right: hit points, shield, tile, the dead flag, presence; for a PLAYER's cat not the max hp (an input). Elements (statuses, gear), link and readability: nothing does.
            const bool human = i < g.cat_count && g.human_cat[i];
            const bool no = a.e0 != b.e0 || a.e1 != b.e1 || a.elems != b.elems || a.linked != b.linked || a.readable != b.readable || (human && a.maxhp != b.maxhp);
            if (no) { ++unrepairable; if (first_unrep == 0xFFFFu) { first_unrep = i; _snprintf_s(unrep_why, sizeof(unrep_why), _TRUNCATE, "cat %u%s:%s", i, human ? " (a player's)" : "", line); } }
        }
        {   // the first few, on the one REPORT line
            const size_t used = strlen(diff_summary);
            if (used < sizeof(diff_summary) - 64) _snprintf_s(diff_summary + used, sizeof(diff_summary) - used, _TRUNCATE, " [cat %u:%s]", i, line);
        }
    }

    if (differ == 0) {
        // Worth its own loud line. Every hashed field agreeing while the hash
        // did not means the divergence is somewhere the state hash does not
        // look: the rng stream, the queue, or state no cat row carries.
        log_line_lvl(LogLevel::Warn, "LOCKSTEP",
                     "  every cat row AGREES. Whatever diverged is not in the "
                     "cat table -- look at the rng and queue halves of the hash, "
                     "and at the summon tail below, which the snapshot does not "
                     "cover.");
    } else {
        log_line("LOCKSTEP", "  %u of %u cat(s) differ. Diff the two peers' "
                             "APPLY/DOACTION/TRIGGER lines for the first one, "
                             "back from this turn.", differ, n);
    }
    log_line_lvl(LogLevel::Error, "LOCKSTEP", "!! DESYNC REPORT battle %016llx turn %u diff: %u of %u cat row(s) differ, %u of them NOT repairable by the host's board%s%s", (unsigned long long)g.battles.current, m.turn, differ, n,
                 unrepairable, diff_summary[0] ? " --" : "", diff_summary);
    if (unrepairable && g.debounce_turn == m.turn && !g.halted) {
        // THE DEBOUNCE ENDS HERE (2026-10-04, b521766e: a player's summon differed in hit points and tile, the board does not repair a player's cat, so the next turn was played on a diverged
        // state and halted on the stream anyway). Waiting a boundary only helps what the board repairs.
        char why2[192];
        _snprintf_s(why2, sizeof(why2), _TRUNCATE, "turn %u: a difference the host's board cannot repair (%s)", m.turn, unrep_why);
        log_line_lvl(LogLevel::Error, "LOCKSTEP", "!! DESYNC REPORT battle %016llx turn %u: the debounce is OVER -- %s", (unsigned long long)g.battles.current, m.turn, why2);
        halt(why2);
    }
}

// THE OTHER PLAYER'S LOG TAIL, written into this one (proto 76). Each line keeps the sender's own seq and turn, and the tag PEERLOG, so a reader can
// tell the two sides apart and line them up; PEERLOG lines are never shared back.
void write_peer_log(uint8_t from, const PeerLogMsg& m) {
    if (!m.data || !m.size) return;
    log_line_lvl(LogLevel::Warn, "PEERLOG", "===== BEGIN the log tail of peer %u (%s; its battle %016llx, turn %u, %u bytes) =====",
                 (unsigned)from, m.why, (unsigned long long)m.battle_id, m.turn, m.size);
    const char* p = (const char*)m.data;
    const char* end = p + m.size;
    uint32_t lines = 0;
    while (p < end) {
        const char* nl = (const char*)memchr(p, '\n', (size_t)(end - p));
        const size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (n) { log_line_lvl(LogLevel::Trace, "PEERLOG", "p%u| %.*s", (unsigned)from, (int)(n > 400 ? 400 : n), p); ++lines; }
        p += n + 1;
    }
    log_line_lvl(LogLevel::Warn, "PEERLOG", "===== END the log tail of peer %u (%u lines) =====", (unsigned)from, lines);
}

// --- the turn trail ----------------------------------------------------------------------------------------------------------------------------
void trail_record(const HashMsg& mine, int32_t actor) {
    State::Trail& t = g.trail[g.trail_n % 16];
    t = State::Trail{};
    t.turn = mine.turn; t.actor = actor; t.rng = mine.rng_hash; t.state = mine.state_hash;
    t.ms = g.bs.t0 ? (uint32_t)(GetTickCount64() - g.bs.t0) : 0;
    ++g.trail_n;
}
void trail_verdict(uint32_t turn, uint8_t verdict) {
    for (uint32_t k = 0; k < 16 && k < g.trail_n; ++k) {
        State::Trail& t = g.trail[(g.trail_n - 1 - k) % 16];
        if (t.turn == turn) { t.verdict = verdict; return; }
    }
}
void trail_dump(const char* why) {
    log_line("TRAIL", "the last turns of battle %016llx before this (%s) -- turn:actor rng state verdict:", (unsigned long long)g.battles.current, why);
    const uint32_t n = g.trail_n < 16 ? g.trail_n : 16;
    for (uint32_t k = n; k-- > 0;) {
        const State::Trail& t = g.trail[(g.trail_n - 1 - k) % 16];
        log_line("TRAIL", "  turn %3u actor %3d rng %08x state %08x at %u ms  %s", t.turn, (int)t.actor, (unsigned)t.rng, (unsigned)t.state, t.ms,
                 t.verdict == 1 ? "agreed" : (t.verdict == 2 ? "MISMATCH" : "(not compared yet)"));
    }
}

// --- the pre-battle audit ------------------------------------------------------------------------------------------------------------------------
void audit_compare() {
    if (g.bs.audit_done || !g.bs.audit_sent || !g.have_audit_peer || g.audit_peer.battle_id != g.audit_mine.battle_id) return;
    g.bs.audit_done = true;
    uint32_t agree = 0, differ = 0, only_here = 0, only_there = 0;
    for (uint32_t i = 0; i < g.audit_mine.n; ++i) {
        const AuditCat& a = g.audit_mine.cat[i];
        const AuditCat* b = nullptr;
        for (uint32_t j = 0; j < g.audit_peer.n; ++j) if (g.audit_peer.cat[j].id == a.id) b = &g.audit_peer.cat[j];
        if (!b) { ++only_here; log_line_lvl(LogLevel::Warn, "AUDIT", "!! PREBATTLE AUDIT: cat %016llx is on this peer's side only: %s", (unsigned long long)a.id, a.text); continue; }
        if (a.fp == b->fp) { ++agree; continue; }
        ++differ;
        log_line_lvl(LogLevel::Error, "AUDIT", "!! PREBATTLE AUDIT: cat %016llx DIFFERS between the peers (before any turn was played):", (unsigned long long)a.id);
        log_line_lvl(LogLevel::Error, "AUDIT", "     this peer: %s", a.text);
        log_line_lvl(LogLevel::Error, "AUDIT", "     the peer : %s", b->text);
    }
    for (uint32_t j = 0; j < g.audit_peer.n; ++j) {
        bool found = false;
        for (uint32_t i = 0; i < g.audit_mine.n; ++i) if (g.audit_mine.cat[i].id == g.audit_peer.cat[j].id) found = true;
        if (!found) { ++only_there; log_line_lvl(LogLevel::Warn, "AUDIT", "!! PREBATTLE AUDIT: cat %016llx is on the peer's side only: %s", (unsigned long long)g.audit_peer.cat[j].id, g.audit_peer.cat[j].text); }
    }
    g.bs.audit_cats = agree + differ;
    g.bs.audit_differ = differ + only_here + only_there;
    log_line_lvl(g.bs.audit_differ ? LogLevel::Error : LogLevel::Good, "AUDIT", "PREBATTLE AUDIT battle %016llx: %u player cat(s) compared -- %u agree, %u DIFFER, %u only here, %u only there",
                 (unsigned long long)g.audit_mine.battle_id, agree + differ, agree, differ, only_here, only_there);
    if (g.bs.audit_differ) lockstep_share_log("a pre-battle audit difference");
}

// Once per battle, at its first turn boundary and BEFORE the host's board is taken: this peer's player cats, to the others. The cats are read off the characters' cat data.
void audit_send() {
    if (g.bs.audit_sent || !net_active() || net_peer_count() < 2 || g.battles.current == kNoBattle) return;
    g.bs.audit_sent = true;
    AuditMsg m{};
    m.battle_id = g.battles.current;
    for (uint32_t i = 0; i < g.cat_count && m.n < kAuditMaxCats; ++i) {
        if (!g.human_cat[i] || !g.cats[i]) continue;
        const void* data = nullptr;
        if (!mem_read((const uint8_t*)g.cats[i] + kChar_CatData, &data, sizeof(data)) || !data) continue;
        AuditCat& c = m.cat[m.n];
        if (!unlocks_describe_cat(const_cast<void*>(data), c.id, c.text, sizeof(c.text), c.fp)) continue;
        ++m.n;
    }
    g.audit_mine = m;
    const bool ok = net_send_audit(m);
    log_line("AUDIT", "PREBATTLE AUDIT: %u player cat(s) of battle %016llx described to the other player(s)%s", (unsigned)m.n, (unsigned long long)m.battle_id, ok ? "" : " -- NOT SENT");
    for (uint32_t i = 0; i < m.n; ++i) log_line("AUDIT", "  cat %016llx fp %016llx: %s", (unsigned long long)m.cat[i].id, (unsigned long long)m.cat[i].fp, m.cat[i].text);
    audit_compare();
}

// The upload offer (lockstep_take_desync_notice): raised once per battle at its first mismatch, debounced or not, and taken by the menu's frame.
volatile LONG g_desync_notice = 0;
char g_desync_text[192] = {};

void raise_desync_notice(const char* text) {
    if (g.desync_noticed) return;
    g.desync_noticed = true;
    strncpy_s(g_desync_text, sizeof(g_desync_text), text, _TRUNCATE);
    InterlockedExchange(&g_desync_notice, 1);
}

void compare_hash(const HashMsg& mine, const HashMsg& theirs) {
    if (mine.turn != theirs.turn) return;
    if (mine.rng_hash == theirs.rng_hash &&
        mine.queue_depth == theirs.queue_depth &&
        mine.state_hash == theirs.state_hash)
        return;

    // Say which half disagreed, because the two mean different things. RNG or
    // queue means the simulations took different draws -- a real divergence. A
    // state-only miss with the stream still in step is the suspicious one: it
    // is what a wrong field offset looks like, so dump the per-cat table on
    // both peers and let a diff of the two logs name the cat and the field.
    bool rng_ok   = (mine.rng_hash == theirs.rng_hash);
    bool queue_ok = (mine.queue_depth == theirs.queue_depth);

    char why[192];
    _snprintf_s(why, sizeof(why), _TRUNCATE,
                "turn %u hash mismatch (%s%s%s): rng %016llx/%016llx queue %u/%u state %016llx/%016llx",
                mine.turn,
                rng_ok ? "" : "rng ", queue_ok ? "" : "queue ",
                (rng_ok && queue_ok) ? "state only" : "",
                (unsigned long long)mine.rng_hash, (unsigned long long)theirs.rng_hash,
                mine.queue_depth, theirs.queue_depth,
                (unsigned long long)mine.state_hash, (unsigned long long)theirs.state_hash);
    ++g.mismatches;
    if (!g.diverged) { g.diverged = true; g.first_mismatch = mine.turn; }

    // Both peers publish the table their own hash was taken over, whichever
    // half disagreed. It used to go out only for a state-only mismatch, on the
    // reasoning that rng or queue means "a real divergence" and the cats would
    // not add anything -- but "the streams differ" is a symptom, and the cat
    // that took different damage two turns ago is what caused it. The dump is
    // free here: it is sent once, after the run is already lost.
    send_state_dump(mine.turn);

    // --- halting (the default), debounced ---------------------------------
    if (config().net_desync_halt) {
        const bool state_only = rng_ok && queue_ok;
        const bool repeat = g.debounce_turn != ~0u && mine.turn == g.debounce_turn + 1;
        const bool lenient = tune::kDesyncDebounce && state_only && !repeat;
        // ONE LINE PER MISMATCH, whatever happens next, in a fixed shape a script can collect: it is what a debounced desync leaves behind to be fixed later.
        log_line_lvl(LogLevel::Error, "LOCKSTEP", "!! DESYNC REPORT battle %016llx turn %u kind=%s action=%s (#%u this battle): %s",
                     (unsigned long long)g.battles.current, mine.turn, state_only ? "state" : (!rng_ok && !queue_ok ? "rng+queue" : (!rng_ok ? "rng" : "queue")),
                     lenient ? "DEBOUNCED" : (state_only ? "HALT (state differed at two consecutive boundaries)" : "HALT (the simulations drew differently)"), g.mismatches, why);
        if (state_only) dump_cat_states(lenient ? "state-only mismatch (DEBOUNCED -- the next boundary must agree)" : "state-only mismatch");
        char note[192];
        _snprintf_s(note, sizeof(note), _TRUNCATE, "battle %016llx turn %u: %s mismatch, %s", (unsigned long long)g.battles.current, mine.turn,
                    state_only ? "state" : "rng/queue", lenient ? "debounced" : "halted");
        raise_desync_notice(note);
        if (lenient) {
            g.debounce_turn = mine.turn;
            ++g.debounced;
            ++g.stats.desyncs;
            lockstep_share_log("a debounced desync");
            log_line_lvl(LogLevel::Warn, "LOCKSTEP", "!! DESYNC DEBOUNCED at turn %u (#%u this battle): only the state hash differs and the host's board of the next boundary repairs "
                         "what it carries -- NOT halting. If turn %u disagrees as well, it halts. The STATE DIFF below names the cat(s) and field(s): this is a real "
                         "difference between the peers and wants a fix even when it heals.", mine.turn, g.debounced, mine.turn + 1);
            return;
        }
        halt(why);
        return;
    }

    // --- reporting only (net_desync_halt = 0) ----------------------------
    //
    // Still counted as a desync -- the run is not clean and the summary must
    // not pretend otherwise -- but the simulation continues so that the next
    // few boundaries can say whether this was a divergence or a skew.
    ++g.stats.desyncs;

    // The first two get the whole table; after that it would be the same 45
    // lines with one number moving, which is what the delta trace is for.
    const bool worth_dumping = (rng_ok && queue_ok && g.mismatches <= 2);
    if (worth_dumping)
        dump_cat_states("state-only mismatch (NOT halting -- net_desync_halt = 0)");

    log_line("LOCKSTEP", "!! MISMATCH #%u at turn %u (not halting): %s",
             g.mismatches, mine.turn, why);
    if (!worth_dumping && rng_ok && queue_ok)
        log_line("LOCKSTEP", "   (cat table suppressed after the first two --"
                             " read the `delta:` lines instead)");
}

// Reports one matched pair, either way. The AGREES line is not decoration: a
// log that speaks only on mismatch cannot be told apart from a log whose check
// never ran, and telling those two apart is the entire point of this fix.
void report_pair(const HashMsg& mine, const HashMsg& theirs, uint8_t peer) {
    if (mine.rng_hash == theirs.rng_hash &&
        mine.queue_depth == theirs.queue_depth &&
        mine.state_hash == theirs.state_hash) {
        ++g.agreements;
        ++g.bs.agreed;
        trail_verdict(mine.turn, 1);
        // Trace, not Info: this is one line per turn per peer, and in a healthy
        // session it is the single loudest thing in the panel -- 59 of them in
        // one measured run, which is what buries the lines a player needs to
        // see. It stays in the FILE, where it is the evidence the comment above
        // describes; the panel shows the count instead, in the summary and on
        // the session window's `turn` row.
        log_line_lvl(LogLevel::Trace, "LOCKSTEP",
                 "turn %u AGREES with peer %u%s", mine.turn,
                 (unsigned)peer,
                 g.state_hash_on ? " (rng+queue+state)" : " (rng+queue)");

        // The line net_desync_halt exists to print. Two peers that agree again
        // after a mismatch did not have a divergence: they had an effect land
        // on opposite sides of a turn boundary, which is a timing artefact and
        // is fixed somewhere completely different from a simulation bug. Said
        // once, at the moment it re-converges, because that is the moment the
        // evidence exists -- and a halting run never reaches it.
        if (g.diverged && g.debounce_turn != ~0u && mine.turn == g.debounce_turn + 1) {
            log_line_lvl(LogLevel::Warn, "LOCKSTEP", "!! DESYNC REPORT battle %016llx turn %u: the debounced turn-%u mismatch HEALED -- the peers agree again. The difference was real "
                         "(see the STATE DIFF at turn %u); the board covered it this time.", (unsigned long long)g.battles.current, mine.turn, g.debounce_turn, g.debounce_turn);
            g.dump_sent = false;      // a later mismatch of this battle gets its own table exchange
            g.dump_seen = false;
        }
        if (g.diverged) {
            g.diverged = false;
            log_line("LOCKSTEP", "   ^^ TRANSIENT: the turn-%u mismatch has"
                                 " RE-CONVERGED by turn %u without either peer"
                                 " being corrected. State that diverges and comes"
                                 " back is timing, not simulation -- something"
                                 " landed on opposite sides of the boundary."
                                 " Diff the two peers' `delta:` lines around"
                                 " turn %u to see what.",
                     g.first_mismatch, mine.turn, g.first_mismatch);
        }
        return;
    }
    log_line("LOCKSTEP", "!! the disagreement is with peer %u", (unsigned)peer);
    ++g.bs.mismatched;
    trail_verdict(mine.turn, 2);
    trail_dump("a turn hash disagrees");
    compare_hash(mine, theirs);
}

} // namespace

// ---------------------------------------------------------------------------

// --- THE OWNERSHIP NOTES (2026-09-23, 4+4 step B) --------------------------------
//
// Deliberately a small file-static table rather than a field of State: it is told by
// another module (mgmp_catsync) and read by the split, it never needs to survive a
// session, and keeping it here makes the whole mechanism visible in one place.
//
// ABSOLUTE POSITIONS, NOT "MINE/THEIRS". A cat noted as position 1 is position 1 on
// BOTH peers -- that is what makes an order derived from this table identical on both,
// which is the single property the control split depends on. A peer that noted its own
// cats as "mine" would produce a mirrored order and hand each player the other's cats.
namespace {
constexpr uint32_t kMaxOwnerNotes = 64;
struct OwnerNote { uint64_t save_id = 0; uint8_t owner_pos = 0; };
OwnerNote g_owner_note[kMaxOwnerNotes] = {};
uint32_t  g_owner_note_count = 0;
} // namespace

void lockstep_note_owner(uint64_t save_id, uint8_t owner_pos) {
    if (!save_id) return;

    for (uint32_t i = 0; i < g_owner_note_count; ++i) {
        if (g_owner_note[i].save_id != save_id) continue;
        if (g_owner_note[i].owner_pos == owner_pos) return;      // already known, silent
        g_owner_note[i].owner_pos = owner_pos;
        log_line_lvl(LogLevel::Warn, "LOCKSTEP",
                     "!! cat %016llx ownership CORRECTED to session position %u -- the"
                     " split order must agree on both peers, so a correction is worth"
                     " seeing before it moves anybody's cats",
                     (unsigned long long)save_id, owner_pos);
        return;
    }

    if (g_owner_note_count >= kMaxOwnerNotes) {
        log_line_lvl(LogLevel::Warn, "LOCKSTEP",
                     "!! the ownership table is FULL (%u) -- this note for cat %016llx is"
                     " DROPPED. A split that needs a complete table will fall back to"
                     " the fingerprint order rather than use a partial one.",
                     kMaxOwnerNotes, (unsigned long long)save_id);
        return;
    }
    g_owner_note[g_owner_note_count].save_id   = save_id;
    g_owner_note[g_owner_note_count].owner_pos = owner_pos;
    ++g_owner_note_count;
    log_line("LOCKSTEP", "owner note: cat %016llx is session position %u (%u note(s) held)",
             (unsigned long long)save_id, owner_pos, g_owner_note_count);
}

bool lockstep_owner_pos(uint64_t save_id, uint8_t& owner_pos) {
    for (uint32_t i = 0; i < g_owner_note_count; ++i) {
        if (g_owner_note[i].save_id != save_id) continue;
        owner_pos = g_owner_note[i].owner_pos;
        return true;
    }
    return false;
}

// Journal persistence (see the header note, 2026-09-28). Export/import are
// field copies, not struct punning, so the on-disk layout stays (u64, u8) and
// the in-memory struct layout can never drift from it.
uint32_t lockstep_owner_note_export(LockstepOwnerNote* out, uint32_t max) {
    if (!out || !max) return 0;
    const uint32_t n = g_owner_note_count < max ? g_owner_note_count : max;
    for (uint32_t i = 0; i < n; ++i) {
        out[i].save_id   = g_owner_note[i].save_id;
        out[i].owner_pos = g_owner_note[i].owner_pos;
    }
    return n;
}

void lockstep_owner_note_import(const LockstepOwnerNote* in, uint32_t count) {
    if (!in) return;
    for (uint32_t i = 0; i < count; ++i)
        if (in[i].save_id) lockstep_note_owner(in[i].save_id, in[i].owner_pos);
}

void lockstep_set_base(uintptr_t base) {
    g.affecting_elements = nullptr;

    const uintptr_t addr = addr_of_call(C_AffectingElements);
    if (!addr) {
        // Not fatal. Losing this costs detection, not correctness: the hash
        // falls back to exactly what it covered before, which is the state the
        // last several sessions ran on.
        log_line("LOCKSTEP", "!! %s did not resolve by signature -- "
                             "element state will NOT be hashed",
                 kCalls[C_AffectingElements].name);
        return;
    }
    g.affecting_elements = (fn_affecting_elements)addr;
}

void lockstep_init() {
    if (g.active) return;
    if (!g.cs_ready) { InitializeCriticalSection(&g.cs); g.cs_ready = true; }

    // IS THIS A FRESH SESSION, OR A RECONNECT INTO A BATTLE WE NEVER LEFT?
    //
    // A peer that only lost its socket is still standing in the same battle, in
    // the same process, holding the same live Character objects -- pointers are
    // stable within one battle, and this snapshot describes the fight that is
    // still on screen. Wiping it is not "starting clean", it is throwing away
    // the only description of where we are.
    //
    // And wiping it DEADLOCKS, which is how this was found. The chain, measured
    // on 2026-08-26 (host log 021012 / client 021026):
    //
    //   1. reconnect -> go_ready -> lockstep_init -> snapped = false
    //   2. lockstep_fill_choice opens with `if (!g.snapped) return false`, so
    //      every cat -- ours and the peer's -- falls through to the local brain
    //   3. the peer's cats then wait for a local click that is never coming,
    //      and our own cats never consult the 4 decisions the host replayed
    //   4. no decision -> the turn never ends -> TurnControl::NextTurn never
    //      fires -> snapshot_cats, which ONLY runs from the turn boundary,
    //      never runs. Nothing can ever set snapped again.
    //
    // The client's log named it exactly: "0 sent, 0 applied, 4 still pending".
    // The catch-up worked; there was no roster to spend it against.
    //
    // So: keep what describes the BATTLE, reset what describes the SESSION.
    //
    // `snapped` stays set between battles -- it is only cleared when a turn
    // boundary sees a DIFFERENT character list -- so this also fires for a peer
    // that reconnects while standing on the map, preserving a roster that
    // describes the fight that just ended. That is deliberate and harmless:
    // nothing takes a human decision outside a battle, and the first turn
    // boundary of the next battle resets the roster, the turn, the control
    // check and the barrier together. It is the same self-correction that
    // already covers a stale roster today, so it needs no second mechanism.
    const bool rejoin_in_battle = g.snapped && g.snapped_list != nullptr;

    g.active  = true;
    g.halted  = false;
    if (!rejoin_in_battle) {
        g.snapped = false;
        // Turn numbering is per battle and the HASH messages are keyed on it.
        // Resetting to 0 while standing on turn 2 would compare our turn 2
        // against the peer's turn 0 for the rest of the fight.
        g.turn = 0;
    }
    for (uint32_t p = 0; p < kMaxPeers; ++p) {
        g.peer_hash[p].clear();
        g.peer_hash_full_warned[p] = false;
    }
    g.my_hash.clear();
    g.epoch             = 0;
    g.stale_drops       = 0;
    g.said_stale        = false;
    g.local_actor       = false;
    g.state_hash_on     = false;

    // The control split and the barrier go with the battle, not the session,
    // for the same reason the roster does -- and for one more.
    //
    // The barrier opens when both peers have exchanged CONTROL for this battle.
    // Clearing that on a reconnect makes the returning peer wait for a CONTROL
    // the host will never send: the host stayed Ready throughout, so its own
    // control_checked is still set and verify_control returns early forever.
    // The rejoining peer would sit at a barrier nobody can open -- a stall with
    // nothing in either log to explain it, which is precisely the failure the
    // barrier's "no disconnect timeout" note accepts as the better one and
    // therefore must not be manufactured here.
    //
    // The split is a property of (this battle, this membership) and neither
    // changed: the peer that came back is the same peer, at the same index.
    // If membership DID change while we were away the split is stale until the
    // next battle re-derives it -- acceptable, and no worse than the general
    // rule that an explicit net_control list is stale the moment you fight
    // anything else.
    if (!rejoin_in_battle) {
        g.humans          = 0;
        g.control_checked = false;
        for (uint32_t p = 0; p < kMaxPeers; ++p) g.have_peer_control[p] = false;
        g.barrier_said_open = false;
    }
    g.barrier_waits     = 0;
    g.have_prev         = false;
    g.mismatches        = 0;
    g.first_mismatch    = 0;
    g.diverged          = false;
    g.debounce_turn     = ~0u;
    g.debounced         = 0;
    g.desync_noticed    = false;
    g.bs = State::BStats{};
    g.trail_n = 0;
    g.have_audit_peer = false;
    // Per SESSION, not per battle: lockstep_init has exactly one caller,
    // go_ready. Reset here so a reconnect that compares nothing before the run
    // ends reports that honestly instead of inheriting the previous session's
    // agreements -- which is the design notes' open item 2, the case where `0
    // desync(s)` was never backed by a single comparison.
    g.agreements        = 0;
    // Trace unless the barrier is off, in which case the warning is the point.
    log_line_lvl(config().net_join_barrier ? LogLevel::Trace : LogLevel::Warn,
             "LOCKSTEP", "armed (%s)%s",
             net_role() == NetRole::Host ? "host" : "client",
             config().net_join_barrier ? "" : " -- join barrier DISABLED by mgmp.json:"
             " a peer that is not yet in a battle will silently miss the turns"
             " taken without it");
    if (rejoin_in_battle)
        log_line("LOCKSTEP", "re-armed INSIDE battle %016llx at turn %u -- kept the "
                             "roster (%u cat(s)), the control split and the barrier; "
                             "this peer never left the fight",
                 (unsigned long long)g.battles.current, g.turn, g.cat_count);
}

void lockstep_shutdown() {
    if (!g.active) return;
    LockstepStats s = lockstep_stats();

    // `0 desync(s)` is a pass ONLY if something was compared. A session that
    // never reached a shared turn boundary produces the identical number and
    // means nothing by it -- so the agreement count is printed beside it and
    // decides the severity. See the design notes rule 3.
    const LogLevel lvl = s.desyncs      ? LogLevel::Error
                       : !g.agreements  ? LogLevel::Warn
                                        : LogLevel::Trace;
    log_line_lvl(lvl, "LOCKSTEP",
             "done: %u sent, %u applied, %u still pending, %u desync(s)%s",
             s.sent, s.applied, s.pending, s.desyncs,
             g.agreements ? "" : " and NO TURN HASH WAS EVER COMPARED -- this run "
                                 "says nothing about whether the battles stayed "
                                 "in sync");
    if (g.agreements)
        log_line_lvl(s.desyncs ? LogLevel::Error : LogLevel::Trace, "LOCKSTEP",
                 "   %u turn hash(es) compared and agreed", g.agreements);
    g.active = false;
    if (g.cs_ready) { DeleteCriticalSection(&g.cs); g.cs_ready = false; }
}

bool lockstep_active() { return g.active && net_active(); }
bool lockstep_halted() { return g.halted; }
bool lockstep_in_battle() { return g.active && g.snapped && !g.halted; }
bool lockstep_battle_ready() { return g.active && g.snapped && g.battles.current != kNoBattle && g.snapshot_battle == g.battles.current; }

// See the header: the victory-screen moment, readable without a screen, a button, or a live
// session -- which is what makes it usable where lockstep_in_battle() is not.
bool lockstep_battle_retired() {
    return g.battles.current != 0 && g.battles.is_retired(g.battles.current);
}

// See the header note beside the declaration: the turn stream is the signal that survives a
// halt and means the same thing on both peers.
uint64_t lockstep_last_turn_ms() { return g_last_turn_ms; }

// See the header. During a battle the character list resolves; at the victory screen it is gone
// (the probe file's own note: the roster probe "disarms when the holder is released"). That is
// the difference between "the player is thinking" and "the fight is over" -- the two states the
// turn heartbeat alone could not tell apart.
bool lockstep_battle_list_gone() {
    if (!g.snapped && !g.tc) return false;      // no battle was entered at all
    return resolve_char_list(g.tc) == nullptr;
}

bool lockstep_fight_up() {
    return g.snapped && g.snapped_list && g.tc && resolve_char_list(g.tc) == g.snapped_list;
}

bool lockstep_live_health(uint64_t save_id, int32_t& hp, int32_t& maxhp) {
    if (!save_id || !g.active || !g.snapped || g.halted || !g.snapped_list ||
        resolve_char_list(g.tc) != g.snapped_list) return false;
    for (uint32_t i = 0; i < g.cat_count; ++i) {
        const void* data = nullptr;
        uint64_t id = 0;
        if (!g.cats[i] || !mem_read((const uint8_t*)g.cats[i] + kChar_CatData, &data, sizeof(data)) || !data ||
            !mem_read((const uint8_t*)data + kCatData_SaveId, &id, sizeof(id)) || id != save_id) continue;
        int32_t h = 0, m = 0;
        if (!mem_read((const uint8_t*)g.cats[i] + kChar_HP,    &h, sizeof(h)) ||
            !mem_read((const uint8_t*)g.cats[i] + kChar_MaxHP, &m, sizeof(m)) ||
            m <= 0 || m > 100000 || h < -100000 || h > 100000) return false;
        hp = h < 0 ? 0 : h; maxhp = m;
        return true;
    }
    return false;
}

// One question with three answers, because mgmp_aim needs all three and asking
// them separately would mean three lookups of the same character and three
// chances to disagree about the answer.
//
// The three-way shape is the control split's, deliberately: AI cats are decided
// locally on BOTH peers, so they are neither published nor drawn. Treating one
// as remote would paint a preview over a cat that is about to move on its own.
bool lockstep_aim_subject(const void* character, uint8_t& cat, bool& peer_owns) {
    if (!g.active || !g.snapped || !character) return false;
    const uint8_t i = cat_index_of(character);
    if (i == kNoCat)     return false;   // a summon: in nobody's roster
    if (!g.human_cat[i]) return false;   // an AI cat: both peers drive it
    cat       = i;
    peer_owns = !g.local_cat[i];
    return true;
}

// --- the aim preview's write to facing --------------------------------------
//
// Brain::UpdateDecision turns the acting cat toward where it is being aimed
// while a decision is held, and it does it by calling Character::Face -- the
// same durable write the committed action makes. See the PREVIEWFACE target in
// mgmp_addresses.h for the guard it sits under; the short version is that
// whether the turn happens at all depends on a WALL-CLOCK dt timer, so two
// peers at different frame rates end up with different facings.
//
// That was known and was thought to be cosmetic. It is not: sub_14011C6F0, the
// backstab test, is called from Character::receive_damage_passives and reads
// Character+0x388 against the direction the hit came from. A cat facing its
// attacker takes a normal hit; one facing away takes a backstab. So the preview
// reaches the simulation through DAMAGE, and it reaches it on the DEFENDER --
// which is why the old argument ("the preview write only survives in the gap
// between actions, and the next action re-establishes it") was wrong. The gap
// between a cat's own actions is exactly when it gets attacked.
//
// The fix is to let the preview run and then put the field back. Nothing else
// inside UpdateDecision writes facing -- Character::Face is called from it
// exactly once -- and the committed direction is written by Ability::trigger
// and Character::DoAction, which are outside this function. So the cat no
// longer visibly turns while you aim it, and every facing either peer ever
// reads is one an action put there.
struct PreviewFacing {
    uint8_t* ch = nullptr;
    int32_t  fx = 0, fy = 0;
    bool     have = false;
    uint32_t restored = 0;
    bool     said = false;
};
PreviewFacing g_pf;

void lockstep_preview_facing_begin(void* brain) {
    g_pf.have = false;
    // Only inside a net session: a solo game has nobody to diverge from, and
    // the preview is a real piece of feedback to the player.
    if (!brain || !lockstep_active()) return;

    uint8_t* ch = nullptr;
    if (!mem_read((const uint8_t*)brain + kBrain_Character, &ch, sizeof(ch)) || !ch)
        return;
    if (!mem_read(ch + kChar_Facing,     &g_pf.fx, sizeof(g_pf.fx))) return;
    if (!mem_read(ch + kChar_Facing + 4, &g_pf.fy, sizeof(g_pf.fy))) return;
    g_pf.ch   = ch;
    g_pf.have = true;
}

void lockstep_preview_facing_end() {
    if (!g_pf.have) return;
    g_pf.have = false;

    int32_t fx = 0, fy = 0;
    if (!mem_read(g_pf.ch + kChar_Facing,     &fx, sizeof(fx))) return;
    if (!mem_read(g_pf.ch + kChar_Facing + 4, &fy, sizeof(fy))) return;
    if (fx == g_pf.fx && fy == g_pf.fy) return;

    if (!mem_write(g_pf.ch + kChar_Facing,     &g_pf.fx, sizeof(g_pf.fx))) return;
    if (!mem_write(g_pf.ch + kChar_Facing + 4, &g_pf.fy, sizeof(g_pf.fy))) return;
    ++g_pf.restored;

    // Once per session. The count is what matters, not each event -- this fires
    // on most frames of a held decision.
    if (!g_pf.said) {
        g_pf.said = true;
        log_line("LOCKSTEP", "the aim preview turned a cat (%p: (%d,%d) -> (%d,%d)) "
                             "and was put back -- facing is read by the backstab "
                             "test, so it must come from actions only",
                 (void*)g_pf.ch, fx, fy, g_pf.fx, g_pf.fy);
    }
}

uint32_t lockstep_preview_facing_count() { return g_pf.restored; }

// THE OWNER'S FINAL FACING OF A CAT THAT ENDED ITS TURN (proto 72/74). apply_remote writes it before the turn ends, and something later put the old one back (2026-10-03, local run: "facing
// taken" at the end-turn decision, and the turn-boundary delta still read the old facing on this peer). Whatever it is, the value is kept here and written again after Character::EndTurn
// and once more at the turn boundary, before the state is read; each time it had to be put back is logged, naming where, so the culprit shows up.
struct PendingFace { uint8_t* ch = nullptr; int32_t fx = 0, fy = 0; bool on = false; };
PendingFace g_face_pend[4];
unsigned g_face_next = 0;

void face_pend_set(uint8_t* ch, int32_t fx, int32_t fy) {
    for (PendingFace& p : g_face_pend) if (p.on && p.ch == ch) { p.fx = fx; p.fy = fy; return; }
    PendingFace& p = g_face_pend[g_face_next++ % 4];
    p.ch = ch; p.fx = fx; p.fy = fy; p.on = true;
}

// Turns a cat the way the game does: Character::Face writes the facing AND plays the turn animation, and every game caller passes (dir, 0, 0). Writing the field alone leaves the logic right and the
// sprite looking the old way (2026-10-03). The field is written only when Face did not leave it as asked. True when it was Face that did it.
bool turn_cat_via_face(void* ch, int32_t dx, int32_t dy) {
    bool via_face = false;
    if (const uintptr_t face = addr_of_call(C_CharacterFace)) {
        using FaceFn = void(__fastcall*)(void*, uint64_t, bool, bool);
        ((FaceFn)face)(ch, (uint64_t)(uint32_t)dx | ((uint64_t)(uint32_t)dy << 32), false, false);
        int32_t nx = 0, ny = 0;
        mem_read((const uint8_t*)ch + kChar_Facing, &nx, sizeof(nx)); mem_read((const uint8_t*)ch + kChar_Facing + 4, &ny, sizeof(ny));
        via_face = (nx == dx && ny == dy);
    }
    if (!via_face) {
        mem_write((uint8_t*)ch + kChar_Facing, &dx, sizeof(dx));
        mem_write((uint8_t*)ch + kChar_Facing + 4, &dy, sizeof(dy));
    }
    return via_face;
}

void face_pend_apply(const char* when, const void* only) {
    uint32_t live_f = 0, appeared_f = 0;
    bool still_f[kMaxCats];
    const bool have_f = snapshot_membership(g.snapped_list, live_f, still_f, appeared_f);
    for (PendingFace& p : g_face_pend) {
        if (!p.on || (only && p.ch != only)) continue;
        // a cat that left the battle since (it can die of its own end-of-turn damage) is a freed object: nothing is written into it
        if (have_f) {
            bool gone = false;
            for (uint32_t j = 0; j < g.cat_count && j < kMaxCats; ++j)
                if (entry_is(j, p.ch)) { gone = !still_f[j]; break; }
            if (gone) { p.on = false; continue; }
        }
        int32_t fx = 0, fy = 0;
        if (!mem_read(p.ch + kChar_Facing, &fx, sizeof(fx)) || !mem_read(p.ch + kChar_Facing + 4, &fy, sizeof(fy))) { p.on = false; continue; }
        if (fx == p.fx && fy == p.fy) continue;
        mem_write(p.ch + kChar_Facing, &p.fx, sizeof(p.fx));
        mem_write(p.ch + kChar_Facing + 4, &p.fy, sizeof(p.fy));
        log_line("LOCKSTEP", "!! the owner's facing (%d,%d) of cat %p had been changed to (%d,%d) by the time of %s -- written again", p.fx, p.fy, (void*)p.ch, fx, fy, when);
    }
}

void lockstep_after_endturn(void* self) {
    if (self) face_pend_apply("Character::EndTurn", self);
}

// --- the state fence --------------------------------------------------------
//
// See mgmp_lockstep.h. The reason this exists rather than a facing-only guard:
// the call it wraps was justified once by "it only draws", and that claim was
// wrong in a way no fence then in place could see. A fence that watches only
// the field you already know about would repeat the mistake, so this one takes
// the whole cat state -- the same struct the per-turn hash uses.

struct StateFence {
    CatState snap[kMaxCats]{};
    bool     got[kMaxCats]{};
    bool     live[kMaxCats]{};   // whether the game was CALLED for this cat (see fence_read)
    uint32_t count = 0;
    bool     armed = false;
    uint32_t hits  = 0;     // cats that moved, ever, this session
    bool     said  = false;
};

StateFence g_sf;

// ONE CAT'S STATE FOR THE FENCE -- AND THE GAME IS ONLY CALLED FOR A CAT THAT IS ALIVE AND STILL IN THE BATTLE (2026-10-03, the host crash: a call through a null function pointer, 1951 caught access
// violations before it). The fence used to read every snapshot cat with the default in_battle = true, so it called Character::get_affecting_elements on units the game had already removed from the
// live list and freed -- on every highlight redraw, begin and end, about a dozen faulting calls per redraw in a fight with dead maggots. That function is not a read: it first flushes the character's
// deferred status changes (counters at +0xEB4/+0xE74, list compaction, deleting status objects), i.e. it WRITES into the character, and into freed memory that by now holds something else. The crash
// was the game calling a virtual on a 632-byte status object that had been freed while a character still pointed at it. The per-turn hash already skips departed cats (`present`); the fence did not.
// Local only (nothing here is sent), so skipping the call for a dead cat cannot make two peers disagree.
bool fence_read(const void* chr, bool present, CatState& out, bool& called) {
    called = false;
    if (!read_cat_state(chr, out, false)) return false;     // plain guarded reads, no call into the game
    if (!present || out.dead || out.hp <= 0 || !out.linked || off_board(out)) return true;
    uint32_t el[2] = { 0, 0 };
    out.in_battle = 1;
    called = true;
    if (read_affecting_elements(chr, el)) { out.e0 = el[0]; out.e1 = el[1]; out.elems = 1; }
    return true;
}

void lockstep_state_fence_begin() {
    g_sf.armed = false;
    if (!g.active || !g.snapped || g.halted) return;
    g_sf.count = g.cat_count > kMaxCats ? kMaxCats : g.cat_count;
    uint32_t live_now = 0, appeared_now = 0;
    bool still_in[kMaxCats];
    const bool memb = snapshot_membership(g.snapped_list, live_now, still_in, appeared_now);
    for (uint32_t i = 0; i < g_sf.count; ++i) {
        // Membership unknown = nothing is called: a missed elemental change is far cheaper than a call on a freed character.
        const bool present = memb && still_in[i];
        g_sf.got[i] = fence_read(g.cats[i], present, g_sf.snap[i], g_sf.live[i]);
    }
    g_sf.armed = true;
}

uint32_t lockstep_state_fence_end(const char* what) {
    if (!g_sf.armed) return 0;
    g_sf.armed = false;

    uint32_t moved = 0;
    for (uint32_t i = 0; i < g_sf.count; ++i) {
        if (!g_sf.got[i]) continue;
        CatState now{};
        bool called = false;
        if (!fence_read(g.cats[i], g_sf.live[i], now, called)) continue;
        if (called != g_sf.live[i]) { now.e0 = g_sf.snap[i].e0; now.e1 = g_sf.snap[i].e1; }   // died during the call: no elem comparison

        const CatState& was = g_sf.snap[i];
        const bool face_moved = (now.fx != was.fx || now.fy != was.fy);
        // memcmp would fold facing in with the rest; compare the fields that
        // cannot be put back separately, so the log can say which kind it was.
        const bool rest_moved =
            now.hp   != was.hp   || now.shield != was.shield ||
            now.maxhp!= was.maxhp|| now.tx     != was.tx     ||
            now.ty   != was.ty   || now.dead   != was.dead   ||
            now.e0   != was.e0   || now.e1     != was.e1;

        if (!face_moved && !rest_moved) continue;
        ++moved;

        if (face_moved) {
            // Putting it back is correct here for the same reason it is correct
            // in the aim-preview freeze: the only facing either peer may read
            // is the one an action wrote.
            mem_write((uint8_t*)g.cats[i] + kChar_Facing,     &was.fx, sizeof(was.fx));
            mem_write((uint8_t*)g.cats[i] + kChar_Facing + 4, &was.fy, sizeof(was.fy));
        }

        if (!g_sf.said) {
            g_sf.said = true;
            log_line_lvl(rest_moved ? LogLevel::Error : LogLevel::Warn, "LOCKSTEP",
                         "!! %s moved cat %u's state: face (%d,%d)->(%d,%d)%s "
                         "hp %d->%d tile (%d,%d)->(%d,%d) dead %u->%u. Facing is "
                         "put back; the rest is NOT, because a value invented "
                         "here would hide a divergence rather than fix one.",
                     what, i, was.fx, was.fy, now.fx, now.fy,
                     face_moved ? " (restored)" : "",
                     was.hp, now.hp, was.tx, was.ty, now.tx, now.ty,
                     (unsigned)was.dead, (unsigned)now.dead);
        }
    }
    g_sf.hits += moved;
    return moved;
}

uint32_t lockstep_state_fence_hits() { return g_sf.hits; }

static void debug_hits_pump();
static void halt_finish_pump();

void lockstep_pump() {
    if (!g.active) return;
    debug_hits_pump();
    halt_finish_pump();

    NetMsg m{};
    while (net_poll(m)) {
        Guard guard;
        switch (m.type) {
            case MSG_ACTION:
                if (stale_battle(m.action.battle_id, "an action")) break;
                // SAY WHAT ARRIVED, AND WHEN (2026-09-23). The one remaining divergence in an
                // otherwise agreeing battle is a DOUBLED ACTION: the same actor and the same
                // ability class applied twice in one turn, with two different Ability pointers,
                // and it is the peer whose log shows it that then diverges -- its rng moves,
                // its turn-0 state hash differs, and its two scene behaviours play twice. The
                // type-7 half of each pair is local by construction (dec_action refuses 6 and 7
                // at the edge, exactly so that a reaction cannot double-fire), so the question is
                // whether the type-2 half arrived from the peer AND was produced locally as well.
                // This line puts every wire decision in the log with its turn and cat, so the
                // APPLY lines can be read against them instead of against a theory.
                log_line_lvl(LogLevel::Warn, "LOCKSTEP",
                             "!! wire action from peer %u: battle=%016llX turn=%u actor=%u"
                             " type=%u slot=%u/%u target=(%d,%d) dir=(%d,%d) '%s'",
                             (unsigned)m.from, (unsigned long long)m.action.battle_id,
                             (unsigned)m.action.turn, (unsigned)m.action.actor,
                             (unsigned)m.action.type, (unsigned)m.action.slot_kind,
                             (unsigned)m.action.slot_index, m.action.tx, m.action.ty,
                             m.action.dx, m.action.dy, m.action.gon);
                pend_push(m.action);
                break;

            case MSG_HASH: {
                if (g.halted) break;
                if (stale_battle(m.hash.battle_id, "a hash")) break;

                // Match against a turn we have ALREADY passed. Without this the
                // check is one-sided: comparison used to happen only at our own
                // turn boundary against hashes that had already arrived, so the
                // peer running AHEAD found an empty ring every time and compared
                // nothing. Measured -- a clean 4-turn run had the client print
                // four AGREES and the host none. A desync would have halted the
                // trailing peer while the leading one played on.
                if (m.from >= kMaxPeers) break;
                if (const HashMsg* mine = g.my_hash.find(m.hash.battle_id, m.hash.turn)) {
                    report_pair(*mine, m.hash, m.from);
                    break;
                }

                // Otherwise that peer is ahead of us; hold it until we get there.
                if (!g.peer_hash[m.from].push_refusing(m.hash) &&
                    !g.peer_hash_full_warned[m.from]) {
                    // Dropping the NEWEST keeps the held turns contiguous from
                    // where we stand, which is what the boundary consumes. Said
                    // once, because the alternative -- what this code did before
                    // -- was to discard peer hashes forever in silence.
                    g.peer_hash_full_warned[m.from] = true;
                    log_line("LOCKSTEP", "!! peer %u is more than %u turns ahead; "
                                         "dropping its hash for turn %u and any "
                                         "further ones -- those turns go unchecked",
                             (unsigned)m.from, kHashRing, m.hash.turn);
                }
                break;
            }

            // The map layer's message rides the same queue: net_poll drains
            // one queue, so exactly one place may pump it.
            case MSG_ENTERNODE:
                if (m.from != kHostPeer) break;
                follow_on_message(m.enter_node);
                break;

            // Also not epoch-gated: a decision-screen choice belongs to a map
            // node, not to a battle, and the epoch counter does not advance on
            // an event node at all.
            case MSG_CHOICE:
                choice_on_message(m.from, m.choice);
                break;

            case MSG_SAVEFILE:
                savefile_on_message(m.savefile);
                break;

            case MSG_CHECKPOINT:
                checkpoint_on_message(m.from, m.checkpoint);
                break;

            case MSG_SAVEWAIT:
                checkpoint_on_savewait(m.from, m.savewait);
                break;

            case MSG_ABANDON:
                abandon_on_message(m.from, m.abandon);
                break;

            case MSG_ROOMCTL:
                room_on_message(m.from, m.roomctl);
                break;

            // The fresh-session map sync can land here too when a frame spans
            // the transition into Ready; the handler is a no-op unless a fresh
            // select is awaiting it.
            case MSG_CHAPTERMAP:
                checkpoint_on_chaptermap(m.from, m.chaptermap);
                break;

            // The chapter map's node seeds (host -> clients): only stored here, applied on the client's next ready map tick.
            case MSG_MAPSEEDS:
                follow_on_mapseeds(m.from, m.mapseeds);
                break;

            case MSG_UNLOCKS:
                unlocks_on_message(m.from, m.unlocks);
                break;

            case MSG_PROPS:
                unlocks_props_on_message(m.from, m.props);
                break;

            case MSG_UQD:
                unlockq_on_peer(m.from, m.uqd);
                break;

            case MSG_RNGL:
                rngl_on_peer(m.from, m.rngl);
                break;

            case MSG_CHAT:
                chat_on_message(m.from, m.chat);
                break;

            case MSG_DEEP:
                deep_on_peer(m.from, m.deep);
                break;

            // Not battle-gated either: leaving the run is the one thing the
            // host does that no other message reports, and it is true
            // whatever this peer happens to be in the middle of.
            case MSG_HOSTLEFT:
                leave_on_message(m.hostleft);
                break;

            // Cosmetic, and not gated on anything: where a player is standing is
            // true whatever this peer is in the middle of.
            case MSG_PAGE:
                page_on_message(m.from, m.page.page);
                break;

            case MSG_CATS:
                catview_on_message(m.from, m.cats);
                break;

            // Not epoch-gated, and deliberately so: a cat is RUN state, not
            // battle state. It stays true across a battle boundary, so a late
            // one is still correct -- unlike an ACTION, which means nothing
            // outside the battle it was taken in.
            case MSG_CATDATA:
                if (follow_hold_state(m)) break;
                catsync_on_message(m.catdata);
                break;

            // Run state as well, and not epoch-gated for the same reason.
            case MSG_INVENTORY:
                if (follow_hold_state(m)) break;
                invsync_on_message(m.inventory);
                break;

            // Run state too: the used-event list is true across a battle
            // boundary, so a late one is still correct.
            case MSG_RUNHIST:
                if (follow_hold_state(m)) break;
                runhist_on_message(m.runhist);
                break;

            // Pure detection, and about a map node rather than a battle -- so
            // it is neither battle-gated nor stale-counted here. It carries its
            // own node seed and mgmp_nodehash matches on that.
            case MSG_NODEHASH:
                nodehash_on_message(m.from, m.nodehash);
                break;

            // Same shape as NODEHASH: every peer authors one, nothing is applied
            // here. catsync stores it and compares on its own map tick.
            case MSG_CATDIGEST:
                catsync_on_digest(m.from, m.catdigest);
                break;

            // Cosmetic, so it is neither epoch-gated nor stale-counted here.
            // The overlay compares epochs itself, at the moment it would draw,
            // and hides a cursor that belongs to another board instead of
            // discarding the message -- a peer one battle ahead comes back into
            // view the instant we catch up, with no gap to re-fill.
            case MSG_CURSOR:
                cursor_on_message(m.from, m.cursor);
                overlay_on_message(m.from, m.cursor);
                break;

            // Same class as CURSOR exactly: cosmetic, not epoch-gated here.
            // mgmp_aim checks the battle id itself, at the moment it would
            // draw, for the same reason the overlay does.
            case MSG_AIM:
                aim_on_message(m.aim);
                break;

            // DEBUG ONLY, and it writes hp directly -- see MSG_DEBUGHIT for why it
            // is broadcast rather than local. The battle-id check is in the handler.
            case MSG_DEBUGHIT:
                lockstep_on_debug_hit(m.from, m.debughit);
                break;

            // Not a battle message at all, but this is where the queue is drained
            // and the handler decides for itself what to do with the two kinds.
            case MSG_PARTY:
                roster_on_party(m.from, m.party);
                break;

            case MSG_CHAPTER:
                setup_on_chapter_message(m.chapter, m.from);
                break;

            case MSG_CHAPTERSEED:
                setup_on_chapterseed_message(m.chapterseed, m.from);
                break;

            case MSG_SETUP:
                setup_on_message(m.setup, m.from);
                break;

            case MSG_CONTROL:
                // A CONTROL for the NEXT battle is kept, not acted on: it is
                // held until our own epoch catches up, and verify_control is
                // what gates that. Dropping it would leave the split unchecked
                // for a whole battle, which is precisely the failure CONTROL
                // was added to make visible.
                if (stale_battle(m.control.battle_id, "a control split")) break;
                if (m.from < kMaxPeers) {
                    g.peer_control[m.from]      = m.control;
                    g.have_peer_control[m.from] = true;
                } else {
                    log_line("LOCKSTEP", "!! control split from peer %u, which is "
                                         "outside the peer table -- ignored",
                             (unsigned)m.from);
                }
                verify_control();
                break;

            // Deliberately NOT gated on g.halted. This is the one message that
            // has to be processed after a halt, because it only ever arrives
            // after one -- and the peer's HALT usually beats it here.
            case MSG_STATEDUMP:
                if (stale_battle(m.statedump.battle_id, "a state dump")) break;
                compare_state_dump(m.statedump);
                // Answer in kind, so the peer that halted first also gets a
                // diff in its own log. send_state_dump is idempotent.
                send_state_dump(m.statedump.turn);
                break;

            case MSG_HALT:
                if (!g.halted && g.halt_cleared_for && g.halt_cleared_for == g.battles.current) {
                    log_line("LOCKSTEP", "a HALT from the other player for battle %016llx arrived after this peer's halt of it was already lifted -- ignored", (unsigned long long)g.battles.current);
                } else if (!g.halted) {
                    g.halted = true;
                    g.halted_in_battle = true;
                    g.stats.halted = true;
                    ++g.stats.desyncs;
                    log_line("LOCKSTEP", "!! peer halted at turn %u: %s",
                             m.halt.turn, m.halt.reason);
                    _snprintf_s(g.halt_reason, sizeof(g.halt_reason), _TRUNCATE, "the other player halted: %s", m.halt.reason);
                    lockstep_share_log("the other player halted the battle");
                }
                break;

            // The other player's log tail (proto 76), written into this log so either one is enough.
            case MSG_PEERLOG:
                write_peer_log(m.from, m.peerlog);
                break;

            case MSG_AUDIT:
                g.audit_peer = m.audit; g.have_audit_peer = true;
                audit_compare();
                break;

            case MSG_REFUSE:
                log_line("LOCKSTEP", "!! peer refused: %s", m.refuse);
                break;

            // A peer joining while we are already playing. This has to be
            // handled HERE as well as in session_update, because session_update
            // returns early once this peer is Ready and lockstep_pump becomes
            // the only thing draining the queue. Falling into `default` below
            // is what silently dropped the second client's handshake: it
            // connected, received PEERS from the net layer, and was then never
            // accepted, welcomed or refused by anyone.
            case MSG_HELLO:
                session_on_hello(m.from, m.hello);
                break;

            default:
                break;
        }
        // MSG_SAVEFILE is the one frame that owns memory. Releasing every frame
        // unconditionally -- including the ones this switch ignored -- keeps
        // that fact in one place instead of one place per case.
        net_msg_release(m);
    }
}

// Defined just below, after its first caller.
static void apply_remote(const ActionMsg& msg, const void* actor, void* out);

bool lockstep_roll_resolve(uint8_t site, double chance, double luck, bool local) {
    if (!g.active || g.halted || !g.snapped || net_peer_count() < 2) return local;
    const uint64_t battle = g.battles.current;
    if (!battle) return local;
    static uint64_t s_battle = 0;
    static uint32_t s_seq[8] = {};
    if (s_battle != battle) { s_battle = battle; memset(s_seq, 0, sizeof(s_seq)); }
    const uint32_t seq = ++s_seq[site & 7];
    if (net_role() == NetRole::Host) {
        RollMsg m{};
        m.battle = battle; m.turn = g.turn; m.seq = seq; m.site = site; m.result = local ? 1 : 0; m.chance = (float)chance; m.luck = (float)luck;
        net_send_roll(m);
        log_line("ROLL", "site %u #%u (turn %u): chance %.6f luck %.6f -> %s (sent to the clients)", (unsigned)site, (unsigned)seq, (unsigned)g.turn, chance, luck, local ? "yes" : "no");
        return local;
    }
    RollMsg h{};
    const ULONGLONG t0 = GetTickCount64();
    while (!net_host_roll(battle, seq, site, h) && GetTickCount64() - t0 < 3000) Sleep(2);
    if (!net_host_roll(battle, seq, site, h)) {
        log_line_lvl(LogLevel::Warn, "ROLL", "!! site %u #%u: the host's answer did not arrive in 3 s -- this peer's own (%s) is used", (unsigned)site, (unsigned)seq, local ? "yes" : "no");
        return local;
    }
    const bool theirs = h.result != 0;
    if (theirs != local || (float)chance != h.chance || (float)luck != h.luck)
        log_line_lvl(LogLevel::Warn, "ROLL", "!! site %u #%u (turn %u): this peer chance %.6f luck %.6f -> %s, the host's chance %.6f luck %.6f -> %s -- the host's answer is used",
                     (unsigned)site, (unsigned)seq, (unsigned)g.turn, chance, luck, local ? "yes" : "no", (double)h.chance, (double)h.luck, theirs ? "yes" : "no");
    else
        log_line("ROLL", "site %u #%u (turn %u): chance %.6f luck %.6f -> %s (the host's agrees)", (unsigned)site, (unsigned)seq, (unsigned)g.turn, chance, luck, local ? "yes" : "no");
    return theirs;
}

bool lockstep_fill_choice(void* brain, void* out) {
    if (!g.active || !brain || !out) return false;
    if (g.halted) {
        // Freeze both peers rather than let one run on. Forcing type=1 parks
        // the game in its normal "waiting for a decision" state, which is a
        // far kinder failure than a crash or a silently diverged battle.
        uint32_t none = TA_None;
        mem_write(out, &none, sizeof(none));
        return true;
    }

    if (!g.snapped) return false;

    const void* actor = nullptr;
    if (!mem_read((const uint8_t*)brain + kBrain_Owner, &actor, sizeof(actor)) || !actor)
        return false;

    uint8_t cat = cat_index_of(actor);
    // Recorded before the AI and barrier branches below, so it tracks every
    // turn rather than only the ones this function goes on to act upon. A
    // summon (kNoCat) is nobody's, same as an AI cat.
    g.local_actor = (cat != kNoCat) && g.local_cat[cat];
    if (cat == kNoCat) return false;      // a summon: not in the snapshot, so
                                          // neither peer treats it as owned and
                                          // both let their own AI drive it.

    // An AI cat is decided locally on BOTH peers -- never sent, never injected,
    // never suppressed. Getting this wrong is a deadlock, not a desync: a cat
    // that neither side sends for is a cat both sides sit and wait for.
    //
    // "Never suppressed" includes the join barrier below, and that is NOT an
    // oversight -- it is the whole reason the barrier works. Measured
    // 2026-08-24, and it cost a run to learn:
    //
    //   host:    RangedAttackAbility target=(3,8) dir=(-1,0)
    //   client:  RangedAttackAbility target=(7,3) dir=(0,-1)   <- the right one
    //
    // An earlier version of the barrier held AI cats too, on the theory that
    // letting one peer take AI turns the other had not reached was divergence
    // by a quieter door. The opposite is true, twice over:
    //
    //   - Holding an AI brain is NOT free. Brain::UpdateDecision calls GetChoice
    //     only when nothing is cached, so overwriting the decision it just
    //     released makes the AI DERIVE A NEW ONE next frame -- and deriving
    //     draws from the simulation stream. The host sat at the barrier for 529
    //     polls, re-derived that many times, and by the time the barrier opened
    //     its stream had run far past the client's. turn 0 hashes matched
    //     exactly; turn 1 disagreed on rng with state_hash still identical,
    //     which is precisely what "extra draws, same outcome so far" looks like.
    //   - Letting the AI run ahead IS safe, for the reason the control split
    //     already depends on: AI decisions are deterministic and re-derived
    //     identically on both peers (phase 2B left 12 of 29 decisions to
    //     PatternBrain and all 12 matched). A peer ahead on AI turns walks the
    //     same turns with the same draws, and the other peer catches up.
    //
    // Holding a HUMAN brain, by contrast, really is free: GetChoice returns
    // type=1 by itself while waiting on a person and did so for 1695 of 1711
    // calls in one tutorial battle, so suppressing it changes nothing it was
    // going to do anyway. That asymmetry is the whole design.
    if (!g.human_cat[cat]) return false;

    // The join barrier -- human cats only, per the note above. This is what the
    // late-join gap actually needed: the host must not take HUMAN decisions and
    // send them while the client is not in the battle to receive them.
    {
        Guard guard;
        if (const char* why = barrier_blocking()) {
            barrier_wait_tick(why);
            uint32_t none = TA_None;
            mem_write(out, &none, sizeof(none));
            return true;
        }
    }

    Guard guard;

    // --- our cat: let the brain decide, then publish the decision -----------
    if (g.local_cat[cat]) {
        // ...UNLESS a decision for this cat is already sitting in the queue.
        //
        // In a normal battle that never happens: nobody sends us decisions for
        // cats we own, so this branch is inert. It fires only when we have
        // JOINED A BATTLE ALREADY UNDER WAY and the host replayed its history
        // to us -- and then it is the whole reason the catch-up works. Our own
        // human cats already acted in the turns we are fast-forwarding through,
        // and a human decision cannot be re-derived the way an AI one can, so
        // without this the replay would stall on the first turn one of our cats
        // was due to act and wait for a click that already happened.
        //
        // Not gated on a "catching up" flag on purpose: "there is a decision
        // for this cat that we did not make" IS the condition, and a flag would
        // just be a less direct way of asking the same question.
        ActionMsg replayed{};
        if (pend_take(cat, replayed)) {
            log_line("LOCKSTEP", "<= catch-up: replaying our own cat %u's turn-%u "
                                 "decision (we joined this battle late)",
                     (unsigned)cat, replayed.turn);
            apply_remote(replayed, actor, out);
            return true;
        }

        if (g.outstanding) return false;

        TurnAction a{};
        if (!mem_read(out, &a, sizeof(a))) return false;
        if (a.type != TA_Ability && a.type != TA_EndTurn) return false;

        ActionMsg msg{};
        msg.battle_id = g.battles.current;
        msg.turn  = g.turn;
        msg.actor = cat;
        msg.type  = (uint8_t)a.type;
        msg.tx = a.target_x; msg.ty = a.target_y;
        msg.dx = a.dir_x;    msg.dy = a.dir_y;
        if (a.type == TA_EndTurn) {
            // THE FACING THE PLAYER LEFT THE CAT WITH (proto 72). Clicking a tile after the move turns the cat -- a write to Character+0x388 that is no action, so the peer never
            // learned of it (2026-10-03: the client's cat 16 ended turn 13 facing (1,0), the host's read (0,1), and facing decides backstab damage). The end of the turn is the
            // one point where the facing is final, so it rides on the end-turn message, whose direction was always (0,0).
            int32_t fx = 0, fy = 0;
            if (mem_read((const uint8_t*)actor + kChar_Facing, &fx, sizeof(fx)) && mem_read((const uint8_t*)actor + kChar_Facing + 4, &fy, sizeof(fy)) &&
                fx >= -1 && fx <= 1 && fy >= -1 && fy <= 1 && (fx || fy)) { msg.dx = fx; msg.dy = fy; }
        }
        msg.b30 = a.tail[0x30 - 0x28];
        msg.b31 = a.tail[0x31 - 0x28];

        if (a.type == TA_Ability) {
            AbilitySlot slot = ability_slot_of(actor, a.ability);
            msg.slot_kind  = slot.kind;
            msg.slot_index = slot.index;
            ability_gon_name(a.ability, msg.gon, sizeof(msg.gon));
            if (slot.kind == SLOT_UNKNOWN) {
                // Unreproducible on the peer: it has no way to name this
                // ability. Say so loudly rather than send an action the other
                // side will resolve to nullptr and silently skip.
                log_line("LOCKSTEP", "!! cat %u used an ability in no known slot "
                                     "(gon='%s') -- peer cannot resolve it",
                         (unsigned)cat, msg.gon[0] ? msg.gon : "?");
            }
        }

        // Logged BEFORE the send, and logged whether or not the send succeeds.
        // The log is what a peer joining mid-battle replays to catch up, so it
        // has to describe what this peer DID, not what it managed to transmit
        // -- a decision that failed to send is exactly the one a joining peer
        // most needs, and a peer that was not connected yet cannot have been
        // sent anything at all.
        sent_log_push(msg);

        if (net_send_action(msg)) {
            g.outstanding = true;
            ++g.stats.sent;
            log_line("LOCKSTEP", "-> cat %u %s slot=%u:%u target=(%d,%d) gon=%s",
                     (unsigned)cat, a.type == TA_EndTurn ? "endturn" : "ability",
                     msg.slot_kind, msg.slot_index, msg.tx, msg.ty,
                     msg.gon[0] ? msg.gon : "-");
        }
        return false;    // the local brain's own decision proceeds unchanged
    }

    // --- the peer's cat: overwrite unconditionally --------------------------
    //
    // Unconditionally, because this is also what suppresses local input on a
    // cat we do not own: a stray click produces a decision that we replace
    // here, so it can never reach QueueDecision.
    ActionMsg msg{};
    if (!pend_take(cat, msg)) {
        uint32_t none = TA_None;
        mem_write(out, &none, sizeof(none));
        return true;                       // nothing yet -- keep waiting, free
    }
    apply_remote(msg, actor, out);
    return true;
}

// Turn a decision that came off the wire into the TurnAction the brain was
// about to return. Factored out because there are now two callers and they
// differ only in WHOSE cat it is: the peer's, in the ordinary case, and our
// own when we joined a battle late and the host replayed what our cats already
// did. Everything below -- the slot resolution, the GON cross-check, the halt
// on a mismatch -- has to be identical for both, because a replayed decision is
// exactly as authoritative as a live one.
static void apply_remote(const ActionMsg& msg, const void* actor, void* out) {
    const uint8_t cat = msg.actor;

    TurnAction a{};
    memset(&a, 0, sizeof(a));
    a.type     = msg.type;
    a.target_x = msg.tx; a.target_y = msg.ty;
    a.dir_x    = msg.dx; a.dir_y    = msg.dy;
    if (msg.type == TA_EndTurn) {
        // The owner's final facing (see the send side): put it on this peer's copy of the cat before the turn ends, and give the action the (0,0) direction an end turn always had.
        a.dir_x = 0; a.dir_y = 0;
        if (actor && (msg.dx || msg.dy) && msg.dx >= -1 && msg.dx <= 1 && msg.dy >= -1 && msg.dy <= 1) {
            int32_t fx = 0, fy = 0;
            mem_read((const uint8_t*)actor + kChar_Facing, &fx, sizeof(fx)); mem_read((const uint8_t*)actor + kChar_Facing + 4, &fy, sizeof(fy));
            if (fx != msg.dx || fy != msg.dy) {
                // Turn the cat the way the game does -- Character::Face writes the facing AND plays the turn animation. Writing the field alone (what this did until 2026-10-03) left the logic right and
                // the sprite looking the old way on this peer. Every game caller passes (dir, 0, 0). The field is written afterwards only if Face did not leave it as asked.
                const bool via_face = turn_cat_via_face((void*)actor, msg.dx, msg.dy);
                // This runs INSIDE Brain::UpdateDecision (the decision is filled from there), and the aim-preview put-back at its end restores the facing it saw when it began: left alone it would
                // undo this write at once (2026-10-03: the client logged "facing taken" and still read the old facing a turn later, halting on a backstab). The new value is the one to keep.
                if (g_pf.have && g_pf.ch == (uint8_t*)actor) { g_pf.fx = msg.dx; g_pf.fy = msg.dy; }
                face_pend_set((uint8_t*)actor, msg.dx, msg.dy);
                log_line("LOCKSTEP", "cat %u ends its turn facing (%d,%d) -- this peer had (%d,%d): the owner's facing taken (%s)", (unsigned)cat, msg.dx, msg.dy, fx, fy, via_face ? "Character::Face" : "field write");
            }
        }
    }
    a.actor    = nullptr;                  // arrives null from a real brain too;
                                           // Character::DoAction fills it in
    a.tail[0x30 - 0x28] = msg.b30;
    a.tail[0x31 - 0x28] = msg.b31;

    if (msg.type == TA_Ability) {
        AbilitySlot slot{ msg.slot_kind, msg.slot_index };
        a.ability = ability_from_slot(actor, slot);
        if (!a.ability) {
            char why[192];
            _snprintf_s(why, sizeof(why), _TRUNCATE,
                        "cat %u slot %u:%u (gon '%s') is empty on this peer",
                        (unsigned)cat, msg.slot_kind, msg.slot_index, msg.gon);
            halt(why);
            uint32_t none = TA_None;
            mem_write(out, &none, sizeof(none));
            return;
        }
        // Resolve by slot, validate by name. A disagreement here means the two
        // peers' cats no longer hold the same abilities, which is a desync that
        // has already happened -- catching it now beats playing the wrong spell.
        char gon[64];
        if (msg.gon[0] && ability_gon_name(a.ability, gon, sizeof(gon)) &&
            strcmp(gon, msg.gon) != 0) {
            char why[192];
            _snprintf_s(why, sizeof(why), _TRUNCATE,
                        "cat %u slot %u:%u holds '%s', peer sent '%s'",
                        (unsigned)cat, msg.slot_kind, msg.slot_index, gon, msg.gon);
            halt(why);
            uint32_t none = TA_None;
            mem_write(out, &none, sizeof(none));
            return;
        }
    }

    mem_write(out, &a, sizeof(a));
    ++g.stats.applied;
    log_line("LOCKSTEP", "<- cat %u %s slot=%u:%u target=(%d,%d) gon=%s",
             (unsigned)cat, msg.type == TA_EndTurn ? "endturn" : "ability",
             msg.slot_kind, msg.slot_index, msg.tx, msg.ty,
             msg.gon[0] ? msg.gon : "-");
}

void lockstep_reseed_action(const void* actor);     // below, with lockstep_reseed

void lockstep_on_applied(const void* action, const void* actor) {
    if (!g.active || !action) return;

    TurnAction a{};
    if (!mem_read(action, &a, sizeof(a))) return;

    // Types 6 and 7 are generated locally on both peers and are never sent.
    // They must not clear the outstanding guard or advance anything.
    if (a.type != TA_Ability && a.type != TA_EndTurn) return;

    // Match the recorder's precedence exactly (mgmp_hooks.cpp, h_ApplyAction):
    // prefer the action's own actor, fall back to TurnControl's current one.
    // Getting this backwards is what made the replayer cry wolf on a Toss --
    // an action that carries its own actor and is not the turn's actor.
    const void* who = a.actor ? a.actor : actor;

    Guard guard;
    g.outstanding = false;

    // REMEMBERED FOR THE SUMMON RULE. A player-brain unit that appears
    // mid-battle inherits the ownership of the cat whose action produced it, and
    // this is that cat. Both peers apply the same actions in the same order, so
    // the rule resolves to the same owner on both without a message -- which is
    // the property that made deriving the split locally viable in the first
    // place. See adopt_new_cats.
    g.actor = who;
    lockstep_reseed_action(who);     // every action starts from a stream both peers derive: nothing drawn between two actions on one peer only (a preview on the owner's side) can reach this one

    // A cat with no index has not been adopted yet (it appeared after the last
    // turn boundary, or it is not in the live list at all). Inside the roster,
    // this is where a per-action cross-check would go once the state hash covers
    // Character fields.
    uint8_t cat = cat_index_of(who);
    if (cat != kNoCat && g.local_cat[cat]) ++g.stats.applied;
}

// ADOPT WHAT APPEARED, AND SAY WHOSE IT IS.
//
// Until now a summon stood outside the whole model: `cat_index_of` returned
// kNoCat for it, `lockstep_fill_choice` returned early ("neither peer treats it
// as owned"), and `lockstep_aim_subject` answered false -- so a unit with a
// PLAYER brain was driven by each peer's own window, the two sims diverged on
// it, and both players saw its ability bar enabled. Reported from play
// 2026-09-21, with one peer eleven turns ahead of the other by the end of it.
//
// It is not a narrow class of unit: it is any summon the game hands a
// PlayerBrain, which is what a "controllable ally" IS.
//
// WHAT FIXES IT IS THE ADOPTION, NOT A NEW CHANNEL. A summon APPENDS to the live
// list, so it can take the next free roster index without disturbing any
// existing one (see adopt_new in mgmp_split.h). Once it has an index, everything
// downstream -- ownership, the decision channel, the aim preview, the combat
// menu -- works unchanged, because all of them are index-based already.
//
// The owner is inherited from the cat whose action produced the summon, which
// both peers know from the same applied action and therefore agree on. Three
// outcomes, and the third is the honest one:
//
//   * an AI brain  -> nobody's, exactly as before: both peers re-derive it
//                     identically, which is why that case never needed anything;
//   * a player brain and the actor is a human cat in the roster -> the actor's
//                     owner, on both peers, by arithmetic;
//   * a player brain with no attributable actor -> nobody's, LOGGED LOUDLY. Both
//                     peers then drive it locally, which is the old behaviour --
//                     it is a gap, and a gap that says so is worth more than one
//                     inferred from a divergence three turns later.
void adopt_new_cats() {
    if (!g.snapped || !g.snapped_list || g.halted) return;

    uint32_t    live_n = 0;
    const void* data   = nullptr;
    if (!mem_read((const uint8_t*)g.snapped_list + kList_Count, &live_n, sizeof(live_n))) return;
    if (!mem_read((const uint8_t*)g.snapped_list + kList_Data,  &data, sizeof(data)) || !data) return;
    if (live_n == 0 || live_n > kMaxCats) return;

    const void* live[kMaxCats] = {};
    for (uint32_t j = 0; j < live_n; ++j)
        mem_read((const uint8_t*)data + j * sizeof(void*), &live[j], sizeof(void*));

    // An address that is on the roster but now holds ANOTHER unit (see entry_is) is not known: it is an arrival like any other.
    const void* known[kMaxCats + 16] = {};
    for (uint32_t j = 0; j < g.cat_count; ++j) known[j] = entry_is(j, g.cats[j]) ? g.cats[j] : nullptr;
    uint32_t known_n = g.cat_count;
    for (uint32_t k = 0; k < g.retired_n && known_n < kMaxCats + 16; ++k)      // replaced by a transform: not an arrival
        if (is_retired(g.retired[k])) known[known_n++] = g.retired[k];
    uint8_t        idx[kMaxCats] = {};
    const uint32_t room = g.cat_count < kMaxCats ? kMaxCats - g.cat_count : 0;
    const uint32_t n    = adopt_new(known, known_n, live, live_n, idx, room);

    if (n == 0 && room == 0 && live_n > g.cat_count)
        log_line_lvl(LogLevel::Warn, "LOCKSTEP",
                     "!! %u live character(s) against a full roster array (%u): anything that "
                     "appeared has no index, so it is outside the split and outside the hash. "
                     "Raise kMaxCats.", live_n, kMaxCats);

    for (uint32_t k = 0; k < n; ++k) {
        const void* ch    = live[idx[k]];
        const uint32_t i  = g.cat_count;
        for (uint32_t j = 0; j < g.cat_count; ++j)
            if (g.cats[j] == ch && !known[j]) {      // the address of a roster entry that is another unit now
                uint32_t now = 0;
                read_ident(ch, now);
                log_line_lvl(LogLevel::Warn, "LOCKSTEP", "!! roster entry %u (creation key %08x) has left the battle and its address %p was reused by a NEW unit (creation key %08x): adopted as roster index %u, not taken for the old one",
                             j, g.cat_key[j], ch, now, i);
            }
        g.cats[i]   = ch;
        note_ident(i);
        g.summon[i] = true;

        const void* brain = nullptr;
        char bcls[96] = "?";
        if (mem_read((const uint8_t*)ch + kChar_Brain, &brain, sizeof(brain)) && brain)
            rtti_class_name(brain, bcls, sizeof(bcls));
        g.human_cat[i] = (strstr(bcls, tune::kReplayBrains) != nullptr);
        if (g.human_cat[i]) ++g.summon_humans;   // NOT g.humans -- see the field note:
                                                 // CONTROL's count is the snapshot's

        const uint8_t src     = g.actor ? cat_index_of(g.actor) : kNoCat;
        const bool    inherits = g.human_cat[i] && src != kNoCat && g.human_cat[src];
        g.local_cat[i] = inherits && g.local_cat[src];

        ++g.cat_count;                     // the next iteration indexes past this one

        if (!g.human_cat[i])
            log_line("LOCKSTEP", "roster index %u added mid-battle (brain %s) -- an AI unit, "
                                 "re-derived locally on both peers as before", i, bcls);
        else if (inherits)
            log_line("LOCKSTEP", "roster index %u added mid-battle (brain %s): owned by %s, "
                                 "inherited from cat %u (the applied action's actor)",
                     i, bcls, g.local_cat[i] ? "THIS peer" : "the peer", (unsigned)src);
        else
            log_line_lvl(LogLevel::Warn, "LOCKSTEP",
                         "!! roster index %u added mid-battle with a PLAYER brain (%s) and no "
                         "attributable owner (actor %s) -- both peers will drive it locally, "
                         "which diverges on its first move. Known gap in summon ownership.",
                         i, bcls, src == kNoCat ? "not in the roster" : "not a human cat");
    }
}

// IS THIS CAT ONE OF THE RUN'S FAMILIARS? Read the same way the weaken aid does:
// the run's own familiar list, by cat id. Used to keep a debug sweep off the run's
// extra cats -- they are AI, so the human test alone would not spare them.
bool cat_is_familiar(const void* chr) {
    const void** slot = (const void**)addr_of_data(D_MewDirectorPtr);
    const void* dir = nullptr;
    uint32_t count = 0;
    const uint64_t* ids = nullptr;
    if (!mem_read(slot, &dir, sizeof(dir)) || !dir) return false;
    if (!mem_read((const uint8_t*)dir + kDir_CatFamiliars + 4, &count, 4) || !count || count > 64)
        return false;
    if (!mem_read((const uint8_t*)dir + kDir_CatFamiliars + 8, &ids, sizeof(ids)) || !ids)
        return false;

    const void* data = nullptr;
    uint64_t id = 0;
    if (!mem_read((const uint8_t*)chr + kChar_CatData, &data, sizeof(data)) || !data) return false;
    if (!mem_read((const uint8_t*)data + kCatData_SaveId, &id, sizeof(id)) || !id) return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t other = 0;
        if (mem_read(&ids[i], &other, sizeof(other)) && other == id) return true;
    }
    return false;
}

// EVERY ENEMY, NO TARGETING -- see the header. The adoption is the actual fix for
// "the spawned enemy cannot be hit": without it this would sweep the same roster the
// arm-and-click path sees, and miss exactly the cats the report was about.
// THE LIVE ROSTER, FOR A FIGHT THIS PEER NEVER SNAPSHOTTED (2026-09-22).
//
// g.cats[]/g.human_cat[] only exist once a battle has been snapshotted, and a snapshot
// requires a session -- so after "continue alone" (session closed, lockstep inert) the
// kill-all aid had nothing but the PREVIOUS fight's roster, whose Character pointers are
// dangling. Left unfixed it fired damage at whatever now occupied those addresses:
// measured 2026-09-22, 25 "enemies" hit across 31 stale entries, the player's own cat
// damaged and the enemies on the board untouched. Hence the refusal below, first.
//
// But the pointer it needs is NOT session-scoped. A TurnControl is in hand in exactly one
// place -- the NextTurn hook -- and mgmp_listprobe and mgmp_roster are both handed it
// there, ABOVE the session gate, for this very reason ("The probe needs a POINTER, not a
// session"). lockstep_set_turn_control is the same hand-off for this module, and
// resolve_char_list already knows how to walk from it to the live character list.
//
// So this builds a THROWAWAY roster instead of refusing, using the same two tests the
// snapshot uses to decide who is a player's cat: a PlayerBrain class from the RTTI name,
// and the run's own familiar list (cat_is_familiar). Nothing here is hashed, nothing is
// numbered, and the array never outlives the call -- it is a reading of the board, not a
// second roster.
void lockstep_set_turn_control(void* turn_control) {
    if (!turn_control || turn_control == g.tc) return;
    g.tc = turn_control;
    log_line_lvl(LogLevel::Trace, "LOCKSTEP",
                 "turn control 0x%llX -- the live character list is reachable from here"
                 " even without a session (a test aid needs the board, not a peer)",
                 (unsigned long long)(uintptr_t)turn_control);
}

uint32_t build_live_roster(const void* list, const void* out[], bool human[], uint32_t cap) {
    uint32_t    count = 0;
    const void* data  = nullptr;
    if (!list) return 0;
    if (!mem_read((const uint8_t*)list + kList_Count, &count, sizeof(count)) ||
        !count || count > cap)
        return 0;
    if (!mem_read((const uint8_t*)list + kList_Data, &data, sizeof(data)) || !data) return 0;

    uint32_t n = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const void* chr = nullptr;
        if (!mem_read((const uint8_t*)data + i * sizeof(void*), &chr, sizeof(chr)) || !chr)
            continue;

        bool is_human = false;
        const void* brain = nullptr;
        if (mem_read((const uint8_t*)chr + kChar_Brain, &brain, sizeof(brain)) && brain) {
            char cls[96] = {};
            if (rtti_class_name(brain, cls, sizeof(cls)))
                is_human = strstr(cls, tune::kReplayBrains) != nullptr;
        }
        out[n]   = chr;
        human[n] = is_human;
        ++n;
    }
    return n;
}

// ONE HP WRITE, TWO CALLERS (2026-09-22).
//
// This was the middle of lockstep_graze_enemy_on_tile, and extracting it is what makes the
// all-enemies sweep work in a run with no snapshot. The sweep HOLDS the Character already
// -- it has just read that cat's dead flag, TacticsObject and tile -- so making it look the
// cat up AGAIN by tile, against a roster that only exists inside a session, threw away the
// one thing it had. Measured that day: 33 entries, ZERO skipped, ZERO hit, because all 33
// hits went through the tile lookup and found nothing. A sweep that skips nothing and hits
// nothing is the shape of "the second lookup is the bug".
//
// The write itself is unchanged, field for field: hp only, floored at 0, one dword. The
// game's own death handling does the rest, which is the property that made the original aid
// useful and is not this function's business to reimplement.
// An EnterNode precedes the first NextTurn. During that interval g.cats still
// describes the previous fight, even if its allocations now hold live cats.
bool debug_snapshot_ready() {
    return g.active && g.snapped && !g.halted &&
           g.snapshot_battle != kNoBattle && g.snapshot_battle == g.battles.current &&
           g.snapped_list && resolve_char_list(g.tc) == g.snapped_list;
}

// Check live identity at the write boundary as well as the cached human bit.
// Unknown brains are not evidence of an enemy (especially during construction).
bool debug_is_enemy(const void* chr) {
    if (!chr || cat_is_familiar(chr)) return false;
    const void* data = nullptr;
    uint64_t id = 0;
    uint8_t pos = 0;
    if (mem_read((const uint8_t*)chr + kChar_CatData, &data, sizeof(data)) && data &&
        mem_read((const uint8_t*)data + kCatData_SaveId, &id, sizeof(id)) && id &&
        lockstep_owner_pos(id, pos)) return false;
    const void* brain = nullptr;
    char cls[96] = {};
    return mem_read((const uint8_t*)chr + kChar_Brain, &brain, sizeof(brain)) && brain &&
           rtti_class_name(brain, cls, sizeof(cls)) &&
           !strstr(cls, tune::kReplayBrains);
}

bool debug_apply_hit(const void* chr, int32_t amount, int32_t tx, int32_t ty) {
    if (amount <= 0 || !debug_is_enemy(chr)) return false;

    int32_t hp = 0, maxhp = 0;
    if (!mem_read((const uint8_t*)chr + kChar_HP, &hp, 4)) return false;
    if (!mem_read((const uint8_t*)chr + kChar_MaxHP, &maxhp, 4)) return false;
    const int32_t after = (hp > amount) ? hp - amount : 0;
    if (!mem_write((uint8_t*)chr + kChar_HP, &after, 4)) return false;

    log_line_lvl(LogLevel::Warn, "LOCKSTEP",
                 "!! DEBUG HIT on request: enemy at tile (%d,%d), hp %d/%d -> %d. This EDITS"
                 " the battle; session callers broadcast the same hit to the peer. The game's own"
                 " death handling is what kills it.", tx, ty, hp, maxhp, after);
    return true;
}

// HOW A BATTLE ENDED, in the log (2026-10-03): nothing said so, and "the humans were at 3 hp and the fight was won" is the first thing a settlement problem needs to be told apart from. Called when the
// run leaves a battle (the next node is entered, or the run is settled). Once per battle. Every read is guarded.
// ONE LINE PER BATTLE with the numbers a halt rate needs both ways: how many battles there were, and what each cost (2026-10-04). Grep SUMMARY-SYNC over many logs.
namespace {
void log_battle_stats(const char* why, bool halted) {
    uint32_t uqc = 0, uqm = 0;
    unlockq_stats(uqc, uqm);
    uint32_t rlc = 0, rlm = 0, rll = 0, dpc = 0, dpm = 0;
    rngl_stats(rlc, rlm, rll); deep_stats(dpc, dpm);
    log_line("BATTLE", "SUMMARY-SYNC (%s) battle %016llx role=%s sim=%u proto=%u: turns %u in %llu s | hashes agreed %u, mismatched %u, debounced %u, %s | board: applied %u, with repairs %u, late %u, "
                       "units rewritten %u moved %u replaced %u spawned %u trimmed %u, stuck %u, unmatched %u, the build itself differed in %u unit(s) | audit: %s%u cat(s) compared, %u differ | unlock queries: %u action(s) compared, %u differ | rng ledger: %u action(s) compared, %u differ (%u between actions) | derived values: %u turn(s) compared, %u differ",
             why, (unsigned long long)g.battles.current, net_role() == NetRole::Host ? "host" : "client", (unsigned)tune::kSimRevision, (unsigned)kProtoVersion,
             (unsigned)g.turn, (unsigned long long)(g.bs.t0 ? (GetTickCount64() - g.bs.t0) / 1000 : 0), g.bs.agreed, g.bs.mismatched, g.debounced, halted || g.halted || g.halted_in_battle ? "HALTED" : "completed",
             g.bs.board_applied, g.bs.board_differed, g.bs.board_late, g.bs.rewritten, g.bs.moved, g.bs.replaced, g.bs.spawned, g.bs.trimmed, g.bs.stuck, g.bs.bad, g.bs.turn0_differed,
             g.bs.audit_done ? "" : "(not completed) ", g.bs.audit_cats, g.bs.audit_differ, uqc, uqm, rlc, rlm, rll, dpc, dpm);
}
} // namespace

void lockstep_log_battle_summary(const char* why) {
    static uint64_t s_done_for = kNoBattle;
    // only a battle that was actually snapshotted (an event, a shop or a map node enters a "battle" id too, with the previous battle's numbers still in place)
    if (!g.snapped || g.battles.current == kNoBattle || g.battles.current == s_done_for || g.snapshot_battle != g.battles.current || g.bs.agreed + g.bs.mismatched == 0) return;
    s_done_for = g.battles.current;
    log_battle_stats(why, false);
    unsigned humans = 0, humans_alive = 0, enemies = 0, enemies_alive = 0;
    char text[420]; int w = 0;
    text[0] = 0;
    for (uint32_t i = 0; i < g.cat_count; ++i) {
        CatState st{};
        if (!g.cats[i] || !read_cat_state(g.cats[i], st, false)) continue;   // false: plain reads only -- most of these units are gone, and the game must not be called on them
        const bool alive = !st.dead && st.hp > 0;
        if (g.human_cat[i]) {
            ++humans; if (alive) ++humans_alive;
            if (w >= 0 && (size_t)w < sizeof(text) - 40) w += _snprintf_s(text + w, sizeof(text) - w, _TRUNCATE, " %u:%d/%d%s", i, st.hp, st.maxhp, alive ? "" : "(down)");
        } else if (debug_is_enemy(g.cats[i])) {
            ++enemies; if (alive) ++enemies_alive;
        }
    }
    log_line("BATTLE", "summary (%s): battle %016llx after %u turn(s): humans standing %u of %u [id:hp/max]%s | enemies left %u of %u",
             why, (unsigned long long)g.battles.current, (unsigned)g.turn, humans_alive, humans, text, enemies_alive, enemies);
}

// True once the fight is WON: the battle is snapshotted and live, it had enemies, and every one of them is down. The level-up pool is built in the very frame of the last kill, BEFORE the character list
// goes away (live test 2026-10-04: the unlock queries of the level-up still came while lockstep_fight_up() was true), so the unlock answers have to end here, not at the list's disappearance.
// Plain reads only: the game must not be called on these units.
bool lockstep_enemies_all_down() {
    if (!lockstep_fight_up() || g.halted) return false;
    unsigned enemies = 0;
    for (uint32_t i = 0; i < g.cat_count; ++i) {
        CatState st{};
        if (!g.cats[i] || g.human_cat[i] || !read_cat_state(g.cats[i], st, false) || !debug_is_enemy(g.cats[i])) continue;
        ++enemies;
        if (!st.dead && st.hp > 0) return false;
    }
    return enemies > 0;
}

bool lockstep_dev_force_halt() {
    if (!config().dev_tools || !g.active || !g.snapped || g.halted || !net_active() || net_peer_count() < 2) return false;
    log_line_lvl(LogLevel::Warn, "LOCKSTEP", "!! HALT requested from the developer page: the battle is halted on purpose, as a real desync would (the other peer is told)");
    halt("simulated desync (the developer button) -- nothing is actually wrong");
    return true;
}

const char* lockstep_halt_reason() { return g.halt_reason; }

void lockstep_dev_arm_stat_repair() {
    g.exp_stats = true;
    log_line_lvl(LogLevel::Warn, "STATS", "!! EXPERIMENT ARMED: from now on this peer's board writes a player's cat's stats and max hp too, and the cats it touches are watched");
}
// A player's cat of this peer's own to experiment on: the first roster cat that is a player's and local, else any player's. Null when there is none.
const void* lockstep_dev_player_cat() {
    if (!g.snapped) return nullptr;
    const void* any = nullptr;
    for (uint32_t i = 0; i < g.cat_count; ++i) {
        if (!g.human_cat[i] || !g.cats[i] || !entry_is(i, g.cats[i])) continue;
        if (g.local_cat[i]) return g.cats[i];
        if (!any) any = g.cats[i];
    }
    return any;
}

// THIS PEER'S LOG TAIL, to the other player(s) (proto 76). A state line first (who this is, where it stands), then the newest lines of the ring that are
// not chatter (Info and up) and not the peer's own tail coming back. Rate-limited: a desync, its halt and the peer's halt arrive together.
void lockstep_share_log(const char* why) {
    if (!net_active() || net_peer_count() < 2) return;
    static ULONGLONG last = 0;
    static uint32_t sent = 0;
    const ULONGLONG t = GetTickCount64();
    if (sent >= 16 || (last && t - last < 10000)) {
        log_line("PEERLOG", "(not sharing this log's tail for '%s': %s)", why ? why : "?", sent >= 16 ? "already shared 16 times this session" : "shared less than 10 s ago");
        return;
    }
    last = t; ++sent;

    constexpr uint32_t kFetch = 4096, kKeep = 400, kMaxBytes = 120 * 1024;
    LogEntry* ring = (LogEntry*)malloc(sizeof(LogEntry) * kFetch);
    char* text = (char*)malloc(kMaxPeerLogBytes);
    if (!ring || !text) { free(ring); free(text); return; }
    uint32_t cursor = 0;
    const uint32_t n = log_ring_fetch(cursor, ring, kFetch, nullptr);
    // NOT A FIXED 400 LINES (2026-10-04): at least that many, and back to the start of the turn before the one the boundary check last agreed on, so the actions that led to a difference are in it
    // however chatty they were; capped by the size the message can carry. A late detection used to scroll the divergent action out of the tail.
    uint32_t first = 0, kept = 0;
    size_t bytes = 0;
    const uint32_t want_turn = g.turn >= 3 ? g.turn - 3 : 0;
    for (uint32_t i = n; i-- > 0;) {
        if (strcmp(ring[i].tag, "PEERLOG") == 0) continue;
        bytes += strlen(ring[i].text) + 24;
        if (bytes >= kMaxBytes) break;
        first = i;
        ++kept;
        if (kept >= kKeep && (ring[i].turn <= want_turn || !g.snapped)) break;
    }
    size_t len = 0;
    auto add = [&](const char* fmt, ...) {
        if (len >= kMaxPeerLogBytes - 512) return;
        va_list ap; va_start(ap, fmt);
        const int w = _vsnprintf_s(text + len, kMaxPeerLogBytes - len, _TRUNCATE, fmt, ap);
        va_end(ap);
        if (w > 0) len += (size_t)w;
    };
    const NetStats ns = net_stats();
    add("STATE role=%s self=%u peers=%u proto=%u sim=%u | stage '%s' | battle %016llx turn %u cats %u halted %d mismatches %u debounced %u | net sent %u recv %u dropped %u\n",
        net_role() == NetRole::Host ? "host" : "client", (unsigned)net_self(), (unsigned)net_peer_count(), (unsigned)kProtoVersion, (unsigned)tune::kSimRevision,
        log_stage_last(), (unsigned long long)g.battles.current, g.turn, g.cat_count, (int)g.halted, g.mismatches, g.debounced, ns.sent, ns.received, ns.dropped);
    for (uint32_t i = first; i < n && kept; ++i) {
        if (strcmp(ring[i].tag, "PEERLOG") == 0) continue;
        add("%06u %04u %-9s %s\n", ring[i].seq, ring[i].turn, ring[i].tag, ring[i].text);
    }
    PeerLogMsg m{};
    m.battle_id = g.battles.current; m.turn = g.turn;
    strncpy_s(m.why, sizeof(m.why), why ? why : "", _TRUNCATE);
    m.size = (uint32_t)len; m.data = (uint8_t*)text;
    const bool ok = len && net_send_peerlog(m);
    log_line("PEERLOG", "-> %s this log's last %u lines (%u bytes) with the other player(s): %s", ok ? "shared" : "could NOT share", kept, (unsigned)len, why ? why : "");
    free(ring); free(text);
}

bool lockstep_take_desync_notice(char* text, size_t cap) {
    if (!InterlockedExchange(&g_desync_notice, 0)) return false;
    if (text && cap) strncpy_s(text, cap, g_desync_text, _TRUNCATE);
    return true;
}

void lockstep_roster_replace(const void* old_chr, void* new_chr) {
    if (!g.snapped || !old_chr || !new_chr) return;
    for (uint32_t i = 0; i < g.cat_count; ++i) {
        if (g.cats[i] != old_chr) continue;
        retire_unit(old_chr);
        g.cats[i] = new_chr;
        int32_t nk = 0;
        g.cat_key_ok[i] = mem_read((const uint8_t*)new_chr + kChar_Ident, &nk, 4);
        g.cat_key[i] = (uint32_t)nk;
        log_line("LOCKSTEP", "roster entry %u: unit %p replaced in place by %p (creation key %08x)", i, old_chr, new_chr, (unsigned)nk);
        return;
    }
}

int lockstep_debug_hit_all(int32_t amount) {
    if (!config().dev_tools) return 0;                 // developer tool: never in a release
    if (g.active && (!debug_snapshot_ready() || !g.control_checked)) {
        log_line_lvl(LogLevel::Warn, "LOCKSTEP",
                     "DEBUG HIT ALL refused: current battle roster/control is not ready; retry after turn 0");
        return 0;
    }
    // WHICH ROSTER: THE SNAPSHOT, OR THE LIVE BOARD (2026-09-22).
    //
    // The snapshot is preferred whenever it exists -- it is the roster the rest of the
    // module agrees with, and inside a session the aid must hit exactly what the hashes
    // cover. Without one (a single-player run, or the solo half of "continue alone") the
    // live character list is read instead, which is what makes this aid usable in the one
    // case where it is the only way to end a fight quickly.
    const void* roster[kMaxCats] = {};
    bool        human[kMaxCats]  = {};
    uint32_t    n = 0;
    bool        live = false;

    if (g.active && g.snapped && g.cat_count) {
        adopt_new_cats();   // the deathrattle's children, before anything looks at them
        const uint32_t take = g.cat_count < kMaxCats ? g.cat_count : kMaxCats;
        for (uint32_t i = 0; i < take; ++i) {
            roster[i] = g.cats[i];
            human[i]  = g.human_cat[i];
        }
        n = take;
    } else {
        n    = build_live_roster(resolve_char_list(g.tc), roster, human, kMaxCats);
        live = true;

        if (!n) {
            log_line_lvl(LogLevel::Error, "LOCKSTEP",
                         "!! DEBUG HIT ALL refused: there is no battle snapshot AND the"
                         " live character list could not be read, so the only roster in"
                         " hand would be an earlier fight's -- and hitting those tiles"
                         " damages whatever stands there now. Both faults are reported: a"
                         " TurnControl has %s been handed over (see lockstep_set_turn_"
                         " control), and the chain from it did not resolve.",
                         g.tc ? "" : "NEVER");
            return 0;
        }
        log_line_lvl(LogLevel::Warn, "LOCKSTEP",
                     "no snapshot (no session): sweeping the LIVE character list instead"
                     " -- %u entr(ies). Nothing here is hashed and no index is assigned;"
                     " this is a reading of the board, which is what makes the aid work in"
                     " a single-player run", n);
    }

    int hits = 0;

    // WHY A SWEEP HIT NOTHING (2026-09-22). "0 cat(s) hit" says only that every entry was
    // skipped, and there are five ways to be skipped -- each with a different cause and a
    // different fix. Measured that day: 34 live entries, 0 hits, and nothing in the log to
    // say whether the player test, the familiar test, the dead flag or the link check had
    // done it. These counters answer it in one run.
    uint32_t n_human = 0, n_familiar = 0, n_dead = 0, n_unlinked = 0, n_notile = 0;

    for (uint32_t i = 0; i < n; ++i) {
        const void* chr = roster[i];
        if (!chr) continue;
        if (human[i]) { ++n_human; continue; }                // never the players' cats
        uint8_t dead = 0;
        if (!mem_read((const uint8_t*)chr + kChar_Dead, &dead, 1) || dead)
            { ++n_dead; continue; }
        if (cat_is_familiar(chr)) { ++n_familiar; continue; }  // nor the run's familiars

        // The tile, believed only when the TacticsObject points back at this
        // Character -- the same rule every other reader here applies, and for the
        // same reason: a stale link reads as somebody else's position.
        const void* to = nullptr;
        if (!mem_read((const uint8_t*)chr + kChar_TObj, &to, sizeof(to)) || !to)
            { ++n_unlinked; continue; }
        const void* owner = nullptr;
        if (!mem_read((const uint8_t*)to + kTObj_Owner, &owner, sizeof(owner)) || owner != chr)
            { ++n_unlinked; continue; }
        int32_t tile[2] = {};
        if (!mem_read((const uint8_t*)to + kTObj_Tile, tile, sizeof(tile)))
            { ++n_notile; continue; }

        // IN A SESSION: the same call as before, broadcast and all -- both peers must write
        // the same numbers, and the tile lookup works there because there IS a roster.
        // WITH NO SESSION: nothing to broadcast and nothing to look up, but the cat itself
        // is already in hand, which is exactly what this branch is for (2026-09-22).
        if (g.active && g.snapped)
            hits += lockstep_debug_hit(tile[0], tile[1], amount);
        else if (debug_apply_hit(chr, amount, tile[0], tile[1]))
            ++hits;
    }

    log_line_lvl(LogLevel::Warn, "LOCKSTEP",
                 "!! DEBUG HIT ALL: %d damage to every enemy on the field -- %d cat(s) hit"
                 " across %u roster entr(ies), each one broadcast so both peers write the"
                 " same numbers. %s Skipped: %u player cat(s), %u familiar(s), %u dead,"
                 " %u unlinked (no TacticsObject, or it does not point back), %u whose"
                 " tile would not read.",
                 amount, hits, n,
                 live ? "The roster was the LIVE character list, not a snapshot: a"
                        " single-player run has no snapshot to sweep."
                      : "Mid-battle arrivals were adopted first, which is what makes a"
                        " deathrattle's children hittable at all.",
                 n_human, n_familiar, n_dead, n_unlinked, n_notile);
    return hits;
}

int lockstep_debug_hit(int32_t tx, int32_t ty, int32_t amount) {
    if (!config().dev_tools) return 0;
    if (!debug_snapshot_ready() || !g.control_checked) return 0;
    const int hit = lockstep_graze_enemy_on_tile(tx, ty, amount);

    // BROADCAST, and this is what makes the tool usable rather than a one-way
    // door. Applied only locally, the host walks out of a fight the client is
    // still standing in -- measured 2026-09-22, and the client had no way back.
    // Both peers writing the SAME number leaves the two states equal again, so the
    // next turn's hash agrees instead of reporting a desync we caused on purpose.
    if (!hit || !net_active()) return hit;

    DebugHitMsg m{};
    m.battle_id = g.battles.current;
    m.tx        = tx;
    m.ty        = ty;
    m.amount    = amount;
    if (net_send_debughit(m))
        log_line_lvl(LogLevel::Warn, "LOCKSTEP",
                     "-> DEBUGHIT broadcast: tile (%d,%d), %d damage, %d cat(s) hit here"
                     " -- the peers write the same number, which is what keeps this from"
                     " being a desync", tx, ty, amount, hit);
    else
        log_line_lvl(LogLevel::Error, "LOCKSTEP",
                     "!! the debug hit was applied here and could NOT be broadcast -- the"
                     " peers are now in different battles. That is the failure this"
                     " broadcast exists to prevent.");
    return hit;
}

void lockstep_on_debug_hit(uint8_t from, const DebugHitMsg& m) {
    // A peer's message must not be able to write hit points on this machine unless this one opted in to developer tools.
    if (!config().dev_tools) {
        log_line_lvl(LogLevel::Warn, "LOCKSTEP", "DEBUGHIT from peer %u ignored: developer tools are off here", (unsigned)from);
        return;
    }
    if (!debug_snapshot_ready()) {
        log_line_lvl(LogLevel::Warn, "LOCKSTEP", "DEBUGHIT refused: current battle roster is not ready");
        return;
    }
    // A tile means nothing without the board it is on, exactly as with AIM.
    if (m.battle_id != g.battles.current) {
        log_line("LOCKSTEP", "debug hit from peer %u for battle %016llx ignored -- this"
                             " peer is in %016llx",
                 (unsigned)from, (unsigned long long)m.battle_id,
                 (unsigned long long)g.battles.current);
        return;
    }
    // ADOPT MID-BATTLE ARRIVALS FIRST, ON THIS SIDE TOO (2026-09-22).
    //
    // The sender adopts before sweeping, so the host could kill a deathrattle's child
    // on the turn it appeared -- and the client, which applies the hit BY TILE against
    // its OWN roster, could not: the cat was not in it yet, the graze found nothing,
    // and the two ended the turn with the enemy alive on one peer and dead on the
    // other. Reported from play, and it is the same omission as the one the all-enemies
    // button was written to fix, one layer down.
    //
    // adopt_new_cats is the same call a turn boundary makes, and it is safe to make
    // here: it only ever APPENDS, so no index moves and nothing already in flight
    // becomes stale.
    // HITS ARE QUEUED, IN ORDER, AND A HIT THAT FINDS NOTHING WAITS (2026-10-03). The host's boss (ENEMY_MAGNUS) had just moved (7,7)->(6,6); the host hit it there, but the
    // move was still being played on this peer, so the unit was still on its old tile: "0 cat(s) hit here", the boss lived on one peer and died on the other, and the next turn
    // hash halted -- twice, in the same battle. Hit by tile is only meaningful once the board here has caught up, so a miss is retried every frame (debug_hits_pump) for
    // tune::kDebugHitWaitMs, and the hits behind it wait their turn.
    if (g.dbg_n >= State::kDebugQueue) {                       // a flood: land the oldest one as it is, rather than lose the newest
        log_line_lvl(LogLevel::Warn, "LOCKSTEP", "!! debug hit queue full -- the oldest is applied now, wherever it lands");
        g.dbg_waiting = false;
        (void)lockstep_graze_enemy_on_tile(g.dbg_q[0].tx, g.dbg_q[0].ty, g.dbg_q[0].amount);
        memmove(g.dbg_q, g.dbg_q + 1, (State::kDebugQueue - 1) * sizeof(DebugHitMsg));
        memmove(g.dbg_t, g.dbg_t + 1, (State::kDebugQueue - 1) * sizeof(ULONGLONG));
        memmove(g.dbg_from, g.dbg_from + 1, State::kDebugQueue - 1);
        --g.dbg_n;
    }
    g.dbg_q[g.dbg_n] = m; g.dbg_t[g.dbg_n] = GetTickCount64(); g.dbg_from[g.dbg_n] = from; ++g.dbg_n;
    debug_hits_pump();
}

// Land the queued debug hits, oldest first: one that hits is done; one that finds nothing waits (the unit may still be on its way to that tile on this peer) until
// tune::kDebugHitWaitMs, then is dropped with a warning -- the board then differs and the next turn hash says so.
// A HALTED BATTLE IS FINISHED BY STRIKING DOWN THE ENEMIES (see tune::kHaltAutoFinish). Each peer does it for itself: nothing is broadcast (the session is halted, and the two battles no longer agree anyway);
// what both must reach is the same END -- the fight won -- and the game's own victory flow takes it from there. Every kHaltFinishEveryMs: adopt what appeared since (a deathrattle's children), then write
// hp 0 to every enemy-side unit still alive (players' cats, their familiars and summons are never touched). Stops when the battle's character list is gone, which is the game leaving the fight.
static void halt_finish_pump() {
    if (!tune::kHaltAutoFinish || !g.halted || !g.snapped || g.battles.current == kNoBattle) return;
    static uint64_t s_battle = 0;
    static ULONGLONG s_next = 0;
    static uint32_t s_sweeps = 0, s_hit_total = 0;
    static bool s_done = false;
    const ULONGLONG now = GetTickCount64();
    if (s_battle != g.battles.current) { s_battle = g.battles.current; s_next = now + tune::kHaltFinishEveryMs; s_sweeps = s_hit_total = 0; s_done = false; log_line_lvl(LogLevel::Warn, "HALT", "HALT AUTO-FINISH armed for battle %016llx: every %u ms every enemy still standing is struck down, until the battle is won", (unsigned long long)s_battle, (unsigned)tune::kHaltFinishEveryMs); }
    if (s_done || now < s_next) return;
    s_next = now + tune::kHaltFinishEveryMs;
    if (!lockstep_fight_up()) {
        s_done = true;
        log_line_lvl(LogLevel::Warn, "HALT", "HALT AUTO-FINISH over: the battle's character list is gone -- the fight has ended (%u sweep(s), %u unit(s) struck down)", s_sweeps, s_hit_total);
        // THE HALT ENDS WITH THE FIGHT (2026-10-04). The halt was about this battle; left on, it blocked the next node (the checkpoint refuses to enter while the lockstep is halted, and a halted session never
        // snapshots the next battle). Lifted here, on each peer when its own fight is over. The shared stream is put where both peers derive it from the battle's identity: the two had drifted apart, which is what
        // halted the battle, and the node-entry hash compares the stream.
        g.halted = false;
        g.stats.halted = false;
        g.halt_reason[0] = 0;
        g.halt_cleared_for = g.battles.current;
        if (uint64_t* st = rng_global_stream()) {
            uint64_t x = g.battles.current ^ 0x484C544641544552ull, out[4];
            for (int i = 0; i < 4; ++i) { x += 0x9E3779B97F4A7C15ull; uint64_t z = x; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; out[i] = z ^ (z >> 31); }
            if (!(out[0] | out[1] | out[2] | out[3])) out[0] = 1;
            mem_write(st, out, sizeof(out));
        }
        log_line_lvl(LogLevel::Warn, "HALT", "the halt of battle %016llx is LIFTED: the next node can be entered and the next battle is synchronised again; the shared stream was put back to the value both peers derive from the battle", (unsigned long long)g.battles.current);
        return;
    }
    adopt_new_cats();
    uint32_t hit = 0, alive = 0;
    for (uint32_t i = 0; i < g.cat_count; ++i) {
        const void* chr = g.cats[i];
        if (!chr || g.human_cat[i] || !debug_is_enemy(chr)) continue;
        uint8_t dead = 0;
        int32_t hp = 0;
        if (!mem_read((const uint8_t*)chr + kChar_Dead, &dead, 1) || dead || !mem_read((const uint8_t*)chr + kChar_HP, &hp, 4) || hp <= 0) continue;
        ++alive;
        const int32_t zero = 0;
        if (mem_write((uint8_t*)chr + kChar_HP, &zero, 4)) ++hit;
    }
    ++s_sweeps; s_hit_total += hit;
    if (s_sweeps <= 30 || hit) log_line_lvl(LogLevel::Warn, "HALT", "HALT AUTO-FINISH sweep %u: %u enem%s alive, hp 0 written to %u (the game's own death handling takes them)", s_sweeps, alive, alive == 1 ? "y" : "ies", hit);
}

static void debug_hits_pump() {
    while (g.dbg_n) {
        if (!config().dev_tools || !debug_snapshot_ready()) return;
        const DebugHitMsg m = g.dbg_q[0];
        const uint8_t from = g.dbg_from[0];
        const ULONGLONG waited = GetTickCount64() - g.dbg_t[0];
        bool done = true;
        if (m.battle_id != g.battles.current) {
            log_line("LOCKSTEP", "debug hit for battle %016llx dropped -- this peer is in %016llx", (unsigned long long)m.battle_id, (unsigned long long)g.battles.current);
        } else {
            adopt_new_cats();
            const int hit = lockstep_graze_enemy_on_tile(m.tx, m.ty, m.amount);
            if (hit) {
                log_line_lvl(LogLevel::Warn, "LOCKSTEP", "<- DEBUGHIT from peer %u: tile (%d,%d), %d damage -- %d cat(s) hit here%s",
                             (unsigned)from, m.tx, m.ty, m.amount, hit, waited > 50 ? " (after waiting for the unit to arrive on the tile)" : "");
            } else if (waited < tune::kDebugHitWaitMs) {
                if (!g.dbg_waiting) {
                    g.dbg_waiting = true;
                    log_line_lvl(LogLevel::Warn, "LOCKSTEP", "<- DEBUGHIT from peer %u: tile (%d,%d) holds no enemy here yet -- waiting for the board to catch up (up to %u ms)",
                                 (unsigned)from, m.tx, m.ty, (unsigned)tune::kDebugHitWaitMs);
                }
                done = false;
            } else {
                log_line_lvl(LogLevel::Warn, "LOCKSTEP", "<- DEBUGHIT from peer %u: tile (%d,%d), %d damage -- 0 cat(s) hit here", (unsigned)from, m.tx, m.ty, m.amount);
                log_line_lvl(LogLevel::Warn, "LOCKSTEP", "!! that debug hit landed on nothing here even after %u ms -- the two peers now disagree about the board", (unsigned)waited);
            }
        }
        if (!done) return;
        g.dbg_waiting = false;
        memmove(g.dbg_q, g.dbg_q + 1, (State::kDebugQueue - 1) * sizeof(DebugHitMsg));
        memmove(g.dbg_t, g.dbg_t + 1, (State::kDebugQueue - 1) * sizeof(ULONGLONG));
        memmove(g.dbg_from, g.dbg_from + 1, State::kDebugQueue - 1);
        --g.dbg_n;
    }
}

void lockstep_arm_enemy_hit(int32_t amount) {
    if (!config().dev_tools) { g.armed_hit = 0; return; }
    if (amount <= 0) { g.armed_hit = 0; return; }
    g.armed_hit = amount;
    log_line_lvl(LogLevel::Warn, "LOCKSTEP",
                 "!! DEBUG HIT ARMED for %d damage -- LEFT-CLICK an enemy on the board."
                 " Two steps on purpose: a panel button cannot be pressed while the"
                 " mouse is over an enemy, so the target has to be chosen afterwards.",
                 amount);
}

int32_t lockstep_armed_enemy_hit() { return g.armed_hit; }

void lockstep_disarm_enemy_hit() { g.armed_hit = 0; }

int lockstep_graze_enemy_on_tile(int32_t tx, int32_t ty, int32_t amount) {
    // A SILENT ZERO IS THE ONE THING THIS MUST NOT DO (2026-09-22). This function answers
    // "what is at this tile", and its answer comes from the snapshot roster -- so with no
    // session it can only ever miss. It used to say nothing at all about that, which is how
    // a solo player came to click an enemy and watch nothing happen.
    if (!debug_snapshot_ready()) {
        log_line_lvl(LogLevel::Warn, "LOCKSTEP",
                     "!! DEBUG HIT by tile found nothing to look at: this peer is not in a"
                     " battle it has snapshotted, and a tile is resolved against that roster."
                     " Use \"999 damage to EVERY enemy\" instead -- it holds the characters"
                     " themselves and needs no roster.");
        return 0;
    }
    if (amount <= 0) return 0;

    // NOT the roster's index order and NOT a new walk of the character list: this
    // is the list the battle layer itself snapshotted, with the human/AI split it
    // already computed. An enemy is a cat no human brain drives; that is exactly
    // the test the control split uses in the other direction.
    int hit = 0;
    for (uint32_t i = 0; i < g.cat_count; ++i) {
        const void* chr = g.cats[i];
        if (!chr || g.human_cat[i]) continue;

        // The tile lives on the TacticsObject and is only believed when the object
        // points back at this Character -- the same rule read_cat_state applies, and
        // for the same reason: a stale link reads as somebody else's position.
        const void* to = nullptr;
        if (!mem_read((const uint8_t*)chr + kChar_TObj, &to, sizeof(to)) || !to) continue;
        const void* owner = nullptr;
        if (!mem_read((const uint8_t*)to + kTObj_Owner, &owner, sizeof(owner)) || owner != chr)
            continue;
        int32_t tile[2] = {};
        if (!mem_read((const uint8_t*)to + kTObj_Tile, tile, sizeof(tile))) continue;
        if (tile[0] != tx || tile[1] != ty) continue;

        // One write, one place to fix it: debug_apply_hit is the same dword this block used
        // to write inline, and it is now shared with the all-enemies sweep (2026-09-22).
        // The roster index is no longer printed; the tile is, which is what a reader can
        // line up against the board.
        (void)i;
        if (debug_apply_hit(chr, amount, tx, ty)) ++hit;
    }
    return hit;
}


// TEST AID (tune::kTestWeakenEnemies): every enemy to 1 hp at turn 0.
//
// Applied on BOTH peers from the same roster and the same rule, which is the whole
// point -- unlike the debug-hit panel this cannot desync anything, so the session
// stays readable afterwards.
//
// The enemy test is the one the debug hit already uses above: a cat no human brain
// drives. Familiars are also AI, so they are EXCLUDED explicitly, by the run's own
// familiar list -- otherwise this would quietly kill the run's extra cats, which is
// a real loss and not something a test aid should cause.
//
// One 8-byte store per cat, because HP and shield share it: Character::Die zeroes
// +0x4B0 as a QWORD. Writing only the dword would leave a shield to absorb the one
// point of damage this exists to guarantee.
uint32_t weaken_enemies_once() {
    if (!debug_snapshot_ready()) return 0;

    // The familiar ids, so the allies among the AI cats can be left alone.
    uint64_t familiar[64] = {};
    uint32_t familiar_n = 0;
    {
        const void** slot = (const void**)((uintptr_t)GetModuleHandleW(nullptr) + kRva_MewDirectorPtr);
        const void* dir = nullptr;
        if (mem_read(slot, &dir, sizeof(dir)) && dir) {
            uint32_t count = 0;
            const uint64_t* ids = nullptr;
            if (mem_read((const uint8_t*)dir + kDir_CatFamiliars + 4, &count, 4) && count <= 64 &&
                mem_read((const uint8_t*)dir + kDir_CatFamiliars + 8, &ids, sizeof(ids)) && ids) {
                for (uint32_t i = 0; i < count; ++i)
                    if (mem_read(&ids[i], &familiar[familiar_n], sizeof(uint64_t))) ++familiar_n;
            }
        }
    }

    uint32_t done = 0;
    for (uint32_t i = 0; i < g.cat_count; ++i) {
        const void* chr = g.cats[i];
        if (!chr || g.human_cat[i] || !debug_is_enemy(chr)) continue;
        uint8_t dead = 0;
        if (!mem_read((const uint8_t*)chr + kChar_Dead, &dead, 1) || dead) continue;

        if (familiar_n) {
            const void* data = nullptr;
            uint64_t id = 0;
            const bool have_id =
                mem_read((const uint8_t*)chr + kChar_CatData, &data, sizeof(data)) && data &&
                mem_read((const uint8_t*)data + kCatData_SaveId, &id, sizeof(id)) && id;
            bool ally = false;
            for (uint32_t f = 0; have_id && f < familiar_n; ++f)
                if (familiar[f] == id) ally = true;
            if (ally) continue;
        }

        // COST NOTHING WHEN IT IS ALREADY DONE, and that is what makes running this
        // every turn acceptable: a cat already at 1 is one read and no store.
        int32_t hp = 0;
        if (!mem_read((const uint8_t*)chr + kChar_HP, &hp, 4) || hp <= 1) continue;

        const uint64_t one = 1;   // hp = 1, shield = 0, in the one store Die uses
        if (!mem_write((uint8_t*)chr + kChar_HP, &one, sizeof(one))) continue;
        ++done;
    }

    // ONLY DONE IF IT ACTUALLY DID SOMETHING, and that is the fix for the first
    // version of this. The roster is snapshotted DURING the turn, so on the first
    // turn boundary `g.cat_count` can still be 0: the walk found nothing, and the
    // once-flag was set anyway -- so it never tried again and the aid silently did
    // nothing for the whole session. Measured 2026-09-22: enemies at full health, the
    // battle won anyway, and not one TEST AID line in either log.
    //
    // Retrying costs a walk of at most kMaxCats entries per turn and is
    // self-limiting: the first turn whose snapshot holds enemies is the one that
    // counts. Silence is no longer possible either -- the line below fires whenever
    // it works, and a session where it never appears now means this neither ran nor
    // found a roster, which is a thing to look at rather than to assume.
    // EVERY TURN, NOT ONCE PER BATTLE -- because a deathrattle can spawn enemies
    // mid-battle, and those arrive after the first turn. Measured 2026-09-22: a
    // summoned enemy kept full health and could not be targeted either, which is the
    // report that produced this change and the all-enemies button next to it.
    //
    // The once-per-battle flag is gone with it. `done` now counts only cats whose
    // health ACTUALLY changed, so this logs when it does something and is silent when
    // it does not -- which is what the flag was really for.
    if (!done) return 0;
    log_line("LOCKSTEP", "!! TEST AID: weakened %u enemy cat(s) to 1 hp at this battle's"
                         " first usable turn. Applied on BOTH peers from the same roster"
                         " and the same rule, so this does NOT desync anything -- the"
                         " session stays readable, which the panel's one-sided hit cannot"
                         " say. Set tune::kTestWeakenEnemies = false to play properly.",
             done);
    return done;
}

// ---- THE HOST'S BOARD (proto 61) ----------------------------------------------------------------------------------------------------------------
//
// Every peer builds the battle from its OWN save, and whatever that build reads from per-save data (a pickup's roll, a corpse's pick, a placement
// draw) can come out differently: measured 2026-10-02 (t10), 34 of 36 units equal and two coins on other tiles -- a turn-0 halt on the rng and the
// state hash. The shared stream is reset at the battle build and at every definition load (mgmp_unlocks) and the level is forced, which removed the
// big divergences, but a single unread per-save input is enough to leave the boards apart. So the first turn boundary of a battle carries a SNAPSHOT:
//
//   host    -> every NON-PLAYER unit of its roster (index, kind, hp / max hp / shield, tile, facing) and the state of the shared simulation stream;
//   client  -> waits for it (the host builds first and follows no one, so it is normally already here), overwrites its own units with it, and puts
//              its stream where the host's is. From that point both peers draw the same sequence, so the enemies' own decisions are the host's too.
//
// Nothing is guessed: a unit whose kind (Character+0x248) differs from the host's at that index cannot be repaired by overwriting numbers and is
// reported, not touched; a roster of another size is reported and nothing but the stream is taken over. Tiles are moved with the game's own
// TacticsObject::Move (the call the move preview uses, bracketed the way the game brackets it) and only onto a tile no other unit stands on, in
// passes, so a chain of moves resolves and a swap (two units wanting each other's tile) is reported instead of forced.
namespace {
constexpr ULONGLONG kBoardWaitMs = tune::kBoardWaitMs;
constexpr uintptr_t kChar_Guard  = 0xEC0;   // the re-entrancy counter the game raises around a Move (see mgmp_aim)
constexpr uintptr_t kChar_Type   = 0x248;   // std::string, the authored type name
constexpr uintptr_t kChar_InitA  = 0x954;   // the turn-order sort's primary key (2*speed + bonus; recompute_stats 0x140102C37)
constexpr uintptr_t kChar_InitB  = 0x958;   // its tie-break: a random number drawn when the character is created (sub_1400F1150 / sub_1400F70F0)
constexpr uintptr_t kChar_InitBase = 0x5DC; // the initiative base set once at creation (base_initiative + the random initiative_variation); +0x954 = 2*speed + this
constexpr uintptr_t kChar_Speed  = 0x5CC;   // the speed stat (stats start at +0x5BC: str dex con int spd cha lck)

uint32_t board_ident(const void* chr) {
    char type[96] = {};
    if (!chr || !mem_read_std_string((const uint8_t*)chr + kChar_Type, type, sizeof(type)) || !type[0]) return 0;
    const uint64_t h = fnv1a(type, strlen(type));
    return (uint32_t)(h ^ (h >> 32)) | 1u;   // never 0: 0 means "unreadable"
}

typedef void (__fastcall *fn_board_move)(void* tobj, uint64_t tile, char a, char b);
typedef void (__fastcall *fn_board_recompute)(void* self, void* ability, char flag);

// POD-only on purpose (an SEH frame cannot hold objects with destructors). True when the unit ends on the tile.
bool board_move_unit(const void* chr, int32_t x, int32_t y) {
    const uintptr_t mv = addr_of_call(C_TacticsMove);
    const uintptr_t rc = addr_of_call(C_RecomputeStats);
    if (!mv || !chr) return false;
    const void* tobj = nullptr;
    if (!mem_read((const uint8_t*)chr + kChar_TObj, &tobj, sizeof(tobj)) || !tobj) return false;
    const uint64_t packed = ((uint64_t)(uint32_t)y << 32) | (uint32_t)x;   // iVec2D {x, y} by value
    int32_t guard = 0;
    mem_read((const uint8_t*)chr + kChar_Guard, &guard, sizeof(guard));
    __try {
        int32_t up = guard + 1;
        mem_write((uint8_t*)chr + kChar_Guard, &up, sizeof(up));
        ((fn_board_move)mv)((void*)tobj, packed, 1, 0);
        if (rc) ((fn_board_recompute)rc)((void*)chr, nullptr, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    mem_write((uint8_t*)chr + kChar_Guard, &guard, sizeof(guard));
    int32_t now[2] = {};
    return mem_read((const uint8_t*)tobj + kTObj_Tile, now, sizeof(now)) && now[0] == x && now[1] == y;
}

// HOST: capture and send. The chunks are kept for a peer that joins this battle later (lockstep_catchup).
void board_publish() {
    if (g.cat_count == 0 || g.battles.current == kNoBattle) return;
    uint32_t idx[kMaxCats];
    uint32_t total = 0;
    for (uint32_t i = 0; i < g.cat_count; ++i) idx[total++] = i;      // EVERY unit (proto 69): the players' cats carry their turn-order keys too, flagged kBoardHuman
    uint64_t rng[4] = {};
    if (const uint64_t* s = rng_global_stream()) mem_read(s, rng, sizeof(rng));

    // A UNIT THE BATTLE HAS DROPPED IS A FREED OBJECT (see snapshot_membership): nothing may be read off it for a state row, and above all the game must not be called on it -- read_cat_state's
    // in_battle=true runs Character::get_affecting_elements, which clears a deferred-status list, i.e. WRITES. 2026-10-03: the host crashed twice in the draw / a virtual call on a freed
    // object, after 1951 swallowed faults from that very call; board_publish ran it over the whole roster at every turn boundary. Gone units are sent flagged and empty.
    uint32_t live_n = 0, appeared_n = 0;
    bool still_n[kMaxCats];
    const bool have_memb = snapshot_membership(g.snapped_list, live_n, still_n, appeared_n);

    g.board_sent_n = 0;
    uint32_t first = 0;
    do {
        BoardMsg& m = g.board_sent[g.board_sent_n];
        m = BoardMsg{};
        m.battle = g.battles.current; m.turn = g.turn; m.cats = g.cat_count; m.total = total; m.first = first;
        memcpy(m.rng, rng, sizeof(rng));
        m.count = (uint8_t)((total - first) < kBoardChunk ? (total - first) : kBoardChunk);
        for (uint32_t k = 0; k < m.count; ++k) {
            const uint32_t i = idx[first + k];
            BoardUnit& u = m.units[k];
            if (have_memb && !still_n[i]) {
                u = BoardUnit{};
                u.index = (uint8_t)i; u.flags = kBoardGone;
                continue;
            }
            CatState st{};
            read_cat_state(g.cats[i], st, false);       // plain reads: the elements the call would add are not part of the board
            u.index = (uint8_t)i; u.ident = board_ident(g.cats[i]);
            u.hp = st.hp; u.shield = st.shield; u.maxhp = st.maxhp;
            u.flags = (uint8_t)((st.dead ? kBoardDead : 0) | (st.linked ? kBoardLinked : 0) | (g.human_cat[i] ? kBoardHuman : 0) | (unit_is_champion(g.cats[i]) ? kBoardChampion : 0)
                                   | (!st.dead && unit_is_removed(g.cats[i]) ? kBoardRemoved : 0));       // taken off the board, not dead: a client holding it alive takes it off too
            if (g.human_cat[i]) {                          // proto 77: a player's stats ride along (the experiment, and the record)
                for (int k = 0; k < 7; ++k) mem_read((const uint8_t*)g.cats[i] + 0x5BC + k * 4, &u.stat[k], 4);
                mem_read((const uint8_t*)g.cats[i] + 0x5E8, &u.stat_bonus, 4);
            }
            if (!g.human_cat[i]) unit_definition_name(g.cats[i], u.def, kBoardDefLen);   // proto 75: what a peer holding another kind here replaces its unit with
            u.tx = st.tx; u.ty = st.ty; u.fx = st.fx; u.fy = st.fy;
            mem_read((const uint8_t*)g.cats[i] + kChar_InitA, &u.key_a, 4); mem_read((const uint8_t*)g.cats[i] + kChar_InitB, &u.key_b, 4); mem_read((const uint8_t*)g.cats[i] + kChar_Speed, &u.speed, 4); mem_read((const uint8_t*)g.cats[i] + kChar_InitBase, &u.init_base, 4);
        }
        first += m.count;
        ++g.board_sent_n;
    } while (first < total && g.board_sent_n < sizeof(g.board_sent) / sizeof(g.board_sent[0]));
    g.board_sent_for = g.battles.current;

    uint32_t sent = 0;
    for (uint32_t c = 0; c < g.board_sent_n; ++c) if (net_send_board(g.board_sent[c])) ++sent;
    if (g.turn == 0) log_line("BOARD", "-> the host's board for battle %016llx: %u unit(s) (players' cats carry only their turn-order keys) of %u in %u chunk(s) (%u sent), stream %016llx -- the clients take it over at their first turn boundary",
             (unsigned long long)g.battles.current, total, g.cat_count, g.board_sent_n, sent, (unsigned long long)rng[0]);
}

// THE DERIVED VALUES OF EVERY UNIT, as one digest each (mgmp_diag: deep_publish), taken at the boundary before the board repairs anything: the stats, speed, turn-order keys and max hp the state hash does not cover.
void deep_boundary() {
    if (!g.snapped || g.cat_count == 0 || g.battles.current == kNoBattle) return;
    DeepRow rows[kDeepUnits];
    const uint32_t n = g.cat_count < kDeepUnits ? g.cat_count : kDeepUnits;
    for (uint32_t i = 0; i < n; ++i) {
        CatState st{};
        if (!g.cats[i] || !read_cat_state(g.cats[i], st, false) || !st.readable || !st.in_battle || st.dead) continue;      // gone or dead: its memory may be reused, nothing to compare
        const uint8_t* c = (const uint8_t*)g.cats[i];
        DeepRow& r = rows[i];
        bool ok = mem_read(c + kChar_Speed, &r.speed, 4) && mem_read(c + kChar_InitA, &r.key_a, 4) && mem_read(c + kChar_InitB, &r.key_b, 4) && mem_read(c + kChar_InitBase, &r.init_base, 4);
        for (int k = 0; k < 7; ++k) ok = mem_read(c + 0x5BC + k * 4, &r.stat[k], 4) && ok;
        ok = mem_read(c + 0x5E8, &r.bonus, 4) && ok;
        r.maxhp = st.maxhp;
        r.ok = ok;
    }
    deep_publish(g.battles.current, g.turn, rows, n);
}

// CLIENT: take the host's board over.
void board_apply(const BoardAssembled& b, bool quiet) {
    uint32_t written = 0, moved = 0, same = 0, bad = 0, stuck = 0, deadnote = 0, keys_differ = 0, killed = 0, replaced = 0, spawned = 0, trimmed = 0;
    char said[16][200]; uint32_t nsaid = 0;
    auto say = [&](const char* fmt, ...) {
        if (nsaid >= 16) return;
        va_list ap; va_start(ap, fmt);
        _vsnprintf_s(said[nsaid], sizeof(said[nsaid]), _TRUNCATE, fmt, ap);
        va_end(ap); ++nsaid;
    };

    // Which units are still in THIS peer's live list: a departed one is a freed object, so it is neither read nor written nor called (see board_publish).
    uint32_t live_a = 0, appeared_a = 0, gone_skipped = 0;
    bool still_a[kMaxCats];
    const bool have_memb = snapshot_membership(g.snapped_list, live_a, still_a, appeared_a);
    auto here_present = [&](uint32_t i) { return !have_memb || (i < kMaxCats && still_a[i]); };

    // HOST-AUTHORITATIVE UNIT SET (2026-10-03, proto 75 + kBoardRemoved): the host's board says WHICH units exist, not only how the ones both peers have are doing. Where the rosters differ in
    // size this peer first makes its roster the host's:
    //   * entries past the host's roster are units only THIS peer made (a summon one side's sim produced, a dev spawn): taken off the board silently and dropped from the roster -- they are
    //     remembered as retired, so the live list still holding them is not taken for new arrivals;
    //   * entries the host has and this peer has not get a placeholder here, and the per-unit pass below makes the unit from the host's definition name (the same path that makes a unit the host
    //     has and this peer's copy of is gone or dead).
    auto here_alive = [&](uint32_t i) {
        if (i >= g.cat_count || !g.cats[i] || !here_present(i)) return false;
        CatState s{};
        return read_cat_state(g.cats[i], s, false) && !s.dead && s.hp > 0 && !unit_is_removed(g.cats[i]);
    };
    auto tile_taken = [&](int32_t x, int32_t y) {
        for (uint32_t j = 0; j < g.cat_count; ++j) {
            CatState o{};
            if (g.cats[j] && here_present(j) && read_cat_state(g.cats[j], o, false) && o.linked && !unit_is_removed(g.cats[j]) && o.tx == x && o.ty == y && !unit_is_pickup(g.cats[j])) return true;
        }
        return false;
    };
    // Make the host's unit: on its tile when free, else on the free tile nearest to it (the move pass puts it right once the unit standing there has moved on).
    auto spawn_unit = [&](const BoardUnit& u, const char*& why) -> void* {
        const void* like = nullptr;
        for (uint32_t j = 0; j < g.cat_count && !like; ++j) if (here_alive(j)) like = g.cats[j];
        int32_t w = 0, h = 0;
        if (!like || !g.tc || !unit_board_size(like, w, h)) { why = "no live unit here to anchor the spawn on"; return nullptr; }
        int32_t bx = u.tx, by = u.ty, best = 0x7FFFFFFF;
        const bool host_tile_ok = u.tx >= 0 && u.ty >= 0 && u.tx < w && u.ty < h && !tile_taken(u.tx, u.ty);
        if (!host_tile_ok) {
            for (int32_t y = 0; y < h; ++y) for (int32_t x = 0; x < w; ++x) {
                const int32_t d = (x - u.tx) * (x - u.tx) + (y - u.ty) * (y - u.ty);
                if (d < best && !tile_taken(x, y)) { best = d; bx = x; by = y; }
            }
            if (best == 0x7FFFFFFF) { why = "no free tile on the board"; return nullptr; }
        }
        const int32_t keys[3] = { u.key_a, u.key_b, u.init_base };      // the host's, so the unit takes the host's place in the turn order
        return unit_spawn_at(g.tc, like, u.def, bx, by, keys, why);
    };

    if (g.cat_count > b.cats) {
        bool humans = false;
        for (uint32_t j = b.cats; j < g.cat_count; ++j) if (g.human_cat[j]) humans = true;
        if (humans) say("this roster has %u unit(s) to the host's %u and one past the host's is a player's -- not trimmed", g.cat_count, b.cats);
        else {
            for (uint32_t j = b.cats; j < g.cat_count; ++j) {
                const void* chr = g.cats[j];
                char kind[64] = {};
                unit_definition_name(chr, kind, sizeof(kind));
                if (here_alive(j)) {
                    if (unit_remove_silent(const_cast<void*>(chr))) { ++trimmed; say("unit %u ('%s') exists only here -- taken off the board", j, kind); }
                    else say("unit %u ('%s') exists only here and could not be taken off the board", j, kind);
                }
                if (chr && here_present(j)) retire_unit(chr);
                g.cats[j] = nullptr; g.human_cat[j] = false; g.local_cat[j] = false; g.summon[j] = false; g.cat_key_ok[j] = false; g.cat_key[j] = 0;
                if (j < kMaxCats) { still_a[j] = false; g.fresh_left[j] = 0; }
            }
            g.cat_count = b.cats;
        }
    }
    while (g.cat_count < b.cats && g.cat_count < kMaxCats) {                       // the host's units this peer has no entry for: a placeholder, made by the per-unit pass
        const uint32_t i = g.cat_count;
        g.cats[i] = nullptr; g.human_cat[i] = false; g.local_cat[i] = false; g.summon[i] = true; g.cat_key_ok[i] = false; g.cat_key[i] = 0;
        still_a[i] = false;
        ++g.cat_count;
    }

    if (b.cats != g.cat_count) {
        log_line_lvl(LogLevel::Error, "BOARD", "!! the host's roster has %u unit(s), this one %u -- the boards cannot be matched; only the stream is taken over", b.cats, g.cat_count);
    } else {
        struct Want { uint32_t i; int32_t x, y; };
        Want want[kMaxCats]; uint32_t nwant = 0;
        for (uint32_t k = 0; k < b.total; ++k) {
            const BoardUnit& u = b.unit[k];
            const uint32_t i = u.index;
            const bool host_human = (u.flags & kBoardHuman) != 0;
            if (i >= g.cat_count) { ++bad; say("unit %u is out of range here", i); continue; }
            const bool host_gone = (u.flags & kBoardGone) != 0, host_removed = (u.flags & kBoardRemoved) != 0, host_dead = (u.flags & kBoardDead) != 0;
            if (host_human || g.human_cat[i]) {
                if (host_gone || !here_present(i)) {
                    ++gone_skipped;      // gone on the host, or gone here: the object is freed on that side, nothing of it is to be matched or touched
                    if (!host_gone) continue;
                    if (here_present(i)) say("unit %u has left the host's battle and is still in this one -- left alone (the game removes it itself)", i);
                    continue;
                }
            } else if (host_gone || host_removed) {
                // the host no longer has this unit on its board (gone from its list, or taken off silently); one still standing here, alive, is taken off the same way
                ++gone_skipped;
                if (here_alive(i)) {
                    char kind[64] = {};
                    unit_definition_name(g.cats[i], kind, sizeof(kind));
                    if (unit_remove_silent(const_cast<void*>(g.cats[i]))) { ++trimmed; say("unit %u ('%s') is %s on the host and alive here -- taken off the board", i, kind, host_gone ? "gone" : "removed"); }
                    else { ++bad; say("unit %u ('%s') is %s on the host and alive here -- it could not be taken off", i, kind, host_gone ? "gone" : "removed"); }
                }
                continue;
            } else if (!host_dead) {
                // alive on the host's board: a peer whose copy is gone, dead or taken off makes it again from the host's definition
                bool need = !here_present(i) || !g.cats[i];
                if (!need) { CatState s{}; need = read_cat_state(g.cats[i], s, false) && (s.dead || unit_is_removed(g.cats[i])); }
                if (need) {
                    const bool host_on = (u.flags & kBoardLinked) && !(u.tx == kTileOffBoard && u.ty == kTileOffBoard);
                    if (!host_on || !u.def[0]) { ++bad; say("unit %u is gone here and alive on the host, which sent no %s -- not made", i, !u.def[0] ? "definition name" : "tile"); continue; }
                    const char* why = nullptr;
                    void* made = spawn_unit(u, why);
                    if (!made) { ++bad; say("unit %u ('%s' on the host) is gone here and could not be made: %s", i, u.def, why ? why : "?"); continue; }
                    if (g.cats[i] && here_present(i)) retire_unit(g.cats[i]);
                    g.cats[i] = made;
                    note_ident(i);
                    if (i < kMaxCats) { still_a[i] = true; g.fresh_left[i] = State::kFreshTurns; }
                    ++spawned;
                    say("unit %u was missing here and is now the host's '%s' (made with the game's spawn)", i, u.def);
                }
            } else if (!here_present(i)) { ++gone_skipped; continue; }
            if (host_human != g.human_cat[i]) { ++bad; say("unit %u is a player's cat on one peer only (host %d, here %d) -- left alone", i, (int)host_human, (int)g.human_cat[i]); continue; }
            uint32_t mine = board_ident(g.cats[i]);
            if (mine != u.ident) {
                // HOST-AUTHORITATIVE KIND (proto 75): a unit of another kind than the host's at this index (2026-10-03: a pickup rolled differently -- 'unit 54 is another kind here (3aeecded) than on the host
                // (d4b86345)', a turn-18 halt) cannot be repaired by overwriting numbers, so it is REPLACED by the game's own transform with the host's definition. Only where that is safe: a unit
                // the host named, of the same champion state, that is not a player's cat; anything else stays what it was and the turn hash says whether it matters.
                char here_def[kBoardDefLen] = {};
                const bool have_here = unit_definition_name(g.cats[i], here_def, sizeof(here_def));
                const bool champ_same = unit_is_champion(g.cats[i]) == ((u.flags & kBoardChampion) != 0);
                if (!u.def[0] || !have_here || !champ_same || strcmp(here_def, u.def) == 0) {
                    ++bad; say("unit %u is another kind here (%08x '%s') than on the host (%08x '%s'%s) -- not replaceable, left alone", i, mine, here_def, u.ident, u.def, champ_same ? "" : ", champion state differs"); continue;
                }
                const char* why = nullptr;
                void* made = unit_transform_to(const_cast<void*>(g.cats[i]), u.def, why);
                if (!made) { ++bad; say("unit %u '%s' could not be replaced by the host's '%s': %s", i, here_def, u.def, why ? why : "?"); continue; }
                retire_unit(g.cats[i]);
                g.cats[i] = made;
                int32_t nk = 0;
                g.cat_key_ok[i] = mem_read((const uint8_t*)made + kChar_Ident, &nk, 4);
                g.cat_key[i] = (uint32_t)nk;
                if (i < kMaxCats) still_a[i] = true;
                mine = board_ident(made);
                ++replaced;
                say("unit %u was a '%s' and is now the host's '%s' (replaced by the game's transform; display key %08x%s)", i, here_def, u.def, mine, mine == u.ident ? "" : " -- STILL DIFFERENT from the host's");
                if (mine != u.ident) { ++bad; continue; }
            }
            {   // THE TURN-ORDER KEYS, for every unit (players' cats too): the next turn's order is a shuffle plus a sort on them
                uint8_t* c = (uint8_t*)g.cats[i];
                int32_t ka = 0, kb = 0, sp = 0, ib = 0;
                mem_read(c + kChar_InitA, &ka, 4); mem_read(c + kChar_InitB, &kb, 4); mem_read(c + kChar_Speed, &sp, 4); mem_read(c + kChar_InitBase, &ib, 4);
                if (ka != u.key_a || kb != u.key_b || ib != u.init_base) {
                    ++keys_differ;
                    say("unit %u turn-order keys here %d/%d (speed %d, base %d), the host's %d/%d (speed %d, base %d) -- the host's taken", i, ka, kb, sp, ib, u.key_a, u.key_b, u.speed, u.init_base);
                    // the base first: +0x954 is derived from it, and the game recomputes it from it whenever the unit's stats are touched
                    mem_write(c + kChar_InitBase, &u.init_base, 4); mem_write(c + kChar_InitA, &u.key_a, 4); mem_write(c + kChar_InitB, &u.key_b, 4);
                    if (g.cat_key_ok[i]) g.cat_key[i] = (uint32_t)u.key_b;      // the entry's identity is its creation key: it follows the host's value
                } else if (sp != u.speed) say("unit %u speed here %d, the host's %d (keys equal)", i, sp, u.speed);
            }
            if (host_human) {
                // DIAGNOSIS ONLY, nothing is overwritten: a player's cat is built on each peer from the same bytes and is NOT repaired here (its stats are inputs, and writing max hp
                // alone would be undone by the next recompute_stats). A differing value is the earliest sign a cat was built differently -- 2026-10-03, Kylo: stats [5 5 5 8 6 6 6]
                // on the host, [5 3 4 10 6 8 8] here, same 944 bytes -- so say WHICH unit and WHICH values, once per change, instead of leaving it to the hash's halt.
                CatState hs{};
                if (!read_cat_state(g.cats[i], hs, false)) continue;
                int32_t sp = 0;
                mem_read((const uint8_t*)g.cats[i] + kChar_Speed, &sp, 4);
                const bool dif_hp = hs.hp != u.hp, dif_max = hs.maxhp != u.maxhp, dif_sh = hs.shield != u.shield, dif_sp = sp != u.speed;
                const bool dif_pos = (u.flags & kBoardLinked) && hs.linked && (hs.tx != u.tx || hs.ty != u.ty);
                if (dif_hp || dif_max || dif_sh || dif_sp || dif_pos) {
                    const uint64_t sig = ((uint64_t)(uint32_t)hs.hp << 48) ^ ((uint64_t)(uint32_t)hs.maxhp << 32) ^ ((uint64_t)(uint32_t)hs.shield << 24) ^ ((uint64_t)(uint32_t)sp << 16)
                                         ^ ((uint64_t)(uint32_t)u.hp * 0x9E3779B1u) ^ ((uint64_t)(uint32_t)u.maxhp * 0x85EBCA6Bu) ^ ((uint64_t)(uint32_t)u.speed << 8) ^ ((uint64_t)(uint32_t)hs.tx * 31u + (uint32_t)hs.ty) ^ ((uint64_t)(uint32_t)u.tx * 131u + (uint32_t)u.ty);
                    static uint64_t last_sig[kMaxCats] = {};
                    static uint64_t last_battle = 0;
                    static uint32_t last_turn = 0;
                    if (b.battle != last_battle || b.turn < last_turn) { memset(last_sig, 0, sizeof(last_sig)); last_battle = b.battle; }
                    last_turn = b.turn;
                    if (last_sig[i] != sig) {
                        last_sig[i] = sig;
                        const uint8_t* c = (const uint8_t*)g.cats[i];
                        int32_t stat[7] = {}, bonus = 0;
                        bool ok = true;
                        for (int k = 0; k < 7; ++k) ok = mem_read(c + 0x5BC + k * 4, &stat[k], 4) && ok;
                        mem_read(c + 0x5E8, &bonus, 4);
                        uint64_t cid = 0; const void* cd = nullptr;
                        if (mem_read(c + kChar_CatData, &cd, sizeof(cd)) && cd) mem_read((const uint8_t*)cd + kCatData_SaveId, &cid, sizeof(cid));
                        char what[160]; what[0] = 0; size_t n = 0;
                        auto add = [&](const char* fmt, ...) {
                            if (n >= sizeof(what) - 1) return;
                            va_list ap; va_start(ap, fmt);
                            const int w = _vsnprintf_s(what + n, sizeof(what) - n, _TRUNCATE, fmt, ap);
                            va_end(ap); if (w > 0) n += (size_t)w;
                        };
                        if (dif_max) add(" maxhp %d vs %d;", hs.maxhp, u.maxhp);
                        if (dif_hp)  add(" hp %d vs %d;", hs.hp, u.hp);
                        if (dif_sh)  add(" shield %d vs %d;", hs.shield, u.shield);
                        if (dif_sp)  add(" speed %d vs %d;", sp, u.speed);
                        if (dif_pos) add(" tile (%d,%d) vs (%d,%d);", hs.tx, hs.ty, u.tx, u.ty);
                        // maxhp = max(1, 4*con + bonus): when the bonus term is the same on both sides, the host's constitution is what its max hp implies
                        const int32_t host_con = (u.maxhp - bonus) / 4;
                        log_line_lvl(LogLevel::Warn, "BOARD", "!! PLAYER CAT %u differs from the host's (turn %u, here vs host):%s this peer: stats [%d %d %d %d %d %d %d] bonus %d, catdata id %016llX; the host's max hp %d implies con %d here %d -- hit points, shield, tile and facing are put right by this board (layer 1); the stats are not",
                                     i, b.turn, what, stat[0], stat[1], stat[2], stat[3], stat[4], stat[5], stat[6], bonus, (unsigned long long)cid, u.maxhp, host_con, stat[2]);
                        (void)ok;
                    }
                }
                if (g.exp_stats) {
                    // LAYER 2, THE EXPERIMENT: this peer's cat's stats and max hp are written to the host's, and the log says what the game then does with them. Armed only by the dev button.
                    uint8_t* cc = (uint8_t*)g.cats[i];
                    int32_t now[7] = {}, nb = 0, nmax = 0;
                    for (int k = 0; k < 7; ++k) mem_read(cc + 0x5BC + k * 4, &now[k], 4);
                    mem_read(cc + 0x5E8, &nb, 4); mem_read(cc + kChar_MaxHP, &nmax, 4);
                    bool differs = nb != u.stat_bonus || nmax != u.maxhp;
                    for (int k = 0; k < 7; ++k) if (now[k] != u.stat[k]) differs = true;
                    if (differs) {
                        for (int k = 0; k < 7; ++k) mem_write(cc + 0x5BC + k * 4, &u.stat[k], 4);
                        mem_write(cc + 0x5E8, &u.stat_bonus, 4); mem_write(cc + kChar_MaxHP, &u.maxhp, 4);
                        log_line_lvl(LogLevel::Warn, "STATS", "!! EXPERIMENT: player cat %u stats [%d %d %d %d %d %d %d] bonus %d max hp %d -> the host's [%d %d %d %d %d %d %d] bonus %d max hp %d WRITTEN",
                                     i, now[0], now[1], now[2], now[3], now[4], now[5], now[6], nb, nmax, u.stat[0], u.stat[1], u.stat[2], u.stat[3], u.stat[4], u.stat[5], u.stat[6], u.stat_bonus, u.maxhp);
                        bool have = false; for (auto& w : g.sw) if (w.left > 0 && w.cat == i) have = true;
                        if (!have) for (auto& w : g.sw) if (w.left <= 0) { w.cat = i; w.left = 4; break; }
                    }
                }
                if (!tune::kBoardRepairPlayerCats) continue;
                // LAYER 1 (2026-10-04, b521766e: a player's summon took 10 damage and a push on the host and none here, the board left it, turn 6 halted): below, the hit points, shield, tile and
                // facing are overwritten like any unit's -- they are results of the battle, not inputs -- and the max hit points are not (see the write below).
            }
            CatState st{};
            if (!read_cat_state(g.cats[i], st, false)) { ++bad; say("unit %u is unreadable here", i); continue; }
            if (!host_human && !st.dead && host_dead && b.turn > 0 && unit_remove_game_way(const_cast<void*>(g.cats[i]))) {
                // DEAD ON THE HOST, ALIVE HERE (2026-10-03, host's 'remove (game way)'): the host's unit is dead and off the board in the very boundary this board comes from, so this one is put through the
                // same sequence now -- a lethal hit is only finished a beat later (it stood on its tile, not dead, elements still read, at the hash: a halt). Before any write: Die wants the unit as it is.
                ++killed;
                say("unit %u is dead on the host and alive here -- put through the game's own delete sequence (Die, OnCorpsePop, Remove)", i);
                continue;
            }
            uint8_t* c = (uint8_t*)g.cats[i];
            bool changed = false;
            if (st.hp != u.hp)         { mem_write(c + kChar_HP,     &u.hp,     4); changed = true; }
            if (st.shield != u.shield) { mem_write(c + kChar_Shield, &u.shield, 4); changed = true; }
            if (st.maxhp != u.maxhp && !host_human) { mem_write(c + kChar_MaxHP,  &u.maxhp,  4); changed = true; }      // a player's max hp is derived from its stats: not written
            if (st.fx != u.fx || st.fy != u.fy) {
                // through the game's own turn, so the sprite follows the field (see turn_cat_via_face); an off-board unit has no facing worth turning
                if (u.fx >= -1 && u.fx <= 1 && u.fy >= -1 && u.fy <= 1 && (u.fx || u.fy)) turn_cat_via_face(c, u.fx, u.fy);
                else { mem_write(c + kChar_Facing, &u.fx, 4); mem_write(c + kChar_Facing + 4, &u.fy, 4); }
            }
            if ((st.dead != 0) != ((u.flags & kBoardDead) != 0)) {
                ++deadnote;
                if (!st.dead && (u.flags & kBoardDead) && b.turn > 0 && debug_apply_hit(g.cats[i], 1000000, st.tx, st.ty)) {
                    ++killed;        // dead on the host, alive here: the game's own death handling takes it from a lethal hit, as with a debug hit
                    say("unit %u is dead on the host and alive here -- killed (the game's death handling does the rest)", i);
                } else if (host_human && !st.dead && (u.flags & kBoardDead)) say("a player's cat %u is dead on the host and alive here -- its hit points are written (%d), the game's own death handling takes it from there", i, u.hp);
                else say("unit %u is %s here but %s on the host -- the flag is not rewritten", i, st.dead ? "dead" : "alive", (u.flags & kBoardDead) ? "dead" : "alive");
            }
            if (changed) { ++written; say("unit %u%s: hp %d/%d shield %d -> host's %d/%d shield %d", i, host_human ? " (a player's cat)" : "", st.hp, st.maxhp, st.shield, u.hp, host_human ? st.maxhp : u.maxhp, u.shield); }
            const bool host_on = (u.flags & kBoardLinked) && !(u.tx == kTileOffBoard && u.ty == kTileOffBoard);
            const bool here_on = st.linked && !(st.tx == kTileOffBoard && st.ty == kTileOffBoard);
            if (host_on && here_on && (st.tx != u.tx || st.ty != u.ty)) want[nwant++] = Want{ i, u.tx, u.ty };
            else if (!changed) ++same;
        }
        // The moves, in passes: a unit goes only where no other unit stands NOW.
        for (int pass = 0; pass < 8 && nwant; ++pass) {
            uint32_t left = 0, did = 0;
            for (uint32_t w = 0; w < nwant; ++w) {
                bool taken = false;
                for (uint32_t j = 0; j < g.cat_count && !taken; ++j) {
                    if (j == want[w].i) continue;
                    CatState o{};
                    if (here_present(j) && read_cat_state(g.cats[j], o, false) && o.linked && o.tx == want[w].x && o.ty == want[w].y && !unit_is_pickup(g.cats[j])) taken = true;     // a pickup shares its tile with a unit
                }
                if (taken) { want[left++] = want[w]; continue; }
                CatState was{};
                read_cat_state(g.cats[want[w].i], was, false);
                if (board_move_unit(g.cats[want[w].i], want[w].x, want[w].y)) {
                    ++moved; ++did;
                    say("unit %u moved (%d,%d) -> the host's (%d,%d)", want[w].i, was.tx, was.ty, want[w].x, want[w].y);
                } else { ++stuck; say("unit %u could not be moved to (%d,%d)", want[w].i, want[w].x, want[w].y); }
            }
            nwant = left;
            if (!did) break;
        }
        for (uint32_t w = 0; w < nwant; ++w) { ++stuck; say("unit %u wants (%d,%d), which another unit holds -- left where it is", want[w].i, want[w].x, want[w].y); }
    }

    for (auto& w : g.sw) {      // LAYER 2: what the game does with the written stats, boundary after boundary (the last of the four forces a recompute)
        if (w.left <= 0 || w.cat >= g.cat_count || !g.cats[w.cat]) { w.left = 0; continue; }
        const uint8_t* cc = (const uint8_t*)g.cats[w.cat];
        int32_t st7[7] = {}, bn = 0, mx = 0, hp = 0, ia = 0;
        for (int k = 0; k < 7; ++k) mem_read(cc + 0x5BC + k * 4, &st7[k], 4);
        mem_read(cc + 0x5E8, &bn, 4); mem_read(cc + kChar_MaxHP, &mx, 4); mem_read(cc + kChar_HP, &hp, 4); mem_read(cc + kChar_InitA, &ia, 4);
        log_line_lvl(LogLevel::Warn, "STATS", "EXPERIMENT watch (%d boundaries left): player cat %u stats [%d %d %d %d %d %d %d] bonus %d max hp %d hp %d key %d", w.left, w.cat, st7[0], st7[1], st7[2], st7[3], st7[4], st7[5], st7[6], bn, mx, hp, ia);
        if (w.left == 1) {
            const uintptr_t rc = addr_of_call(C_RecomputeStats);
            if (rc) {
                __try { ((fn_board_recompute)rc)((void*)cc, nullptr, 1); } __except (EXCEPTION_EXECUTE_HANDLER) {}
                int32_t a7[7] = {}, ab = 0, am = 0;
                for (int k = 0; k < 7; ++k) mem_read(cc + 0x5BC + k * 4, &a7[k], 4);
                mem_read(cc + 0x5E8, &ab, 4); mem_read(cc + kChar_MaxHP, &am, 4);
                log_line_lvl(LogLevel::Warn, "STATS", "EXPERIMENT recompute probe on player cat %u: stats [%d %d %d %d %d %d %d] bonus %d max hp %d -> after the game's own recompute [%d %d %d %d %d %d %d] bonus %d max hp %d -- %s",
                             w.cat, st7[0], st7[1], st7[2], st7[3], st7[4], st7[5], st7[6], bn, mx, a7[0], a7[1], a7[2], a7[3], a7[4], a7[5], a7[6], ab, am,
                             (!memcmp(st7, a7, sizeof(st7)) && bn == ab && mx == am) ? "the written values SURVIVE a recompute" : "a recompute CHANGES them (it re-derives them from something else)");
            }
        }
        --w.left;
    }
    uint64_t before = 0;
    uint64_t* s = rng_global_stream();
    if (s) {
        mem_read(s, &before, sizeof(before));
        if (!mem_write(s, b.rng, sizeof(b.rng))) log_line_lvl(LogLevel::Error, "BOARD", "!! the shared stream could not be set");
    }
    (void)gone_skipped;
    ++g.bs.board_applied;
    g.bs.rewritten += written; g.bs.moved += moved; g.bs.replaced += replaced; g.bs.spawned += spawned; g.bs.trimmed += trimmed; g.bs.stuck += stuck; g.bs.bad += bad;
    if (written || moved || bad || stuck || replaced || spawned || trimmed || keys_differ || deadnote) ++g.bs.board_differed;
    if (b.turn == 0) g.bs.turn0_differed = written + moved + replaced + spawned + trimmed + keys_differ + bad;
    const bool differed = written || moved || bad || stuck || deadnote || keys_differ || killed || replaced || spawned || trimmed;
    if (spawned || trimmed) log_line_lvl(LogLevel::Warn, "BOARD", "!! the unit set differed from the host's: %u unit(s) MADE here from the host's board, %u taken off the board here", spawned, trimmed);
    if (replaced) log_line_lvl(LogLevel::Warn, "BOARD", "!! %u unit(s) of another kind than the host's were REPLACED by the host's kind this boundary", replaced);
    if (!quiet || differed) {
        log_line_lvl(quiet ? LogLevel::Warn : LogLevel::Info, "BOARD", "<- the host's board of turn %u applied (battle %016llx): %u unit(s), %u rewritten, %u moved, %u already equal, %u left alone (mismatch), %u not moved, %u life flag(s) differ (%u killed), %u unit(s) had other turn-order keys (the host's taken); stream %016llx -> %016llx (the host's)%s",
                     b.turn, (unsigned long long)b.battle, b.total, written, moved, same, bad, stuck, deadnote, killed, keys_differ, (unsigned long long)before, (unsigned long long)b.rng[0],
                     quiet && differed ? "  -- THE BOARDS HAD DRIFTED APART and were put back together" : "");
        for (uint32_t k = 0; k < nsaid; ++k) log_line("BOARD", "   %s", said[k]);
    }
    if (bad || stuck) log_line_lvl(LogLevel::Warn, "BOARD", "!! %u unit(s) could not be matched and %u could not be placed -- the turn hash will say whether the boards still differ", bad, stuck);
}

// Once per battle, at its first turn boundary, before the first hash: the host publishes, a client waits for it and takes it over.
void board_sync() {
    if (g.battles.current == kNoBattle) return;
    if (g.board_battle == g.battles.current && g.board_turn == g.turn) return;
    if (g.turn != 0 && !tune::kBoardEveryTurn) return;
    if (!net_active() || net_peer_count() < 2) return;
    g.board_battle = g.battles.current;
    g.board_turn = g.turn;
    if (net_role() == NetRole::Host) { board_publish(); return; }
    if (net_role() != NetRole::Client) return;
    // EVERY TURN BOUNDARY (proto 71), not only the first: the host publishes its board here, and a client that finds its own different (a unit on another tile, other hit points, a
    // unit the host already lost) puts it right BEFORE the turn is hashed -- so what the hash compares is the repaired board, and a drift costs one line in the log instead of a halt.
    // Applying is a no-op where the boards agree. A later turn waits only briefly: the host's board is normally already here, and a turn played without it is repaired at the next one.
    const bool first = g.turn == 0;
    const ULONGLONG limit = first ? kBoardWaitMs : (ULONGLONG)tune::kBoardTurnWaitMs;
    static BoardAssembled box;     // 12 KB: not on the game thread's stack
    const ULONGLONG t0 = GetTickCount64();
    while (!net_host_board(g.battles.current, g.turn, box) && GetTickCount64() - t0 < limit) Sleep(5);
    if (!net_host_board(g.battles.current, g.turn, box)) {
        ++g.bs.board_late;
        log_line_lvl(LogLevel::Warn, "BOARD", "!! (%llu ms into the battle) the host's board for battle", (unsigned long long)(g.bs.t0 ? GetTickCount64() - g.bs.t0 : 0));
        log_line_lvl(LogLevel::Warn, "BOARD", "!! the host's board for battle %016llx, turn %u did not arrive within %llu ms -- this peer plays the board it has",
                     (unsigned long long)g.battles.current, (unsigned)g.turn, (unsigned long long)limit);
        if (first) room_sync_trouble("the host's battle board");
        return;
    }
    if (first) log_line("BOARD", "the host's board arrived (waited %llu ms)", (unsigned long long)(GetTickCount64() - t0));
    board_apply(box, !first);
}

} // namespace

void lockstep_turn_boundary(void* turn_control) {
    // BEFORE THE EARLY RETURN, DELIBERATELY.
    //
    // The RE roster probe needs a TurnControl* and this is the only place one is
    // ever in hand. It used to be handed over below the guard, which made the
    // probe silently useless in a SINGLE-PLAYER run -- no session, no wallclock,
    // no arming, and a log that says nothing about why (measured 2026-09-21: a
    // whole play session with the probe 'on' and not one hit). The probe does not
    // need a session; it needs a pointer.
    listprobe_set_turn_control(turn_control);

    if (!g.active) return;
    // the last chance to put the owner's final facing of the cat that just ended its turn in place, before the state is read, hashed and logged
    face_pend_apply("the turn boundary", nullptr);
    for (PendingFace& p : g_face_pend) p.on = false;

    // A new battle means a new character list, and the old roster describes
    // cats that no longer exist. Detecting it matters more now than it did in
    // phase 4: with the split derived from the roster, a stale roster means a
    // stale split, and the second battle of a run would be played with the
    // first battle's ownership.
    //
    // The vector OBJECT is the right thing to compare, not its data pointer:
    // summons append and can reallocate the data mid-battle, which is not a new
    // battle. Both peers notice at the same logical point -- the first turn
    // boundary of the new battle -- so the turn numbering the HASH messages are
    // keyed on stays aligned across the reset.
    if (g.snapped && !g.halted) {
        const void* list = resolve_char_list(turn_control);
        if (list && (list != g.snapped_list || g.snapshot_battle != g.battles.current)) {
            ++g.epoch;
            log_line("LOCKSTEP", "new battle -- re-snapshotting the roster"
                                 " (was %u cats over %u turns; epoch %u now)",
                     g.cat_count, g.turn, g.epoch);

            g.snapped           = false;
            g.snapped_list      = nullptr;
            g.turn              = 0;
            g.board_battle      = 0;
            g.board_turn        = ~0u;
            g.outstanding       = false;
            g.control_checked   = false;
            g.state_hash_on     = false;
            for (uint32_t p = 0; p < kMaxPeers; ++p) g.peer_hash_full_warned[p] = false;
            g.stale_drops       = 0;
            g.said_stale        = false;

            // The catch-up log belongs to the battle that just ended.
            g.sent_log_n        = 0;
            g.sent_log_full     = false;

            // Both are per battle for the same reason the barrier is: a new
            // battle's cat 3 is a different cat, so last battle's readings are
            // not a baseline, and a mismatch count carried across would make
            // the second battle look like a continuation of the first.
            g.have_prev         = false;
            g.mismatches        = 0;
            g.first_mismatch    = 0;
            g.diverged          = false;
            g.debounce_turn     = ~0u;
            g.debounced         = 0;
            g.desync_noticed    = false;
            g.halted_in_battle  = false;
            g.bs = State::BStats{};
            g.bs.t0 = GetTickCount64();
            g.trail_n = 0;
            // The peer's audit of THIS battle may have got here before this peer noticed the battle was new (the peer is ahead by a frame): it is kept, not wiped -- wiping it left the compare undone on whichever
            // peer was behind, and the log said "(not completed)" for battles whose cats were never compared.
            if (!(g.have_audit_peer && g.audit_peer.battle_id == g.battles.current)) g.have_audit_peer = false;

            // The barrier is per battle, not per session: the peer has to be
            // shown to be in THIS one. Re-arming it here is what makes it cover
            // every battle of a run rather than only the first -- a peer can
            // fall behind at any node, and the second battle is no safer than
            // the first.
            g.barrier_waits     = 0;
            g.barrier_said_open = false;

            // Purge rather than clear. Anything the peer already sent for THIS
            // new battle -- it may have got here first -- is still wanted, and
            // wiping it is what lost a late peer the actions taken without it.
            //
            // g.battles is already up to date here: the map layer called
            // lockstep_enter_battle at EnterNode, which retired the battle we
            // just left and made the new node's seed current. This detection
            // fires later, at the first turn boundary, so "retired" already
            // means what these purges need it to mean.
            uint32_t da = pend_purge_retired();
            uint32_t dh = 0;
            for (uint32_t p = 0; p < kMaxPeers; ++p)
                dh += g.peer_hash[p].purge_retired(g.battles);
            g.my_hash.clear();                 // all ours, all from the old battle
            if (da || dh)
                log_line("LOCKSTEP", "  dropped %u stale action(s) and %u stale hash(es)"
                                     " from the battle we just left", da, dh);
            if (g.pend_count)
                log_line("LOCKSTEP", "  kept %u action(s) the peer had already sent"
                                     " for this battle", g.pend_count);

            // The peer's split only survives if it is for this battle or a
            // later one; anything older describes a roster that no longer
            // exists.
            for (uint32_t p = 0; p < kMaxPeers; ++p)
                if (g.have_peer_control[p] &&
                    g.battles.is_retired(g.peer_control[p].battle_id))
                    g.have_peer_control[p] = false;
        }
    }

    snapshot_cats(turn_control);
    if (!g.snapped || g.halted) return;
    if (config().test_weaken) weaken_enemies_once();

    // WHAT APPEARED SINCE THE LAST BOUNDARY GETS AN INDEX HERE, before anything
    // reads the roster: the live-count line below, the ownership checks and the
    // decision channel all come after this point, and a summon that arrived
    // during the previous turn has to be visible to all three.
    for (uint32_t j = 0; j < g.cat_count && j < kMaxCats; ++j) if (g.fresh_left[j]) --g.fresh_left[j];
    adopt_new_cats();

    // THE HOST'S BOARD AND STREAM, before anything is hashed (see board_sync): outside the guard, a client may wait for it here.
    if (g.turn == 0) audit_send();
    board_sync();

    Guard guard;
    HashMsg mine = build_hash(turn_control);
    net_send_hash(mine);
    { const uint8_t ai = g.actor ? cat_index_of(g.actor) : kNoCat; trail_record(mine, ai == kNoCat ? -1 : (int32_t)ai); }
    deep_boundary();

    // Log every turn's hash, not only mismatches. A log that goes quiet when
    // things are fine cannot be distinguished from a log whose check never
    // ran -- and the first run of this hook produced exactly that ambiguity.
    // `chars` is the LIVE character-list count, not our snapshot's.
    //
    // WHAT THE HASH COVERS NOW, AND WHAT IS ONLY COUNTED.
    //
    // The roster is snapshotted at battle start and EXTENDED by adoption at each
    // boundary (adopt_new_cats), so from the turn after it appears a summon is an
    // entry in g.cats like any other -- and therefore inside the state hash. That
    // is deliberate: a PLAYER-driven summon is exactly the unit whose divergence
    // used to go unnoticed, and owning it is what the adoption is for.
    //
    // The live count below is NOT hashed and stays a log line, for the reason it
    // always had -- a death animation can move it a frame apart between peers,
    // and a false halt is worse than a late one, the same reasoning that keeps
    // passive counts out of the hash. Since adoption runs before this point,
    // `appeared` here should be 0: anything else arrived between the boundary and
    // this line and will be adopted at the next one. A non-zero appeared on ONE
    // peer is the earliest sign the two rosters are drifting apart.
    //
    // Counted by MEMBERSHIP rather than by length, because an append and a
    // removal in the same turn cancel out and a bare count then reports "45/45,
    // nothing happened" through a roster change big enough to desync. The
    // appeared/left pair is what actually has to match between the two logs.
    uint32_t live_chars = 0, appeared = 0, departed = 0;
    bool still_in[kMaxCats];
    const void* list = resolve_char_list(turn_control);
    if (!list) list = g.snapped_list;
    if (snapshot_membership(list, live_chars, still_in, appeared))
        departed = snapshot_departed(still_in);
    else if (list)
        mem_read((const uint8_t*)list + kList_Count, &live_chars, sizeof(live_chars));

    char roster[96];
    roster[0] = 0;
    if (appeared || departed)
        _snprintf_s(roster, sizeof(roster), _TRUNCATE,
                    "  ROSTER +%u/-%u", appeared, departed);

    // After the hash is built (so it reads the same instant the hash covers)
    // and before it is logged, so a turn's deltas sit above the hash line they
    // explain rather than under the next turn's.
    trace_state_deltas();

    log_line("LOCKSTEP", "turn %u hash rng=%016llx state=%016llx queue=%u chars=%u/%u%s",
             mine.turn,
             (unsigned long long)mine.rng_hash,
             (unsigned long long)mine.state_hash, mine.queue_depth,
             live_chars, g.cat_count, roster);

    // Remember it before comparing: if the peer's counterpart arrives later,
    // match-on-arrival in lockstep_pump needs to find this turn here.
    g.my_hash.push_evicting(mine);

    // Compare against anything the peer has already sent for this turn. A peer
    // running ahead is normal and not itself a desync -- run E measured the
    // same battle at 23,211 frames and at 12,230 -- so an unmatched hash is
    // simply kept until its counterpart arrives.
    // Every player's, not just one. A four-player battle has three counterparts
    // for this turn and any of them can be the one that diverged; stopping at
    // the first match would leave the other two unchecked.
    for (uint32_t p = 0; p < kMaxPeers; ++p) {
        if (p == net_self()) continue;
        HashMsg theirs{};
        if (g.peer_hash[p].take(mine.battle_id, mine.turn, theirs))
            report_pair(mine, theirs, (uint8_t)p);
    }

    ++g.turn;
}

namespace {
uint64_t reseed_mix(uint64_t& x) {
    uint64_t z = (x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
uint64_t g_rs_battle = 0;
uint32_t g_rs_turn = ~0u, g_rs_idx = ~0u, g_rs_repeat = 0, g_rs_logged = 0, g_rs_action = 0, g_rs_action_logged = 0;
} // namespace

void lockstep_reseed(const void* actor, int arg) {
    if (!tune::kReseedPerTurn || !g.active || !g.snapped || g.halted || g.battles.current == kNoBattle || !net_active() || net_peer_count() < 2) return;
    uint64_t* s = rng_global_stream();
    if (!s) return;
    uint32_t idx = 0xFFFEu;                                   // the turn control's own start (the shuffle of the turn order)
    if (actor) {
        idx = 0xFFFFu;                                        // an actor outside the snapshot (a summon)
        for (uint32_t i = 0; i < g.cat_count; ++i) if (entry_is(i, actor)) { idx = i; break; }
    }
    if (g.battles.current != g_rs_battle) { g_rs_battle = g.battles.current; g_rs_turn = ~0u; g_rs_logged = 0; g_rs_action_logged = 0; }
    if (g.turn != g_rs_turn || idx != g_rs_idx) { g_rs_turn = g.turn; g_rs_idx = idx; g_rs_repeat = 0; } else ++g_rs_repeat;
    uint64_t x = g.battles.current ^ 0x5253454544545552ull ^ ((uint64_t)g.turn * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)idx * 0xC2B2AE3D27D4EB4Full) ^
                 ((uint64_t)(uint32_t)arg * 0x165667B19E3779F9ull) ^ ((uint64_t)g_rs_repeat * 0xD6E8FEB86659FD93ull);
    uint64_t st[4];
    for (int i = 0; i < 4; ++i) st[i] = reseed_mix(x);
    uint64_t before = 0;
    mem_read(s, &before, sizeof(before));
    if (!mem_write(s, st, sizeof(st))) return;
    unlockq_flush(g.turn, g_rs_action);                       // what the last action's unlock checks and property reads were (a line only when there were some)
    rngl_flush(g.turn, g_rs_action);                          // ... and the draws it made on the shared stream, by call site (compared with the other peer's)
    g_rs_action = 0;                                          // a new actor-turn: its actions are numbered from 0
    if (g_rs_logged < 6) {
        ++g_rs_logged;
        log_line("RESEED", "turn %u, %s %u: the shared stream %016llx -> %016llx (derived from battle, turn and actor: the same on every peer)%s", (unsigned)g.turn,
                 actor ? "actor" : "turn control", actor ? (unsigned)idx : 0u, (unsigned long long)before, (unsigned long long)st[0],
                 g_rs_logged == 6 ? " -- the rest of this battle is not logged" : "");
    }
}

// THE SAME, AT EVERY ACTION (2026-10-03, a halt in the middle of a battle): the Tinkerer's TinkererCraft made 'wp_Battery' on the host and 'wp_Stick' on the client -- the weapon it crafts is a draw from the
// shared stream (a pool pick, then the weapon's durability range), and the stream the two peers started that action from had drifted apart INSIDE the turn (the boundaries all agreed: the board sets the
// stream at each one). The owner of a cat runs things the other peer does not -- aim and ability previews, the menus -- and anything of that kind that touches the stream between two actions moves it on one
// peer only. So each applied action restarts the stream from (battle, turn, actor, the action's number in that actor-turn), which both peers know: what happens between two actions can no longer reach the next one.
// The stream before the reseed is logged for the first actions of a battle on both peers, so the first drift shows up as two different numbers on the same line.
void lockstep_reseed_action(const void* actor) {
    if (!tune::kReseedPerTurn || !g.active || !g.snapped || g.halted || g.battles.current == kNoBattle || !net_active() || net_peer_count() < 2) return;
    uint64_t* s = rng_global_stream();
    if (!s) return;
    uint32_t idx = 0xFFFFu;                                   // an actor outside the snapshot (a summon)
    for (uint32_t i = 0; i < g.cat_count; ++i) if (entry_is(i, actor)) { idx = i; break; }
    if (g.battles.current != g_rs_battle) { g_rs_battle = g.battles.current; g_rs_turn = ~0u; g_rs_logged = 0; g_rs_action_logged = 0; }
    unlockq_flush(g.turn, g_rs_action);                       // ... and after each action
    rngl_flush(g.turn, g_rs_action);
    const uint32_t n = ++g_rs_action;
    uint64_t x = g.battles.current ^ 0x4143544E52534544ull ^ ((uint64_t)g.turn * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)idx * 0xC2B2AE3D27D4EB4Full) ^ ((uint64_t)n * 0x165667B19E3779F9ull) ^
                 ((uint64_t)g_rs_repeat * 0xD6E8FEB86659FD93ull);
    uint64_t st[4];
    for (int i = 0; i < 4; ++i) st[i] = reseed_mix(x);
    uint64_t before = 0;
    mem_read(s, &before, sizeof(before));
    if (!mem_write(s, st, sizeof(st))) return;
    if (g_rs_action_logged < 400) {
        ++g_rs_action_logged;
        log_line("RESEED", "turn %u action %u of actor %u: the stream was %016llx, now %016llx", (unsigned)g.turn, (unsigned)n, idx == 0xFFFFu ? 0xFFFFu : (unsigned)idx, (unsigned long long)before, (unsigned long long)st[0]);
    }
}

uint64_t lockstep_battle_id()  { return g.active ? g.battles.current : kNoBattle; }
bool     lockstep_local_actor(){ return g.active && g.snapped && g.local_actor; }

void lockstep_reset_cat_owners() {
    // The map now consumes these before the first battle. A save-id reused by
    // another run must not inherit the previous run's absolute owner note.
    g_derived_history.clear();
    g_owner_note_count = 0;
    for (auto& note : g_owner_note) note = {};
    for (auto& cat : g.level_cats) cat = {};
    g.level_probe_node = 0;
    // A NEW RUN MEANS A NEW SPLIT, so the remembered one is not evidence any more:
    // different party, different positions, possibly different players. Cleared
    // here and refilled at the next battle's split.
    for (auto& cat : g.owned_cats) cat = {};
    g.owned_count = 0;
    g.owned_valid = false;
}

bool lockstep_cat_is_mine(uint64_t save_id, bool& mine) {
    mine = false;
    if (!save_id || !g.owned_valid) return false;
    for (uint32_t i = 0; i < g.owned_count; ++i) {
        if (g.owned_cats[i].save_id != save_id) continue;
        mine = g.owned_cats[i].mine;
        return true;
    }
    return false;   // not in the human roster: unknown, and the caller keeps its rule
}

bool lockstep_cat_owner(uint64_t save_id, uint64_t seed, uint8_t& owner) {
    owner = kNoPeer;
    // The last verified battle split is authoritative after the live roster has
    // been retired.  Consult it first.  A derived PlayerBrain is intentionally
    // kept in level_cats as an unreadable entry; returning false on that entry
    // used to abort the scan before it reached the real cat being upgraded.
    for (uint32_t i = 0; i < g.owned_count; ++i) {
        if (g.owned_cats[i].save_id != save_id || g.owned_cats[i].owner == kNoPeer) continue;
        owner = g.owned_cats[i].owner;
        return true;
    }
    if (!g.active || !g.snapped || g.halted || !g.control_checked || !leave_in_run()) {
        // The victory/level-up screen is after the battle's live roster has
        // been retired.  Keep using the owner captured at the last verified
        // split instead of reporting owner=255 and letting each side choose
        // its local fallback cat.
        for (uint32_t i = 0; i < g.owned_count; ++i) {
            if (g.owned_cats[i].save_id != save_id || g.owned_cats[i].owner == kNoPeer) continue;
            owner = g.owned_cats[i].owner;
            return true;
        }
        return false;
    }
    uint32_t matches = 0;
    uint8_t found = kNoPeer;
    for (uint32_t i = 0; i < g.cat_count; ++i) {
        if (!g.human_cat[i] || g.summon[i]) continue;
        const auto& cat = g.level_cats[i];
        if (!cat.readable) continue;
        if (cat.save_id == save_id && cat.seed == seed) {
            ++matches;
            found = cat.owner;
        }
    }
    if (matches != 1 || found == kNoPeer) {
        for (uint32_t i = 0; i < g.owned_count; ++i) {
            if (g.owned_cats[i].save_id != save_id || g.owned_cats[i].owner == kNoPeer) continue;
            owner = g.owned_cats[i].owner;
            return true;
        }
        return false;
    }
    uint8_t ids[kMaxPeers] = {};
    const uint8_t count = net_peer_count();
    if (!net_peer_ids(ids, kMaxPeers)) {
        for (uint32_t i = 0; i < g.owned_count; ++i) {
            if (g.owned_cats[i].save_id != save_id || g.owned_cats[i].owner == kNoPeer) continue;
            owner = g.owned_cats[i].owner;
            return true;
        }
        return false;
    }
    for (uint8_t i = 0; i < count && i < kMaxPeers; ++i) {
        if (ids[i] != found) continue;
        owner = found;
        return true;
    }
    for (uint32_t i = 0; i < g.owned_count; ++i) {
        if (g.owned_cats[i].save_id != save_id || g.owned_cats[i].owner == kNoPeer) continue;
        owner = g.owned_cats[i].owner;
        return true;
    }
    return false;
}

void lockstep_probe_roster_ids(const void* subject) {
    if (!tune::kLevelUpProbe) return;
    if (!g.active || !g.snapped || !g.cat_count) {
        log_line("CHOICE", "PROBE owner UNKNOWN: active=%u snapped=%u slots=%u",
                 (unsigned)g.active, (unsigned)g.snapped, g.cat_count);
        return;
    }
    const uint64_t here = follow_here_seed();
    if (!here || here != g.level_probe_node) {
        log_line("CHOICE", "PROBE owner UNKNOWN: snapshot node=%016llX screen node=%016llX",
                 (unsigned long long)g.level_probe_node, (unsigned long long)here);
        return;
    }

    uint64_t seed = 0, save_id = 0;
    const bool seed_ok = subject && mem_read((const uint8_t*)subject + kCatData_Seed,
                                             &seed, sizeof(seed));
    const bool id_ok = subject && mem_read((const uint8_t*)subject + kCatData_SaveId,
                                           &save_id, sizeof(save_id));
    log_line("CHOICE", "PROBE subject: CatData=0x%llX seed_ok=%u id_ok=%u "
                       "seed=%016llX save_id=%016llX",
             (unsigned long long)(uintptr_t)subject, (unsigned)seed_ok, (unsigned)id_ok,
             (unsigned long long)seed, (unsigned long long)save_id);
    if (!seed_ok || !id_ok) {
        log_line("CHOICE", "PROBE owner UNKNOWN: subject identity unreadable");
        return;
    }

    const void* dir = nullptr;
    if (!mem_read((const void*)addr_of_data(D_MewDirectorPtr), &dir, sizeof(dir)) || !dir) {
        log_line("CHOICE", "PROBE owner UNKNOWN: no readable MewDirector");
        return;
    }
    uint32_t n = 0;
    const uint64_t* ids = nullptr;
    if (!mem_read((const uint8_t*)dir + kDir_CatIdCount, &n, sizeof(n)) || !n || n > 64) {
        log_line("CHOICE", "PROBE owner UNKNOWN: run cat count=%u (expected 1..64)", n);
        return;
    }
    uint64_t run_ids[64] = {};
    if (!mem_read((const uint8_t*)dir + kDir_CatIdData, &ids, sizeof(ids)) || !ids ||
        !mem_read(ids, run_ids, n * sizeof(uint64_t))) {
        log_line("CHOICE", "PROBE owner UNKNOWN: run id list unreadable");
        return;
    }
    uint32_t in_run = 0;
    for (uint32_t k = 0; k < n; ++k) {
        log_line("CHOICE", "PROBE run[%u]=%016llX subject_save_id=%s", k,
                 (unsigned long long)run_ids[k], run_ids[k] == save_id ? "MATCH" : "different");
        if (run_ids[k] == save_id) ++in_run;
    }

    uint32_t matches = 0, missing = 0, slot = 0;
    for (uint32_t i = 0; i < g.cat_count; ++i) {
        if (!g.human_cat[i] || g.summon[i]) continue;
        const auto& cat = g.level_cats[i];
        if (!cat.readable) { ++missing; continue; }
        const bool match = cat.save_id == save_id && cat.seed == seed;
        log_line("CHOICE", "PROBE compare slot %u %s fp=%08X seed=%016llX save_id=%016llX "
                           "same_pointer=%u identity=%s",
                 i, g.local_cat[i] ? "MINE" : "peer", g.cat_fp[i],
                 (unsigned long long)cat.seed, (unsigned long long)cat.save_id,
                 (unsigned)(cat.data == subject), match ? "MATCH" : "different");
        if (match) { ++matches; slot = i; }
    }
    if (in_run != 1 || matches != 1 || missing) {
        log_line("CHOICE", "PROBE owner UNKNOWN: run_matches=%u roster_matches=%u "
                           "unreadable_humans=%u -- no ownership decision",
                 in_run, matches, missing);
        return;
    }
    log_line("CHOICE", "PROBE owner CANDIDATE: node=%016llX save_id=%016llX seed=%016llX "
                       "slot=%u fp=%08X owner=%s same_pointer=%u -- compare both peers; "
                       "diagnostic only",
             (unsigned long long)here, (unsigned long long)save_id, (unsigned long long)seed,
             slot, g.cat_fp[slot], g.local_cat[slot] ? "MINE" : "peer",
             (unsigned)(g.level_cats[slot].data == subject));
}

bool lockstep_peer_owns_character(const void* character) {
    if (!g.active || !g.snapped || !character) return false;
    const uint8_t cat = cat_index_of(character);
    if (cat == kNoCat)      return false;   // a summon: nobody's, both AIs drive it
    if (!g.human_cat[cat])  return false;   // an AI cat: decided locally on both
    return !g.local_cat[cat];
}

// Called by the map layer from BOTH peers as they enter a node -- the host from
// its EnterNode hook, the client from the follow tick that drives it into the
// same node. `seed0` is MapNode+0x118's first word, so the two peers arrive at
// the same value without exchanging it, which is the whole point.
//
// Every node calls this, not just the combat ones. A shop or an event is a
// battle-less "battle" as far as identity goes, and giving it an id is what
// makes a CURSOR or a stray ACTION from the previous fight droppable while
// standing in a shop.
void lockstep_enter_battle(uint64_t seed0) {
    Guard guard;
    if (seed0 == kNoBattle) return;
    if (seed0 == g.battles.current) return;      // same node; not a transition

    // THE BATTLE WE ARE LEAVING GETS ITS SUMMARY HERE, ON EVERY PEER (2026-10-04): a client followed the host into the next node through the game's own entry call (the trampoline), which does not pass the
    // EnterNode detour that logged it, so a client's log never had a single SUMMARY-SYNC -- and with one uploaded log per room that is the log that may be the only one. Once per battle, so the detour's call stays harmless.
    lockstep_log_battle_summary("the next node is entered");

    const uint64_t left = g.battles.current;
    g.battles.enter(seed0);
    g.armed_hit = 0;
    g.dbg_n = 0; g.dbg_waiting = false;

    // The log describes the battle we just left, and nothing in it can be
    // replayed into the new one.
    g.sent_log_n    = 0;
    g.sent_log_full = false;

    log_line("LOCKSTEP", "battle id now %016llx%s", (unsigned long long)seed0,
             left == kNoBattle ? " (first this session)" : "");
}

// A peer connected or reconnected. Hand it everything this peer has already
// decided in the battle it is joining, so it can fast-forward instead of
// playing a different battle from turn 0.
//
// Sent to that peer only: a broadcast would re-inject every decision into peers
// that already applied them, and pend_take has no way to tell a replay from a
// live decision -- correctly, because there is no difference except who needs
// it.
void lockstep_catchup(uint8_t peer) {
    Guard guard;
    if (!g.active || g.battles.current == kNoBattle) return;
    // The host's board of this battle first: the joiner replays the decisions from turn 0 on top of it.
    if (net_role() == NetRole::Host && g.board_sent_for == g.battles.current && g.board_sent_n) {
        uint32_t sent = 0;
        for (uint32_t c = 0; c < g.board_sent_n; ++c) if (net_send_board_to(peer, g.board_sent[c])) ++sent;
        log_line("BOARD", "-> the host's board of battle %016llx re-sent to peer %u (%u/%u chunk(s))", (unsigned long long)g.battles.current, (unsigned)peer, sent, g.board_sent_n);
    }
    if (g.sent_log_n == 0) {
        log_line("LOCKSTEP", "peer %u joined battle %016llx -- nothing to replay,"
                             " it can derive this battle from the node seed alone",
                 (unsigned)peer, (unsigned long long)g.battles.current);
        return;
    }
    if (g.sent_log_full) {
        // Loud, and deliberately not a refusal: the peer is better off in the
        // right node with an incomplete history -- where the per-turn hash will
        // catch the divergence and halt -- than left on the map with no way to
        // report anything at all.
        log_line("LOCKSTEP", "!! peer %u is joining a battle whose decision log"
                             " overflowed -- replaying the %u we still have; expect"
                             " a hash mismatch rather than a silent desync",
                 (unsigned)peer, g.sent_log_n);
    }

    uint32_t sent = 0;
    for (uint32_t i = 0; i < g.sent_log_n; ++i)
        if (net_send_action_to(peer, g.sent_log[i])) ++sent;

    log_line("LOCKSTEP", "-> replayed %u/%u decision(s) of battle %016llx to peer %u"
                         " so it can catch up to turn %u",
             sent, g.sent_log_n, (unsigned long long)g.battles.current,
             (unsigned)peer, g.turn);
}

LockstepStats lockstep_stats() {
    LockstepStats s = g.stats;
    s.pending = g.pend_count;
    return s;
}

} // namespace mgmp
