// mgmp_choice -- see mgmp_choice.h for why replicating the CHOICE replaces
// replicating everything the choice does.
#include "mgmp_choice.h"
#include "mgmp_follow.h"
#include "mgmp_net.h"
#include "mgmp_config.h"
#include "mgmp_tuning.h"
#include "mgmp_mem.h"
#include "mgmp_log.h"
#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_lockstep.h"   // lockstep_probe_roster_ids -- TEMPORARY, see kLevelUpProbe
#include "mgmp_unlocks.h"    // unlocks_level_screen_opens
#include "mgmp_rtti.h"       // rtti_class_name, so the probe can name the screen
#include "mgmp_catsync.h"    // serialize_cat -- the cat's own serialized image, the only
                             // way the level-up probe can see a change behind a pointer
#include "mgmp_listprobe.h"  // listprobe_watch_qword -- borrows one watchpoint slot to name
                             // the writer of this screen's subject field (2026-09-23)
#include "MinHook.h"         // the per-peer level-up hook is installed from here, because the
                             // target address is DERIVED (LVLSELECT+0x24C0) rather than one of
                             // the generated signatures -- see install_start_levelup_hook

#include "mgmp_nodehash.h"   // nodehash_peer_event_name -- the peer's event name, the
                             // shared signal the per-player event split is decided on

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace mgmp {
namespace {

// --- the two option arrays -------------------------------------------------
//
// Both are {T* begin, T* end} with sizeof(T) == 240, and both are built in
// authored file order. Read off setupActionChoice__inner_0 (v20 = *(v1+28),
// v161 = *(v1+29), then "v20 += 240") and off the level-up button callback,
// which resolves *(LevelUpScreen+864) + 240 * index.
constexpr uintptr_t kEvt_OptBegin  = 224;
constexpr uintptr_t kEvt_OptEnd    = 232;

constexpr uintptr_t kLvl_OptBegin  = 864;
constexpr uintptr_t kLvl_OptEnd    = 872;
constexpr uintptr_t kLvl_Committed = 792;   // select_option's "already chose" guard

constexpr uintptr_t kOptStride     = 240;

// An event option entry's stat key -- "str".."lck", "coins", "quest", "none",
// "home". A std::string at entry+64, compared against exactly those literals
// all the way through setupActionChoice.
constexpr uintptr_t kEvtOpt_Stat = 64;

// The event's own NAME is mgmp_addresses.h's kEvt_EventName (0x1A10) -- the same offset
// mgmp_nodehash reads to fill NodeHashMsg::event. The two peers comparing THESE is the
// shared signal the per-player split needs: see DESIGN-independent.md sec. 5 and
// nodehash_peer_event_name. (This file briefly had its own copy of the constant; the
// compiler caught the duplicate, which is the shared header doing its job.)

// How long a client holds its own event click waiting to learn whether the two peers
// are looking at the same event. Short, because in the shared case the peer's name is
// on the wire already (it is published the moment its screen opens), and in the
// per-player case waiting longer would just be a screen that does not respond.
constexpr uint64_t kEventOwnWaitMs = 1000;

// --- PARALLEL LEVEL-UPS (2026-09-23) -------------------------------------------------
//
// ON, and reversible in one line, because this changes how a screen the OTHER player owns
// gets answered on this peer.
//
// WHAT THE SCREEN MAY OFFER, per the user's own account of the game and confirmed by the
// live probe, is SEVEN things and nothing else: gain an active, gain a passive, a stat
// gain, replace an active (when four are held), replace a passive (when two are held),
// upgrade an active, upgrade a passive. Every one of them writes the CAT. None touches the
// Inventory -- and the note that used to live in this file claiming options could reach
// Inventory::insert_item is gone precisely because a live screen disproved its type list
// ([0]type=5:'FullPower' [1]type=5:'TrueSight' [2]type=5:'Rockin' [3]type=1:'Speed').
//
// That is what makes a placeholder answer safe: it writes only the cat, and the cat is
// exactly what mgmp_catsync carries -- the owner's copy overwrites this one before the next
// battle is built ("the last moment before a battle can be built out of a stale cat").
// ★ OFF, AND THE MEASUREMENT THAT DID IT (2026-09-23, the run after the ownership polarity fix).
//
// This flag's whole reach is the case `owner != net_self()` -- a screen for a cat THIS peer does
// not own -- and it answers that screen locally with a placeholder press. The recorded argument
// for its safety was "the seven option kinds all write the cat, and catsync carries the cat". The
// first half is true; the second is not, and the run proves it in one line each:
//
//   CHOICE  PROBE options step=0 n=4: [1]type=5:'TrueSight' [3]type=1:'Speed'
//   CHOICE  !! PARALLEL level-up: this peer's own copy of cat=...024D's screen was answered with
//           option 3 after 16 ms of silence from its owner.
//   CHOICE  <- peer 0 chose level-up: cat=...024D ... option=1/4 'TrueSight'   <- the owner
//
// 024D belongs to peer 0. The owner took 'TrueSight'; this peer pressed 'Speed' on its copy; the
// two cats then differed by NINE HP on a cat whose maxhp agreed (5/20 against 14/20), which is a
// STAT WINDOW difference and therefore exactly the kind of thing catsync's push is supposed to
// repair -- and did not, because the push carries the CAT (abilities, stats, the serialized
// window) while this divergence was written into the live copy AFTER the push had been applied.
// The next battle then halted on `turn 0 hash mismatch (state only)` with rng and queue IDENTICAL
// on both peers and 1 of 22 cat rows differing -- the one row was that cat's hp.
//
// So the placeholder is retired. Its replacement is the feature the user actually asked for:
// kPerPeerLevelTarget, ON, which re-points the screen at THIS peer's own cat so that nobody ever
// has to answer a screen for someone else's cat -- and therefore nobody ever writes one. If a
// re-point cannot be made (no own cat left unlevelled, hook refused), the fallback is now the OLD
// behaviour rather than a divergence: this peer WAITS and the owner's choice arrives and is
// applied by the normal path below. The cost is a visible screen for as long as the owner takes
// to click. That is the trade this project wants: a wait, never two different cats.
constexpr bool     kParallelLevelUp  = false;
// Kept for the day a placeholder is wanted again, and kept at 0 so that if the flag above is ever
// flipped back on, the behaviour is the one it was measured with rather than a new one.
constexpr uint64_t kParallelGraceMs  = 0;

// --- PER-PEER LEVEL-UP SUBJECT (2026-09-23) ------------------------------------------
//
// The user's goal: each peer levels its OWN cats rather than both watching the same draw.
// Measured first, so the reason is on the record: after a battle BOTH peers were offered
// cat 24D (owner=0), even though route A had already put each peer's own four into its
// party. The draw therefore does not read the party -- it reads the run's cat list, which
// write_party and mgmp_catsync deliberately keep IDENTICAL on both peers (tonight's own
// fix), so a draw from that list gives the same cat on both peers by construction.
//
// So the subject is re-pointed at the screen instead: LevelUpScreen+0xA0 is the subject
// CatData*, and this peer holds the whole run's cats, not just its own.
//
// WHAT IT CANNOT FIX, stated plainly: the options were rolled for the cat the game drew
// (the roller is seeded from that cat's id and its +0xC30 counter), so the re-pointed cat
// is offered the DRAWN cat's options. They are still all cat-only options -- the seven
// kinds this game has, every one of which writes the cat -- so mgmp_catsync still
// converges, but the result is not the roll this cat would have got on its own. If that
// matters more than the split, set this to false.
// ON, at the user's instruction, while the upstream probe runs (2026-09-23). Know what it
// does and does not reach, because both halves are measured:
//
//   IT WORKS for this module's logic -- the log shows the client being handed its own cat
//   ("the game drew cat ...24D (not this peer's); this peer's screen now has ITS OWN cat
//   ...28A"), then `owner=1 self=1 -- choose locally`, and the 0 ms placeholder correctly
//   stops firing.
//
//   IT DOES NOT REACH the picture. The scan written for that question reports the old
//   subject held at "NOTHING ELSE" in the screen object, so the portrait, the stat numbers
//   and the option roll all come from the game's own pending-level-up state rather than from
//   this pointer. While that is so, the panel shows the DRAWN cat's numbers next to a
//   level-up that lands on this peer's cat -- deliberately visible, not hidden.
//
// The upstream probe (a watchpoint on screen+0xA0, which names the instruction and its
// callers) is what would let display, options and the XP accounting follow together.
// OFF AGAIN, AND THIS TIME ON THE RECORD (2026-09-23, measured): the per-peer split has to
// wait for a reconciliation that is not yet guaranteed.
//
// What it did, in order. The re-point (and the hook that was to replace it) makes each peer
// level ITS OWN cat: the client levelled 28A, the host levelled the 24D it drew. The two
// peers therefore hold DIFFERENT cat data, and the next battle is built from it:
//
//   host  : HALT at turn 0: turn 0 hash mismatch (state only):
//           rng fc48bdca8d6a8d64/fc48bdca8d6a8d64 queue 0/0 state ee497c.../ad0c79...
//   client: the same line with the two states exchanged
//
// rng and queue agree, the control split agrees to the character ("9:owner0:id1fe
// 7:owner0:id24d ..."), and only the STATE differs -- i.e. the level-up divergence was still
// in the cats when the fight started.
//
// Why mgmp_catsync does not already fix that: it runs on the client's map tick and on the
// host's NODE ENTRY, and the host builds the battle the moment it enters. A push that lands
// after that is a push that arrived too late, which is exactly what this log shows. The
// owner-authoritative cat sync is the right mechanism; what it lacks is a BARRIER -- the host
// has to have the peer's cats in hand before it enters a node.
//
// So: split off until that barrier exists. With it off, both peers level the SAME drawn cat
// (which is what the game does) and the panel shows that same cat, so the visible state stays
// honest as well.
// ON AGAIN (2026-09-23, second attempt), now that BOTH halves the first attempt lacked are
// in place. The first attempt's HALT was a turn 0 state hash mismatch -- the level-up
// divergence still in the cats when the host built the next battle. Two things closed that:
//
//   * the placeholder's press is marked as OUR OWN injection (g.injecting), so it actually
//     lands on the cat instead of being swallowed by the out-of-battle owner guard (measured:
//     "0 cat(s) changed and sent, 4 unchanged" while a screen had visibly been answered);
//   * a level-up publish goes out the moment a press lands (catsync_publish right after the
//     injection below), which shrinks the divergence window from "until the next node" to
//     "until this call returns".
//
// The hook itself was verified separately: it installed at LVLSELECT+0x24C0 on the first
// live screen and the address arithmetic checked out against the log.
// OFF, BECAUSE THE PARTY DOES IT NOW (2026-09-23, the user's own idea, finally testable).
//
// Everything above chases the drawn cat after the fact -- re-point the screen's subject, hook
// the level-up start, hook the queue entry -- and the picture never followed, because the panel
// is a SNAPSHOT taken when the screen is built.
//
// Then the user asked why the PARTY cannot decide the draw, and the measurement answered the
// question that had been hiding in plain sight: it was never tested. The client's swap (route
// A) is released at node entry and re-applied only when a BUTTON updates -- and between the
// fight ending and the level-up screen there is no button update at all. The log says it
// exactly: released at node entry, then nothing until the screen, and the client drew the
// host's cat. roster_party_swap_watch now applies the swap ON THE FRAME THE FIGHT ENDS, so
// this peer's own four are in the party AT THE MOMENT OF THE DRAW.
//
// If that works, the game draws one of ITS cats by itself and builds the whole screen around
// it -- no hook, no re-point, and the panel cannot lie because it shows the cat that is really
// being levelled. That is strictly better than anything the machinery above could produce, so
// it is switched off and left in place as the record of what was tried.
// OFF, WHILE THE PARTY ROUTE IS PURSUED INSTEAD (2026-09-23).
//
// The user's direction: get the swap in place on the client BEFORE the level-up screen appears,
// so the game's own draw picks one of its cats and the panel cannot lie. That route needs this
// machinery OUT of the way -- with it on, the subject is re-pointed after the draw and the
// picture is built for a cat the game did not choose.
//
// What the party route needs, and both pieces already exist (found while reading lockstep):
//   * g.battles / is_retired -- the mod already knows a battle is OVER, which is the victory-
//     screen moment the user described (no button is pressed during it, so the button detour
//     cannot see it, and lockstep_in_battle() is unusable after a halt);
//   * barrier_decide / BarrierFacts (net_join_barrier) -- the node-entry barrier that the
//     reconciliation task needs, already implemented for the join case.
// ON AT THE USER'S INSTRUCTION (2026-09-23, and for the first time with the instrument the
// earlier attempts were missing). The user wants each peer to level its own cat and is
// willing to carry the two costs below, both of which are measured rather than feared:
//
//   1. THE PICTURE LAGS THE LOGIC. The panel is a SNAPSHOT taken when the screen is built,
//      so re-pointing +0xA0 afterwards changes what this module acts on (owner, self,
//      placeholder) but not what the player sees. The screen scan proved the drawn cat is
//      not held anywhere else in the screen OBJECT ("NOTHING ELSE"), and the queue hook at
//      base+0x52AF0 installed but never fired for a level-up screen -- so the build site is
//      still unnamed. That is now instrumented instead of guessed: choice_reset_run arms an
//      execute watchpoint on the LevelUpScreen vtable store (base+0x3C6CFB) whenever this
//      flag is on, and its stack names the caller that builds the display. Whoever reads
//      this next: that log line is the lead for making the picture follow.
//
//   2. THE CATS REALLY DO DIVERGE, SO THE RECONCILE HAS TO HOLD. Each peer writes a
//      different cat, and only the OWNER may publish (mgmp_catsync's rule), so both
//      directions now carry real edits. The two pieces that cover it are already in place:
//      a publish the moment a press lands (so the window is "until this call returns", not
//      "until the next node"), and the host's node entry, which applies what arrived and
//      publishes its own cats immediately BEFORE it can build a battle. What is NOT covered
//      is a same-instant race -- a node entered before the other peer's push lands. The
//      first attempt at this halted on exactly that residue ("turn 0 hash mismatch (state
//      only)"), and the cure for it is a barrier, not a shorter window: the mod already has
//      barrier_decide/BarrierFacts (net_join_barrier) holding decisions until both peers
//      are in the battle, and extending that to "the peer's cats are in hand before the
//      node is entered" is the next piece of work if this test halts.
//
// Kept honest: if the session halts at turn 0 again, the residue is the cats, the fix is
// the barrier above, and the log line to look for is the turn 0 state hash mismatch.
// ★ CONTROLLED EXPERIMENT, OFF (2026-09-23, at the user's observation).
//
// The user noticed that on the client, the cat that plays its action TWICE in a battle is the
// cat the shared draw picked for the level-up -- and the log agrees with them: the client's
// per-peer re-point names the drawn cat 024D, hands the screen its own 02AA, and the very next
// battle duplicates 024D's ability. That is this flag's machinery (the level-up screen's subject
// is re-pointed and the local copy is answered), and the duplicate is the one remaining
// divergence in a battle that otherwise agrees.
//
// So: off, as a control. The same battle that duplicated 024D is the test -- if the duplicate is
// gone with this off, the re-point is the cause and the fix belongs there (or in the placeholder
// answer it drives); if it is NOT gone, the cause is elsewhere and this goes back on, because the
// user asked for each peer to level its own cat. Everything below is kept as the record of how
// the feature works and what it cost, so it can go back on the moment the interaction is fixed.
// ★ OFF AGAIN, AT THE USER'S REQUEST, AND WITH A BETTER INSTRUMENT THIS TIME (2026-09-23).
//
// The user suspects the per-peer level-up is what puts the extra environment action into the
// client's queue: two environment objects holding the SAME std::function, both pushed, and the
// walker that pushes them has a "handled this turn" flag that would have stopped the second push
// from the same object. This flag was already turned off once and the duplicate did not go away
// (PER-PEER lines 0 on both peers, DUPLICATE still 2 on the client), but that run did not carry
// the queue instruments, so "no duplicate" and "the same duplicate" could not be told apart from
// "the feature was off but the fallback path kept the behaviour". This time the push log, the
// backtrace and the walk-object pointer are all in the same build, so a single battle answers it:
// with the flag off, if the two pushes disappear the user's reading is right and the feature is
// what to fix; if they remain, identical obj= values or not, the cause is elsewhere and this goes
// straight back on, because each peer levelling its own cat is a feature the user asked for.
// ★ BACK ON, AND NOW ON THE OTHER SIDE OF THE FIX (2026-09-23, at the user's request).
//
// Off was a control, twice, against two different suspicions -- that this flag's re-point was what
// duplicated a cat's ability in the next battle, and that it was what pushed a second environment
// action. Both of those are now attributed elsewhere and fixed elsewhere: the duplicated ability
// was a cat being written into a live fight by the double-apply path (fixed in mgmp_catsync by the
// detached apply, and rng now AGREES at every boundary in the run), and the second environment
// action disappeared with it (the last run's `APPLY` probe shows no `<<<< DUPLICATE` at all).
//
// What is left, measured in that same run, is a divergence this flag PREVENTS: with it off both
// peers are offered the DRAWN cat's screen, the non-owner pressed a placeholder on someone else's
// cat, and the two cats ended up differing by nine HP -- the turn-0 state-only mismatch above.
// With it on, the screen is re-pointed at this peer's own cat (the LVLSELECT+0x24C0 hook and the
// queue hook at base+0x52AF0 both do it), so every peer answers its own screen and no peer's cat
// is ever written by the other's hand. The known cost stays as documented below: the options were
// rolled for the DRAWN cat, so they are that cat's roll applied to this one. No divergence, a
// cosmetic mismatch in which four options appear.
//
// ★ OFF AGAIN, AND THIS TIME BECAUSE IT IS REDUNDANT (2026-09-23, the user's reading, and the
// design note agrees with it). DESIGN-independent.md sec. 8.3 / S4 says the feature was ALREADY
// complete: "升级各自的猫 -- 这一步已经完成 (proto 29 起, 升级由猫的所有者决定并复制) ... 不需要新
// salt". With the roster and the ownership table right, "independent" means exactly that -- the
// owner answers its own cats' level-ups and the other peer replicates -- and this re-point is an
// EXTRA mechanism on top, not a requirement.
//
// What that extra mechanism costs, measured in the run that turned it back on: it re-points the
// pending level-up at a cat of THIS peer's own, so the two peers level DIFFERENT cats with
// DIFFERENT options
//
//   host   -> level-up: owner=0 cat=...024D ... option=1/4 'TrueSight'
//   client -> level-up: owner=1 cat=...028A ... option=3/4 'Taint'
//
// where the shared draw would have had them answer the SAME cat's screen (the owner deciding, the
// other replicating). Two different options applied to two different cats is two sets of cat data
// that must both be carried and reconciled before the next fight -- strictly more to get wrong,
// for a feature the ownership rule already provides.
constexpr bool kPerPeerLevelTarget = true;
// The queue getter is not a safe substitution point.  On the second level-up
// it can return a live game object whose surrounding entry was already replaced
// by the opener hook; returning a different CatData then makes the game's own
// builder dereference mismatched metadata (the observed client AV at +0x527A2).
// The opener is earlier and sufficient to select the local cat, so leave this
// experimental hook disabled while retaining the code for reference.
constexpr bool kPerPeerQueueHook = false;
// A LevelUpOption's type and its authored name, both read straight off select_option's
// own switch.
//
// THE PARENTHETICAL THAT USED TO BE HERE ("1 stats, 2/3 pools, 4/7 ability, 5, 6 item")
// IS WRONG AND IS GONE (2026-09-23, measured). A live level-up screen offered
// `[0]type=5:'FullPower' [1]type=5:'TrueSight' [2]type=5:'Rockin' [3]type=1:'Speed'` --
// three passive abilities all carrying type 5, so 5 is an ABILITY, not an item. The
// mapping has to be re-derived from live screens (PROBE options in probe_level_stats
// prints type next to name for exactly that) before anything is allowed to depend on a
// type, and in particular before a placeholder answer is chosen by type: the question
// that decides whether a placeholder is safe is "does this option touch the Inventory",
// and that question is still open.
constexpr uintptr_t kLvlOpt_Type = 0;
constexpr uintptr_t kLvlOpt_Name = 200;

// The shipped UI pads out to 4 buttons per screen. This bound only has to be
// loose enough not to reject a legitimate list and tight enough to refuse a
// wild pointer.
constexpr uint32_t kMaxOptions = 64;

using fn_cap = void (__fastcall*)(void*);

struct State {
    bool on        = false;
    bool is_client = false;

    // Resolved from the module base, prologue-checked, exactly like catsync's
    // call targets: a bad address here would run an unrelated function with our
    // arguments on the game's stack.
    uintptr_t base     = 0;
    bool      resolved = false;
    fn_cap    evt_commit = nullptr;   // sub_140937F30
    fn_cap    lvl_click  = nullptr;   // sub_140386810

    void* world_event  = nullptr;   // latest WorldEvent*,    from its update
    void* level_screen = nullptr;   // latest LevelUpScreen*, from its update

    // HOW LONG THIS PEER HAS BEEN SITTING ON A LEVEL-UP SCREEN WHOSE SUBJECT IT DOES
    // NOT OWN (2026-09-22). The "the cat's owner decides" rule is right for a BATTLE
    // upgrade, where both peers open the screen and exactly one of them must answer --
    // but a level-up bought in the SHOP is a LOCAL UI action, so the screen exists on
    // one peer only, and if the roll picks the other player's cat then nobody can
    // answer: this peer swallows its own clicks while it waits, and the owner has no
    // screen to answer from. Reported from play; the screen had no way out at all.
    //
    // The wait is therefore BOUNDED. In the shared cases the owner answers within a
    // frame or two, so the bound is never reached and nothing changes; in the local
    // case it expires and this peer takes its own choice. See choice_on_level_select.
    void*    owner_wait_screen = nullptr;
    uint64_t owner_wait_since  = 0;

    // THIS PEER'S OWN EVENT CLICK, HELD (2026-09-22). The client swallows its clicks on
    // an event screen and then has to decide, from evidence, whether the host's answer
    // is about the screen it is looking at. While it waits, the click it made is kept
    // here -- so that "the events turn out to be different" ends with this player
    // choosing, rather than with a screen nobody can answer.
    uint32_t event_local_index  = 0;
    void*    event_local_screen = nullptr;
    uint64_t event_local_at     = 0;
    bool     event_local_have   = false;
    void* probed_update_screen = nullptr;
    void* probed_button_screen = nullptr;
    uint64_t probed_button_indices = 0;

    // A CHOICE can arrive before this peer's screen exists -- the client is
    // following the host into the node and may not have opened it yet. Hold it
    // rather than dropping it, the way mgmp_follow holds a node.
    bool     pending[2]          = {false, false};
    // The node the host was standing in when it made the choice, and the node
    // this peer is standing in now. A held choice may only be applied when the
    // two agree -- the single check that stops a choice outliving its node.
    uint64_t pending_seed[2]     = {0, 0};
    uint64_t here_seed           = 0;
    uint64_t story_block_seed    = 0;     // the node whose event this client replays from the host: its save is not changed by the result (see choice_story_event_block_active)
    uint32_t story_block_said    = 0;     // bit per kind already logged for that node

    // NODES THIS PEER WILL NEVER ENTER, so a choice that arrives for one after
    // the fact can be refused instead of held forever.
    //
    // The order is not fixed: mgmp_follow can discard a node before the host
    // has even made its choice there, in which case the CHOICE arrives second
    // and there is nothing held for choice_on_node_skipped to drop. Without
    // this ring that message would sit in the pending slot indefinitely and
    // block the next real one.
    //
    // Same device, and the same 16 entries, as the retired-battle_id ring in
    // mgmp_battleid.h -- and for the same reason: an id that falls off the end
    // degrades to "unknown, hold it", never to a wrong apply.
    static constexpr uint32_t kSkippedRing = 16;
    uint64_t skipped[kSkippedRing] = {};
    uint32_t skipped_next          = 0;
    uint32_t pending_index[2]    = {0, 0};
    uint32_t pending_count[2]    = {0, 0};
    uint32_t pending_aux[2]      = {0, 0};
    char     pending_name[2][48] = {};
    bool     pending_warned[2]   = {false, false};

    static constexpr uint32_t kMaxEventChoices = 64;
    ChoiceMsg event_queue[kMaxEventChoices]{};
    uint32_t event_count = 0;
    uint32_t event_step = 0;
    bool event_faulted = false;

    struct HeldLevel {
        ChoiceMsg message;
        uint8_t from = kNoPeer;
        bool warned = false;
    };
    static constexpr uint32_t kPendingPerPeer = 64;
    static constexpr uint32_t kPendingLevels = kPendingPerPeer * kMaxPeers;
    HeldLevel level_pending[kPendingLevels] = {};
    uint32_t level_pending_count = 0;
    uint32_t level_step = 0;
    void* owner_logged_screen = nullptr;
    uint8_t owner_logged_peer = kNoPeer;

    // Set while we drive the game's own commit path, so the hook that exists to
    // SWALLOW a click does not swallow the click we just injected. Not
    // thread-local: every path that touches it is the game thread.
    bool injecting = false;

    uint32_t sent      = 0;
    uint32_t applied   = 0;
    uint32_t swallowed = 0;

    CRITICAL_SECTION cs;
    bool cs_ready = false;
};

State g;

struct Guard {
    Guard()  { if (g.cs_ready) EnterCriticalSection(&g.cs); }
    ~Guard() { if (g.cs_ready) LeaveCriticalSection(&g.cs); }
};

const char* kind_name(uint8_t k) { return k == kChoiceLevelUp ? "level-up" : "event"; }

// Both called with the Guard held.
void remember_skipped(uint64_t seed) {
    if (!seed) return;
    for (uint32_t i = 0; i < State::kSkippedRing; ++i)
        if (g.skipped[i] == seed) return;          // already known, keep it once
    g.skipped[g.skipped_next] = seed;
    g.skipped_next = (g.skipped_next + 1) % State::kSkippedRing;
}

bool was_skipped(uint64_t seed) {
    if (!seed) return false;
    for (uint32_t i = 0; i < State::kSkippedRing; ++i)
        if (g.skipped[i] == seed) return true;
    return false;
}

// --- reading a std::string out of the game ---------------------------------
//
// MSVC's std::string is {union{char buf[16]; char* ptr;}, size, capacity}, so
// the characters are inline when capacity <= 15 and behind a pointer otherwise.
// Copy at most cap-1 and always terminate: a garbled length must not walk.
bool read_game_string(const void* str, char* out, uint32_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!str) return false;
    uint64_t len = 0, capacity = 0;
    if (!mem_read((const uint8_t*)str + 16, &len, sizeof(len))) return false;
    if (!mem_read((const uint8_t*)str + 24, &capacity, sizeof(capacity))) return false;
    if (len > 4096) return false;                  // not a string we recognise
    const uint8_t* chars = (const uint8_t*)str;
    if (capacity > 15) {
        void* p = nullptr;
        if (!mem_read((const uint8_t*)str, &p, sizeof(p)) || !p) return false;
        chars = (const uint8_t*)p;
    }
    uint32_t n = (uint32_t)len;
    if (n > cap - 1) n = cap - 1;
    if (n && !mem_read(chars, out, n)) { out[0] = '\0'; return false; }
    out[n] = '\0';
    return true;
}

// --- the option arrays -----------------------------------------------------

bool read_options(const void* screen, uintptr_t begin_off, uintptr_t end_off,
                  uint8_t*& begin, uint32_t& count) {
    begin = nullptr; count = 0;
    if (!screen) return false;
    uint8_t* b = nullptr; uint8_t* e = nullptr;
    if (!mem_read((const uint8_t*)screen + begin_off, &b, sizeof(b))) return false;
    if (!mem_read((const uint8_t*)screen + end_off,   &e, sizeof(e))) return false;
    if (!b || e < b) return false;
    const uintptr_t bytes = (uintptr_t)(e - b);
    if (bytes % kOptStride) return false;           // not the array we think it is
    const uintptr_t n = bytes / kOptStride;
    if (n == 0 || n > kMaxOptions) return false;
    begin = b; count = (uint32_t)n;
    return true;
}

void option_name(uint8_t kind, const uint8_t* entry, char* out, uint32_t cap) {
    const uintptr_t off = (kind == kChoiceLevelUp) ? kLvlOpt_Name : kEvtOpt_Stat;
    if (!read_game_string(entry + off, out, cap)) snprintf(out, cap, "?");
}

// IS THIS EVENT A STORY EVENT, OR ONE OF THE PER-PLAYER "?" ONES?
// (2026-09-22. See DESIGN-independent.md sec. 5 and 8.)
//
// The design: each player picks its own random "?" events, and only the STORY
// events -- chapter, going home, quest items -- stay shared and host-decided.
// That makes this predicate the thing the whole split hangs on, so it is worth
// being precise about how it is answered.
//
// THE SIGNAL IS ALREADY IN THE OPTION ITSELF, and not by accident: an event
// option's "stat key" (kEvtOpt_Stat) is the string the game compares against
// "str".."lck", "coins", "quest", "none", "home" all the way through
// setupActionChoice -- i.e. THE KEY IS ALSO THE NAME, which is why option_name
// reads this same field. "quest" and "home" are exactly the two kinds the design
// calls story, and the chapter transition is story by construction.
//
// NO NAME LIST IS MAINTAINED. A hardcoded list of story event names would have to
// be re-derived for every content update, and would fail silently when it went
// stale; the authored data already says what an option IS.
//
// UNKNOWN ANSWERS "STORY", and the asymmetry is the reason: a random event treated
// as story costs one independent choice, while a story event treated as random
// lets the peers diverge on the chapter, which breaks the run. Every ambiguous
// reading -- no options, unreadable strings -- therefore takes the safe branch.
//
// The user's corroborating rule, kept here so it is not lost: the event the map
// shows after its FINAL BOSS is always a story event, everything else is random.
// This predicate is expected to agree with that rule; the CHOICE log prints the
// verdict at every event screen, so the first post-boss event in a live session
// is the test of it. If they ever disagree, this predicate is the one to distrust
// -- and the fix is to make it stricter, never looser.
bool event_is_story(uint8_t* begin, uint32_t count) {
    if (!begin || !count) return true;
    for (uint32_t i = 0; i < count; ++i) {
        char key[48] = {};
        if (!read_game_string(begin + (uintptr_t)i * kOptStride + kEvtOpt_Stat,
                              key, sizeof(key)))
            continue;
        if (strcmp(key, "quest") == 0 || strcmp(key, "home") == 0) return true;
    }
    return false;
}

// Log entry before reading: a failed read must not look like a hook that never ran.
void probe_level_screen(void* screen, const char* why) {
    if (!tune::kLevelUpProbe) return;
    char cls[128] = "?";
    rtti_class_name(screen, cls, sizeof(cls));
    log_line("CHOICE", "PROBE level-up %s: screen=0x%llX (%s)",
             why, (unsigned long long)(uintptr_t)screen, cls);

    const void* subject = nullptr;
    if (!screen || !mem_read((const uint8_t*)screen + kLvl_CatData,
                             &subject, sizeof(subject)) || !subject) {
        log_line("CHOICE", "PROBE owner UNKNOWN: screen+0x%X has no readable subject",
                 (unsigned)kLvl_CatData);
        return;
    }
    log_line("CHOICE", "PROBE screen+0x%X -> CatData=0x%llX",
             (unsigned)kLvl_CatData, (unsigned long long)(uintptr_t)subject);
    lockstep_probe_roster_ids(subject);
}

// --- injecting -------------------------------------------------------------
//
// Both are the game's own commit path with our index substituted, rather than a
// reimplementation of what the commit does. That matters: sub_140937F30 also
// sets MewDirector+1812 and calls sub_14091AA00, and the level-up callback also
// copy-constructs the LevelUpOption through sub_14037BBB0. Neither is something
// worth reproducing by hand.

bool inject_event(uint32_t index);   // defined below; the helper above uses it

// TAKE THIS PEER'S OWN HELD EVENT CLICK. Reached when the evidence says the two peers
// are NOT looking at the same event (or the peer's name never arrived): the client's
// click is the only answer that describes the screen actually in front of it.
void apply_local_event_choice(void* screen) {
    if (!g.event_local_have) return;
    if (g.event_local_screen != screen) {      // a different screen: nothing to take
        g.event_local_have = false;
        return;
    }
    const uint32_t index = g.event_local_index;
    g.event_local_have   = false;
    const bool ok = inject_event(index);
    log_line("CHOICE", "-> applied this peer's OWN event choice: option %u%s",
             index, ok ? "" : "  (!! the injection FAILED -- the screen is unanswered)");
}

bool inject_event(uint32_t index) {
    uint8_t* begin = nullptr; uint32_t count = 0;
    if (!read_options(g.world_event, kEvt_OptBegin, kEvt_OptEnd, begin, count)) return false;
    if (index >= count) return false;

    // sub_140937F30 reads only a1+8 and a1+16 and never touches the vftable
    // slot, so a bare three-qword block is a complete capture for it.
    void* cap[3] = { nullptr, g.world_event, begin + (uintptr_t)index * kOptStride };
    if (!g.evt_commit) return false;
    // Calling the spliced address re-enters our own hook, which sees `injecting`
    // and answers "run the original" -- so the game's commit runs exactly once
    // and we do not need the trampoline exposed outside mgmp_hooks.cpp.
    g.injecting = true;
    g.evt_commit(cap);
    g.injecting = false;
    return true;
}

bool inject_level(uint32_t index) {
    uint8_t* begin = nullptr; uint32_t count = 0;
    if (!read_options(g.level_screen, kLvl_OptBegin, kLvl_OptEnd, begin, count)) return false;
    if (index >= count) return false;

    // sub_140386810's capture is {vftable, LevelUpScreen*, int index}; it reads
    // the index as a 32-bit signed at +16 and does the +240*index itself.
    struct Cap { void* vt; void* screen; int32_t index; int32_t pad; };
    Cap cap = { nullptr, g.level_screen, (int32_t)index, 0 };
    if (!g.lvl_click) return false;
    g.injecting = true;
    g.lvl_click(&cap);
    g.injecting = false;
    return true;
}

struct LevelSubject {
    uint64_t id = 0;
    uint64_t seed = 0;
};

bool read_level_subject(void* screen, LevelSubject& subject) {
    const uint8_t* data = nullptr;
    return screen && mem_read((const uint8_t*)screen + kLvl_CatData, &data, sizeof(data)) && data &&
           mem_read(data + kCatData_SaveId, &subject.id, sizeof(subject.id)) &&
           mem_read(data + kCatData_Seed, &subject.seed, sizeof(subject.seed)) &&
           subject.id != 0 && subject.id != UINT64_MAX;
}

bool read_level_option(const uint8_t* option, uint32_t& type, char (&name)[48]) {
    uint64_t length = 0;
    return mem_read(option + kLvlOpt_Type, &type, sizeof(type)) &&
           mem_read(option + kLvlOpt_Name + 16, &length, sizeof(length)) && length < sizeof(name) &&
           read_game_string(option + kLvlOpt_Name, name, sizeof(name));
}

void remove_level_pending(uint32_t index) {
    for (uint32_t i = index + 1; i < g.level_pending_count; ++i)
        g.level_pending[i - 1] = g.level_pending[i];
    --g.level_pending_count;
}

void apply_level_pending(void* screen) {
    if (!g.level_pending_count || !screen) return;
    uint8_t committed = 0;
    if (!mem_read((const uint8_t*)screen + kLvl_Committed, &committed, 1) || committed) return;

    for (uint32_t i = 0; i < g.level_pending_count;) {
        auto& held = g.level_pending[i];
        const ChoiceMsg m = held.message;
        uint8_t sender_owner = kNoPeer;
        if (m.node_seed == g.here_seed && lockstep_cat_owner(m.cat_id, m.cat_seed, sender_owner) &&
            sender_owner != held.from) {
            log_line("CHOICE", "!! discarded held upgrade from peer %u: cat=%016llX belongs to peer %u",
                     (unsigned)held.from, (unsigned long long)m.cat_id, (unsigned)sender_owner);
            remove_level_pending(i);
            continue;
        }
        if (was_skipped(m.node_seed) || (m.node_seed == g.here_seed && m.level_step < g.level_step)) {
            log_line("CHOICE", "discarding completed/stale upgrade: node=%016llX step=%u cat=%016llX",
                     (unsigned long long)m.node_seed, m.level_step, (unsigned long long)m.cat_id);
            remove_level_pending(i);
            continue;
        }
        if (!g.here_seed || m.node_seed != g.here_seed || m.level_step != g.level_step) {
            if (!held.warned) {
                held.warned = true;
                log_line("CHOICE", "holding upgrade: node=%016llX step=%u cat=%016llX; here=%016llX step=%u",
                         (unsigned long long)m.node_seed, m.level_step, (unsigned long long)m.cat_id,
                         (unsigned long long)g.here_seed, g.level_step);
            }
            ++i;
            continue;
        }
        LevelSubject subject;
        uint8_t owner = kNoPeer;
        if (!read_level_subject(screen, subject) ||
            !lockstep_cat_owner(subject.id, subject.seed, owner)) {
            if (!held.warned) {
                held.warned = true;
                log_line("CHOICE", "holding upgrade step=%u: subject/owner not ready", m.level_step);
            }
            return;
        }
        // ★ WHY IS THIS HELD CHOICE NOT BEING APPLIED (2026-09-23). The decision channel is the
        // whole basis of the "sync by construction" plan: the peer sends the OPTION it chose, and
        // this peer re-applies it through the game's own commit path, so both ends hold cat data
        // produced by the same code instead of by a byte-level write (and the byte-level write is
        // the one thing measured to duplicate the environment effect). It never fired: a run with
        // it enabled logged appliedLevel=0 and the two ends differed in seven cats. Every guard
        // that can hold it back is in scope here, so this prints them together -- owner, node seed,
        // step, and whether the screen's subject is the cat the message names. One line per
        // (node, step, cat), because this runs every frame while the screen is up.
        {
            static uint64_t said_node = 0;
            static uint64_t said_cat  = 0;
            static uint32_t said_step = 0xFFFFFFFFu;
            if (said_node != m.node_seed || said_step != m.level_step || said_cat != m.cat_id) {
                said_node = m.node_seed; said_step = m.level_step; said_cat = m.cat_id;
                log_line_lvl(LogLevel::Warn, "CHOICE",
                             "!! held upgrade check: cat=%016llX from=%u owner=%u | node"
                             " msg=%016llX here=%016llX | step msg=%u here=%u | subject"
                             " %016llX/%016llX vs msg %016llX/%016llX",
                             (unsigned long long)m.cat_id, (unsigned)held.from, (unsigned)owner,
                             (unsigned long long)m.node_seed, (unsigned long long)g.here_seed,
                             m.level_step, g.level_step,
                             (unsigned long long)subject.id, (unsigned long long)subject.seed,
                             (unsigned long long)m.cat_id, (unsigned long long)m.cat_seed);
            }
        }

        if (subject.id != m.cat_id || subject.seed != m.cat_seed) {
            if (!held.warned) {
                held.warned = true;
                log_line("CHOICE", "!! upgrade subject mismatch at step=%u: message=%016llX/%016llX "
                                   "screen=%016llX/%016llX -- holding, not choosing another cat",
                         m.level_step, (unsigned long long)m.cat_id, (unsigned long long)m.cat_seed,
                         (unsigned long long)subject.id, (unsigned long long)subject.seed);
            }
            ++i;
            continue;
        }
        if (held.from != owner) {
            log_line("CHOICE", "!! refused upgrade from peer %u: cat=%016llX belongs to peer %u",
                     (unsigned)held.from, (unsigned long long)m.cat_id, (unsigned)owner);
            remove_level_pending(i);
            continue;
        }
        uint8_t* begin = nullptr;
        uint32_t count = 0;
        if (!read_options(screen, kLvl_OptBegin, kLvl_OptEnd, begin, count)) {
            if (!held.warned) {
                held.warned = true;
                log_line("CHOICE", "holding upgrade step=%u: option array not ready", m.level_step);
            }
            return;
        }
        uint32_t type = 0;
        char name[48] = {};
        if (m.count != count || m.index >= count ||
            !read_level_option(begin + (uintptr_t)m.index * kOptStride, type, name) ||
            type != m.aux || strcmp(name, m.name) != 0) {
            if (!held.warned) {
                held.warned = true;
                log_line("CHOICE", "!! upgrade options differ for cat=%016llX step=%u: index=%u "
                                   "count=%u/%u type=%u/%u name='%s'/'%s' -- refusing to apply",
                         (unsigned long long)m.cat_id, m.level_step, m.index,
                         count, m.count, type, m.aux, name, m.name);
            }
            return;
        }
        const uint8_t from = held.from;
        if (!inject_level(m.index) ||
            !mem_read((const uint8_t*)screen + kLvl_Committed, &committed, 1) || !committed) {
            log_line("CHOICE", "!! upgrade injection did not commit: cat=%016llX step=%u",
                     (unsigned long long)m.cat_id, m.level_step);
            return;
        }
        ++g.level_step;
        ++g.applied;
        remove_level_pending(i);
        log_line("CHOICE", "applied peer %u's level-up: cat=%016llX seed=%016llX step=%u option=%u/%u '%s'",
                 (unsigned)from, (unsigned long long)m.cat_id, (unsigned long long)m.cat_seed,
                 m.level_step, m.index, m.count, m.name);
        return;
    }
}

void receive_level_choice(uint8_t from, const ChoiceMsg& m) {
    if (!m.node_seed || !m.cat_id || m.cat_id == UINT64_MAX ||
        !m.count || m.count > kMaxOptions || m.index >= m.count) {
        log_line("CHOICE", "!! invalid upgrade from peer %u -- refused", (unsigned)from);
        return;
    }
    if (was_skipped(m.node_seed) || (m.node_seed == g.here_seed && m.level_step < g.level_step)) {
        log_line("CHOICE", "ignored stale upgrade from peer %u: node=%016llX step=%u",
                 (unsigned)from, (unsigned long long)m.node_seed, m.level_step);
        return;
    }
    uint8_t owner = kNoPeer;
    if (m.node_seed == g.here_seed && lockstep_cat_owner(m.cat_id, m.cat_seed, owner) && owner != from) {
        log_line("CHOICE", "!! refused upgrade from peer %u: cat=%016llX belongs to peer %u",
                 (unsigned)from, (unsigned long long)m.cat_id, (unsigned)owner);
        return;
    }
    uint32_t authored = 0;
    for (uint32_t i = 0; i < g.level_pending_count; ++i) {
        const auto& held = g.level_pending[i];
        if (held.from != from) continue;
        ++authored;
        const ChoiceMsg& old = held.message;
        if (old.node_seed != m.node_seed || old.level_step != m.level_step) continue;
        if (old.cat_id != m.cat_id || old.cat_seed != m.cat_seed ||
            old.index != m.index || old.count != m.count || old.aux != m.aux || strcmp(old.name, m.name) != 0)
            log_line("CHOICE", "!! conflicting upgrade at node=%016llX step=%u -- keeping the first",
                     (unsigned long long)m.node_seed, m.level_step);
        return;
    }
    if (authored >= State::kPendingPerPeer || g.level_pending_count >= State::kPendingLevels) {
        log_line("CHOICE", "!! upgrade queue quota reached for peer %u: refused cat=%016llX node=%016llX step=%u",
                 (unsigned)from, (unsigned long long)m.cat_id, (unsigned long long)m.node_seed, m.level_step);
        return;
    }
    auto& held = g.level_pending[g.level_pending_count++];
    held.message = m;
    held.message.name[sizeof(held.message.name) - 1] = '\0';
    held.from = from;
    held.warned = false;
    log_line("CHOICE", "<- peer %u chose level-up: cat=%016llX seed=%016llX node=%016llX step=%u option=%u/%u '%s'",
             (unsigned)from, (unsigned long long)m.cat_id, (unsigned long long)m.cat_seed,
             (unsigned long long)m.node_seed, m.level_step, m.index, m.count, held.message.name);
}

// WorldEvent+19F9 is set by setupActionChoice (91A818) and cleared by
// commit (91EAB3). Options remain allocated while the coroutine transitions;
// an allocated option array alone does NOT mean this page can accept a click.
bool event_ready(void* screen) {
    uint8_t ready = 0;
    return screen && mem_read((const uint8_t*)screen + 0x19F9, &ready, 1) && ready == 1;
}
void remove_event(uint32_t i) {
    for (uint32_t j = i + 1; j < g.event_count; ++j) g.event_queue[j - 1] = g.event_queue[j];
    --g.event_count;
}
void apply_event_pending(void* screen) {
    if (g.event_faulted || !event_ready(screen)) return;
    for (uint32_t i = 0; i < g.event_count; ++i) {
        const ChoiceMsg m = g.event_queue[i];
        if (m.node_seed != g.here_seed || m.level_step != g.event_step) continue;
        char page[64]{};
        if (!read_game_string((const uint8_t*)screen + kEvt_EventName, page, sizeof(page)) ||
            strcmp(page, m.event_name) != 0) return; // may still be on the previous page
        uint8_t* begin = nullptr; uint32_t count = 0;
        if (!read_options(screen, kEvt_OptBegin, kEvt_OptEnd, begin, count)) return;
        if (m.index >= count || count != m.count) {
            g.event_faulted = true;
            log_line_lvl(LogLevel::Error, "CHOICE", "!! event page '%s' step=%u option count/index differ; choice not applied", page, m.level_step);
            return;
        }
        char key[48]{};
        option_name(kChoiceEvent, begin + m.index * kOptStride, key, sizeof(key));
        if (strcmp(key, m.name) != 0) {
            g.event_faulted = true;
            log_line_lvl(LogLevel::Error, "CHOICE", "!! event page '%s' step=%u options differ; choice not applied", page, m.level_step);
            return;
        }
        // A STORY EVENT, AND ONLY THAT (2026-10-04): the "?" events are shared too now (kPerPlayerNodes is off), so the client replays the host's click on every event -- but a random event is as much the client's own as
        // the host's (its tokens, counters and unlocks are progress this save earns), while a story event is the host's: one of its options is a quest or home option, or the host chose one. For the rest of a story
        // event's node the result of the host's choice may not change THIS peer's save (the commit below runs the option's result script on it).
        // THE QUEST EVENTS ARE NAMED "Quest_<name>" (maps/*.gon: `quest_event { type special_event level Quest_X }`), and their options carry ordinary stat keys ('lck', 'int', ...): the first live test of a story
        // node (Quest_DeadKing, option 'lck') was taken for a random event by the option keys alone. So the name counts first.
        const bool story = strncmp(page, "Quest_", 6) == 0 || event_is_story(begin, count) || strcmp(m.name, "quest") == 0 || strcmp(m.name, "home") == 0;
        if (story) {
            if (g.story_block_seed != g.here_seed) { g.story_block_seed = g.here_seed; g.story_block_said = 0; }
            log_line("CHOICE", "STORY EVENT '%s' (option '%s') replayed from the host: until this node ends the result's changes to THIS peer's save (legacy tokens, quest progress, adventure unlocks, legacy counters, the items it gives) are skipped", page, m.name);
        } else {
            log_line("CHOICE", "event '%s' (option '%s') replayed from the host is a RANDOM one: its result is applied to this peer's save as it always was", page, m.name);
        }
        if (!inject_event(m.index)) return;
        // The native commit must close the choice phase. Do not count a mere
        // callback invocation as success, nor retry an ambiguous partial commit.
        const void* selected = nullptr;
        if (event_ready(screen) ||
            !mem_read((const uint8_t*)screen + 0x100, &selected, sizeof(selected)) ||
            selected != begin + m.index * kOptStride) {
            g.event_faulted = true;
            log_line_lvl(LogLevel::Error, "CHOICE", "!! event '%s' commit not acknowledged by native state", page);
            return;
        }
        remove_event(i);
        ++g.event_step; ++g.applied;
        g.event_local_have = false;
        log_line("CHOICE", "applied host event '%s' step=%u option=%u/%u", page, m.level_step, m.index, count);
        return; // at most one page per native update
    }
}

} // namespace

// ---------------------------------------------------------------------------

void choice_set_base(uintptr_t base) {
    g.base = base;
    g.resolved = false;
    g.evt_commit = nullptr;
    g.lvl_click  = nullptr;

    // An armed client that cannot inject is worse than a desync: it swallows
    // clicks it has no way to replace and the screen is permanently dead with
    // nothing to time out. So both call targets must resolve or the whole
    // feature disarms.
    const uintptr_t evt = addr_of(T_EventChoice);
    const uintptr_t lvl = addr_of_call(C_LevelUpClick);
    if (!evt || !lvl) {
        log_line("CHOICE", "!! %s did not resolve by signature"
                           " -- choice replication is OFF",
                 !evt ? "WorldEvent option commit" : "LevelUpScreen button click");
        return;
    }
    g.evt_commit = (fn_cap)evt;
    g.lvl_click  = (fn_cap)lvl;
    g.resolved = true;
}

void choice_init() {
    if (!g.cs_ready) { InitializeCriticalSection(&g.cs); g.cs_ready = true; }
    g.on          = tune::kChoice;
    g.is_client   = (net_role() == NetRole::Client);
    g.world_event = nullptr;
    g.level_screen = nullptr;
    g.probed_update_screen = nullptr;
    g.probed_button_screen = nullptr;
    g.probed_button_indices = 0;
    g.pending[0] = g.pending[1] = false;
    g.event_count = g.event_step = 0; g.event_faulted = false;
    g.event_local_have = false;
    g.injecting = false;
    if (!g.on) { log_line("CHOICE", "choice replication disabled by net_choice = 0"); return; }
    if (!g.resolved) {
        // Refusing is the safe direction. An armed client SWALLOWS every local
        // click on these screens, so one that cannot inject the host's choice
        // would not desync -- it would sit on a dead screen forever with no
        // way to press anything.
        g.on = false;
        log_line("CHOICE", "!! call targets unresolved -- choice replication is OFF."
                           " This peer keeps its own event and level-up clicks; expect"
                           " the two runs to diverge on the first event.");
        return;
    }
    log_line("CHOICE", "armed -- STORY events (one with a quest/home option): host "
                       "decides, this peer replicates; RANDOM \"?\" events: each peer "
                       "picks its own and nothing crosses the wire; level-ups: the cat "
                       "owner decides, other peers replicate");
}

// Defined further down, next to the trampoline it drives (see kPerPeerLevelTarget).
void install_start_levelup_hook();

// And the three build-site hooks the offline byte scan located (see kLevelScreenProbe).
void install_level_screen_hooks();
void reset_level_caches(bool new_run);

// --- THE NOTE THE DEBUG PANEL READS (2026-09-23) --------------------------------------
//
// See the declaration in the header for why this exists: the picture is not reachable, so the
// panel says what is true instead. The fields are filled every frame a level-up screen is up
// (choice_on_level_update) and the note expires on its own, which is what keeps it from
// outliving the screen: a level-up screen lasts seconds, and a note that lingered could be
// read as being about a cat that is no longer being upgraded.
//
// g_note_drawn is the cat the GAME drew -- the shared draw, the one the picture belongs to --
// and g_note_subject is the cat this peer will actually apply the level-up to, which is this
// peer's own after the per-peer re-point. When the two agree there is nothing to warn about.
static uint64_t g_note_ms      = 0;
static uint64_t g_note_subject = 0;
static uint64_t g_note_drawn   = 0;
static uint8_t  g_note_owner   = 0xFF;
static bool     g_note_mine    = false;

bool choice_level_note(char* out, uint32_t n) {
    if (!out || n < 32) return false;
    if (!g_note_ms || GetTickCount64() - g_note_ms > 4000) return false;
    if (!g_note_subject) return false;

    // THE WORDING HAS TO CHECK WHOSE CAT IT IS, NOT JUST WHETHER THE IDS DIFFER (2026-09-23,
    // measured). The first version said "this peer upgrades ITS OWN cat" whenever the drawn
    // cat and the subject differed -- and the client's second level-up printed exactly that
    // about cat 02DA, which the owner table says belongs to the HOST (owner=0, self=1,
    // "waiting for owner"). A note whose whole purpose is to stop the screen from lying must
    // not lie itself: the "its own" claim is now gated on the subject actually being mine.
    if (g_note_mine && g_note_drawn && g_note_drawn != g_note_subject) {
        _snprintf_s(out, n, _TRUNCATE,
                    "LEVEL-UP: this peer upgrades ITS OWN cat %llX. The picture and the four"
                    " options belong to the shared draw (cat %llX, owner %u) -- the level"
                    " lands on the cat named here, not on the one shown",
                    (unsigned long long)g_note_subject, (unsigned long long)g_note_drawn,
                    (unsigned)g_note_owner);
        return true;
    }
    if (g_note_mine)
        _snprintf_s(out, n, _TRUNCATE,
                    "LEVEL-UP: cat %llX -- this peer's own, this peer decides", 
                    (unsigned long long)g_note_subject);
    else
        _snprintf_s(out, n, _TRUNCATE,
                    "LEVEL-UP: cat %llX -- the OTHER peer's (owner %u); it decides, this peer"
                    " is answered for so it is not held up", 
                    (unsigned long long)g_note_subject, (unsigned)g_note_owner);
    return true;
}

void choice_reset_run() {
    Guard guard;
    reset_level_caches(true);

    // WHAT NAMES THE DRAW, WITHOUT PATCHING ANYTHING (2026-09-23).
    //
    // The old guess here was an execute breakpoint on a vtable store at 0x3C6CFB. It never
    // fired, and the reason is now known from the byte scan: 0x3C6CFB is not part of a
    // level-up screen's construction at all. Two sites ARE known, both found by scanning the
    // pinned build for writes to the screen's own layout, and both are watched instead of
    // hooked -- which is what makes this safe where a hook would not be:
    //
    //   * base+0x3D5F00 fills the screen's option storage by copying 0x20-byte templates out
    //     of globals. It CANNOT be hooked: its prologue is `mov eax,0x53A58; call __chkstk`
    //     (a 343 KB frame) and it reads its payload from the fifth and later STACK arguments,
    //     which a four-argument detour would not forward -- the original would run against
    //     whatever the detour's frame left there. An execute watchpoint forwards nothing and
    //     patches nothing, so the objection does not apply.
    //   * base+0x38F3A0 is the function that stores a vtable into an object at [rbp+0x2E0]
    //     (the construction of a screen-shaped object).
    //
    // WHY THESE TWO, when the goal is the draw: the panel's data is a BY-VALUE COPY taken
    // when the screen is built, so the only way to make it show another cat is to act before
    // that copy -- and the only way to find where "before" is, is the CALLER CHAIN of these
    // two sites. The probe unwinds callers through .pdata and prints them as RVAs, which is
    // exactly the chain that leads up to whoever chose the cat.
    // ARMED THROUGH THE PROBE'S OWN MODE, NOT FROM HERE (2026-09-23, corrected by the log).
    // This block used to hand two execute watchpoints to listprobe_watch_exec -- and the very
    // next run showed the party investigation re-aiming all four slots out from under them:
    //
    //   user exec breakpoint: slot[2] the option-template copier ... armed
    //   slot[0] party.count ... slot[1] party.data ... slot[2] 2nd.count ... slot[3] 2nd.data
    //
    // so the breakpoints were gone before the battle they were armed for. The address now
    // lives in mgmp_listprobe's MODE 3 (tune::kCatSelectorProbe + kRva_PanelBuilder), which
    // owns the tick and therefore owns the slots, and this module arms nothing.
    // INSTALL EARLY, NOT AT THE FIRST LEVEL-UP SCREEN (2026-09-23, measured): the lazy
    // install only armed itself when a screen already existed, so the FIRST level-up of
    // every session ran unprotected -- the user saw the drawn cat again, exactly as the
    // "too late for this screen" note predicted. A new run starts well before any battle,
    // so installing here covers the first one too. Idempotent: the installer keeps a
    // static "tried" flag and logs once.
    // 385A90 is a UI resource helper, not a CatData level-up entry; do not hook it.
    install_level_screen_hooks();     // the build site found by the offline byte scan
    follow_reset_run();
    g.here_seed = 0;
    g.story_block_seed = 0;
    g.level_step = 0;
    g.level_pending_count = 0;
    g.world_event = nullptr;
    g.level_screen = nullptr;
    g.owner_logged_screen = nullptr;
    g.owner_logged_peer = kNoPeer;
    g.probed_update_screen = nullptr;
    g.probed_button_screen = nullptr;
    g.probed_button_indices = 0;
    g.pending[0] = g.pending[1] = false;
    g.event_count = g.event_step = 0; g.event_faulted = false;
    g.event_local_have = false;
    g.pending_warned[0] = g.pending_warned[1] = false;
    memset(g.skipped, 0, sizeof(g.skipped));
    g.skipped_next = 0;
    lockstep_reset_cat_owners();
    log_line("CHOICE", "new run: cleared upgrade sequence, pending choices and cat owners");
}

void choice_shutdown() {
    if (!g.on) return;
    log_line_lvl(LogLevel::Trace, "CHOICE",
             "done: %u sent, %u applied, %u local click(s) suppressed",
             g.sent, g.applied, g.swallowed);
    g.on = false;
    if (g.cs_ready) { DeleteCriticalSection(&g.cs); g.cs_ready = false; }
}

// --- the world event -------------------------------------------------------

bool choice_on_event_commit(void* cap) {
    if (!g.on || !cap) return true;
    if (g.injecting) return true;           // this is our own injected click

    Guard guard;

    void* we    = nullptr;
    void* entry = nullptr;
    if (!mem_read((const uint8_t*)cap + 8,  &we,    sizeof(we)))    return true;
    if (!mem_read((const uint8_t*)cap + 16, &entry, sizeof(entry))) return true;
    g.world_event = we;

    uint8_t* begin = nullptr; uint32_t count = 0;
    const bool have = read_options(we, kEvt_OptBegin, kEvt_OptEnd, begin, count);

    // ONE FUNCTION, TWO KINDS OF EVENT, AND THE SPLIT IS THE POINT (2026-09-22).
    //
    // A STORY event is shown to both peers by construction, so its choice is still
    // the host's and still crosses the wire. A RANDOM "?" event is now EACH
    // PLAYER'S OWN -- the pools are per-player, so the two peers are not even
    // looking at the same screen -- and therefore nothing is replicated at all:
    // the click standing here is the right one, on this peer's event.
    //
    // Replicating a random event's choice would be worse than useless: it would
    // apply a decision about a screen the other peer cannot see. See
    // event_is_story, and DESIGN-independent.md sec. 3 and 5.
    // THE SPLIT ONLY MEANS ANYTHING WHEN THE EVENTS THEMSELVES DIFFER (2026-09-22,
    // and this was a live bug within the hour). With per-player node content OFF
    // (tune::kPerPlayerNodes -- see mgmp_tuning.h) both peers are shown the SAME
    // event, so "each peer picks its own" is not independence: it is one event
    // resolved two ways, which diverges the run. Reported as: the host entered
    // chapter 3 and the client did not.
    //
    // The two halves of the per-player design go on and off TOGETHER, which is what
    // the comment on kPerPlayerNodes says and what this line now enforces.
    // THE DECISION IS NO LONGER MADE HERE (2026-09-22). It used to be made from the
    // OPTIONS ("any option keyed quest/home => story") and that stopped working the
    // moment the two peers began drawing different events: the options are then not
    // common ground, so the two peers could classify the same node differently, and a
    // host that decided "story" published a choice which the client injected BY INDEX
    // into a different event -- options never seen, straight to the resolution.
    //
    // What both peers still share is WHICH EVENT they are looking at, and that is what
    // is used now, on the receiving side, where the evidence is (see apply_event_pending
    // and nodehash_peer_event_name). Here it is only reported.
    if (have) {
        char ours[64] = {};
        if (read_game_string((const uint8_t*)we + kEvt_EventName, ours, sizeof(ours)) && ours[0])
            log_line("CHOICE", "this event is '%s' (%u option(s)); the peer applies our"
                               " choice only if it is looking at this same event",
                     ours, count);
    }

    if (g.is_client) {
        // Swallow as before, and KEEP the click: whether it is used depends on evidence
        // that has not arrived yet. See apply_event_pending and the update tick.
        if (have) {
            const uintptr_t delta = (uintptr_t)((uint8_t*)entry - begin);
            if (delta % kOptStride == 0 && delta / kOptStride < count) {
                g.event_local_index  = (uint32_t)(delta / kOptStride);
                g.event_local_screen = we;
                g.event_local_at     = GetTickCount64();
                g.event_local_have   = true;
                log_line("CHOICE", "held this peer's event click (option %u/%u) until we"
                                   " know whether the peer is looking at this event",
                         g.event_local_index, count);
            }
        }
        ++g.swallowed;
        return false;                        // do NOT run the original
    }

    // Host: name the index, publish it, then let the click through. Published whether
    // or not the peer is looking at the same event -- it drops it if it is not, which
    // is the only side that can tell.
    if (!have) {
        log_line("CHOICE", "!! could not read this event's option array -- the peer will"
                           " not be told which option was picked");
        return true;
    }
    const uintptr_t delta = (uintptr_t)((uint8_t*)entry - begin);
    if (delta % kOptStride || delta / kOptStride >= count) {
        log_line("CHOICE", "!! the chosen option is not an element of this event's array"
                           " -- not publishing");
        return true;
    }

    if (g.event_faulted || !event_ready(we)) return false;
    ChoiceMsg m{};
    m.kind = kChoiceEvent; m.index = (uint32_t)(delta / kOptStride); m.count = count;
    m.node_seed = follow_here_seed(); m.level_step = g.event_step;
    option_name(kChoiceEvent, (const uint8_t*)entry, m.name, sizeof(m.name));
    if (!m.node_seed || !read_game_string((const uint8_t*)we + kEvt_EventName, m.event_name, sizeof(m.event_name)) || !m.event_name[0])
        return false;
    if (!net_send_choice(m)) {
        log_line_lvl(LogLevel::Error, "CHOICE", "!! event choice send failed; host commit held");
        return false;
    }
    ++g.event_step; ++g.sent;
    log_line("CHOICE", "-> event '%s' step=%u option=%u/%u ('%s')", m.event_name, m.level_step, m.index, m.count, m.name);
    return true;
}

bool choice_story_event_block_active() {
    return g.on && g.is_client && g.story_block_seed != 0 && g.story_block_seed == g.here_seed;
}

void choice_story_event_blocked(const char* what) {
    uint32_t bit = 0;
    for (const char* c = what ? what : ""; *c; ++c) bit = bit * 31 + (unsigned char)*c;
    bit = 1u << (bit % 31);
    if (g.story_block_said & bit) return;
    g.story_block_said |= bit;
    log_line_lvl(LogLevel::Warn, "CHOICE", "STORY EVENT: this peer's save was NOT changed -- skipped %s (the host's choice, replayed here)", what ? what : "?");
}

void choice_on_event_update(void* world_event) {
    if (!g.on || !world_event) return;
    Guard guard;
    g.world_event = world_event;
    if (g.is_client) apply_event_pending(world_event);

    // ...AND IF NOTHING ANSWERED OUR CLICK, TAKE IT OURSELVES (2026-09-22). The bound is
    // what turns "waiting for the peer" from a way to stop the run into a delay: in the
    // shared case the peer's name lands in milliseconds, here we are only reached when
    // it never did. See kEventOwnWaitMs.
    if (tune::kPerPlayerNodes && g.is_client && g.event_local_have && g.event_local_screen == world_event &&
        !g.event_count && !g.event_faulted && event_ready(world_event) &&
        (GetTickCount64() - g.event_local_at) >= kEventOwnWaitMs) {
        log_line("CHOICE", "the peer's event name did not arrive within %llu ms -- taking"
                           " this peer's own event choice. Either the two peers are on"
                           " different events (per-player, expected) or the sample was"
                           " lost; letting the screen sit unanswered is not an option.",
                 (unsigned long long)kEventOwnWaitMs);
        apply_local_event_choice(world_event);
    }
}

// --- the level-up screen ---------------------------------------------------

// TEMPORARY (tune::kLevelUpProbe): reach the second level-up screen through one of
// its own buttons. See the declaration in the header for what that screen is and why
// the one-screen-four-indices check makes the read trustworthy.
//
// It has to live in the PUBLIC section, not beside the other probes: those sit inside
// this file's anonymous namespace, where the definition links as an internal symbol
// and the call from mgmp_hooks fails to resolve. (Paid for twice now.)
//
// The name is read per button per frame, which is the cost tune::kLogButtons already
// pays -- hence a temporary instrument rather than a design.
void choice_on_button(void* button) {
    if (!g.on || !button || !tune::kLevelUpProbe) return;

    char name[64];
    if (!mem_read_std_string((const uint8_t*)button + kBtn_Name, name, sizeof(name)))
        return;
    if (strcmp(name, kBtnName_LevelupBoon) != 0) return;

    constexpr uintptr_t kBtn_Callback = 240;   // mgmp_addresses.h kBtn_Callback's value,
                                               // local for the same reason listprobe's is
    void* cb = nullptr;
    if (!mem_read((const uint8_t*)button + kBtn_Callback, &cb, sizeof(cb)) || !cb) {
        log_line("CHOICE", "PROBE boon button '%s': Button+%u did not read as a callable",
                 name, (unsigned)kBtn_Callback);
        return;
    }

    void*    screen = nullptr;
    uint32_t index  = 0;
    if (!mem_read((const uint8_t*)cb + 8,  &screen, sizeof(screen)) || !screen ||
        !mem_read((const uint8_t*)cb + 16, &index,  sizeof(index))) {
        log_line("CHOICE", "PROBE boon button '%s': callable 0x%llX has no "
                           "{screen, index} at +8/+16", name,
                 (unsigned long long)(uintptr_t)cb);
        return;
    }

    Guard guard;
    if (g.probed_button_screen != screen) {
        g.probed_button_screen = screen;
        g.probed_button_indices = 0;
    }
    if (index >= kMaxOptions) {
        log_line("CHOICE", "PROBE boon button: invalid index=%u screen=0x%llX",
                 index, (unsigned long long)(uintptr_t)screen);
        return;
    }
    const uint64_t bit = uint64_t(1) << index;
    if (g.probed_button_indices & bit) return;
    const bool first = g.probed_button_indices == 0;
    g.probed_button_indices |= bit;
    log_line("CHOICE", "PROBE boon button '%s': callable=0x%llX screen=0x%llX "
                       "index=%u seen_indices=%016llX", name,
             (unsigned long long)(uintptr_t)cb,
             (unsigned long long)(uintptr_t)screen, index,
             (unsigned long long)g.probed_button_indices);
    if (first) probe_level_screen(screen, "boon button");
}

bool choice_on_level_select(void* screen, void* opt) {
    if (!g.on || g.injecting) return true;
    Guard guard;
    uint8_t committed = 0;
    if (!screen || !opt || !mem_read((const uint8_t*)screen + kLvl_Committed, &committed, 1)) {
        log_line("CHOICE", "!! suppressed upgrade: screen/commit flag unreadable");
        return false;
    }
    if (committed) return false;
    g.level_screen = screen;

    LevelSubject subject;
    uint8_t owner = kNoPeer;
    if (!g.here_seed || !read_level_subject(screen, subject) ||
        !lockstep_cat_owner(subject.id, subject.seed, owner)) {
        // THE SAME BOUNDED WAIT, AND THIS IS THE GUARD THAT ACTUALLY FIRED (2026-09-22).
        //
        // lockstep_cat_owner needs a battle in progress, and a SHOP purchase is not
        // one: so outside a battle the owner is not "the other player", it is UNKNOWN
        // -- and this branch swallowed every click before the bounded wait further
        // down could ever see them. Measured from play: a skill-upgrade screen in the
        // shop was still unselectable after thirty seconds.
        //
        // The map-side answer exists and is used here for the LOG, so the reading says
        // which of the three cases this is rather than "unknown" for all of them:
        // lockstep_cat_is_mine is the cache copied out of the last battle's split, and
        // it keeps working between nodes. (The action does not depend on it -- the
        // bound below is what unsticks the screen either way.)
        constexpr uint64_t kOwnerWaitMs = 5000;
        const uint64_t now = GetTickCount64();
        if (g.owner_wait_screen != screen) {
            g.owner_wait_screen = screen;
            g.owner_wait_since  = now;
        }
        if ((now - g.owner_wait_since) >= kOwnerWaitMs) {
            bool mine = false;
            const bool known = subject.id && lockstep_cat_is_mine(subject.id, mine);
            log_line_lvl(LogLevel::Warn, "CHOICE",
                         "!! a level-up screen could not identify its owner (cat=%016llX"
                         " seed=%016llX%s%s) after %llu ms -- taking the LOCAL choice."
                         " This is the shop case: no battle, so no control split to read,"
                         " and before this the screen had no way out at all.",
                         (unsigned long long)subject.id, (unsigned long long)subject.seed,
                         known ? (mine ? ", which the last battle said is MINE"
                                       : ", which the last battle said is the PEER's") : "",
                         known ? "" : ", unknown to the last battle too",
                         (unsigned long long)kOwnerWaitMs);
            g.owner_wait_screen = nullptr;
            g.owner_wait_since  = 0;
            return true;
        }
        log_line("CHOICE", "!! suppressed upgrade: cat=%016llX seed=%016llX owner unknown --"
                           " waiting for verified control (%llu ms so far, gives up at %llu)",
                 (unsigned long long)subject.id, (unsigned long long)subject.seed,
                 (unsigned long long)(now - g.owner_wait_since),
                 (unsigned long long)kOwnerWaitMs);
        ++g.swallowed;
        return false;
    }
    if (owner != net_self()) {
        // BOUNDED WAIT -- see the State comment. Record when this screen first asked,
        // and give the owner a few seconds to answer. Anything longer than a person's
        // reaction is enough for the shared cases (the owner's answer arrives in
        // milliseconds, it is the same turn on both peers) and far too short for a
        // player to be left staring at a screen that cannot be answered.
        constexpr uint64_t kOwnerWaitMs = 5000;
        const uint64_t now = GetTickCount64();
        if (g.owner_wait_screen != screen) {
            g.owner_wait_screen = screen;
            g.owner_wait_since  = now;
        }
        const bool waited_long_enough = (now - g.owner_wait_since) >= kOwnerWaitMs;

        // A choice that has ARRIVED but not applied yet is the owner answering: keep
        // waiting for it, however long the bound says, because letting the click
        // through here would race the answer that is already in hand.
        bool answer_in_hand = false;
        for (uint32_t i = 0; i < g.level_pending_count; ++i)
            if (g.level_pending[i].message.cat_id == subject.id) answer_in_hand = true;

        if (waited_long_enough && !answer_in_hand) {
            log_line_lvl(LogLevel::Warn, "CHOICE",
                         "!! this level-up belongs to peer %u and it has not answered in"
                         " %llu ms -- taking the LOCAL choice instead. This happens when"
                         " the screen exists on one peer only (a shop purchase), where the"
                         " owner has nothing to answer from; without this the screen had no"
                         " way out and the run stopped there (measured 2026-09-22).",
                         (unsigned)owner, (unsigned long long)kOwnerWaitMs);
            g.owner_wait_screen = nullptr;
            g.owner_wait_since  = 0;
            return true;                       // let the local click through
        }

        log_line("CHOICE", "suppressed local level-up: cat=%016llX belongs to peer %u (self=%u,"
                           " waited %llu ms%s)",
                 (unsigned long long)subject.id, (unsigned)owner, (unsigned)net_self(),
                 (unsigned long long)(now - g.owner_wait_since),
                 answer_in_hand ? ", the owner's answer is in hand" : "");
        ++g.swallowed;
        return false;
    }
    g.owner_wait_screen = nullptr;   // ours: nothing to wait for
    g.owner_wait_since  = 0;

    uint8_t* begin = nullptr;
    uint32_t count = 0, want_type = 0;
    char want_name[48] = {};
    if (!read_options(screen, kLvl_OptBegin, kLvl_OptEnd, begin, count) ||
        !read_level_option((const uint8_t*)opt, want_type, want_name)) {
        log_line("CHOICE", "!! suppressed upgrade: option identity unreadable");
        return false;
    }
    uint32_t found = 0, matches = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t type = 0;
        char name[48] = {};
        if (!read_level_option(begin + (uintptr_t)i * kOptStride, type, name)) {
            log_line("CHOICE", "!! suppressed upgrade: option %u unreadable", i);
            return false;
        }
        if (type == want_type && strcmp(name, want_name) == 0) { found = i; ++matches; }
    }
    if (matches != 1) {
        log_line("CHOICE", "!! suppressed upgrade: type=%u name='%s' matches %u options", want_type, want_name, matches);
        return false;
    }

    ChoiceMsg m{};
    m.kind = kChoiceLevelUp;
    m.index = found;
    m.count = count;
    m.aux = want_type;
    m.node_seed = g.here_seed;
    m.cat_id = subject.id;
    m.cat_seed = subject.seed;
    m.level_step = g.level_step;
    memcpy(m.name, want_name, sizeof(m.name));
    if (!net_send_choice(m)) {
        log_line("CHOICE", "!! suppressed upgrade: sending cat=%016llX step=%u failed; retry after reconnect",
                 (unsigned long long)m.cat_id, m.level_step);
        return false;
    }
    ++g.sent;
    ++g.level_step;
    log_line("CHOICE", "-> level-up: owner=%u cat=%016llX seed=%016llX node=%016llX step=%u option=%u/%u '%s'",
             (unsigned)owner, (unsigned long long)m.cat_id, (unsigned long long)m.cat_seed,
             (unsigned long long)m.node_seed, m.level_step, m.index, m.count, m.name);

    // PUSH THIS PEER'S CATS THE MOMENT THE CHOICE LANDS (2026-09-23, at the user's request).
    //
    // This is the instant the split's divergence is created: the owner has just committed an
    // option to its OWN cat, so from here until the peer has that cat the two runs hold
    // different cat data. The placeholder path already published at its press; this path --
    // the OWNER's own click, which is the only path that runs when each peer levels its own
    // cat -- did not, so the client's levelled cat rode the one-second map tick or, worse, the
    // next node entry. And the node entry is the one moment that is TOO LATE for the other
    // peer: the host enters first and builds the battle immediately, so a push that leaves
    // with the client's own entry arrives after the host's build. Measured 2026-09-23, twice,
    // as "turn 0 hash mismatch (state only)" with the same rng and queue on both peers.
    //
    // Sending it here leaves the host seconds -- the walk to the next node, a click -- to have
    // applied it, which is exactly what catsync_apply_pending does at its node entry, BEFORE
    // it publishes and before a battle can be built. Cheap and idempotent: one serialize per
    // cat and nothing sent when the bytes already match the baseline.
    catsync_publish("a level-up choice landed on this peer's own cat", false);
    return true;
}

// READ-ONLY, AND IT EXISTS FOR EXACTLY ONE DECISION (2026-09-23).
//
// The plan for parallel level-ups needs a PLACEHOLDER answer on the non-owner's screen --
// something pressed before the owner's real choice arrives, so that peer does not sit and
// wait. the design notes records that all seven option types land on the subject CatData's
// CatStats block (+1804..+1828 = +0x70C..+0x724) or on Inventory::insert_item. That has to
// be true HERE, not merely in the notes, because the two cases behave very differently
// afterwards:
//
//   * a stat/skill option writes the cat, and mgmp_catsync (SerializeCatData, verified
//     bidirectional) overwrites the whole cat from the owner's copy before the next node,
//     so a wrong placeholder simply disappears;
//   * an ITEM option would leave a spare item in the non-owner's inventory, and the
//     inventory push runs host -> client, so for the client's own cats the peer that gets
//     overwritten is the one holding the CORRECT item. That is not a divergence, it is
//     damage, and it is why a placeholder may only be a stat option.
//
// The dump is taken once when a screen is first seen (step N) and once more after that
// screen's choice has gone out (step N+1), so the pair of lines shows which fields the
// option touched. 96 bytes from +0x700 covers the whole named block.
static void probe_level_stats(void* level_screen, uint32_t step) {
    const uint8_t* data = nullptr;
    if (!level_screen ||
        !mem_read((const uint8_t*)level_screen + kLvl_CatData, &data, sizeof(data)) || !data)
        return;

    static const void* last_cat  = nullptr;
    static uint32_t    last_step = UINT32_MAX;
    if (last_cat == (const void*)data && last_step == step) return;
    last_cat  = data;
    last_step = step;

    // AND THE WINDOW ABOVE WAS THE WRONG ONE (2026-09-23, measured). The first live
    // level-up replaced a passive ability and left +0x700..+0x760 byte-identical -- the
    // option wrote somewhere else in the object entirely. Rather than guess a second
    // offset, the dump is now a PER-BLOCK hash of the whole CatData: the step=N and step=N+1
    // lines differ only in the block(s) that changed, so the offset names itself.
    //
    // CatData is 0xC58 bytes (the id at +0xC48 is the last field we know of), so twelve
    // 0x100 blocks cover it. FNV-1a, same as the other hashes in this project; the value
    // printed is folded to 32 bits only to keep the line readable.
    constexpr uint32_t kBlock = 0x100;
    constexpr uint32_t kSpan  = 0xC58;

    uint8_t buf[kSpan] = {};
    if (!mem_read(data, buf, kSpan)) return;

    char line[320] = {};
    int  off = 0;
    for (uint32_t b = 0; b * kBlock < kSpan && off < (int)sizeof(line) - 16; ++b) {
        const uint32_t len = (kSpan - b * kBlock < kBlock) ? (kSpan - b * kBlock) : kBlock;
        uint64_t h = 1469598103934665603ull;
        for (uint32_t i = 0; i < len; ++i) {
            h ^= buf[b * kBlock + i];
            h *= 1099511628211ull;
        }
        off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, "%04X:%08X ",
                           b * kBlock, (uint32_t)(h ^ (h >> 32)));
    }

    log_line("CHOICE", "PROBE blocks step=%u cat=%p: %s", step, (const void*)data, line);
    log_line("CHOICE", "PROBE blocks -- diff the step=N and step=N+1 lines: the block that"
                       " changed is where this option writes. A change confined to the"
                       " CatStats block (+0x70C) is safe as a placeholder on the other"
                       " peer's screen; anything else has to be looked at first");

    // THE CAT'S SERIALIZED IMAGE IS THE RIGHT INSTRUMENT -- AND IT IS NOT REACHABLE FROM
    // HERE YET (2026-09-23). The block hash above came back unchanged for a replaced
    // passive ability, because that option edits an object the CatData only POINTS at,
    // while SerializeCatData walks those pointers and produces exactly what mgmp_catsync
    // compares between the peers. That is the number that decides whether a placeholder
    // answer on the other peer's screen would be reconciled.
    //
    // It cannot be called from this file as things stand, and the reason is worth the two
    // lines: mgmp_catsync.cpp defines `serialize_cat(void*, uint8_t**)` inside its
    // ANONYMOUS NAMESPACE, so the symbol has internal linkage. Declaring it in
    // mgmp_catsync.h (global scope) makes it visible to the compiler and still leaves the
    // linker with "unresolved external symbol ?serialize_cat@@YAIPEAXPEAPEAE@Z" -- which
    // is what happened twice. Fixing it means either moving that function out of the
    // anonymous namespace, or resolving SerializeCatData by signature from here; both are
    // small, and neither belongs in a build that is being used to play.

    // AND THE OPTION TYPES (2026-09-23). The enum is read off select_option's own switch:
    // 1 stats, 2/3 pools, 4/7 ability, 5/6 ITEM. A placeholder answer must be a non-item
    // type: an item option writes Inventory::insert_item, and the inventory push is
    // host -> client -- so for the client's own cats a placeholder item would be the one
    // that survives and the owner's real one the one that is overwritten.
    uint8_t* ob = nullptr;
    uint8_t* oe = nullptr;
    if (mem_read((const uint8_t*)level_screen + kLvl_OptBegin, &ob, sizeof(ob)) &&
        mem_read((const uint8_t*)level_screen + kLvl_OptEnd,   &oe, sizeof(oe)) &&
        ob && oe >= ob) {
        const uintptr_t n = (uintptr_t)(oe - ob) / kOptStride;
        char txt[256] = {};
        int  off = 0;
        for (uintptr_t i = 0; i < n && i < kMaxOptions && off < (int)sizeof(txt) - 48; ++i) {
            uint32_t type = 0;
            char     name[48] = {};
            if (!read_level_option(ob + i * kOptStride, type, name)) continue;
            off += _snprintf_s(txt + off, sizeof(txt) - off, _TRUNCATE, "[%u]type=%u:'%s' ",
                               (unsigned)i, type, name);
        }
        log_line("CHOICE", "PROBE options step=%u n=%u: %s -- types 5 and 6 are ITEM, so a"
                           " placeholder answer must pick one of the others",
                 step, (unsigned)n, txt);
    }
}

// One of MY cats, out of the run's own id list, by session position. Null when ownership
// cannot be read at all -- in which case nothing is changed.
//
// ROTATION, because this module cannot yet read a cat's level or XP (see kCatData_*: only
// the seed, the id and the stats block are known), so successive re-points walk through
// this peer's cats instead of always feeding it the first one.
//
// ★ AND THE ROTATION USED TO STEP ON EVERY CALL, WHICH WAS A BUG THAT SHOWED UP ON SCREEN
// (2026-09-23, measured, and it is the run whose two sides looked like different cats).
//
// One level-up asks this function up to four times -- the opener's write, the screen re-point,
// the level-up start hook and the queue hook -- and `found[next_pick++ % nfound]` handed each
// caller a DIFFERENT one of this peer's cats. Measured in one screen:
//
//   CHOICE  !! OPENER: the pending level-up named cat ...024D (owner 0) -- wrote THIS peer's cat
//           ...028A into the copy the screen is built from, BEFORE it was built
//   CHOICE  !! PER-PEER level-up: the game drew cat ...024D (not this peer's); this peer's screen
//           now has ITS OWN cat ...02AA
//   CHOICE  LEVEL-UP: this peer upgrades ITS OWN cat 2AA
//
// The copy the panel is built from held 28A's data, the module read the screen as 2AA, and the
// level landed on 2AA -- three mechanisms, two different cats, one screen. The panel and the
// effect disagreed about everything, and the next battle opened on a `rng` mismatch (not the usual
// state-only one) with the two rosters visibly apart: 024d at 1/20 against the host's 14/20, 02b3
// at 5/16 against 1/16, and those two even sitting at swapped indices (7 and 8) because the party
// order had been written by different cats' data.
//
// So the pick is now MEMOIZED FOR THE DURATION OF ONE LEVEL-UP: whoever asks first chooses,
// everyone else is handed that same cat, and the rotation happens BETWEEN level-ups -- driven by
// my_level_cat_begin, called from the opener, which is the one place that runs once per level-up
// before anything at all is built.
static uint32_t s_next_pick = 0;
static uint64_t s_pick_key  = 0;
static void*    s_pick_cat  = nullptr;
static void*    s_done_entry = nullptr;
static uint64_t s_done_id = 0;
static void*    s_done_screen = nullptr;
static uint64_t s_done_subject = 0;

// Allocator addresses and the drawn host cat can both repeat across nodes.
// These caches describe one node only; the rotation belongs to one run.
void reset_level_caches(bool new_run) {
    s_pick_key = 0;
    s_pick_cat = nullptr;
    s_done_entry = nullptr;
    s_done_id = 0;
    s_done_screen = nullptr;
    s_done_subject = 0;
    if (new_run) s_next_pick = 0;
}
void* my_level_cat() {
    if (s_pick_cat) return s_pick_cat;      // one level-up, one cat -- see the note above
    const void** slot = (const void**)addr_of_data(D_MewDirectorPtr);
    const void*  md = nullptr;
    void* registry = nullptr;
    const uint64_t* ids = nullptr;
    uint32_t count = 0;
    if (!slot || !mem_read(slot, &md, sizeof(md)) || !md) return nullptr;
    if (!mem_read((const uint8_t*)md + kDir_CatRegistry, &registry, sizeof(registry)) || !registry)
        return nullptr;
    if (!mem_read((const uint8_t*)md + kDir_CatIdCount, &count, 4) || !count || count > 64)
        return nullptr;
    if (!mem_read((const uint8_t*)md + kDir_CatIdData, &ids, sizeof(ids)) || !ids) return nullptr;

    using fn_by_id = void* (__fastcall*)(void* registry, uint64_t id);
    fn_by_id by_id = (fn_by_id)addr_of_call(C_CatDataById);
    if (!by_id) return nullptr;

    // THE LIST AT +1468/+1472 IS THE PARTY, NOT THE WHOLE RUN (2026-09-23, measured).
    //
    // The first version of this function walked only that list and therefore never found a
    // cat of its own on a peer that is not the host: tonight the party held the HOST's four
    // (2da 24d 1fe 2b3) and this peer's four live in the FAMILIARS list at +0x640 -- which
    // is exactly what the control split reads (count at kDir_CatFamiliars+4, data at +8).
    // The re-point returned null, did nothing, said nothing, and the client went on showing
    // the host's cat while the 0 ms placeholder answered for it in 16 ms.
    //
    // Both lists, then: this peer's cats may be in either depending on which side it is.
    const uint64_t* fam       = nullptr;
    uint32_t        fam_count = 0;
    mem_read((const uint8_t*)md + kDir_CatFamiliars + 4, &fam_count, 4);
    mem_read((const uint8_t*)md + kDir_CatFamiliars + 8, &fam, sizeof(fam));
    if (fam_count > 64) fam_count = 0;

    void*    found[8] = {};
    uint32_t nfound = 0;
    for (uint32_t pass = 0; pass < 2 && nfound < 8; ++pass) {
        const uint64_t* list = pass ? fam : ids;
        const uint32_t  n    = pass ? (fam ? fam_count : 0u) : count;
        for (uint32_t i = 0; i < n && nfound < 8; ++i) {
            uint64_t id = 0;
            if (!mem_read(&list[i], &id, sizeof(id)) || !id) continue;
            uint8_t pos = 0;
            if (!lockstep_owner_pos(id, pos) || pos != net_peer_pos()) continue;
            bool dup = false;
            for (uint32_t k = 0; k < nfound && !dup; ++k)
                if (found[k] == by_id(registry, id)) dup = true;
            void* cat = by_id(registry, id);
            if (cat && !dup) found[nfound++] = cat;
        }
    }
    if (!nfound) {
        // SAID OUT LOUD, because a silent null is what made the last round unreadable: the
        // caller changes nothing, and without this line the only visible symptom is that
        // the other player's screen went by.
        static bool said = false;
        if (!said) {
            said = true;
            log_line_lvl(LogLevel::Warn, "CHOICE",
                         "!! PER-PEER level-up: no cat of this peer (position %u) could be"
                         " found in the run: party %u id(s), %u familiar id(s). The subject"
                         " is left as the game drew it.",
                         (unsigned)net_peer_pos(), count, fam_count);
        }
        return nullptr;
    }
    s_pick_cat = found[s_next_pick++ % nfound];
    return s_pick_cat;
}

// A NEW LEVEL-UP GETS A NEW PICK (2026-09-23).
//
// Called by the opener, before anything is built, so the rotation stays "one cat per level-up"
// rather than "one cat for the session" -- and IDEMPOTENT for the same (drawn cat, node), because
// the opener runs many times while a screen is up and must not re-roll the choice underneath the
// mechanisms that have already read it. Those two properties together are what makes all four
// callers name the same cat for one level-up.
void my_level_cat_begin(uint64_t drawn_id) {
    const uint64_t key = drawn_id ^ (g.here_seed * 0x9E3779B97F4A7C15ull);
    if (key == s_pick_key && s_pick_cat) return;      // same level-up: keep the same cat
    s_pick_key = key;
    s_pick_cat = nullptr;
}

// Aim the screen at one of my cats, once per screen. Everything downstream -- the owner log,
// the placeholder above it, the probe -- reads the subject through this same field, so the
// re-point has to happen before any of them look.
void per_peer_level_target(void* level_screen) {
    if (!kPerPeerLevelTarget || !level_screen) return;

    // READ THE SUBJECT FIRST, BECAUSE THE DEDUPE KEY NEEDS IT (2026-09-23, measured).
    //
    // The de-dupe used to be the screen POINTER alone, on the assumption that each level-up
    // builds a new screen object. The game reuses it: the client's second level-up ran with
    // the SAME screen address as its first, so `done_screen == level_screen` held, the
    // function returned before doing anything, and that screen kept the drawn cat --
    //
    //   level-up screen: cat=...02DA owner=0 self=1 -- waiting for owner
    //   !! PARALLEL level-up: ... answered with option 0 after 15 ms of silence from its owner
    //
    // i.e. the one screen the re-point existed for is exactly the one it skipped, and the
    // player watched the screen vanish on its own. (The same reuse is why the block watch set
    // up for the panel's builder never needed a second screen to fire: the block did not have
    // to be freed and reallocated, it was never given up at all.)
    //
    // Keying on the screen AND the subject fixes it and cannot over-fire: two different cats
    // at one address are two different level-ups.
    const uint8_t* cur = nullptr;
    uint64_t cur_id = 0;
    if (!mem_read((const uint8_t*)level_screen + kLvl_CatData, &cur, sizeof(cur)) || !cur) return;
    if (!mem_read(cur + kCatData_SaveId, &cur_id, sizeof(cur_id)) || !cur_id) return;

    if (s_done_screen == level_screen && s_done_subject == cur_id) return;

    uint8_t pos = 0;
    const bool target_seen = lockstep_owner_pos(cur_id, pos) != 0;
    if (target_seen && pos == net_peer_pos()) {     // already mine: leave it alone
        s_done_screen  = level_screen;
        s_done_subject = cur_id;
        return;
    }

    void* mine = my_level_cat();
    if (!mine) return;                              // ownership unreadable: change nothing

    uint64_t new_id = 0;
    if (!mem_read((const uint8_t*)mine + kCatData_SaveId, &new_id, sizeof(new_id)) || !new_id)
        return;
    if (!mem_write((uint8_t*)level_screen + kLvl_CatData, &mine, sizeof(mine))) return;

    s_done_screen  = level_screen;
    s_done_subject = cur_id;
    // What the draw was, remembered for the panel's note: after the write above this module
    // reads the screen as this peer's cat, and the picture stays the drawn cat's, so the two
    // ids are exactly the difference the note has to state.
    g_note_drawn = cur_id;
    log_line_lvl(LogLevel::Warn, "CHOICE",
                 "!! PER-PEER level-up: the game drew cat %016llX (not this peer's); this"
                 " peer's screen now has ITS OWN cat %016llX. The options were rolled for"
                 " the drawn cat -- if they look wrong for the new one, that is why.",
                 (unsigned long long)cur_id, (unsigned long long)new_id);

    // THE LOGIC FOLLOWED, THE PICTURE DID NOT (2026-09-23, measured): the write above made
    // this module read the new cat (owner=1, self=1, choose locally, and the placeholder
    // correctly stopped firing) while the player still saw the DRAWN cat on screen. So the
    // UI is fed from another field of the same screen -- find it: every qword in the
    // screen's first 0x400 bytes that still holds the old subject pointer is a candidate,
    // and the answer is a second write to those offsets rather than another guess.
    char offs[192] = {};
    int  ooff = 0;
    for (uintptr_t o = 0; o + 8 <= 0x400 && ooff < (int)sizeof(offs) - 24; o += 8) {
        uintptr_t v = 0;
        if (!mem_read((const uint8_t*)level_screen + o, &v, 8)) break;
        if (v != (uintptr_t)cur) continue;
        ooff += _snprintf_s(offs + ooff, sizeof(offs) - ooff, _TRUNCATE, " +0x%llX",
                            (unsigned long long)o);
    }
    log_line("CHOICE", "!! PER-PEER: the old subject is still held at%s of screen 0x%llX"
                       " -- what the player SEES comes from one of those fields, not from"
                       " +0x%X (which this module reads)",
             offs[0] ? offs : " NOTHING ELSE", (unsigned long long)(uintptr_t)level_screen,
             (unsigned)kLvl_CatData);

    // AND THE RUN OBJECT, BECAUSE NEITHER OF THE OTHER TWO PANS OUT (2026-09-23, measured).
    //
    // The screen scan above says NOTHING ELSE -- the drawn cat is not held anywhere else in
    // the screen object. And the queue hook (base+0x52AF0) installed cleanly yet never fired
    // for this screen, so that path is not what builds the picture either. The remaining
    // candidate is the run object itself: "the level-up in progress" is exactly the kind of
    // thing a director keeps. Same read-only scan, over its first 0x800 bytes, printing the
    // offsets that hold the drawn cat.
    {
        const void** slot = (const void**)addr_of_data(D_MewDirectorPtr);
        const void*  md   = nullptr;
        char txt[224] = {};
        int  off = 0;
        if (slot && mem_read(slot, &md, sizeof(md)) && md) {
            for (uintptr_t o = 0; o + 8 <= 0x800 && off < (int)sizeof(txt) - 24; o += 8) {
                uintptr_t v = 0;
                if (!mem_read((const uint8_t*)md + o, &v, 8)) break;
                if (v != (uintptr_t)cur) continue;
                off += _snprintf_s(txt + off, sizeof(txt) - off, _TRUNCATE, " +0x%llX",
                                   (unsigned long long)o);
            }
        }
        log_line("CHOICE", "!! PER-PEER: the drawn cat is ALSO held at%s of MewDirector 0x%llX"
                           " -- if one of those is the level-up-in-progress field, that is"
                           " what the panel is reading",
                 txt[0] ? txt : " (nowhere in the first 0x800 bytes)",
                 (unsigned long long)(uintptr_t)md);
    }
}

// --- PER-PEER LEVEL-UP: THE GAME'S OWN START, WITH ITS ARGUMENT SWAPPED (2026-09-23) ---
//
// Everything before this aimed at LevelUpScreen+0xA0 -- the field this module reads -- and
// the measured result was HALF a solution: ownership, choose-locally and the placeholder all
// followed the new cat, while the panel, the stat numbers and the option roll still belonged
// to the cat the game drew (the old subject is held at NOTHING ELSE in the screen object, so
// those come from somewhere the field write cannot reach).
//
// Where they come from: 0x37A4B1 is the ONLY direct call of 0x385A90, and 0x385A90 starts a
// level-up FOR THE CAT IT IS HANDED -- it allocates the screen's subject wrapper once, stores
// it at +0xA0, then runs the real work against its first argument (0x52460 and 0x5254F0 both
// take the cat). The cat arrives as arg1, from 0x52AF0, "the next entry in the level-up
// queue". The third argument is another screen object and is passed through untouched.
//
// So the intervention is exactly one argument wide, and everything else stays the game's own
// work: picture, numbers, the four options and the effects are all built around whatever cat
// this function is given.
//
// ADDRESS DERIVED, NOT SIGNED: the signature table is generated (gen_sigs.py, "do not hand-
// edit") and this build's IDB is not here, so the target is LVLSELECT + 0x24C0 -- a fixed
// offset inside the one build this mod already pins itself to (0x385A90 - 0x3835D0). If
// LVLSELECT does not resolve, nothing is installed and the log says so.
using fn_start_levelup = void (__fastcall*)(void* screen, void* cat, void* host);
fn_start_levelup o_start_levelup = nullptr;

void __fastcall h_start_levelup(void* screen, void* cat, void* host) {
    // FIRST, SAY THAT THE CALL HAPPENED AT ALL (2026-09-23, measured). Every previous run of
    // this hook was silent, and silence has two readings that matter here: the game never
    // starts a level-up through this function on this peer, or it does and our substitution
    // branch declines. One line per distinct cat settles which -- and it also gives us the
    // third argument, the object 0x90A70 handed in, which is the last unexamined candidate
    // for where the PICTURE comes from (the screen holds it nowhere else, the run object does
    // not hold it in its first 0x800 bytes, and the queue getter is never called).
    if (cat) {
        uint64_t seen_id = 0;
        mem_read((const uint8_t*)cat + kCatData_SaveId, &seen_id, sizeof(seen_id));
        static uint64_t said[8] = {};
        static uint32_t said_n = 0;
        bool already = false;
        for (uint32_t i = 0; i < said_n; ++i) if (said[i] == seen_id) already = true;
        if (!already && said_n < 8) {
            said[said_n++] = seen_id;
            char txt[160] = {};
            int  off = 0;
            for (uintptr_t o = 0; o + 8 <= 0x200 && off < (int)sizeof(txt) - 24; o += 8) {
                uintptr_t v = 0;
                if (!host || !mem_read((const uint8_t*)host + o, &v, 8)) break;
                if (v != (uintptr_t)cat) continue;
                off += _snprintf_s(txt + off, sizeof(txt) - off, _TRUNCATE, " +0x%llX",
                                   (unsigned long long)o);
            }
            log_line_lvl(LogLevel::Warn, "CHOICE",
                         "!! PER-PEER (start): the game called the level-up start with cat"
                         " %016llX (screen 0x%llX, host 0x%llX); the host object holds that"
                         " cat at%s",
                         (unsigned long long)seen_id, (unsigned long long)(uintptr_t)screen,
                         (unsigned long long)(uintptr_t)host,
                         txt[0] ? txt : " (nowhere in its first 0x200 bytes)");
        }
    }
    if (kPerPeerLevelTarget && cat) {
        uint64_t id = 0, mine_id = 0;
        uint8_t  pos = 0;
        // THE FIRST ARGUMENT OF THIS FUNCTION IS NOT A CatData* -- MEASURED, 2026-09-23.
        //
        // The observation line above the substitution prints it: the game called this with
        // `cat 000000854B8FF0A8`, which is a plain heap pointer, not a cat (reading a save id
        // out of it yields nonsense). So this branch has never matched, and if it ever did it
        // would hand the game a CatData where it expects something else -- a crash, not a
        // level-up. Hence the plausibility gate: a real cat's id is a small number (the run's
        // ids are 32-bit and in the low thousands), so anything else is refused outright.
        if (mem_read((const uint8_t*)cat + kCatData_SaveId, &id, sizeof(id)) && id &&
            id < (1ull << 32) &&
            lockstep_owner_pos(id, pos) && pos != net_peer_pos()) {
            void* mine = my_level_cat();
            if (mine && mem_read((const uint8_t*)mine + kCatData_SaveId, &mine_id,
                                 sizeof(mine_id)) && mine_id) {
                log_line_lvl(LogLevel::Warn, "CHOICE",
                             "!! PER-PEER (hook): the game is starting a level-up for cat"
                             " %016llX, which is not this peer's -- handing it MY cat %016llX"
                             " instead, so the screen, the numbers, the four options and the"
                             " effects are the game's own work around that cat.",
                             (unsigned long long)id, (unsigned long long)mine_id);
                cat = mine;
            }
        }
    }
    o_start_levelup(screen, cat, host);
}

// --- AND THE LAYER THE PICTURE ACTUALLY READS: THE QUEUE ENTRY (2026-09-23) -----------
//
// The hook above swapped the cat handed to "start the level-up", and the log shows it works
// for the EFFECT: at one node the host levelled 24D and the client levelled 28A, each
// choosing locally (-> level-up: owner=0 cat=24D / owner=1 cat=28A). The PANEL still drew the
// drawn cat, and the scan settled why: the old subject is held at NOTHING ELSE in the screen
// object, so the picture is not built from that field at all.
//
// It is built from the level-up QUEUE ENTRY. 0x52AF0 is "take the next entry from the queue"
// -- 55 bytes, called with a string key in rdx, filling a 0x20 struct the caller owns and
// RETURNING the cat (that is the value 0x37A4B1 feeds to set_subject). Hooking it covers the
// picture, the stat numbers and the option roll together, because all three are derived from
// what it hands back.
//
// 0x52AF0 is not in the signature table, so the address is module base + RVA -- the base the
// mod already hands this module through choice_set_base.
using fn_next_levelup = void* (__fastcall*)(void* out_struct, const char* key);
fn_next_levelup o_next_levelup = nullptr;

void* __fastcall h_next_levelup(void* out_struct, const char* key) {
    void* cat = o_next_levelup(out_struct, key);
    if (!kPerPeerLevelTarget || !cat) return cat;

    uint64_t id = 0;
    uint8_t  pos = 0;
    if (!mem_read((const uint8_t*)cat + kCatData_SaveId, &id, sizeof(id)) || !id) return cat;
    if (!lockstep_owner_pos(id, pos) || pos == net_peer_pos()) return cat;   // mine already

    void* mine = my_level_cat();
    if (!mine || mine == cat) return cat;

    uint64_t mine_id = 0;
    if (!mem_read((const uint8_t*)mine + kCatData_SaveId, &mine_id, sizeof(mine_id)) || !mine_id)
        return cat;

    // BOTH PLACES: the entry the caller is filling, and the return value. Whichever of the
    // two the picture reads, it now reads this peer's own cat.
    if (out_struct) {
        for (uintptr_t o = 0; o + 8 <= 0x20; o += 8) {
            uintptr_t v = 0;
            if (!mem_read((const uint8_t*)out_struct + o, &v, 8)) break;
            if (v == (uintptr_t)cat)
                mem_write((uint8_t*)out_struct + o, &mine, sizeof(mine));
        }
    }
    log_line_lvl(LogLevel::Warn, "CHOICE",
                 "!! PER-PEER (queue): the level-up entry names cat %016llX, which is not"
                 " this peer's -- rewritten to MY cat %016llX in the entry and in the"
                 " return, so the panel, the numbers and the options are built around it",
                 (unsigned long long)id, (unsigned long long)mine_id);
    return mine;
}

void install_start_levelup_hook() {
    static bool tried = false;
    if (tried) return;
    tried = true;
    if (!kPerPeerLevelTarget) return;
    void* sel = (void*)addr_of(T_LevelSelect);
    if (!sel) {
        log_line_lvl(LogLevel::Warn, "CHOICE",
                     "!! PER-PEER hook: LVLSELECT did not resolve, so the derived address"
                     " cannot be trusted -- NOT installed");
        return;
    }
    void* target = (uint8_t*)sel + 0x24C0;
    if (MH_CreateHook(target, (void*)&h_start_levelup, (void**)&o_start_levelup) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        log_line_lvl(LogLevel::Warn, "CHOICE",
                     "!! PER-PEER hook: MinHook refused 0x%llX (LVLSELECT+0x24C0) -- NOT"
                     " installed; the +0xA0 re-point still runs on its own",
                     (unsigned long long)(uintptr_t)target);
        o_start_levelup = nullptr;
        return;
    }
    log_line_lvl(LogLevel::Warn, "CHOICE",
                 "!! PER-PEER hook INSTALLED at 0x%llX (LVLSELECT+0x24C0): the game's own"
                 " level-up start now takes this peer's own cat",
                 (unsigned long long)(uintptr_t)target);

    // And the queue entry, which is what the picture is built from.  The opener
    // substitution above is the supported path; the queue hook is intentionally
    // disabled because it returns a different object than the game's entry owns.
    if (!kPerPeerQueueHook) {
        log_line_lvl(LogLevel::Warn, "CHOICE",
                     "!! PER-PEER (queue) hook disabled: opener substitution is used;"
                     " returning a different queue object caused second-level-up AVs");
        return;
    }
    if (!g.base) {
        log_line_lvl(LogLevel::Warn, "CHOICE",
                     "!! PER-PEER (queue): this module was never handed the module base --"
                     " the queue hook is NOT installed");
        return;
    }
    void* qtarget = (void*)(g.base + 0x52AF0);
    if (MH_CreateHook(qtarget, (void*)&h_next_levelup, (void**)&o_next_levelup) != MH_OK ||
        MH_EnableHook(qtarget) != MH_OK) {
        log_line_lvl(LogLevel::Warn, "CHOICE",
                     "!! PER-PEER (queue): MinHook refused 0x%llX -- the panel will keep"
                     " showing the drawn cat", (unsigned long long)(uintptr_t)qtarget);
        o_next_levelup = nullptr;
        return;
    }
    log_line_lvl(LogLevel::Warn, "CHOICE",
                 "!! PER-PEER (queue) hook INSTALLED at 0x%llX (base+0x52AF0): the level-up"
                 " entry itself now names this peer's own cat",
                 (unsigned long long)(uintptr_t)qtarget);
}

// --- THE BUILD SITE, FOUND OFFLINE (2026-09-23) --------------------------------------
//
// Four runtime instruments had already failed to name where the panel's snapshot comes from:
// a watchpoint on +0xA0 (armed on an address that does not exist until AFTER the build), the
// run object's first 0x800 bytes (not there), the queue getter at 0x52AF0 (never called), and
// an execute breakpoint on the vtable store at 0x3C6CFB (never executed -- wrong guess). The
// static question is the right one, and tools/scan_store_a0.py existed to answer it.
//
// Scanning the pinned build for STORES to this screen's own known layout -- +0xA0 the subject
// (what this module reads), +0x318 the committed flag, +0x360/+0x368 the option array's
// begin/end -- put four stores inside one small span, 0x54FAC..0x5586B, in three functions:
//
//   0x54D3D  writes +0x368 (the option array's end)
//   0x5511D  writes +0xA0  (the subject)
//   0x557ED  writes +0x360 AND +0x368 (the array, seven bytes apart)
//
// That pair of functions IS the build site -- one sets the subject, one fills the options --
// which is why the panel, the stat numbers and the four options agree with each other and
// none of them with a post-hoc write to +0xA0. And none of the three has a direct E8 caller,
// so they are reached through a vtable: that is the real reason every "hook whoever opens the
// screen" attempt came up empty. There is no direct call to find, and the screen object does
// not exist yet when a runtime watchpoint could first be armed.
//
// THIS BLOCK MEASURES FIRST. All three are hooked; each logs its arguments once per distinct
// pointer. Only the subject writer is allowed to substitute (kLevelScreenSubst), because its
// shape is the one that is known -- rcx = the screen, rdx = the cat -- while the other two
// take arguments that have not been identified yet. The substitute is the SAME cat this
// module already hands the level-up start, so if it lands, the picture, the numbers and the
// option roll are all the game's own work done around this peer's cat, and the panel stops
// lying -- which is what the whole per-peer split needed.
constexpr bool kLevelScreenProbe = true;
// ★ OFF TOO, AND THE USER WAS RIGHT ABOUT WHY (2026-09-23). Turning kPerPeerLevelTarget off did
// not stop this peer from levelling its own cat, and the reason is that the machinery has THREE
// independent switches, not one: kPerPeerLevelTarget re-points the screen and installs the hooks,
// while the two substitution flags below replace the cat at the build site and at the
// entry->cat resolver. With only the first one off, these two happily kept handing the peer's cat
// back as this peer's own, so the "control" test was not a control at all -- the behaviour it was
// meant to remove was still running through a different door. All three go off together now.
constexpr bool kLevelScreenSubst = true;

// AND THE RESULT SUBSTITUTION IS OFF -- MEASURED, NOT FEARED (2026-09-23). 0x52460 is a
// GENERIC key -> object resolver, not the level-up's own conversion. On the client it was
// called with the keys a settings screen uses -- "label", "normal", "hair_toggle",
// "dust_toggle", "noise_toggle", "timer" -- all of which resolved to "not a cat of this run",
// and then once with a key resolving to cat 01FE while the screen on display was the level-up
// of cat 24D. So the panel is not built from this call at all, and substituting its result
// whenever it happened to name a cat would apply SOMEONE ELSE'S level-up to this peer's cat:
// a real extra level, on the peer that owns that cat, with nothing to reconcile it away.
// Logged, not patched, until the call that actually builds the panel is named.
constexpr bool kEntryToCatSubst = false;

// The pinned build's RVAs, not signatures: if the game updates these are wrong, so each is
// checked against the expected prologue and refused (with a log line) rather than patched.
//
// THREE BYTES IN, AND THE FIRST DEPLOY PROVED WHY THE CHECK EXISTS (2026-09-23): the scan
// walks back to the 0xCC padding before a function and reports the first byte AFTER the run,
// but the version that printed the RVAs started one byte inside that run -- so all three
// addresses landed on 0xCC. Every hook reported
//
//   !! BUILD SITE 0x5511D ...: the prologue does not match this build (got CC CC CC 48 ...)
//
// and nothing was patched. The real entries are the first byte after the padding, i.e. the
// reported value plus three, and their prologues are the ones expected below.
constexpr uintptr_t kRvaLvlOptEnd     = 0x54D40;
constexpr uintptr_t kRvaLvlSetSubject = 0x55120;
constexpr uintptr_t kRvaLvlOptions    = 0x557F0;

// AND THE ONE THAT MATTERS (2026-09-23, read out of the pinned build instruction by
// instruction). Following the call site the notes call "the only direct caller of the
// level-up start" (0x37A4B1) gave the whole chain, and it explains every dead end so far:
//
//   37A4A3  call 0x52AF0        ; the queue getter
//   37A4AB  mov  rdx, rax       ; arg2 = the ENTRY -- and 0x52AF0 ends in "mov rax, rbx;
//                               ;   ret", i.e. it returns the CALLER'S OWN 0x20 struct,
//                               ;   not a cat. That is why the queue hook never substituted
//                               ;   anything: it read a save id out of a struct of pointers.
//   37A4B1  call 0x385A90       ; the level-up start
//
// and inside the start (0x385A90):
//
//   385B0F  mov  rdx, rbp       ; the entry
//   385B17  call 0x52460        ; ENTRY -> CAT, returning the cat in rax
//   385B22  mov  rdx, rax
//   385B28  call 0x90B10        ; apply this level-up to that cat
//
// So the cat the panel, the numbers and the four options are all built around comes out of
// 0x52460, whose argument is a queue entry and whose RETURN VALUE is the cat. That makes it
// the one place where substituting a value reaches the drawn cat itself, and it is a function
// with a direct caller -- so unlike the three setters above, this hook is aimed at the draw.
constexpr uintptr_t kRvaLvlEntryToCat = 0x52460;

// AND THE PANEL BUILDER ITSELF (2026-09-23), found from the inside -- see the note beside
// kRebuildPanelForOwnCat for the chain that led here. Its prologue spills arg2's low byte and
// then pushes five registers, i.e. (void* screen, char flag), and it reads the cat from the
// screen's own subject field -- which is the field this module already re-points.
constexpr uintptr_t kRvaLvlBuildPanel = 0x384450;

using fn_lvl2 = void* (__fastcall*)(void*, void*);
using fn_lvl3 = void* (__fastcall*)(void*, void*, void*);
fn_lvl2 o_lvl_set_subject = nullptr;
fn_lvl3 o_lvl_opt_end     = nullptr;
fn_lvl3 o_lvl_options     = nullptr;
fn_lvl2 o_lvl_entry_to_cat = nullptr;

// "Is this pointer a cat this run knows?" -- the owner table is the same one the re-point
// uses, so this test cannot disagree with the rest of the module about what a cat is.
static bool lvl_looks_like_cat(const void* p, uint64_t& id, uint8_t& pos) {
    id = 0;
    if (!p) return false;
    if (!mem_read((const uint8_t*)p + kCatData_SaveId, &id, sizeof(id)) || !id) return false;
    return lockstep_owner_pos(id, pos) != 0;
}

static void* __fastcall h_lvl_set_subject(void* self, void* cat) {
    uint64_t id = 0;
    uint8_t  pos = 0;
    const bool is_cat = lvl_looks_like_cat(cat, id, pos);

    // One line per distinct argument, so a per-frame caller cannot flood the log.
    static void*    said[8] = {};
    static uint32_t said_n  = 0;
    bool fresh = true;
    for (uint32_t i = 0; i < said_n; ++i) if (said[i] == cat) fresh = false;
    if (fresh && said_n < 8) {
        said[said_n++] = cat;
        log_line_lvl(LogLevel::Warn, "CHOICE",
                     "!! BUILD SITE (subject 0x%llX, called with self=0x%llX arg=0x%llX):"
                     " %s", (unsigned long long)(unsigned long long)kRvaLvlSetSubject,
                     (unsigned long long)(uintptr_t)self, (unsigned long long)(uintptr_t)cat,
                     is_cat ? "the argument IS a cat of this run"
                            : "the argument is NOT a cat (no known save id)");
    }

    if (kLevelScreenSubst && is_cat && pos != net_peer_pos()) {
        void*    mine   = my_level_cat();
        uint64_t mid    = 0;
        uint8_t  mpos   = 0;
        if (mine && mine != cat && lvl_looks_like_cat(mine, mid, mpos)) {
            log_line_lvl(LogLevel::Warn, "CHOICE",
                         "!! BUILD SITE (subject): the screen was being built for cat"
                         " %016llX (owner %u), which is not this peer's -- handing it MY cat"
                         " %016llX instead, so the panel, the numbers and the options are the"
                         " game's own work around that cat.",
                         (unsigned long long)id, (unsigned)pos, (unsigned long long)mid);
            cat = mine;
        }
    }
    return o_lvl_set_subject(self, cat);
}

static void* __fastcall h_lvl_opt_end(void* a0, void* a1, void* a2) {
    static uint32_t hits = 0;
    if (++hits <= 3 || hits % 300 == 0)
        log_line("CHOICE", "BUILD SITE (option end 0x%llX): a0=0x%llX a1=0x%llX a2=0x%llX"
                           " (hit %u)",
                 (unsigned long long)(unsigned long long)kRvaLvlOptEnd,
                 (unsigned long long)(uintptr_t)a0, (unsigned long long)(uintptr_t)a1,
                 (unsigned long long)(uintptr_t)a2, hits);
    return o_lvl_opt_end(a0, a1, a2);
}

static void* __fastcall h_lvl_options(void* a0, void* a1, void* a2) {
    static uint32_t hits = 0;
    if (++hits <= 3 || hits % 300 == 0)
        log_line("CHOICE", "BUILD SITE (options 0x%llX): a0=0x%llX a1=0x%llX a2=0x%llX"
                           " (hit %u)",
                 (unsigned long long)(unsigned long long)kRvaLvlOptions,
                 (unsigned long long)(uintptr_t)a0, (unsigned long long)(uintptr_t)a1,
                 (unsigned long long)(uintptr_t)a2, hits);
    return o_lvl_options(a0, a1, a2);
}

// THE DRAW ITSELF, AS A RETURN VALUE (2026-09-23). See kRvaLvlEntryToCat for the chain that
// leads here. The entry is the queue getter's 0x20-byte struct, so its four qwords are logged
// together with, for each, whether it points at a cat this run knows: that is what says where
// the cat sits inside an entry, and therefore which function QUEUED it -- the place an honest
// per-peer draw would have to intervene.
static void* __fastcall h_entry_to_cat(void* out, void* entry) {
    void* cat = o_lvl_entry_to_cat(out, entry);

    uint64_t id  = 0;
    uint8_t  pos = 0;
    const bool is_cat = lvl_looks_like_cat(cat, id, pos);

    static void*    said[8] = {};
    static uint32_t said_n  = 0;
    bool fresh = true;
    for (uint32_t i = 0; i < said_n; ++i) if (said[i] == entry) fresh = false;
    if (fresh && said_n < 8) {
        said[said_n++] = entry;
        char q[256] = {};
        int  off = 0;
        for (uintptr_t o = 0; o < 0x20 && off < (int)sizeof(q) - 40; o += 8) {
            uintptr_t v = 0;
            if (!entry || !mem_read((const uint8_t*)entry + o, &v, 8)) break;
            uint64_t vid  = 0;
            uint8_t  vpos = 0;
            const bool vcat = v && lvl_looks_like_cat((const void*)v, vid, vpos);
            off += _snprintf_s(q + off, sizeof(q) - off, _TRUNCATE, " +%llX=%llX%s",
                               (unsigned long long)o, (unsigned long long)v,
                               vcat ? "(CAT!)" : "");
        }
        log_line_lvl(LogLevel::Warn, "CHOICE",
                     "!! ENTRY->CAT: entry 0x%llX holds%s -- and 0x%llX resolved it to %s",
                     (unsigned long long)(uintptr_t)entry, q,
                     (unsigned long long)(unsigned long long)kRvaLvlEntryToCat,
                     is_cat ? "a cat of this run" : "something that is NOT a cat of this run");
    }

    if (kEntryToCatSubst && is_cat && pos != net_peer_pos()) {
        void*    mine = my_level_cat();
        uint64_t mid  = 0;
        uint8_t  mpos = 0;
        if (mine && mine != cat && lvl_looks_like_cat(mine, mid, mpos)) {
            log_line_lvl(LogLevel::Warn, "CHOICE",
                         "!! ENTRY->CAT: the entry resolved to cat %016llX (owner %u), which"
                         " is not this peer's -- returning MY cat %016llX instead (see"
                         " kEntryToCatSubst for why this is off by default).",
                         (unsigned long long)id, (unsigned)pos, (unsigned long long)mid);
            return mine;
        }
    }
    return cat;
}

// --- B: THE OPENER, AND THE PENDING LEVEL-UP IT READS (2026-09-23) --------------------
//
// The panel turned out to be a snapshot of the game's PENDING LEVEL-UP STATE, not of the screen
// object: the panel builder was found and run a second time with the game's own arguments, and
// the picture, the numbers and the four options came out byte-identical to the first pass --
// because that state still named the drawn cat. Re-writing the screen (the subject at +0xA0, the
// option container at +0x360/+0x368) therefore cannot reach what the player sees; the only layer
// that can is the state the screen is built FROM.
//
// That layer has a name now. The opener 0x37A290 is the one function in the level-up flow that
// runs BEFORE anything is built: it fetches the entry, builds the screen, and is the only direct
// caller of both the start function (0x385A90) and the panel builder (0x384450):
//
//   37A4A3  call 0x52AF0      ; the key -> string lookup, the "entry"
//   37A48F  call 0x97AF30     ; the screen object
//   37AED7  call 0x384450     ; the panel
//   37A4B1  call 0x385A90     ; apply
//
// Its prologue spills rcx, rdx and r8 and nothing else (measured in the image), so it takes three
// arguments and can be hooked without forwarding stack arguments -- the objection that stopped
// the panel builder itself from being hooked.
//
// WHAT THIS DOES FIRST IS READ. For each call it scans the first argument (the state object) for
// pointers that resolve to a cat this run knows -- the same test the rest of the module uses, so
// it cannot disagree about what a cat is -- and logs the offsets it finds. One level-up is enough
// to see where the pending cat lives; substituting it is a one-line write in this same hook, and
// it is deliberately NOT done yet: several fields in one object may point at the same cat, and
// writing all of them would be a guess where the log can give an answer.
constexpr uintptr_t kRvaLvlOpener = 0x37A290;
// OFF after it did its job (2026-09-23). The scan below is kept as the RECORD of how the pending
// level-up was found -- one line, on both peers, naming +C48=id <the drawn cat> inside the entry --
// and it is off because it answered the question: it walks 3 x 0x1000 bytes on every opener call,
// which is a per-frame cost for information that is now in the design notes.
constexpr bool      kOpenerProbe = false;

using fn_lvl_opener = void* (__fastcall*)(void*, void*, void*);
fn_lvl_opener o_lvl_opener = nullptr;

// AND THE WRITE THAT ENDS THE HUNT (2026-09-23). The scan above answered the question with one
// line, on both peers:
//
//   !! OPENER scan: entry 0x... holds cat material at +BC8=id 1FE +BD8=id 2DA +C48=id 24D
//
// 0xC48 is kCatData_SaveId, so the object the opener is handed CONTAINS A CatData COPY whose
// first byte is at entry+0 -- and that copy is what the screen, the numbers and the four options
// are built from. Nothing done to the screen object afterwards can reach it, which is what six
// instruments and one dead-end write proved.
//
// So the fix is here, and it is a write rather than a re-point: while the pending level-up names
// a cat that is not this peer's, overwrite that copy with THIS peer's cat -- the game's own
// serializer, in read mode (catsync_deserialize_into) -- before the original opener runs. Then
// the screen is built, and the effect applied, around this peer's cat, by the game's own code.
//
// ON, and one line to turn off. It is called once per entry, not once per frame, because the
// opener runs often while a screen is up and the copy only has to be made once.
// ★ OFF, AS THE SINGLE VARIABLE OF A CLEAN TEST (2026-09-23, at the user's insistence and they
// were right twice over). This is the ONE switch that implements "each peer levels its own cat":
// it overwrites the CatData copy inside the pending level-up entry with this peer's cat, using
// the game's own serializer in read mode, before the opener runs. Turning off
// kPerPeerLevelTarget did not stop it, because that flag only covers the screen re-point and the
// hooks -- the behaviour the user kept seeing came through here. And the write is a
// RE-DESERIALIZE (catsync_deserialize_into), which is exactly the operation that appends a
// registration instead of replacing one -- the shape the queue side reported as "two environment
// objects holding the same std::function". So with this off and everything else normal, one
// battle says whether the duplicated push survives:
//   pushes drop to 1 -> this write is the cause, and the feature can be rebuilt to replace
//                       instead of append;
//   pushes stay at 2 -> it is not this, and the cat-apply path is next in line.
// ★ BACK ON (2026-09-23, at the user's request), and this is the switch that makes the PANEL
// per-peer -- the layer the +0xA0 re-point cannot reach.
//
// the design notes §2(c) records what this write does and that it was VERIFIED IN GAME:
//
//   "THE FIX, THEREFORE, IS ONE WRITE, AT THE SOURCE, BEFORE ANYTHING IS BUILT ... and then let the
//    original opener run. The screen, the numbers and the four options are then the game's own work
//    around this peer's cat, the roll is this cat's own roll, and the effect lands on this peer's
//    cat -- verified in game: the two peers level DIFFERENT cats with DIFFERENT options, each
//    chooses locally, and the display is correct on both."
//
// Measured today with it off, which is why it is back on: the re-point still worked (`level-up
// screen: cat=...028A ... owner=1 self=1 -- choose locally` and `LEVEL-UP: this peer upgrades ITS
// OWN cat 28A`), and the player STILL saw the drawn cat 24D -- because the panel is a snapshot of
// the pending level-up's CatData copy, and only this write changes that copy.
//
// It went off as the single variable of the clean test described below. That test has been
// answered, and by a different change: the duplicated environment action it was aimed at is gone
// with the detached cat apply in mgmp_catsync, and the run that measured the fix carried NO
// `<<<< DUPLICATE` on either peer with this off and the re-point on. So the reading to watch now is
// the reverse one: if a duplicate reappears with this ON, then the re-deserialize below appends a
// registration where a replace was needed, and the fix is to make THIS write a replace -- not to
// keep the feature off. The publish that immediately follows a level-up choice (the owner's own
// click path) is what gets the result to the other peer before the next battle is built.
//
// ★ AND OFF AGAIN (2026-09-23): the panel half of the same redundant machinery. It only means
// anything while kPerPeerLevelTarget re-points the screen at a cat of this peer's own, and that is
// off (see its note). With both off, the flow is the documented one: the shared draw builds ONE
// screen, its owner answers it, the other peer replicates the owner's option -- so there is
// nothing for this write to rewrite and nothing for the panel to disagree with. The picture a
// non-owner sees while waiting for the owner's click therefore shows the drawn cat again, which is
// the state DESIGN-independent.md sec. 9.1 already describes, and the manual rule for the
// "replace a passive" sub-screen still applies.
constexpr bool kOpenerSubst = true;

// Native LevelUpScreen constructor stores RDX at screen+A0 (37A350) and
// increments that SAME CatData's level at +C30 (37A357), before building the
// panel/options. Redirect the argument, never deserialize onto the drawn cat
// and never retarget the screen after that increment.
static void* __fastcall h_lvl_opener(void* screen, void* cat, void* host) {
    if (g.on) unlocks_level_screen_opens();       // before the constructor builds the option pool
    if (g.on && kPerPeerLevelTarget && cat) {
        uint64_t drawn = 0;
        uint8_t owner = kNoPeer;
        if (lvl_looks_like_cat(cat, drawn, owner) && owner != net_peer_pos()) {
            // This is a constructor, not a frame update: each call is a new
            // award, including two awards for the same drawn cat in one node.
            s_pick_cat = nullptr;
            my_level_cat_begin(drawn);
            void* mine = my_level_cat();
            uint64_t own_id = 0;
            uint8_t own_pos = kNoPeer;
            if (mine && lvl_looks_like_cat(mine, own_id, own_pos) && own_pos == net_peer_pos()) {
                uint32_t before = 0, after = 0;
                mem_read((const uint8_t*)mine + 0xC30, &before, sizeof(before));
                void* result = o_lvl_opener(screen, mine, host);
                mem_read((const uint8_t*)mine + 0xC30, &after, sizeof(after));
                log_line("CHOICE", "native level-up target %016llX -> own %016llX, level %u -> %u",
                         (unsigned long long)drawn, (unsigned long long)own_id, before, after);
                return result;
            }
            log_line_lvl(LogLevel::Error, "CHOICE", "!! native level-up own target unavailable; keeping original subject");
        }
    }
    return o_lvl_opener(screen, cat, host);
}

// --- THE PANEL, BUILT A SECOND TIME FOR THIS PEER'S OWN CAT (2026-09-23) --------------
//
// This is the display fix, and it is small because the instrument found the right layer.
//
// HOW IT WAS FOUND, so nobody repeats the hunt. Six attempts failed from the OUTSIDE (scanning
// for whoever writes the screen's fields, an execute breakpoint on a believed builder, two
// generic setters, the run object's head, the queue lookups). What worked was watching from the
// INSIDE after learning that the screen object is REUSED (measured: the client's second level-up
// ran at its first one's address): a write watch on the screen's own +0x360/+0x368 -- the option
// array's begin and end -- could therefore be armed for the NEXT level-up, and it fired:
//
//   hit [lvl screen +0x360 (options)] rip=...70CD  callers(rva): 0x384E54 0x37AEE2 0x3C5A31 ...
//
// The RIP is inside 0x386EE0, which the bytes identify as "install these options": it stores
// {begin, end, cap} into its first argument and computes the end with `imul rcx, r12, 0xF0`
// (240 = kOptStride). Its caller is 0x384450 -- the PANEL BUILDER -- and the opener calls it as
//
//   37AED2  xor edx, edx
//   37AED4  mov rcx, r15
//   37AED7  call 0x384450
//
// i.e. (screen, 0): NO cat argument. The builder takes the cat from the screen it is handed --
// from +0xA0, the very field per_peer_level_target already re-points. So the panel can be made
// to show this peer's own cat by running the game's OWN builder once more, after the re-point.
// No argument is fabricated, no data is copied by hand, and the roll stays the game's.
//
// ON, and reversible in one line. The one thing it assumes -- and the next test checks it -- is
// that the option roll is seeded from the cat rather than from the shared RNG: the notes say the
// HEAP roller is `splitmix64(CatData+0x00)` advanced by CatData+0xC30 jumps, which is per-cat and
// therefore consumes nothing the two peers share. If a battle after this ever mismatches on rng,
// that assumption is the first thing to re-examine.
constexpr bool kRebuildPanelForOwnCat = true;

using fn_build_panel = void (__fastcall*)(void*, char);
fn_build_panel o_build_panel = nullptr;
static void*   g_build_screen = nullptr;   // the screen the game last built a panel for

static void __fastcall h_build_panel(void* screen, char flag) {
    if (screen) g_build_screen = screen;
    o_build_panel(screen, flag);
}

void install_level_screen_hooks() {
    static bool tried = false;
    if (tried) return;
    tried = true;
    if (!kLevelScreenProbe) return;
    if (!g.base) {
        log_line_lvl(LogLevel::Warn, "CHOICE",
                     "!! BUILD SITE: no module base -- the three build-site hooks are NOT"
                     " installed (their addresses are module-relative)");
        return;
    }

    // The prologues read out of the pinned build. Refusing on a mismatch is the whole point:
    // these are fixed offsets in one build, not signatures.
    // ONE SITE, AFTER THE CLEANUP OF 2026-09-23: the opener, and nothing else.
    //
    // The four probes that used to be installed here were the failed approaches of the same
    // night, and they are gone because none of them could reach the panel (the details, with
    // every address, are in the design notes' "The two mechanisms that carry the per-peer design"):
    //
    //   0x55120  the subject writer (+0xA0) -- a GENERIC setter, called on the map with a
    //            non-cat argument; it belongs to some other class with the same layout
    //   0x54D40  the option array's end (+0x368) -- same shape, log only, no cat ever seen
    //   0x557F0  the option array (+0x360/+0x368) -- idem
    //   0x52460  "the queue entry -> cat resolver" -- in fact a std::string COPY CONSTRUCTOR.
    //            A substitution armed here once "resolved to a cat" by reading a save id out of
    //            a string buffer, i.e. garbage. Hooking a copy constructor is also a per-call
    //            cost for nothing, which is why this one is deleted outright rather than
    //            switched off
    //   0x384450 the panel builder, captured so it could be run a second time -- superseded:
    //            the panel is a snapshot of the pending level-up's CatData copy, and that copy
    //            is now overwritten at the source (see kOpenerSubst)
    //
    // The opener is kept because it is the ONLY place that runs before the panel is built, and
    // it is where the fix lives.
    static const uint8_t want_opener[8]  = { 0x48, 0x8B, 0xC4, 0x4C, 0x89, 0x40, 0x18, 0x48 };

    struct Site { uintptr_t rva; const uint8_t* want; void* detour; void** orig;
                 const char* what; };
    Site sites[1] = {
        { kRvaLvlOpener,     want_opener,  (void*)&h_lvl_opener,      (void**)&o_lvl_opener,
          "the OPENER (0x37A290): the only place that runs BEFORE the panel is built" },
    };

    for (uint32_t i = 0; i < 1; ++i) {
        Site& s = sites[i];
        uint8_t got[8] = {};
        if (!mem_read((const uint8_t*)(g.base + s.rva), got, sizeof(got))) {
            log_line_lvl(LogLevel::Warn, "CHOICE",
                         "!! BUILD SITE 0x%llX (%s): unreadable -- NOT installed",
                         (unsigned long long)s.rva, s.what);
            continue;
        }
        bool same = true;
        for (uint32_t k = 0; k < 8; ++k) if (got[k] != s.want[k]) same = false;
        if (!same) {
            log_line_lvl(LogLevel::Warn, "CHOICE",
                         "!! BUILD SITE 0x%llX (%s): the prologue does not match this build"
                         " (got %02X %02X %02X %02X ...) -- NOT installed",
                         (unsigned long long)s.rva, s.what,
                         got[0], got[1], got[2], got[3]);
            continue;
        }
        void* target = (void*)(g.base + s.rva);
        if (MH_CreateHook(target, s.detour, s.orig) != MH_OK || MH_EnableHook(target) != MH_OK) {
            log_line_lvl(LogLevel::Warn, "CHOICE",
                         "!! BUILD SITE 0x%llX (%s): MinHook refused -- NOT installed",
                         (unsigned long long)s.rva, s.what);
            *s.orig = nullptr;
            continue;
        }
        log_line_lvl(LogLevel::Warn, "CHOICE",
                     "!! BUILD SITE hook INSTALLED at 0x%llX -- %s",
                     (unsigned long long)s.rva, s.what);
    }
}

void choice_on_level_update(void* level_screen) {
    if (!g.on || !level_screen) return;
    Guard guard;
    g.level_screen = level_screen;
    // 385A90 is a UI resource helper, not a CatData level-up entry; do not hook it.
    install_level_screen_hooks();

    // THE UPSTREAM QUESTION, WATCHED FROM HERE (2026-09-23): who WRITES the subject field.
    // The re-point below proves the field is what this module reads; the picture, the stat
    // numbers and the option roll follow the game's own pending-level-up state instead, so
    // the writer of this field is where a real per-peer draw would have to happen. The
    // probe's own slots are re-aimed by its tick only when the battle chain MOVES, and after
    // a battle it is settled -- so one borrow survives. Armed once per screen.
    // THE PER-SCREEN WATCH ON +0xA0 IS GONE ON PURPOSE (2026-09-23). It could only ever be
    // armed AFTER the screen existed, so it caught this module's own re-point write and
    // nothing else -- measured, in the log, more than once. Slot 3 now holds the execute
    // watchpoint on the construction site instead (see choice_reset_run), and a breakpoint on
    // an address only written before that address exists is not worth a slot.

    // Subject was chosen before the native constructor increments its level.
    // A late pointer swap would separate the level gain from the option effect.
    if (tune::kLevelUpProbe && g.probed_update_screen != level_screen) {
        g.probed_update_screen = level_screen;
        probe_level_screen(level_screen, "update");
    }
    LevelSubject subject;
    uint8_t owner = kNoPeer;
    if (read_level_subject(level_screen, subject))
        lockstep_cat_owner(subject.id, subject.seed, owner);
    if (g.owner_logged_screen != level_screen || g.owner_logged_peer != owner) {
        g.owner_logged_screen = level_screen;
        g.owner_logged_peer = owner;
        log_line("CHOICE", "level-up screen: cat=%016llX seed=%016llX owner=%u self=%u step=%u -- %s",
                 (unsigned long long)subject.id, (unsigned long long)subject.seed,
                 (unsigned)owner, (unsigned)net_self(), g.level_step,
                 owner == kNoPeer ? "waiting for verified owner" : owner == net_self() ? "choose locally" : "waiting for owner");
    }

    // THE NOTE THE PANEL DRAWS (2026-09-23), refreshed every frame this screen is up so it
    // cannot outlive it. `owner` here is the subject's owner, which after the re-point is this
    // peer -- so the note carries the DRAWN cat's owner instead, read from the same table the
    // rest of the module uses.
    {
        g_note_ms      = GetTickCount64();
        g_note_subject = subject.id;
        g_note_mine    = (owner != kNoPeer) && (owner == net_self());
        uint8_t dpos   = 0;
        if (g_note_drawn && g_note_drawn != subject.id && lockstep_owner_pos(g_note_drawn, dpos))
            g_note_owner = dpos;
        else
            g_note_owner = owner;
    }

    // AND HAND THE PROBE THIS SCREEN (2026-09-23), so its SUBJECT AND OPTION FIELDS are watched
    // for the write that fills them. That write is the panel's builder, and this is the one way
    // to learn where it is: the screen object is reused (measured -- the client's second
    // level-up ran at the first one's address), so the next level-up re-writes these fields on
    // an address this module already knows. Idempotent: the probe keeps the last screen and
    // logs only when it changes.
    listprobe_watch_screen(level_screen);

    // (The "build the panel a second time" block that used to be here is gone: it was the
    //  approach that PROVED the snapshot -- running the game's own builder again with the game's
    //  own arguments produced byte-identical content, because the panel is a copy of the pending
    //  level-up's state rather than of the screen. The split is now done at that state, in the
    //  opener's hook -- see kOpenerSubst.)

    // AND SAY IT IN THE LOG TOO, ONCE A SECOND WHILE THE SCREEN IS UP (2026-09-23). The panel
    // draws the same line, but a note that exists only on a window the player may have closed
    // is a note nobody reads -- which is what the first run of it demonstrated. The file
    // record is also what makes it checkable afterwards.
    {
        static uint64_t said_ms = 0;
        const uint64_t  now_ms  = GetTickCount64();
        if (now_ms - said_ms > 1000) {
            said_ms = now_ms;
            char note[256] = {};
            if (choice_level_note(note, sizeof(note)))
                log_line_lvl(LogLevel::Warn, "CHOICE", "%s", note);
        }
    }
    // The measurement that licenses (or forbids) a placeholder on the non-owner's screen:
    // see the note on probe_level_stats. Read-only, one 96-byte read per step.
    // --- AND THE NON-OWNER DOES NOT SIT AND WAIT (2026-09-23) --------------------------
    //
    // The user's rule: each player answers its own cats' level-ups, and the other player's
    // screens are answered for it, so neither player is held up by the other. See
    // kParallelLevelUp above for why a placeholder is safe here -- in one line: the seven
    // possible options all write the cat, and catsync carries the cat.
    //
    // THE GRACE PERIOD IS NOT A SAFETY NET, IT IS A BETTER ANSWER WHEN IT CAN BE TAKEN: if
    // the owner's own choice arrives within it, the normal path below applies that exact
    // option and this peer ends up with what the owner actually chose. The placeholder only
    // covers the case where the owner has not clicked yet, which is the case that made the
    // wait visible in the first place.
    if (kParallelLevelUp && owner != kNoPeer && owner != net_self() && !g.level_pending_count) {
        static void*    phr_screen = nullptr;
        static uint64_t phr_ms     = 0;
        const uint64_t  now        = GetTickCount64();
        if (phr_screen != level_screen) { phr_screen = level_screen; phr_ms = now; }

        uint8_t committed = 0;
        if (!mem_read((const uint8_t*)level_screen + kLvl_Committed, &committed, 1))
            committed = 1;                    // unreadable: press nothing

        if (!committed && now - phr_ms > kParallelGraceMs) {
            uint8_t* ob = nullptr;
            uint8_t* oe = nullptr;
            uint32_t pick = 0;
            bool     have = false;
            if (mem_read((const uint8_t*)level_screen + kLvl_OptBegin, &ob, sizeof(ob)) &&
                mem_read((const uint8_t*)level_screen + kLvl_OptEnd,   &oe, sizeof(oe)) &&
                ob && oe > ob) {
                const uintptr_t n = (uintptr_t)(oe - ob) / kOptStride;
                for (uintptr_t i = 0; i < n && i < kMaxOptions; ++i) {
                    uint32_t type = 0;
                    char     name[48] = {};
                    if (!read_level_option(ob + i * kOptStride, type, name)) continue;
                    if (!have) { pick = (uint32_t)i; have = true; }   // the fallback
                    // A plain stat gain, when the screen offers one: the least interesting
                    // option to have pressed by accident, and the one whose effect the
                    // owner's copy definitely carries.
                    if (type == 1) { pick = (uint32_t)i; have = true; break; }
                }
            }
            // THE PRESS MUST NOT BE SWALLOWED BY OUR OWN SUPPRESSION (2026-09-23, measured).
            //
            // choice_on_level_select returns false for a cat this peer does not own, and
            // outside a battle it cannot even tell whose it is -- its own comment records
            // that this branch "swallowed every click" until a bounded wait unstuck it. A
            // post-battle level-up IS outside a battle, so a placeholder pressed here was
            // suppressed: the screen closed, the cat never changed, and mgmp_catsync said so
            // exactly ("0 cat(s) changed and sent, 4 unchanged"). The two peers were then
            // left with different cats, and the next battle halted on a turn 0 state hash.
            //
            // g.injecting is this module's flag for "this click is OURS, let it through";
            // the injection paths set it for the same reason. Set it around this press.
            bool pressed = false;
            if (have) {
                g.injecting = true;
                pressed = inject_level(pick);
                g.injecting = false;
            }
            if (pressed) {
                // PUSH THE LEVEL-UP NOW, NOT AT THE NEXT MAP TICK (2026-09-23). The first
                // attempt's HALT was exactly this window: each peer levelled its own cat, the
                // push rode the one-second map tick, and the host built the next battle before
                // it arrived. A push that goes out the moment the press lands leaves the host
                // -- whose node entry is a human click away -- holding the reconciled cat.
                catsync_publish("a placeholder level-up just landed on this peer's own cat",
                                false);
            }
            if (pressed) {
                log_line_lvl(LogLevel::Warn, "CHOICE",
                             "!! PARALLEL level-up: this peer's own copy of cat=%016llX's"
                             " screen was answered with option %u after %llu ms of silence"
                             " from its owner. The owner's choice still wins on the cat --"
                             " mgmp_catsync pushes the cat before the next node is built.",
                             (unsigned long long)subject.id, pick,
                             (unsigned long long)(now - phr_ms));
            }
        }
    }

    probe_level_stats(level_screen, g.level_step);

    apply_level_pending(level_screen);
}

// --- receiving -------------------------------------------------------------

void choice_on_message(uint8_t from, const ChoiceMsg& m) {
    if (!g.on) return;
    Guard guard;
    if (from >= kMaxPeers || from == net_self() || m.kind > kChoiceLevelUp) {
        log_line("CHOICE", "!! invalid choice sender=%u kind=%u -- refused", (unsigned)from, (unsigned)m.kind);
        return;
    }
    if (m.kind == kChoiceLevelUp) {
        receive_level_choice(from, m);
        return;
    }
    if (!g.is_client || from != kHostPeer) {
        log_line("CHOICE", "!! refused event choice from peer %u -- only the host decides events", (unsigned)from);
        return;
    }
    constexpr uint8_t kind = kChoiceEvent;

    // A choice for a node mgmp_follow already gave up on. Refusing it here is
    // the other half of choice_on_node_skipped: that one catches the choice
    // that was already held when the node was discarded, this one catches the
    // choice that arrives afterwards. Holding it would block the next real
    // choice and eventually land on the wrong screen.
    if (was_skipped(m.node_seed)) {
        log_line("CHOICE", "!! a %s choice arrived for node %016llx, which this peer"
                           " passed over without entering -- refusing it. The host"
                           " resolved that node and this peer did not.",
                 kind_name(kind), (unsigned long long)m.node_seed);
        return;
    }

    if (!m.node_seed || !m.event_name[0] || m.count == 0 || m.count > 64 || m.index >= m.count ||
        !memchr(m.event_name, 0, sizeof(m.event_name)) || !memchr(m.name, 0, sizeof(m.name))) return;
    if (m.node_seed == g.here_seed && m.level_step < g.event_step) return;
    for (uint32_t i = 0; i < g.event_count; ++i) {
        const ChoiceMsg& old = g.event_queue[i];
        if (old.node_seed != m.node_seed || old.level_step != m.level_step) continue;
        if (old.index != m.index || old.count != m.count || strcmp(old.event_name, m.event_name) || strcmp(old.name, m.name)) {
            g.event_faulted = true;
            log_line_lvl(LogLevel::Error, "CHOICE", "!! conflicting event choice for node=%016llX step=%u", (unsigned long long)m.node_seed, m.level_step);
        }
        return;
    }
    if (g.event_count == State::kMaxEventChoices) {
        g.event_faulted = true;
        log_line_lvl(LogLevel::Error, "CHOICE", "!! event choice queue full; refusing further event commits");
        return;
    }
    g.event_queue[g.event_count++] = m;
    log_line("CHOICE", "queued host event '%s' step=%u option=%u/%u node=%016llX", m.event_name, m.level_step, m.index, m.count, (unsigned long long)m.node_seed);
    // Only an actual WorldEvent update can prove that the cached pointer and
    // option page are alive and ready. Receiving four choices is not four pages.

}

void choice_on_node_entered(uint64_t node_seed) {
    if (!g.on) return;
    Guard guard;
    if (g.here_seed != node_seed) {
        for (uint32_t i = 0; i < g.event_count;) {
            if (g.event_queue[i].node_seed == g.here_seed) remove_event(i); else ++i;
        }
        g.event_step = 0; g.event_local_have = false;
        reset_level_caches(false);
        remember_skipped(g.here_seed);
        for (uint32_t i = 0; i < g.level_pending_count;) {
            if (g.level_pending[i].message.node_seed != g.here_seed) { ++i; continue; }
            log_line("CHOICE", "!! leaving node=%016llX with upgrade step=%u pending -- discarded",
                     (unsigned long long)g.here_seed, g.level_pending[i].message.level_step);
            remove_level_pending(i);
        }
        g.level_step = 0;
    }
    g.here_seed = node_seed;
    g.owner_logged_screen = nullptr;
    g.owner_logged_peer = kNoPeer;

    // The cached screens belong to the node just left. Clearing them is not
    // tidiness: choice_on_message applies straight into whichever pointer is
    // held, so a stale one is both a wrong screen and a possible dead object.
    // The update ticks re-cache a live pointer on their very next frame.
    g.world_event  = nullptr;
    g.level_screen = nullptr;
    g.probed_update_screen = nullptr;
    g.probed_button_screen = nullptr;
    g.probed_button_indices = 0;

    // A choice already waiting for THIS node becomes due the moment we arrive.
    // Re-arm the "holding" line so the next one is reported afresh rather than
    // suppressed by a warning about the node we just left.
    for (uint8_t kind = 0; kind < 2; ++kind)
        if (g.pending[kind] && g.pending_seed[kind] == node_seed)
            g.pending_warned[kind] = false;
}

void choice_on_node_skipped(uint64_t node_seed) {
    if (!g.on || !node_seed) return;
    Guard guard;
    remember_skipped(node_seed);
    for (uint32_t i = 0; i < g.event_count;) {
        if (g.event_queue[i].node_seed == node_seed) remove_event(i); else ++i;
    }
    for (uint32_t i = 0; i < g.level_pending_count;) {
        if (g.level_pending[i].message.node_seed != node_seed) { ++i; continue; }
        log_line("CHOICE", "!! skipped node=%016llX: discarded upgrade step=%u",
                 (unsigned long long)node_seed, g.level_pending[i].message.level_step);
        remove_level_pending(i);
    }

    for (uint8_t kind = 0; kind < 2; ++kind) {
        if (!g.pending[kind] || g.pending_seed[kind] != node_seed) continue;
        log_line("CHOICE", "!! node %016llx was passed over without this peer"
                           " entering it, and a %s choice for it (option %u '%s') was"
                           " still held -- DROPPING it rather than letting it surface"
                           " on a later node. The host resolved that node and this"
                           " peer did not: the two runs have diverged.",
                 (unsigned long long)node_seed, kind_name(kind),
                 g.pending_index[kind], g.pending_name[kind]);
        g.pending[kind]        = false;
        g.pending_warned[kind] = false;
    }
}

} // namespace mgmp
