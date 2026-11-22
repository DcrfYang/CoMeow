// mgmp_follow.cpp -- see mgmp_follow.h.

#include "mgmp_catsync.h"
#include "mgmp_choice.h"
#include "mgmp_indepevt.h"
#include "mgmp_invsync.h"
#include "mgmp_nodehash.h"
#include "mgmp_checkpoint.h"
#include "mgmp_nodesnapshot.h"
#include "mgmp_runhist.h"
#include "mgmp_follow.h"
#include "mgmp_treasure.h"   // treasure_publish_luck
#include "mgmp_unlocks.h"
#include "mgmp_room.h"
#include "mgmp_page.h"       // page_of / page_age_ms -- is another player on the gear screen
#include "mgmp_roster.h"     // roster_tick -- the run edit point is this tick
#include "mgmp_lockstep.h"
#if defined(MGMP_WITH_SETUP)
#include "mgmp_setup.h"
#endif
#include "mgmp_net.h"
#include "mgmp_session.h"   // session_exp_treasure: the shared treasure chest
#include "mgmp_config.h"
#include "mgmp_tuning.h"
#include "mgmp_mem.h"
#include "mgmp_log.h"
#include "mgmp_addresses.h"  // kRva_MewDirectorPtr / kDir_* -- the run, for the probe's
                             // "where is the party" scan (the map screen held no such field)
#include "mgmp_resolve.h"    // addr_of_call(C_MapNodeBlocked): can the client's party marker walk to the host's party node
#include "mgmp_mapvote.h"    // mapvote_node_world: the map's direction for follow_unstick_marker
#include "mgmp_rng.h"        // rng_global_stream / rng_original_randfloat -- the
                             // per-peer node perturbation, see follow_perturb_after_enter

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace mgmp {
namespace {

// MapScreen's node vector, read off MapNode::Click and cross-checked against
// every other MapScreen function that walks it (update, generate_map,
// generate_auto_connections, generate_bonus_nodes, load_map, select_boss_level,
// TickNemesis all use the same +124/+128 pair). Standard CustomVector shape:
// { u32 capacity @ 0x78, u32 count @ 0x7C, MapNode** data @ 0x80 }.
constexpr uintptr_t kMap_NodeCount = 124;
constexpr uintptr_t kMap_NodeData  = 128;

// MapNode+0x118: the node's own 32-byte xoshiro256 seed, written by
// MapScreen::generate_map and copied into TLS+0x178 by EnterNode.
// MapNode+0x138: MapNodeType, from MapNode::str_to_type.
constexpr uintptr_t kNode_Seed = 0x118;
constexpr uintptr_t kNode_Type = 0x138;

// MapNode+0x168: one BYTE, 0 while un-consumed, 1 after. +169/+16A are
// independent flags (native reads at RVA 2262CB/2262D8); never treat these as
// a single consumed dword. Native arrival tests the byte at RVA 39B19D.
//
// Measured on BOTH peers at three nodes (a hard, a treasure and an event): the host's copy
// went 0 -> 1 in all three cases as it cleared them, and the client's copy -- following into
// the same three nodes, same seeds, same objects' worth of state -- stayed 0 in all three.
// One field, six readings, and it is the only difference between the two maps that matches
// "this node is behind us". See follow_mark_node_consumed.
constexpr uintptr_t kNode_Consumed = 0x168;

// THE TWO INSTANT NODES (0.1.120). EnterNode's furniture-box (14) and food-box (15) branches finish the node IN THE SAME CALL (RVA 392fc0, the node-finish routine): they roll their reward and
// then consume the node the PARTY MARKER STANDS ON (marker+0x50), refresh every node and clear MapScreen+0x1E8. Every other node (battle, event, shop, chest) opens a screen, and the finish
// runs when that screen closes -- long after the party has walked there. The host's click walks first and enters on arrival, so its marker is on the node; the client enters at once, from the
// host's message, while its marker still stands on the PREVIOUS node: the finish consumed and refreshed the wrong node, and from then on the client could not click the next one (the room
// B9KU / KD8E food box, node 16 then node 12: every food box since the map vote, none in the 0.1.4 logs where the client's own click had started the walk first).
constexpr uint32_t kNodeType_FurnitureBox = 14;
constexpr uint32_t kNodeType_FoodBox      = 15;
uint64_t s_instant_walk_ms = 6000;      // how long the entry waits for the party to arrive before it is entered anyway

// MapNode::str_to_type, in enum order. 1 and 18 are three-character names whose
// string constants IDA did not render; they are not battle types and nothing
// here depends on knowing them.
const char* node_type_name(uint32_t t) {
    static const char* kNames[] = {
        "none", "?1", "enter", "exit", "home", "battle", "hard", "miniboss",
        "boss", "event", "optional_event", "special_event", "shop", "treasure",
        "furniturebox", "foodbox", "bonus", "empty", "?18"
    };
    return t < (sizeof(kNames) / sizeof(kNames[0])) ? kNames[t] : "?";
}

struct State {
    bool     on             = false;
    bool     is_client      = false;

    void*    map            = nullptr;   // latest MapScreen*, from its update

    // NODES THE HOST HAS ENTERED AND THIS PEER HAS NOT -- A QUEUE, NOT A SLOT.
    //
    // It was one slot, and a second ENTERNODE overwrote it in silence. That is
    // not a race to tolerate: it is the client SKIPPING A WHOLE NODE of the
    // run, and it happened live on 2026-08-25. The host entered node 6 (an
    // event), resolved it and walked on to node 17 faster than this peer's map
    // tick came round; node 17 replaced node 6 in the slot; the client never
    // entered node 6, never built its option list, and never applied the event.
    // Its log says `<- host entered node 6` with no `following host into node
    // 6` after it, and there was no other line anywhere to say a node had been
    // dropped.
    //
    // The run was already wrong at that point. What made it unrecoverable was
    // that node 6's CHOICE stayed held and landed on node 3 two nodes later --
    // see ChoiceMsg::node_seed.
    //
    // A queue is the whole fix, because the peers do not have to be on the same
    // node at the same moment -- only to walk the same nodes in the same order.
    // The battle layer already assumes exactly that: the join barrier exists so
    // a peer can arrive at a fight late. Trailing the host by a node is the same
    // situation one screen earlier.
    static constexpr uint32_t kMaxPendingNodes = 8;
    struct PendingNode {
        uint32_t index = 0;
        uint32_t type  = 0;
        uint32_t count = 0;
        uint64_t seed  = 0;
        // All four words of the node's 32-byte xoshiro256 state as the host read
        // it. `seed` is word 0 and is what everything else compares; the other
        // three exist so a peer on a different save can adopt the whole state --
        // see tune::kAdoptHostSeed. Zero when the host could not read them, which
        // is the honest "only word 0 travelled" case.
        uint64_t words[4] = {};
        uint64_t at    = 0;         // tick the host's choice arrived
        bool     held  = false;     // ...and whether we have said we are holding
        bool     walking = false;   // an instant node (food box, furniture box): the party was sent to it and the entry waits for the arrival (0.1.120)
        bool     walk_marked = false; // ...and the consumed byte was set meanwhile (so the arrival does not enter the node itself) and must be put back
        uint64_t walk_at = 0;       // ...tick the walk began
        NodeSnapshot snapshot;
        EnterNodeMsg message;
    };
    NodeSnapshot incoming;
    bool faulted = false;
    bool transition_logged = false;
    PendingNode pending_q[kMaxPendingNodes];
    uint32_t    pending_head  = 0;
    uint32_t    pending_count = 0;

    // The node this peer is standing in, remembered so a peer that connects
    // afterwards can be told where the run is. ENTERNODE used to be published
    // once, at the moment of entry, and nothing kept it -- so a reconnecting
    // peer loaded the save, landed on the map and waited forever for a message
    // that had already been sent. Measured 2026-08-26.
    bool     have_here      = false;
    uint32_t here_index     = 0;

    // Last tick MapScreen::update ran. See follow_on_map: this is what tells
    // the save flush whether the run is between nodes or inside one.
    uint64_t map_tick       = 0;
    uint64_t map_ready_since = 0;        // 0.1.231: when the map screen last became ready (0 while it is not) -- a special_event entry waits for it to settle
    uint32_t settle_said    = 0xFFFFFFFF;

    // A jump asked for by the debug panel, applied at the tail of the next
    // MapScreen::update -- the same site the follow path enters nodes from.
    bool     jump_pending   = false;
    uint32_t jump_index     = 0;
    // A host can click the next map node during the short shop/event exit
    // transition.  The game calls EnterNode while MapScreen is still marked
    // initializing/entering, so the old path swallowed that click and there
    // was no later input to retry it.  Keep the node index (not its pointer)
    // and replay it from the next ready map tick.
    bool     host_retry_pending = false;
    void*    host_retry_map = nullptr;
    uint32_t host_retry_index = 0;
    uint32_t here_count     = 0;
    uint32_t here_type      = 0;
    uint64_t here_seed      = 0;

    uint32_t entered        = 0;         // nodes followed, for the shutdown line
    uint32_t published      = 0;
    uint32_t suppressed     = 0;
    bool     warned_no_map  = false;

    CRITICAL_SECTION cs;
    bool cs_ready = false;
};

State g;

struct Guard {
    Guard()  { if (g.cs_ready) EnterCriticalSection(&g.cs); }
    ~Guard() { if (g.cs_ready) LeaveCriticalSection(&g.cs); }
};

// --- the pending-node queue -------------------------------------------------
//
// All three are called with the Guard held. `front` is only valid while
// `pending_any()` is true.

bool pending_any() { return g.pending_count != 0; }

State::PendingNode& pending_front() {
    return g.pending_q[g.pending_head];
}

void pending_pop() {
    if (!g.pending_count) return;
    pending_front().snapshot.clear();
    g.pending_head = (g.pending_head + 1) % State::kMaxPendingNodes;
    --g.pending_count;
}

void pending_clear() {
    for (auto& p : g.pending_q) p.snapshot.clear();
    g.incoming.clear();
    g.pending_head = g.pending_count = 0;
    g.host_retry_pending = false;
    g.host_retry_map = nullptr;
    g.host_retry_index = 0;
}

void queue_host_retry(void* map, bool known, uint32_t index, const char* why) {
    if (g.is_client || !known || !map) return;
    if (!g.host_retry_pending || g.host_retry_map != map || g.host_retry_index != index)
        log_line("FOLLOW", "holding host node %u for a later ready-map retry (%s)",
                 index, why ? why : "transition");
    g.host_retry_pending = true;
    g.host_retry_map = map;
    g.host_retry_index = index;
}

bool map_ready(void* map) {
    uint8_t initializing = 0, entering = 0;
    if (!map || !mem_read((uint8_t*)map + 0x1E4, &initializing, 1) ||
        !mem_read((uint8_t*)map + 0x1E8, &entering, 1) || initializing > 1 || entering > 1) {
        if (!g.faulted) log_line_lvl(LogLevel::Error, "FOLLOW", "!! map readiness unreadable -- refusing node entry");
        g.faulted = true;
        return false;
    }
    // EnterNode sets +1E8 before its deferred callback; MapScreen::update still runs during that transition.
    return !initializing && !entering;
}

bool read_nodes(void* map, uint32_t& count, void*& data) {
    count = 0; data = nullptr;
    if (!map) return false;
    if (!mem_read((const uint8_t*)map + kMap_NodeCount, &count, sizeof(count))) return false;
    if (!mem_read((const uint8_t*)map + kMap_NodeData,  &data,  sizeof(data)))  return false;
    // A map with no nodes, or an implausible count, means we are reading the
    // wrong object -- say nothing rather than index into it.
    return data != nullptr && count > 0 && count < 4096;
}

void* node_at(void* map, uint32_t index) {
    uint32_t count = 0; void* data = nullptr;
    if (!read_nodes(map, count, data) || index >= count) return nullptr;
    void* node = nullptr;
    if (!mem_read((const uint8_t*)data + index * sizeof(void*), &node, sizeof(node))) return nullptr;
    return node;
}

bool index_of_node(void* map, void* node, uint32_t& out) {
    uint32_t count = 0; void* data = nullptr;
    if (!read_nodes(map, count, data)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        void* p = nullptr;
        if (!mem_read((const uint8_t*)data + i * sizeof(void*), &p, sizeof(p))) return false;
        if (p == node) { out = i; return true; }
    }
    return false;
}

uint32_t node_type(void* node) {
    uint32_t t = 0;
    if (node) mem_read((const uint8_t*)node + kNode_Type, &t, sizeof(t));
    return t;
}

uint64_t node_seed0(void* node) {
    uint64_t s = 0;
    if (node) mem_read((const uint8_t*)node + kNode_Seed, &s, sizeof(s));
    return s;
}

} // namespace

// ---------------------------------------------------------------------------

void follow_seed_sync_reset();   // the map seed sync below

void follow_init() {
    if (!g.cs_ready) { InitializeCriticalSection(&g.cs); g.cs_ready = true; }
    g.on        = config().net_follow;
    g.is_client = (net_role() == NetRole::Client);

    // THE RUN STATE IS NOT RESET HERE, and the reason is a whole class of bug.
    //
    // follow_init runs on every handshake -- including the one after a socket
    // reconnect, because that path is session_shutdown -> begin -> WELCOME ->
    // go_ready, and the run was never touched. follow_reset_run clears
    // `have_here`, which is a fact about the RUN, so clearing it there made a
    // reconnecting peer forget the node it was standing in. The host's catch-up
    // then looked like a NEW node to it -- `have_here` is exactly the check that
    // recognises "this is the node I am already in" -- and the peer re-entered a
    // node it had finished: a second EnterNode, a second battle, two runs that
    // are no longer the same run, from a reconnect that should have been
    // invisible.
    //
    // The state that DOES belong to a new run is cleared by the things that
    // actually start one: choice_reset_run at the save-slot click and at the
    // savefile redirect (which is where follow_reset_run is called from), and
    // follow_shutdown for the pending queue. Nothing here needs to guess.
    if (!g.on) { log_line("FOLLOW", "map following disabled by net_follow = 0"); return; }
    log_line_lvl(LogLevel::Trace, "FOLLOW", "armed -- %s",
             g.is_client ? "following the host's map choices; local map input is suppressed"
                         : "publishing this peer's map choices");
    if (config().net_follow_delay_ms && g.is_client)
        log_line("FOLLOW", "!! TEST SETTING ACTIVE: net_follow_delay_ms = %u. This peer"
                           " will lag every node by that long ON PURPOSE, to exercise"
                           " the join barrier. Set it to 0 for a real session.",
                 config().net_follow_delay_ms);
    else if (config().net_follow_delay_ms)
        log_line("FOLLOW", "net_follow_delay_ms = %u is set but this peer is the host,"
                           " which never follows -- it will have no effect",
                 config().net_follow_delay_ms);
}

void follow_reset_run() {
    Guard guard;
    follow_seed_sync_reset();
    pending_clear();
    roster_party_swap_reset();
    g.map = nullptr;
    g.have_here = false;
    g.map_tick = 0;
    g.jump_pending = false;
    g.faulted = false;
    g.transition_logged = false;
}

bool follow_hold_state(NetMsg& m) {
    if (!g.on || !g.is_client) return false;
    Guard guard;
    if (g.faulted) return true;
    if (m.from != kHostPeer) {
#if defined(MGMP_WITH_SETUP)
        // Host relays owner-authored cat edits with the original transport ID.
        // These are independent of its node snapshot, so hand them to catsync.
        if(m.type==MSG_CATDATA && setup_has_shared_roster() && setup_runtime_ready()) {
            uint8_t holder=kNoPeer;const int owner=session_cat_owner_any(m.catdata.id);     // 0.1.146: a client's gained cat too
            if(owner>0 && owner<net_seat_span() && net_seat_peer((uint8_t)owner,holder) && holder==m.from)      // the connection that holds the owner's SEAT
                return false;
        }
#endif
        return true; // inventory/history/map transitions remain host-only
    }

    // ★ A CLIENT MID-RUN DOES NOT ABSORB THESE ANY MORE (2026-09-23, and it is the fix for the
    // one remaining divergence in a battle that otherwise agreed).
    //
    // This function's original job is the JOIN batch: a fresh client's run arrives as a burst of
    // CATDATA/INVENTORY/RUNHIST which is only complete when the ENTERNODE behind it lands, so the
    // burst is taken into `incoming` and applied as one transaction. What it also did -- because
    // `take()` cannot tell a joiner's burst from an ordinary mid-run push -- is swallow EVERY cat
    // the host ever published, so this peer's cats could only ever be applied inside a node
    // message. Three measurements follow from it and each of them is in the logs:
    //
    //   - the client's "CATDATA arrived" receipt (catsync_on_message's, which sits BEFORE its hold
    //     rule) appears ZERO times in a run, while the host reports "4 cat(s) changed and sent";
    //   - every cat apply on the client reads "(node snapshot)" and there are no others;
    //   - that apply therefore happens in the same instant the battle is built, and a cat whose
    //     bytes actually CHANGED there (a level-up: 1013 -> 1051) leaves the game holding a SECOND
    //     ability object for it -- two APPLY lines for the same actor and ability class, two
    //     different Ability pointers, its passive and its scene behaviour played twice.
    //
    // A client that has already entered a node is mid-run: it has its own copy of every cat, so
    // nothing about it needs a batch. Returning false hands the message to catsync NOW, and
    // catsync's own rules are the safe ones -- it holds while a fight is up and drains on the map
    // tick, i.e. off the battle screen and well before the next node is built. A fresh joiner
    // (`entered == 0`) still takes the batch path below, creation included, unchanged.
    if (m.type == MSG_CATDATA) {
        // BOTH, AND FOR DIFFERENT REASONS (2026-09-23).
        //
        // The batch copy stays: the ENTERNODE does not carry inventory/history, so the run state
        // is only complete because these messages fold into `incoming`, and without that fold the
        // node's completeness check fails and the client stops following -- measured once, the
        // first version of this change: "!! node 15 arrived without a complete snapshot --
        // following stopped", after which this peer sat on the map while the host played the
        // treasure and event nodes.
        //
        // The immediate delivery is the point of the change: cats are the one half that must be
        // applied EARLY. Folding them in means they are only written when the node message lands
        // -- the same instant the battle is built -- and a cat whose bytes actually changed there
        // (a level-up) leaves the game holding a SECOND ability object for it, so its passive and
        // its scene behaviour run twice. Handing the same message to catsync as well puts the
        // write on the map instead, where catsync holds it through a fight and drains it on the
        // tick; by the time the node lands its bytes are already what this peer holds, so the
        // batch's copy is a no-op. A fresh joiner (`entered == 0`) keeps the batch-only path,
        // because creation is authorized only through the node snapshot's familiar half.
        // ★ AND THE ORDER MATTERS, WHICH IS WHAT THE LAST RUN MEASURED (2026-09-23).
        //
        // The previous version took the message into the batch FIRST and then handed the same
        // struct on to catsync. But NodeSnapshot::take() MOVES the payload -- `cats[i] = m; m =
        // {};` -- so by the time the dispatcher called catsync_on_message it was passing a ZEROED
        // message. Every symptom in that run follows from those two lines and nothing else:
        //
        //   client: 34 x "cat push id=0000000000000000 size=0"
        //   client: ZERO "CATDATA arrived" receipts (catsync drops an empty frame)
        //   client: every cat apply reading "(node snapshot)" -- because the batch, which did
        //           receive the real payload, is only applied when the node lands
        //
        // So a client that is mid-run hands the message on UNTOUCHED and does not put it in the
        // batch at all: it has its own copy of every cat already, catsync's hold/drain rules are
        // the safe ones, and the node snapshot does not need this entry -- its completeness check
        // is about inventory and history, which are still taken below. A FRESH JOINER
        // (`entered == 0`) keeps the batch path exactly as it was, creation included, because a
        // new cat may only be created through the node snapshot's familiar half.
        // A COPY FIRST, THEN THE MOVE (2026-09-23, and the copy is the whole point of this
        // version). take() MOVES the payload into the batch and zeroes the message -- which is
        // right for the batch and fatal for anything that wants to look at the message
        // afterwards. Both are needed:
        //
        //   the batch copy, because the node snapshot's completeness check covers the CAT half as
        //       well as inventory and history. Measured: without it, "!! node 15 arrived without a
        //       complete snapshot -- following stopped", and the client sat on the map while the
        //       host played on;
        //   the immediate delivery, because a cat that changed has to be written while the MAP is
        //       up. Measured: with the batch alone, the only cat applies on the client read "(node
        //       snapshot)" -- the same instant the battle is built -- and a changed cat written
        //       there leaves the game with a SECOND ability object for it, so its passive and its
        //       scene behaviour play twice.
        //
        // The copy shares the payload pointer, which is safe in both directions: catsync borrows
        // it (its byte-stream read is explicitly non-owning) and the hold path copies the bytes
        // into its own stash; ownership stays with the batch, which frees it as before.
        CatDataMsg pass = m.catdata;
        if (!g.incoming.take(m.catdata)) {
            g.faulted = true;
            log_line_lvl(LogLevel::Error, "FOLLOW", "!! node snapshot cat limit exceeded -- following stopped");
            return true;
        }
        // have_here is per-run; entered is only a lifetime diagnostic counter.
        // A new save must regain the fresh-join creation gate after reset.
        if (g.have_here) {
            log_line_lvl(LogLevel::Warn, "FOLLOW",
                         "!! cat push id=%016llX size=%u hash=%08X -- in the node batch AND handed"
                         " to catsync now, so it is written while the map is up",
                         (unsigned long long)pass.id, pass.size, pass.hash);
            catsync_on_message(pass);
        }
        return true;
    }
    if (m.type == MSG_INVENTORY) {
        g.incoming.take(m.inventory);
    } else if (m.type == MSG_RUNHIST) {
        g.incoming.take(m.runhist);
    } else {
        return false;
    }
    return true;
}

void follow_after_enter_node() {
    Guard guard;
    if (!g.on || !g.have_here || g.faulted) return;
    g.map_tick = 0;
    nodehash_on_node(g.here_seed, g.here_index);
    roster_party_swap_after_node(g.here_type);
#if defined(MGMP_WITH_CHECKPOINT)
    checkpoint_on_node(g.here_seed, g.here_index, g.here_type);
#endif
}

void follow_shutdown() {
    follow_seed_sync_reset();
    pending_clear();
    if (!g.on) return;
    log_line_lvl(LogLevel::Trace, "FOLLOW",
             "done: %u published, %u followed, %u local click(s) suppressed",
             g.published, g.entered, g.suppressed);
    g.on = false;
    if (g.cs_ready) { DeleteCriticalSection(&g.cs); g.cs_ready = false; }
}

bool follow_suppresses_local_input() { return g.on && g.is_client; }

// Where this peer is now. Recorded on BOTH peers -- the host so it can tell a
// joiner, the client so its own log says where it thinks it is.
void remember_node(uint32_t index, uint32_t count, uint32_t type, uint64_t seed) {
    g.have_here  = true;
    g.here_index = index;
    g.here_count = count;
    g.here_type  = type;
    g.here_seed  = seed;

    // Tell the choice layer where the run now is. It holds a choice that has
    // not found its screen yet, and a choice belongs to exactly one node -- see
    // ChoiceMsg::node_seed for the run this cost.
    //
    // The seed is PUSHED rather than pulled. mgmp_choice could ask
    // follow_here_seed() when it applies, but that would have it take this
    // module's lock while holding its own, and this call already runs the other
    // way round -- the classic inversion. Handing the value over means neither
    // module ever waits on the other.
    // A save reload resets choice even when this module remembers the same node.
    choice_on_node_entered(seed);

}

// A PLAYER WHO IS CHANGING GEAR HOLDS THE NODE (2026-10-05). The client opened the backpack / gear panel on the map; the host clicked the next node while it was still open; the battle was built from the
// host's copies of the client's cats, taken BEFORE the gear change (a map update, which is what publishes a peer's cats, does not run under that panel). Result: the pre-battle audit found three of the
// client's cats different, and the first action of a cat with the new weapon halted the battle ("cat 30 slot 4:4 holds 'cm_Heal', peer sent 'wp_SleepDart'"). The node waits while any OTHER player's page is
// the gear or collar screen and for a moment after it leaves it (its map ticks again, publishes, and the host applies before the node's own publish). Capped, so a player who walks away from the gear
// screen cannot hold the run for ever.
static bool others_changing_gear() {
    static ULONGLONG s_last = 0, s_first = 0;
    const ULONGLONG t = GetTickCount64();
    bool any = false;
    for (uint8_t p = 0; p < kMaxPeers; ++p) {
        if (p == net_self()) continue;
        const PageState s = page_of(p);
        if ((s == PageState::Equipment || s == PageState::Collar) && page_age_ms(p) < 4000) any = true;
    }
    if (any) { s_last = t; if (!s_first) s_first = t; }
    const bool settling = !any && s_last && t - s_last < 1500;
    if (!any && !settling) { s_first = 0; return false; }
    if (s_first && t - s_first > 90000) return false;          // 90 s: let the run go on
    return true;
}

bool follow_on_enter_node(void* map_screen, void* node, bool* sent) {
    if (sent) *sent = false;
    if (!g.on || !map_screen || !node) return true;

    Guard guard;
    g.map = map_screen;                    // the click proves the pointer is live

    uint32_t index = 0;
    bool     known = index_of_node(map_screen, node, index);
    uint32_t type  = node_type(node);

    // THE HOST'S OWN ENTRIES GO THROUGH HERE, which is why the probe is called at this
    // point rather than only on the follow path (2026-09-22). The host's click does not
    // pass MapScreen::update's follow branch, so it never reached the probe; and the host is
    // precisely the peer whose map advance is the reference. Read-only.
    follow_probe_node(node);

#if defined(MGMP_WITH_CHECKPOINT)
    // Keep the journal barrier active across a dropped socket. Only the explicit
    // play-alone route shuts follow down and invalidates the durable run.  A
    // transiently blocked HOST click is retained so a shop/event transition
    // cannot lose the next node forever.
    if (!checkpoint_can_enter()) {
        queue_host_retry(map_screen, known, index, "checkpoint barrier");
        return false;
    }
#endif
    if (!net_active()) return true;
    // 0.1.146: the host builds the node from the cats it holds -- one that arrived since the last map tick (another player's gained cat) is taken first
    if (!g.is_client) catsync_apply_pending("the host is about to enter a node");

    // --- the client: swallow it ------------------------------------------
    //
    // The host owns the run, so a click here is a second opinion nobody asked
    // for -- and acting on it would put the two peers on different nodes, which
    // no amount of battle-layer lockstep can recover from. Swallowing rather
    // than disabling the UI keeps the change to one function.
    if (g.is_client) {
        ++g.suppressed;
        log_line("FOLLOW", "suppressed local map input: node %u (%s) -- the host"
                           " drives the run",
                 known ? index : 0xFFFFFFFFu, node_type_name(type));
        return false;
    }

    // In local-save setup mode the client must finish selecting its own cats
    // before the host commits the first shared map node. Holding this exact
    // EnterNode choke point leaves the chapter page/map transition intact and
    // retries naturally on the next click.
#if defined(MGMP_WITH_SETUP)
    if (!setup_host_can_enter()) {
        queue_host_retry(map_screen, known, index, "setup barrier");
        log_line("FOLLOW", "holding host node %u (%s) until client setup export is accepted",
                 known ? index : 0xFFFFFFFFu, node_type_name(type));
        return false;
    }
#endif

#if defined(MGMP_WITH_CHECKPOINT)
    if (!checkpoint_can_enter()) {
        queue_host_retry(map_screen, known, index, "checkpoint barrier");
        return false;
    }
#endif
    if (others_changing_gear()) {
        queue_host_retry(map_screen, known, index, "a player is changing gear");
        static ULONGLONG s_said = 0;
        const ULONGLONG t = GetTickCount64();
        if (t - s_said > 3000) { s_said = t; log_line("FOLLOW", "holding host node %u (%s): another player is on the gear screen (or has just left it) -- its cats must reach this peer before the battle is built", known ? index : 0xFFFFFFFFu, node_type_name(type)); }
        return false;
    }
    // --- the host: publish it --------------------------------------------
    if (!known) {
        // Nothing to send that the client could resolve. Do not block the host
        // over it: a run that continues single-player is better than one that
        // stops, and the client will notice it is on a different node the
        // moment a battle starts and the hashes disagree.
        log_line("FOLLOW", "!! entered a node that is not in MapScreen's vector"
                           " -- cannot publish it; the client will not follow");
        return true;
    }

    uint32_t count = 0; void* data = nullptr;
    read_nodes(map_screen, count, data);

    if (g.faulted || !map_ready(map_screen)) {
        queue_host_retry(map_screen, known, index, "map transition");
        log_line_lvl(LogLevel::Warn, "FOLLOW", "node %u held -- map transition is not complete", index);
        return false;
    }
    const uint64_t seed = node_seed0(node);

    // THE CLIENT'S CATS FIRST, THEN OURS. This peer holds anything the client
    // published since the last node -- a gear change on one of ITS cats -- because
    // lockstep_in_battle() stays true on the map between battles, so the drain is
    // here. Applying before publishing is what puts the client's edits into the
    // state the node below is built from, and it is also what keeps the publish
    // honest: a just-applied cat now matches the baseline, so it is not sent back.
    catsync_apply_pending("about to enter the node the host clicked");

    // AND SAY WHAT THE HOST'S TWO LISTS ACTUALLY HOLD, BY ID (2026-09-23). Measured twice: the
    // client's node message arrived carrying four cats and four familiar ids, and ALL EIGHT were
    // the client's own -- the host's four were in neither list. So the publish cannot carry them,
    // and no amount of hashing explains why. This is the line that says which list holds whose
    // cats on the host itself, and it is taken at this moment because this is the moment the
    // publish is supposed to carry everything the client needs to build the same battle.
    // The post-battle promotion can have moved a client cat into the host party since the
    // last map tick; the publish below must carry the owner-split lists.
    // 0.1.212: a client's cat the game took out of the familiar list in the last FIGHT goes back first (CHINAMI 11-08: ZaratanasCollar's familiar emptied it)
    static uint32_t s_last_published_type = 0;
    if (!g.is_client) catsync_host_keep_familiars("about to publish a node", s_last_published_type >= 5 && s_last_published_type <= 8);
    if (!g.is_client) s_last_published_type = type;
    if (!g.is_client) roster_normalize_shared("about to publish a node");
    if (!g.is_client) catsync_dump_lists("about to publish a node");

    EnterNodeMsg m{};
    // This condition means "THE PARTS THAT ARE ON all made it into the message".
    // A module that is off by design returns true (see invsync/runhist's
    // publish_impl), because "nothing to send" is not a failed snapshot -- reading
    // it as one refused every node and locked the run on 2026-09-22.
    if (!catsync_read_familiars(m) ||
        !catsync_publish("entering a node", false, true) ||
        !invsync_publish("entering a node", true) ||
        !runhist_publish("entering a node", true)) {
        g.faulted = true;
        log_line_lvl(LogLevel::Error, "FOLLOW", "!! incomplete node snapshot -- node %u NOT entered", index);
        return false;
    }
    m.index      = index;
    m.node_count = count;
    m.type       = type;
    m.seed0      = seed;

    // The rest of the state, so a peer whose map came from a different save can
    // adopt it instead of refusing the node. This is the same read the game makes
    // in EnterNode (MapNode+0x118..0x137 -> TLS+0x178); word 0 is written into
    // seed0 above as well, so every existing comparison keeps meaning what it
    // meant and the added words are used only by the adoption path.
    uint8_t words[32] = {};
    if (!mem_read((const uint8_t*)node + kNode_Seed, words, sizeof(words)))
        log_line("FOLLOW", "!! could not read the full 32-byte node seed at node %u --"
                           " only word 0 travels, so a peer on a different save cannot"
                           " adopt it", index);
    memcpy(m.seed, words, sizeof(m.seed));
    m.seed[0] = seed;
    // battles and events: the clients answer from the host's unlock answers and event properties (shops and chests stay personal) -- and, with the
    // experimental features on, a treasure node: the shared chest's items are rolled on every peer from the host's answers (mgmp_treasure)
    if ((type >= 5 && type <= 11) || (type == 13 && session_exp_treasure())) unlocks_publish();
    if (type == 13) treasure_publish_luck(seed);   // proto 101: the host's party luck, before the node message (no-op unless the shared chest is on)
    if (!net_send_enter_node(m)) {
        g.faulted = true;
        log_line_lvl(LogLevel::Error, "FOLLOW", "!! node %u could not be sent -- NOT entered", index);
        return false;
    }
    ++g.published;
    if (!g.is_client && g.host_retry_pending && g.host_retry_map == map_screen &&
        g.host_retry_index == index) {
        g.host_retry_pending = false;
        g.host_retry_map = nullptr;
    }
    if (sent) *sent = true;
    lockstep_enter_battle(seed);
    remember_node(index, count, type, seed);
    log_line("FOLLOW", "-> node %u/%u (%s) seed0=%016llx, %u cat familiar(s)",
             index, count, node_type_name(type), (unsigned long long)m.seed0, m.familiar_count);
    return true;
}

// A peer connected or reconnected: tell it which node the run is standing in.
//
// Without this the whole save/cats/inventory catch-up leaves the joiner on the
// map with nothing to do, because ENTERNODE is otherwise published exactly once
// -- at the instant of entry -- and a peer that was not connected then never
// hears about it. That was the "client loads the save and the battle never
// activates" report.
bool follow_on_map() {
    if (!g.map_tick) return false;
    // 250 ms: MapScreen::update does not tick on the frame a node is entered,
    // and a single missed frame is not a reason to refuse a flush. A quarter
    // second is far shorter than any node and far longer than any frame.
    return !g.faulted && (GetTickCount64() - g.map_tick) < 250 && map_ready(g.map);
}

bool  follow_active() { return g.on && net_active(); }
void* follow_map_screen() { return g.map; }
bool  follow_map_ready(void* map) { return map && map_ready(map); }
void* follow_node_by_index(void* map, uint32_t index) { return node_at(map, index); }
bool  follow_index_of(void* map, void* node, uint32_t& index) { return index_of_node(map, node, index); }
uint64_t follow_node_seed(void* node) { return node_seed0(node); }
uint32_t follow_node_type(void* node) { return node_type(node); }
bool  follow_node_consumed(void* node) {
    uint8_t c = 0;
    return node && mem_read((const uint8_t*)node + kNode_Consumed, &c, sizeof(c)) && c != 0;
}
bool  follow_marker(void* map, void*& marker, void*& at, void*& moving, void*& selected) {
    marker = at = moving = selected = nullptr;
    if (!map || !mem_read((const uint8_t*)map + 0xA0, &marker, sizeof(marker)) || !marker) return false;
    mem_read((const uint8_t*)marker + 0x50, &at, sizeof(at));
    mem_read((const uint8_t*)marker + 0x58, &moving, sizeof(moving));
    mem_read((const uint8_t*)marker + 0x60, &selected, sizeof(selected));
    return true;
}

uint32_t follow_node_count() {
    if (!g.map) return 0;
    uint32_t count = 0; void* data = nullptr;
    if (!read_nodes(g.map, count, data)) return 0;
    return count;
}

bool follow_node_info(uint32_t index, uint32_t& type, uint64_t& seed0,
                      const char** type_name) {
    if (!g.map) return false;
    void* node = node_at(g.map, index);
    if (!node) return false;
    type  = node_type(node);
    seed0 = node_seed0(node);
    if (type_name) *type_name = node_type_name(type);
    return true;
}

bool follow_current_node(uint32_t& index) {
    if (!g.have_here) return false;
    index = g.here_index;
    return true;
}

// Deliberately unlocked. It is one naturally-aligned u64 written by the game
// thread in remember_node and read by the game thread when the host stamps a
// CHOICE, so the lock would only buy a lock-ordering hazard against the module
// that wants the value. See remember_node for the other half of that argument.
uint64_t follow_here_seed() { return g.have_here ? g.here_seed : 0; }
bool follow_host_ahead() { return g.pending_count != 0; }
uint32_t follow_here_type() { return g.have_here ? g.here_type : 0; }

// --- where the run is standing, read from the GAME rather than remembered ----
//
// follow_current_node above reports the node this MODULE watched somebody
// enter. That is the wrong answer for the case that matters most: a run
// reloaded from disk has entered nothing this session, so it reports nothing --
// and a reloaded run parked on an unresolved node is exactly when you need to
// be told which node that is.
//
// glaiel::MapNode::Click sets the current node with
//
//     *(MapNode+0x170 -> MapScreen) + 0xA0) + 0x60 = node
//
// and guards its own entry on the same read. MapScreen+0xA0 is the map MARKER
// (sub_14038DE60 calls MapMarker::CanReachNode on it and derives a facing from
// it), and it carries two node slots: +0x50, which is where the marker IS, and
// +0x60, which is what Click selected.
//
// BOTH ARE VALIDATED AGAINST THE NODE VECTOR RATHER THAN TRUSTED. index_of_node
// only reports an index when the pointer is genuinely an element of
// MapScreen+0x80, so a wrong offset yields "unknown" instead of a confident
// wrong number -- the same rule the per-turn state hash follows. That is what
// makes reading two undocumented slots an acceptable risk here.
constexpr uintptr_t kMap_Marker      = 0xA0;
constexpr uintptr_t kMarker_AtNode   = 0x50;
constexpr uintptr_t kMarker_MovingTo = 0x58;
constexpr uintptr_t kMarker_Selected = 0x60;

bool read_marker_node(uintptr_t slot, uint32_t& index) {
    if (!g.map) return false;
    const void* marker = nullptr;
    if (!mem_read((const uint8_t*)g.map + kMap_Marker, &marker, sizeof(marker)) || !marker)
        return false;
    void* node = nullptr;
    if (!mem_read((const uint8_t*)marker + slot, &node, sizeof(node)) || !node)
        return false;
    return index_of_node(g.map, node, index);
}

bool follow_marker_node(uint32_t& index)   { return read_marker_node(kMarker_AtNode,   index); }
bool follow_selected_node(uint32_t& index) { return read_marker_node(kMarker_Selected, index); }

// Native marker arrival (RVA 2222BD..222388) promotes +58 to +50 BEFORE
// invoking EnterNode, then clears +60 even if our hook held that invocation.
// Clicking the same node also clears +60 without invoking arrival (222040).
// Only the stationary current node is durable intent; selection alone is not.
bool follow_arrived_unentered(void* map, uint32_t& index) {
    if (!map || g.is_client || !net_active()) return false;
    const void* marker = nullptr;
    void* node = nullptr;
    const void* moving = nullptr;
    const void* selected = nullptr;
    if (!mem_read((const uint8_t*)map + kMap_Marker, &marker, sizeof(marker)) || !marker ||
        !mem_read((const uint8_t*)marker + kMarker_AtNode, &node, sizeof(node)) || !node ||
        !mem_read((const uint8_t*)marker + kMarker_MovingTo, &moving, sizeof(moving)) || moving ||
        !mem_read((const uint8_t*)marker + kMarker_Selected, &selected, sizeof(selected)) ||
        (selected && selected != node)) return false;
    uint32_t at = 0;
    if (!index_of_node(map, node, at)) return false;
    // THE PARTY ONLY STANDS WHERE IT CLICKED: +50 is written by an arrival, and
    // an arrival only happens for a node somebody clicked. Standing on an
    // UNCONSUMED node is therefore an entry that was attempted and never
    // completed -- exactly the state a boundary save reloads into, and the game
    // offers no way back in: clicking the current node clears the selection
    // (native 222033..2220A4) instead of calling EnterNode. That stranded a run
    // on an unconsumed HOME node on 2026-09-28, because the 2026-09-27 gate
    // below only recovered playable encounters.
    //
    // Recoverable: exit, home, and every encounter node through bonus. None of
    // those is a spawn point, so standing there means the click happened; the
    // chapter exit, the return-home settlement and a bonus room all re-run
    // their flow through the ordinary publish + o_EnterNode chain, exactly like
    // the battle nodes this recovery was built for.
    //
    // Still excluded: none/?1/enter -- "enter" is the chapter-start spawn, where
    // standing unconsumed is the NORMAL start of a chapter, not a failed entry;
    // empty/?18 -- no encounter to re-trigger.
    const uint32_t type = node_type(node);
    if (type < 3 || type > 16) return false;
    uint8_t consumed = 0;
    if (!mem_read((const uint8_t*)node + kNode_Consumed, &consumed, sizeof(consumed))) return false;
    if (consumed != 0) return false;
    // A successful dispatch is never retried while native transition/consumed
    // state catches up. Include seed because another chapter may reuse an index.
    if (g.have_here && g.here_index == at && g.here_seed == node_seed0(node)) return false;
    index = at;
    return true;
}

void follow_test_set_instant_walk_ms(uint64_t ms) { s_instant_walk_ms = ms; }

// A NODE THE PARTY STANDS ON, NEVER ENTERED, IN A RUN PLAYED ALONE (0.1.126; the final boss, room ZK9F). The host's click on the node after the boss was HELD (the checkpoint barrier: the other player had left), the
// party walked there anyway, and the save kept it standing on a node that was never entered. A run the mod started (its cats are session clones) is then dead in single player too: the game's own click on the
// node the party stands on only clears the selection, and nothing else leads on. The recovery the room has for the host (follow_arrived_unentered) is gated on a live room; this is the same test for a peer that is
// alone: the party stands still on an unconsumed playable node, the map is ready, nothing is selected, and it has stayed so for kSoloRecoverMs -- the game's own EnterNode is then run for it, once.
static uint64_t s_solo_recover_ms = 1500;
struct SoloStrand { void* map = nullptr; void* node = nullptr; uint64_t seed = 0; ULONGLONG since = 0; bool done = false; };
static SoloStrand s_solo;
static void* follow_solo_stranded(void* map_screen) {
    if (net_active() || !map_screen || g.faulted) { s_solo = SoloStrand{}; return nullptr; }
    void *marker = nullptr, *at = nullptr, *moving = nullptr, *selected = nullptr;
    if (!follow_marker(map_screen, marker, at, moving, selected) || !at || moving || (selected && selected != at)) { s_solo = SoloStrand{}; return nullptr; }
    uint32_t idx = 0;
    if (!index_of_node(map_screen, at, idx)) { s_solo = SoloStrand{}; return nullptr; }
    const uint32_t type = node_type(at);
    uint8_t consumed = 1;
    if (type < 3 || type > 16 || !mem_read((const uint8_t*)at + kNode_Consumed, &consumed, 1) || consumed) { s_solo = SoloStrand{}; return nullptr; }
    const uint64_t seed = node_seed0(at);
    const ULONGLONG now = GetTickCount64();
    if (s_solo.map != map_screen || s_solo.node != at || s_solo.seed != seed) { s_solo = SoloStrand{ map_screen, at, seed, now, false }; return nullptr; }
    if (s_solo.done || now - s_solo.since < s_solo_recover_ms) return nullptr;
    // only a run the mod made: its party holds session clones (0x70.. / 0x71.. ids); a run of the plain game is not touched
    uint64_t ids[32] = {};
    const uint32_t n = catsync_run_cat_list(ids, 32);
    bool session = false;
    for (uint32_t i = 0; i < n; ++i) if (ids[i] >= 0x70000000ull && ids[i] < 0x80000000ull) session = true;
    if (!session) return nullptr;
    s_solo.done = true;
    log_line_lvl(LogLevel::Warn, "FOLLOW", "recovering a node this party stands on and never entered, alone: node %u (%s), seed=%016llX -- the game's own entry is run for it now", idx, node_type_name(type), (unsigned long long)seed);
    return at;
}

void follow_test_set_solo_recover_ms(uint64_t ms) { s_solo_recover_ms = ms; s_solo = SoloStrand{}; }

void follow_request_jump(uint32_t index) {
    if (!config().dev_tools) return;                   // developer tool: edits the run
    g.jump_pending = true;
    g.jump_index   = index;
}

void follow_catchup(uint8_t peer) {
    if (!g.on || g.is_client || !net_active()) return;

    Guard guard;
    if (!g.have_here) {
        log_line("FOLLOW", "peer %u joined before this peer entered any node --"
                           " nothing to catch it up to; the next node is published"
                           " normally", (unsigned)peer);
        return;
    }

    EnterNodeMsg m{};
    m.index      = g.here_index;
    m.node_count = g.here_count;
    m.type       = g.here_type;
    m.seed0      = g.here_seed;

    // THE WHOLE 32 BYTES, not just word 0, for the same reason the live path
    // sends them: they are what a peer whose map came from a different save
    // adopts to put its RNG on the host's stream (tune::kAdoptHostSeed). This
    // message names a node the joiner may never have visited, so it is the only
    // chance to carry them -- and without them that peer skips the node instead
    // of adopting it. Read through the node vector, so a stale or unreadable map
    // pointer yields the zeros below rather than a confident wrong seed.
    uint8_t words[32] = {};
    if (void* node = node_at(g.map, g.here_index))
        if (mem_read((uint8_t*)node + kNode_Seed, words, sizeof(words)))
            memcpy(m.seed, words, sizeof(m.seed));
    m.seed[0] = m.seed0;

    if (!catsync_read_familiars(m)) {
        log_line_lvl(LogLevel::Error, "FOLLOW", "!! cannot read cat familiars for catch-up");
        return;
    }
    if (!net_send_enter_node_to(peer, m)) {
        // Not silent: a joiner that is not told where the run is sits on the map
        // until the next node, which looks exactly like the bug this whole
        // function exists to fix. "It could not be told" and "it was told" must
        // not read the same in the log.
        log_line_lvl(LogLevel::Warn, "FOLLOW", "!! could not tell peer %u which node"
                     " the run is in (node %u) -- it will be caught up at the next"
                     " node entry instead", (unsigned)peer, m.index);
        return;
    }

    log_line("FOLLOW", "-> re-sent the CURRENT node %u/%u (%s) seed0=%016llx to"
                       " peer %u (it joined after we entered it)",
             m.index, m.node_count, node_type_name(m.type),
             (unsigned long long)m.seed0, (unsigned)peer);
}

void follow_on_message(const EnterNodeMsg& m) {
    if (!g.on) return;
    Guard guard;
    if (g.faulted) return;
    if (!g.is_client) {
        // Symmetric message, asymmetric authority: a host that receives one is
        // talking to a peer that thinks it is the host too.
        log_line("FOLLOW", "!! received a node choice while hosting -- both peers"
                           " believe they own the run");
        return;
    }
    // A full queue means this peer is eight nodes behind, which no amount of
    // waiting is going to fix. Refuse the newest and SAY SO: the run is already
    // wrong, and the one thing that must not happen is what used to -- losing a
    // node without a line in the log. Dropping the newest rather than the
    // oldest keeps the queue contiguous, the same rule the peer-hash ring
    // follows for the same reason.
    if (g.pending_count >= State::kMaxPendingNodes) {
        g.faulted = true;
        log_line("FOLLOW", "!! %u nodes already queued and the host entered another"
                           " (node %u) -- REFUSING it. This peer is too far behind"
                           " to follow the run and the two are no longer the same"
                           " run.",
                 g.pending_count, m.index);
        choice_on_node_skipped(m.seed0);
        return;
    }

    if (g.have_here && m.seed0 == g.here_seed) {
        g.incoming.clear();
        log_line("FOLLOW", "ignored catch-up for current node %u", m.index);
        return;
    }
    // ...OR A NODE THIS PEER IS ALREADY BEHIND ON. The catch-up repeats the node
    // the run is standing in, and a peer trailing by one or more nodes already
    // has that node queued -- so without this the same node is queued TWICE and
    // the peer walks into it, out of it, and back into it. Measured shape: that
    // is what a reconnect during a lag does, and the second entry carries a
    // snapshot taken at a different moment than the first.
    //
    // Matched by SEED rather than index, because the seed is what the node's
    // simulation is built from: it is the only name for a node both peers arrive
    // at independently, which is the rule the rest of this file already follows.
    for (uint32_t i = 0; i < g.pending_count; ++i) {
        const State::PendingNode& p =
            g.pending_q[(g.pending_head + i) % State::kMaxPendingNodes];
        if (p.seed && p.seed == m.seed0) {
            g.incoming.clear();
            log_line("FOLLOW", "ignored a second copy of node %u -- already queued,"
                               " %u node(s) ahead of this peer",
                     m.index, g.pending_count);
            return;
        }
    }
    // A COMPLETE SNAPSHOT IS "EVERYTHING THAT IS ON" (2026-09-22). The two
    // per-player modules no longer travel (tune::kInvSync / kRunHist), so their
    // halves are legitimately absent and requiring them here stopped the CLIENT on
    // the host's first node. The check still holds for whatever IS on, which is
    // the part that matters: a snapshot missing the cats, or missing a payload
    // that this build does send, is still a fault.
    // A HOST WITH NO CAT OF ITS OWN LEFT (0.1.229; room WEEH 11-15: both of the host's cats died in a battle the other players won, the game moved
    // their cats into its party, and its node message carried no cat at all -- the host only sends the cats it edits). That is a complete
    // snapshot: the party it names is the other players' cats, which this peer holds. Refused, it stopped this peer following for the rest of
    // the run while the host played the event alone, and the run could not go on. Only when the host's party names no cat of the host's seat.
    bool host_has_none = false;
    if (!g.incoming.cat_count && m.party_count) {
        host_has_none = true;
        for (uint32_t i = 0; i < m.party_count && i < 16 && host_has_none; ++i) host_has_none = session_cat_owner_any(m.party_ids[i]) > 0;
        if (host_has_none)
            log_line_lvl(LogLevel::Warn, "FOLLOW", "node %u: the host's message carries no cat -- its party is %u cat(s) of the other players (the host has none of its own left): followed", m.index, m.party_count);
    }
    if ((tune::kInvSync  && !g.incoming.have_inventory) ||
        (tune::kRunHist  && !g.incoming.have_history) ||
        (!g.incoming.cat_count && !host_has_none)) {
        g.faulted = true;
        log_line_lvl(LogLevel::Error, "FOLLOW", "!! node %u arrived without a complete snapshot -- following stopped", m.index);
        return;
    }
    State::PendingNode& p =
        g.pending_q[(g.pending_head + g.pending_count) % State::kMaxPendingNodes];
    p.snapshot.swap(g.incoming);
    p.message = m;
    ++g.pending_count;
    p.index = m.index;
    // A queued future node must not replace the team while this peer is still editing.
    p.type  = m.type;
    p.count = m.node_count;
    p.seed  = m.seed0;
    memcpy(p.words, m.seed, sizeof(p.words));
    // Timed from ARRIVAL, not from the first map update that sees it: the point
    // of the delay is to put a known gap between the host entering a node and
    // this peer entering it, and the map screen may not be ready for a while
    // either way.
    p.at    = GetTickCount64();
    p.held  = false;
    p.walking = p.walk_marked = false; p.walk_at = 0;
    log_line("FOLLOW", "<- host entered node %u/%u (%s) seed0=%016llx%s",
             m.index, m.node_count, node_type_name(m.type),
             (unsigned long long)m.seed0,
             g.pending_count > 1 ? " -- QUEUED, this peer is still behind" : "");
}

// WHICH NODE TYPES MAY DIFFER BETWEEN THE PEERS (2026-09-22).
//
// The user's rule, and the reason it is not "all of them": each player sees its
// own random "?" events and its own chest loot, but A BATTLE MUST BE
// BIT-IDENTICAL on both peers -- the fight is the shared, hashed simulation, and
// any difference there is a desync rather than a feature.
//
// MapNode types, in enum order (see node_type_name): 5 battle, 6 hard, 7 miniboss,
// 8 boss. Those four are the battles -- "hard" is a battle with harder enemies, and
// the two boss types are battles by definition. Everything else on the map draws
// only for itself: event, optional_event, special_event, shop, treasure,
// furniturebox, foodbox, bonus -- and the inert types (enter, exit, home, empty)
// never draw at all, so perturbing them changes nothing and costs nothing.
//
// Note what makes this safe even if the classification were ever wrong: EnterNode
// re-seeds the simulation stream at EVERY node entry from that node's own seed, so
// a perturbation cannot survive past the node it was made in. A battle entered
// afterwards starts from its own seed regardless. The one thing this MUST NOT do is
// perturb INSIDE a battle node, which is exactly what this predicate excludes.
void follow_probe_node(void* node) {
    // BOTH ROLES, and that is the correction that makes this measurement work at all
    // (2026-09-22). It started client-only, and the client's map NEVER advances -- not for
    // battles, not for events, not for anything (measured, five entries, its map window
    // byte-identical every time) -- so there is nothing on that side to compare against.
    // The host is where the same node is consumed for real, and both peers hold the same
    // map objects; the field that differs between the two dumps of one node is the marker.
    if (!tune::kNodeProbe || !node) return;

    // THE PREVIOUS NODE IS THE POINT OF THE SECOND VERSION (2026-09-22).
    //
    // The first version dumped the node being entered, which answers nothing: consecutive
    // entries are DIFFERENT nodes, so "what a consumed node looks like" needs the same
    // object read twice. Measured with v1: four entries (battle, event, hard, treasure) and
    // the map window byte-identical across all four, i.e. this peer's map had not advanced
    // for ANY of them -- and the node windows could not say which field marks it because
    // each was a different node.
    //
    // So: remember the last node and re-read it here, before the new one. That is the
    // before/after pair.
    static const void* s_prev = nullptr;

    if (s_prev && s_prev != node) {
        char pw[256] = {};
        int  po = 0;
        for (uintptr_t o = 0x130; o <= 0x178 && po < (int)sizeof(pw) - 12; o += 4) {
            uint32_t v = 0;
            if (!mem_read((const uint8_t*)s_prev + o, &v, 4)) break;
            po += _snprintf_s(pw + po, sizeof(pw) - po, _TRUNCATE, " %03X=%08X",
                              (unsigned)o, v);
        }
        log_line("FOLLOW", "NODEPROBE PREVIOUS node %p re-read AFTER it was consumed: %s",
                 s_prev, pw);
    }

    uint32_t index = 0;
    const bool have_index = g.map && index_of_node(g.map, node, index);

    char node_words[256] = {};
    int  noff = 0;
    for (uintptr_t o = 0x130; o <= 0x178 && noff < (int)sizeof(node_words) - 12; o += 4) {
        uint32_t v = 0;
        if (!mem_read((const uint8_t*)node + o, &v, 4)) break;
        noff += _snprintf_s(node_words + noff, sizeof(node_words) - noff, _TRUNCATE,
                            " %03X=%08X", (unsigned)o, v);
    }

    char map_words[256] = {};
    int  moff = 0;
    for (uintptr_t o = 0x1C0; o <= 0x200 && moff < (int)sizeof(map_words) - 12; o += 4) {
        uint32_t v = 0;
        if (!g.map || !mem_read((const uint8_t*)g.map + o, &v, 4)) break;
        moff += _snprintf_s(map_words + moff, sizeof(map_words) - moff, _TRUNCATE,
                            " %03X=%08X", (unsigned)o, v);
    }

    // NOTE for whoever picks this up next: the marker may well live in the RUN rather than
    // on this screen (the save carries `adventure_state` and `on_adventure`, both run
    // fields), and a small-value scan of the MewDirector is the way to look. It is not here
    // only because this file has no access to the director's RVA -- adding that include is a
    // separate, checkable step rather than a guess made at the end of a long session.

    log_line("FOLLOW", "NODEPROBE index=%u (%s) type=%s seed0=%016llx node+130..178:%s",
             have_index ? index : 0xFFFFFFFFu, have_index ? "found" : "not in the vector",
             node_type_name(node_type(node)),
             (unsigned long long)node_seed0(node), node_words);
    log_line("FOLLOW", "NODEPROBE   (index=%u of the map's node vector) map+1C0..200:%s",
             have_index ? index : 0xFFFFFFFFu, map_words);

    // THE SAME WORDS AS FLOATS, BECAUSE THE PARTY'S POSITION IS A COORDINATE (2026-09-22).
    //
    // The user's own observation split this problem in two: the "completed" flag is one
    // thing (found: MapNode+0x168, now written) and WHERE THE PARTY STANDS is another,
    // driven by the click on the next node rather than by the node being finished. So the
    // position is a second field, and a position on an isometric map is two floats.
    //
    // In the data this probe already collected, one group of dwords near the map object
    // (1C8/1CC/1D0/1D4) changes per node on the HOST and barely at all on the client, and
    // read as floats they look like small coordinates (4092BC33 ~ 4.59, 40831B33 ~ 2.4).
    // Printing both readings for a wider range, plus the node's own, is what turns that
    // suspicion into "this field equals that node's position" -- a comparison with an
    // objective answer rather than a guess about layout.
    char map_floats[420] = {};
    int  mf = 0;
    for (uintptr_t o = 0x1A0; o <= 0x240 && mf < (int)sizeof(map_floats) - 22; o += 4) {
        uint32_t v = 0;
        if (!g.map || !mem_read((const uint8_t*)g.map + o, &v, 4)) break;
        float f = 0.0f;
        memcpy(&f, &v, 4);
        mf += _snprintf_s(map_floats + mf, sizeof(map_floats) - mf, _TRUNCATE,
                          " %X=%X/%.3f", (unsigned)o, v, f);
    }
    log_line("FOLLOW", "NODEPROBE   map+1A0..240 as hex/float:%s", map_floats);

    char node_floats[380] = {};
    int  nf = 0;
    for (uintptr_t o = 0x138; o <= 0x170 && nf < (int)sizeof(node_floats) - 22; o += 4) {
        uint32_t v = 0;
        if (!mem_read((const uint8_t*)node + o, &v, 4)) break;
        float f = 0.0f;
        memcpy(&f, &v, 4);
        nf += _snprintf_s(node_floats + nf, sizeof(node_floats) - nf, _TRUNCATE,
                          " %X=%X/%.3f", (unsigned)o, v, f);
    }
    log_line("FOLLOW", "NODEPROBE   node+138..170 as hex/float:%s", node_floats);

    // BYTE BY BYTE, BECAUSE A DWORD CAN HIDE TWO FIELDS (2026-09-22).
    //
    // MapNode+0x16C is the strongest remaining candidate for "the party is standing here":
    // it differs between the peers at EVERY node compared so far (host 00000001 / 80000001
    // / C4000001, client 3F000000 / 00000000 / 00000000), and the host's low byte is 1 in
    // all of them while its high bytes vary per node. That pattern -- one flag, one varying
    // byte-set -- is invisible in a dword dump and obvious in a byte dump, which is the same
    // lesson as the range/units mistake: print the thing at the size you mean.
    char node_bytes[160] = {};
    int  nb = 0;
    for (uintptr_t o = 0x158; o <= 0x170 && nb < (int)sizeof(node_bytes) - 4; ++o) {
        uint8_t b = 0;
        if (!mem_read((const uint8_t*)node + o, &b, 1)) break;
        nb += _snprintf_s(node_bytes + nb, sizeof(node_bytes) - nb, _TRUNCATE, "%02X ", b);
    }
    log_line("FOLLOW", "NODEPROBE   node+158..170 byte by byte: %s", node_bytes);

    // AND THE RUN, BECAUSE THE MAP SCREEN HAS NOW BEEN RULED OUT (2026-09-22).
    //
    // Where the party stands is NOT in MapScreen+1A0..240: measured, the host's own values
    // there are byte-identical across its own advances (they are the map's layout and
    // animation state), and the client's differ from the host's only in transition bytes.
    // The user's observation says the party moves when a node is CLICKED, and the run -- not
    // the screen -- is what a click updates in a game whose save carries `adventure_state`.
    //
    // So this scans the run and prints only slots that could be a coordinate or an index:
    // a small integer, or a float of sane magnitude (a pointer read as a float is ~1e38 and
    // filters itself out, which is the point of printing both readings). The comparison that
    // identifies the field is the same one that found MapNode+0x168: the same entry, two
    // peers, one of which advanced.
    char run_c[460] = {};
    int  rc = 0;
    {
        const void** slot =
            (const void**)((uintptr_t)GetModuleHandleW(nullptr) + kRva_MewDirectorPtr);
        const void* dir = nullptr;
        if (mem_read(slot, &dir, sizeof(dir)) && dir) {
            // THE RANGE IS IN HEX, THE ADDRESSES HEADER IS IN DECIMAL, AND THE FIRST RUN OF
            // THIS SCAN CAME BACK EMPTY BECAUSE OF IT (2026-09-22): the run's own fields are
            // written there as 1416 / 1424 / 1432 / 1468 / 1472, i.e. 0x588 / 0x590 / 0x598 /
            // 0x5BC / 0x5C0 -- all of them BELOW 0x1000, which was the start of the window.
            // The scan therefore skipped exactly the region it existed to look at. 0x30..0x800
            // covers the whole of the run state including the cat registry and id vector.
            for (uintptr_t o = 0x30; o <= 0x800 && rc < (int)sizeof(run_c) - 24;
                 o += 4) {
                uint32_t v = 0;
                if (!mem_read((const uint8_t*)dir + o, &v, 4) || !v) continue;
                float f = 0.0f;
                memcpy(&f, &v, 4);
                const bool small_u32 = (v <= 0x1000);
                const bool coord = (f == f) && (f >= -1000.0f) && (f <= 1000.0f) &&
                                   (f > 0.001f || f < -0.001f);
                if (!small_u32 && !coord) continue;
                rc += _snprintf_s(run_c + rc, sizeof(run_c) - rc, _TRUNCATE, " %X=%u/%g",
                                  (unsigned)o, v, (double)f);
            }
        }
    }
    log_line("FOLLOW", "NODEPROBE   run+30..800 (small ints and sane floats only):%s",
             run_c);

    s_prev = node;
}

// MapNode+0x140: CustomVector<MapNode*> of neighbours {cap, count@0x144, data@0x148}.
// Native consume (RVA 228240) walks it after setting +0x168.
constexpr uintptr_t kNode_LinkCount = 0x144;
constexpr uintptr_t kNode_LinkData = 0x148;
constexpr uintptr_t kNode_Flag169 = 0x169;
constexpr uint32_t kNodeType_Empty = 17;

// THE CASCADE THE FIRST VERSION MISSED (2026-09-28, 17:48 session). Native consume
// is not one byte: RVA 228240 sets +0x168 and then RECURSES into every neighbour
// that is unconsumed, has +0x169 == 0 and is type 17 (empty). The host left shop
// node 15 through that function and its empty neighbour 19 went 0 -> 1; the client
// wrote only node 15's byte, so the two maps differed by node 19 alone (both saves:
// the one differing chapter_map byte), the checkpoint fingerprint refused at the
// next boundary, and the run stopped one step before the boss. This mirrors the
// native walk exactly -- same three tests, same order -- and returns how many
// neighbours it marked. Depth is bounded; the native recursion is not, but a map
// has 22 nodes and a marked node is never revisited.
static unsigned cascade_consume_empty_neighbours(void* node, unsigned depth) {
    if (!node || depth > 64) return 0;
    uint32_t count = 0; uintptr_t data = 0;
    if (!mem_read((const uint8_t*)node + kNode_LinkCount, &count, sizeof(count)) ||
        !mem_read((const uint8_t*)node + kNode_LinkData, &data, sizeof(data)) ||
        !data || count > 64) return 0;
    unsigned marked = 0;
    for (uint32_t i = 0; i < count; ++i) {
        void* next = nullptr;
        if (!mem_read((const void*)(data + i * sizeof(void*)), &next, sizeof(next)) || !next) continue;
        uint8_t consumed = 1, flag = 1;
        if (!mem_read((const uint8_t*)next + kNode_Consumed, &consumed, 1) || consumed) continue;
        if (!mem_read((const uint8_t*)next + kNode_Flag169, &flag, 1) || flag) continue;
        if (node_type(next) != kNodeType_Empty) continue;
        const uint8_t one = 1;
        if (!mem_write((uint8_t*)next + kNode_Consumed, &one, 1)) {
            log_line_lvl(LogLevel::Error, "FOLLOW",
                         "!! could not mark empty neighbour %p consumed -- this peer's map"
                         " will differ from the host's at the next checkpoint", next);
            continue;
        }
        log_line("FOLLOW", "native consume cascade: empty neighbour %p marked CONSUMED"
                           " (the host's RVA 228240 makes the same write)", next);
        marked += 1 + cascade_consume_empty_neighbours(next, depth + 1);
    }
    return marked;
}

// A PARTY LEFT A NODE BEHIND (0.1.141; room F499, and the alley food box before it). The party marker stands on a node from which NOTHING unfinished can
// be walked to, while a node next to it is done -- the party should be standing there: its walk there never happened (a client entered the node ahead of its
// icon, an own event, a selection the game dropped). The game has no way out of it: a done node cannot be clicked, the node after it is not next to the party,
// and every later save keeps the party there, so the run is stuck in single player too. Here, on the map, standing still for a moment: when the game's own
// walk test (C_MapNodeBlocked) finds no unfinished node reachable from where the party stands, the done neighbours are tried -- each by putting the marker
// on it for the test and back -- and the party is put on the one that leads on, the one furthest along the map (from the start node towards the boss) and
// ahead of where it stands. Not on a client in a room: there the host's party node decides (align_party_marker). Once per standing.
namespace {
struct Unstick { void* map = nullptr; void* at = nullptr; ULONGLONG since = 0; bool looked = false; } s_unstick;
constexpr ULONGLONG kUnstickMs = 1500;
using fn_node_blocked = bool(__fastcall*)(void* marker, void* node);

// The map's direction: from its start node towards its boss, in the map layer; false when it does not read (then nothing is moved).
struct MapDir { float sx = 0, sy = 0, dx = 0, dy = 0; };
bool map_direction(void* map, MapDir& d) {
    float sx = 0, sy = 0, bx = 0, by = 0;
    bool have_s = false, have_b = false;
    uint32_t count = 0; void* data = nullptr;
    if (!read_nodes(map, count, data)) return false;
    for (uint32_t i = 0; i < count && i < 64; ++i) {
        void* node = node_at(map, i);
        const uint32_t t = node_type(node);
        if (!have_s && t == 2) have_s = mapvote_node_world(node, sx, sy);
        if (!have_b && t == 8) have_b = mapvote_node_world(node, bx, by);
    }
    if (!have_s || !have_b || (bx - sx) * (bx - sx) + (by - sy) * (by - sy) < 1.0f) return false;
    d.sx = sx; d.sy = sy; d.dx = bx - sx; d.dy = by - sy;
    return true;
}
// How far `to` lies ahead of `from` along the map's direction (> 0: ahead); -1e30 when a position does not read.
float ahead_of(const MapDir& d, void* from, void* to) {
    float fx = 0, fy = 0, tx = 0, ty = 0;
    if (!mapvote_node_world(from, fx, fy) || !mapvote_node_world(to, tx, ty)) return -1e30f;
    return (tx - fx) * d.dx + (ty - fy) * d.dy;
}
// The unfinished nodes linked to `from` and ahead of it that the game would let the party walk to from there (the marker stands on `from` for the test).
uint32_t leads_on(void* marker, void* from, const MapDir& d, fn_node_blocked blocked) {
    uint32_t links = 0; uintptr_t ldata = 0;
    if (!mem_read((const uint8_t*)from + kNode_LinkCount, &links, sizeof(links)) || !mem_read((const uint8_t*)from + kNode_LinkData, &ldata, sizeof(ldata)) || !ldata || links > 64) return 0;
    void* was = nullptr;
    if (!mem_read((const uint8_t*)marker + kMarker_AtNode, &was, sizeof(was))) return 0;
    if (from != was && !mem_write((uint8_t*)marker + kMarker_AtNode, &from, sizeof(from))) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < links; ++i) {
        void* next = nullptr;
        if (!mem_read((const void*)(ldata + i * sizeof(void*)), &next, sizeof(next)) || !next || next == from) continue;
        if (node_type(next) <= 2) continue;                                   // none / ?1 / the chapter's start
        uint8_t consumed = 1;
        if (!mem_read((const uint8_t*)next + kNode_Consumed, &consumed, 1) || consumed) continue;
        if (ahead_of(d, from, next) <= 0) continue;
        bool b = false;
        if (blocked) { __try { b = blocked(marker, next); } __except (EXCEPTION_EXECUTE_HANDLER) { b = false; } }
        if (!b) ++n;
    }
    if (from != was) mem_write((uint8_t*)marker + kMarker_AtNode, &was, sizeof(was));
    return n;
}
} // namespace

static void follow_unstick_marker(void* map) {
    if (!map || g.faulted || (net_active() && g.is_client)) { s_unstick = Unstick{}; return; }
    void *marker = nullptr, *at = nullptr, *moving = nullptr, *selected = nullptr;
    if (!follow_marker(map, marker, at, moving, selected) || !marker || !at || moving || selected) { s_unstick = Unstick{}; return; }
    const ULONGLONG now = GetTickCount64();
    if (s_unstick.map != map || s_unstick.at != at) { s_unstick = Unstick{ map, at, now, false }; return; }
    if (s_unstick.looked || now - s_unstick.since < kUnstickMs) return;
    s_unstick.looked = true;
    uint8_t at_done = 0;
    if (!mem_read((const uint8_t*)at + kNode_Consumed, &at_done, 1) || !at_done) return;   // standing on an unfinished node: follow_solo_stranded's case
    uint32_t at_idx = 0;
    if (!index_of_node(map, at, at_idx)) return;
    MapDir d;
    if (!map_direction(map, d)) return;                                       // no direction: nothing is judged
    const fn_node_blocked blocked = (fn_node_blocked)addr_of_call(C_MapNodeBlocked);   // closed areas only; null = not used
    if (leads_on(marker, at, d, blocked)) return;                             // something ahead can be walked to: not stuck
    uint32_t links = 0; uintptr_t ldata = 0;
    if (!mem_read((const uint8_t*)at + kNode_LinkCount, &links, sizeof(links)) || !mem_read((const uint8_t*)at + kNode_LinkData, &ldata, sizeof(ldata)) || !ldata || links > 64) return;
    void* best = nullptr; float best_p = 0; uint32_t best_r = 0;
    for (uint32_t i = 0; i < links; ++i) {
        void* next = nullptr;
        if (!mem_read((const void*)(ldata + i * sizeof(void*)), &next, sizeof(next)) || !next || next == at) continue;
        uint8_t consumed = 0;
        if (!mem_read((const uint8_t*)next + kNode_Consumed, &consumed, 1) || !consumed) continue;
        const float p = ahead_of(d, at, next);
        if (p <= 0) continue;                                                 // behind the party: never walked back to
        const uint32_t r = leads_on(marker, next, d, blocked);
        if (!r) continue;
        if (!best || p > best_p) { best = next; best_p = p; best_r = r; }
    }
    uint32_t best_idx = 0;
    if (!best || !index_of_node(map, best, best_idx)) {
        log_line_lvl(LogLevel::Warn, "FOLLOW", "!! the party on node %u has no unfinished node ahead of it, and no done node ahead of it leads on -- left as it is (the end of a chapter looks like this)", at_idx);
        return;
    }
    if (!mem_write((uint8_t*)marker + kMarker_AtNode, &best, sizeof(best))) return;
    log_line_lvl(LogLevel::Warn, "FOLLOW", "!! UNSTUCK: the party stood on node %u with no unfinished node ahead of it, while node %u ahead of it is done -- "
                 "the party is put on node %u (%u unfinished node(s) ahead from there)%s", at_idx, best_idx, best_idx, best_r, net_active() ? " [host]" : " [alone]");
}

void follow_mark_node_consumed(void* node) {
    if (!tune::kNodeConsumeMark || !g.on || !g.is_client || !node) return;

    // THE ONE FIELD THAT DIFFERS BETWEEN THE PEERS, and it took a two-peer measurement to
    // find (2026-09-22). MapNode+0x168 is 0 until the game has finished with a node and 1
    // after; at three nodes (hard, treasure, event) the HOST's copy went 0 -> 1 in every
    // case and the CLIENT's -- following into the same three nodes, with the same seeds --
    // stayed 0 in every case. The client's map therefore never learns which nodes are
    // behind it, which is exactly the reported symptom: after "continue alone", a solo peer
    // walks back into nodes the host had already cleared.
    //
    // Native uses a byte (RVA 228252); adjacent +169/+16A are independent flags.
    // THE WRITE IS DELIBERATELY THE NARROWEST ONE THAT CAN WORK: one byte, only when the
    // value is currently 0 (never over anything the game wrote), never back to 0, and read
    // back afterwards. Everything else on the node -- seeds at +0x118, the type at +0x138,
    // the geometry -- belongs to the game and is left alone.
    uint8_t before = 0;
    if (!mem_read((const uint8_t*)node + kNode_Consumed, &before, sizeof(before))) {
        log_line_lvl(LogLevel::Error, "FOLLOW",
                     "!! could not read MapNode+0x%X, so this peer's map was NOT told the"
                     " node is done -- a solo peer will walk it again", (unsigned)kNode_Consumed);
        return;
    }
    if (before) return;                     // already marked, by the game or by an earlier call

    // NOT A SPECIAL EVENT'S NODE AT ITS ENTRY (0.1.204): of the 7 crashes at Mewgenics.exe+0x9BDA4B/0x9BDABF (a shader's program or uniform read after it was
    // freed, in the frame's draw: GPC 11-07 00:21, DESKTOP-4NLA8EA 11-07 22:59, the 11-05 open one ...) 6 came within four lines of THIS write on a
    // special_event node, as the client followed the host into it -- and none of ~10,000 writes on other node types. The node's look changes with the
    // flag while the screen goes over to the event. Left to the map sync (the host's flag words are adopted on the map, between frames that draw it).
    if (!strcmp(node_type_name(node_type(node)), "special_event")) {
        log_line("FOLLOW", "the followed special_event node %p is NOT marked consumed at its entry (the write crashed the draw of its look, 0x9BDA) -- the map sync takes the host's flag later", node);
        return;
    }

    const uint8_t after = 1;
    if (!mem_write((uint8_t*)node + kNode_Consumed, &after, sizeof(after))) {
        log_line_lvl(LogLevel::Error, "FOLLOW",
                     "!! MapNode+0x%X is not writable -- this peer's map still does not know"
                     " the node is done", (unsigned)kNode_Consumed);
        return;
    }

    uint8_t check = 0;
    mem_read((const uint8_t*)node + kNode_Consumed, &check, sizeof(check));

    log_line("FOLLOW", "marked the followed node CONSUMED on this peer's map: node %p (%s),"
                       " MapNode+0x%X 0 -> %u (%s). The host's own code makes this same write"
                       " when it finishes a node -- this is that write, for a peer that only"
                       " ever followed into it.",
             node, node_type_name(node_type(node)), (unsigned)kNode_Consumed, check,
             check == 1 ? "read back agrees" : "READ-BACK DISAGREES");
    if (check == 1) cascade_consume_empty_neighbours(node, 0);
}

// THE PARTY STANDS ON THE NODE BEFORE IT IS ENTERED (0.1.218). Crash Mewgenics.exe+0x9BDA4B/0x9BDABF -- the frame's draw binds a shader program that was freed
// (0x1409BDA20 via the batch flush 0x140A485A0) -- kept coming after 0.1.204 had stopped marking special_event nodes done at their entry: yuge 11-08 23:08
// (special_event), yuge 11-10 20:54 (a BATTLE node, marked done), DESKTOP-9CMVOS6 11-10 22:01 (special_event right after the Core boss, room D6UL). What all three had in
// common, as the last mod line before the crash, was follow_move_marker_to: the marker's SELECTION written right AFTER the client's EnterNode, so the map set off a walk
// in the very frame the screen went over to the event or the battle. The game never does that: its click selects, the walk runs on a live map, and the node is entered
// when the marker has ARRIVED (standing on it, no walk, no selection). That arrival state is what is written now, before the entry; after it nothing is written.
void follow_place_marker_on(void* node) {
    if (!g.on || !g.is_client || !g.map || !node) return;
    void* marker = nullptr;
    if (!mem_read((const uint8_t*)g.map + kMap_Marker, &marker, sizeof(marker)) || !marker) {
        log_line("FOLLOW", "marker: MapScreen+0x%X did not read as the marker object -- the party icon stays where it was", (unsigned)kMap_Marker);
        return;
    }
    uint32_t idx = 0;
    if (!index_of_node(g.map, node, idx)) {
        log_line_lvl(LogLevel::Warn, "FOLLOW", "marker: the followed node %p is not in this peer's node vector -- the marker is left alone", node);
        return;
    }
    void *at = nullptr, *moving = nullptr, *sel = nullptr;
    mem_read((const uint8_t*)marker + kMarker_AtNode, &at, sizeof(at));
    mem_read((const uint8_t*)marker + kMarker_MovingTo, &moving, sizeof(moving));
    mem_read((const uint8_t*)marker + kMarker_Selected, &sel, sizeof(sel));
    if (at == node && !moving && !sel) return;                     // already standing there: the instant nodes' walk (0.1.120) arrived on its own
    void* const none = nullptr;
    bool ok = mem_write((uint8_t*)marker + kMarker_MovingTo, &none, sizeof(none)) && mem_write((uint8_t*)marker + kMarker_Selected, &none, sizeof(none)) &&
              mem_write((uint8_t*)marker + kMarker_AtNode, &node, sizeof(node));
    // ...and WHERE IT IS DRAWN (0.1.219; Dcrf 11-10 23:20, room E7BN: "我的猫在地图上不移动，但可以正常进入节点" -- +0x50 is only the node the party stands on, the icon
    // stayed where the last walk had left it). The game's own arrival (0x1402222BD) writes the same: the marker's transform (+0x48) at +0x80/+0x88 = the node's
    // look position (node+0x160 -> +0x70/+0x74, floats) as doubles, +0x90 = 0, and the walk's progress +0x68 back to 0.
    {
        const uint8_t* look = nullptr; uint8_t* xform = nullptr; float pos[2] = {};
        if (mem_read((const uint8_t*)node + 0x160, &look, sizeof(look)) && look && mem_read(look + 0x70, pos, sizeof(pos)) &&
            mem_read((const uint8_t*)marker + 0x48, &xform, sizeof(xform)) && xform) {
            const double at[3] = { (double)pos[0], (double)pos[1], 0.0 };
            const double zero = 0.0;
            ok = mem_write(xform + 0x80, at, sizeof(at)) && mem_write((uint8_t*)marker + 0x68, &zero, sizeof(zero)) && ok;
        } else ok = false;
    }
    uint32_t at_idx = 0;
    const bool at_known = at && index_of_node(g.map, at, at_idx);
    log_line("FOLLOW", "marker: the party is put ON node %u before this peer enters it (it stood on node %d%s%s) -- the state the game's own walk leaves on arrival; nothing of the map is written after the entry (crash 0x9BDA)%s",
             idx, at_known ? (int)at_idx : -1, moving ? ", walking" : "", sel ? ", a node selected" : "", ok ? "" : " -- A WRITE FAILED");
}

void follow_move_marker_to(void* node) {
    if (!g.on || !g.is_client || !g.map || !node) return;

    // THE VISUAL HALF: WHERE THE PARTY IS DRAWN (2026-09-22).
    //
    // The functional half is follow_mark_node_consumed: with the node marked done, a solo
    // peer can pick the next node and play on, which the user found from play. What was
    // left was that the party ICON stayed at the start of the map on this peer.
    //
    // The marker is the game's own object, and its two slots are already documented in this
    // file from MapNode::Click:
    //
    //     *(MapScreen+0xA0) + 0x50  = the node the marker IS on
    //     *(MapScreen+0xA0) + 0x60  = the node CLICK SELECTED
    //
    // and Click writes +0x60, after which the game animates the marker from +0x50 to it.
    // So this writes +0x60 too -- the selection, never the position. No animation state is
    // faked, no coordinate is invented, and the marker travels the way it always does.
    const void* marker = nullptr;
    if (!mem_read((const uint8_t*)g.map + kMap_Marker, &marker, sizeof(marker)) || !marker) {
        log_line("FOLLOW", "marker: MapScreen+0x%X did not read as the marker object -- this"
                           " peer's party icon stays where it was", (unsigned)kMap_Marker);
        return;
    }

    // VALIDATED AGAINST THE NODE VECTOR BEFORE ANY WRITE, the same rule the reader uses: a
    // node that is not an element of this map's vector means one of the two pointers is not
    // what it is believed to be, and that is not the moment to write to it.
    uint32_t idx = 0;
    if (!index_of_node(g.map, node, idx)) {
        log_line_lvl(LogLevel::Warn, "FOLLOW",
                     "marker: the followed node %p is not in this peer's node vector --"
                     " refusing to move the marker", node);
        return;
    }

    void* cur = nullptr;
    if (!mem_read((const uint8_t*)marker + kMarker_Selected, &cur, sizeof(cur))) {
        log_line("FOLLOW", "marker: +0x%X is not readable -- marker not moved",
                 (unsigned)kMarker_Selected);
        return;
    }
    if (cur == node) return;                 // already selected; Click would do nothing either
    void* standing = nullptr;
    if (mem_read((const uint8_t*)marker + kMarker_AtNode, &standing, sizeof(standing)) && standing == node) return;   // the party is there: selecting its own node only clears the selection

    // THE LAST WALK NEVER HAPPENED (0.1.139, room F499). The previous followed node is still only SELECTED (the marker never set off: this player was on
    // its own event there while the host went on), and the next one is selected over it now. From the node before, the next node is no walk: the game
    // clears the selection, the party stays a node behind for good, and every later save keeps it there -- the boss node, reachable only from the last
    // node, could not be clicked even after a restore. The marker is put on the node it was sent to first, so this walk starts where the party really is.
    void* moving_now = nullptr;
    if (cur && cur != standing && mem_read((const uint8_t*)marker + kMarker_MovingTo, &moving_now, sizeof(moving_now)) && !moving_now) {
        uint32_t cur_idx = 0, at_idx = 0;
        if (index_of_node(g.map, cur, cur_idx)) {
            const bool at_known = standing && index_of_node(g.map, standing, at_idx);
            if (mem_write((uint8_t*)marker + kMarker_AtNode, &cur, sizeof(cur))) {
                standing = cur;
                log_line_lvl(LogLevel::Warn, "FOLLOW", "!! marker: the walk to node %u never started (the party still stood on node %d) -- the party is put on node %u before it walks to node %u",
                             cur_idx, at_known ? (int)at_idx : -1, cur_idx, idx);
            }
        }
    }

    if (!mem_write((uint8_t*)marker + kMarker_Selected, &node, sizeof(node))) {
        log_line("FOLLOW", "marker: +0x%X is not writable -- marker not moved",
                 (unsigned)kMarker_Selected);
        return;
    }

    void* check = nullptr;
    mem_read((const uint8_t*)marker + kMarker_Selected, &check, sizeof(check));

    log_line("FOLLOW", "marker: selected node %u on this peer's map (MapScreen+0x%X -> +0x%X),"
                       " %p -> %s. This is the slot MapNode::Click writes, so the game moves"
                       " the party icon itself -- nothing about the position is faked",
             idx, (unsigned)kMap_Marker, (unsigned)kMarker_Selected,
             cur, (check == node) ? "written and read back" : "READ-BACK DISAGREES");
}

bool node_may_differ(uint32_t type) {
    return type != 5 && type != 6 && type != 7 && type != 8;
}

void follow_perturb_after_enter(void* node) {
    if (!g.on || !g.is_client || g.faulted || !node) return;
    if (!tune::kPerPlayerNodes) return;

    const uint32_t type = node_type(node);
    if (!node_may_differ(type)) {
        log_line_lvl(LogLevel::Trace, "FOLLOW",
                     "node %u is a BATTLE ('%s') -- this peer's stream is left alone,"
                     " the fight must be identical on both peers",
                     g.here_index, node_type_name(type));
        return;
    }

    uint64_t* stream = rng_global_stream();
    if (!stream) {
        log_line("FOLLOW", "!! could not read the simulation stream at node %u -- this"
                           " peer's event will be the same as the host's", g.here_index);
        return;
    }

    // THE STREAM ITSELF, NOT THE DRAW API -- and the first version of this got that
    // wrong in a way worth recording, because the log said so and nothing else would
    // have. It called rng_original_randfloat(), which is the ORIGINAL behind the
    // draw recorder's detour -- and that detour is installed only when
    // debug.record is on. The startup banner spells it out (`[-] RANDFLT randfloat
    // ... off: needs debug.record`), so in an ordinary session the pointer is null,
    // the call was skipped, and every node logged:
    //
    //   !! the randfloat original is not resolved -- cannot make this peer's node
    //      content differ at node 17
    //
    // The stream needs no API at all. It is four u64s of xoshiro256 state at a known
    // TLS address, rng_global_stream() gives the address unconditionally (it is a
    // plain TLS read), and ANY non-zero state is a state the generator can draw from.
    // So the perturbation changes the state directly: one fixed, odd constant XORed
    // into the first word. It is deterministic on purpose -- the same node produces
    // the same difference every session, which is what makes a bug report here
    // reproducible -- and it is not a draw, so it does not enter the recorder, which
    // is right: this is a deliberate difference between the peers, not a divergence
    // to be hunted.
    const uint64_t before = stream[0];
    stream[0] ^= 0x9E3779B97F4A7C15ull;
    if (!(stream[0] | stream[1] | stream[2] | stream[3])) stream[0] = 1;   // never all-zero
    const uint64_t after = stream[0];

    log_line("FOLLOW", "perturbed this peer's simulation stream at node %u (%s) -- one"
                       " extra draw, %016llx -> %016llx. The host draws its own event,"
                       " chest and shop stock from the same seed, so the two now differ"
                       " on purpose; EnterNode re-seeds at the next node, so the next"
                       " BATTLE is unaffected",
             g.here_index, node_type_name(type),
             (unsigned long long)before, (unsigned long long)after);
}

// Hash only verified persistent node fields, never object addresses or UI
// selection. The paired baseline differs in its last-event UI header, but its
// entire saved node array is identical (2026-09-27).
uint64_t follow_resume_checkpoint(void* map) {
    uint32_t count = 0; void* data = nullptr;
    if (!read_nodes(map, count, data)) return 0;
    uint64_t h = 0xcbf29ce484222325ull;
    auto hash_bytes = [&h](const void* p, size_t n) {
        const auto* bytes = (const uint8_t*)p;
        for (size_t i = 0; i < n; ++i) { h ^= bytes[i]; h *= 0x100000001b3ull; }
    };
    hash_bytes(&count, sizeof(count));
    char types[130] = {};   // the node types in order, for the log: two maps that differ say where
    char firsts[24 * 17 + 1] = {};   // each node's first seed word (up to 24 nodes), for the same reason
    char extra[24 * 22 + 1] = {};    // each node's consumed dword and a fold of seed words 1..3
    for (uint32_t i = 0; i < count; ++i) {
        void* node = node_at(map, i);
        uint64_t seeds[4] = {};
        uint32_t type = 0, consumed = 0;
        if (!node || !mem_read((uint8_t*)node + kNode_Seed, seeds, sizeof(seeds)) ||
            !mem_read((uint8_t*)node + kNode_Type, &type, 4) ||
            !mem_read((uint8_t*)node + kNode_Consumed, &consumed, 4)) return 0;
        hash_bytes(seeds, sizeof(seeds)); hash_bytes(&type, 4); hash_bytes(&consumed, 4);
        if (i < 128) types[i] = (char)(type < 10 ? '0' + type : 'a' + (type - 10) % 26);
        if (i < 24) _snprintf_s(firsts + i * 17, 18, _TRUNCATE, "%016llx ", (unsigned long long)seeds[0]);
        if (i < 24) _snprintf_s(extra + i * 22, 23, _TRUNCATE, "%u:%016llx ", consumed, (unsigned long long)(seeds[1] ^ (seeds[2] * 31) ^ (seeds[3] * 131)));
    }
    static uint64_t logged = 0;
    if (h && h != logged) {
        logged = h;
        log_line("FOLLOW", "map %016llx: %u node(s), types %s -- compare with the other peer: a refused checkpoint means these differ", (unsigned long long)h, count, types);
        log_line("FOLLOW", "map %016llx: node seed words %s", (unsigned long long)h, firsts);
        log_line("FOLLOW", "map %016llx: consumed:fold(seed words 1..3) per node: %s", (unsigned long long)h, extra);
    }
    return h ? h : 1;
}

// ---- THE MAP SYNC ---------------------------------------------------------------------------------------------------------------------------
//
// Two peers with different saves generate the same map STRUCTURE (the log says "22 node(s), types ..." on both) and different node SEEDS, and different
// lock bytes: generate_map flags the nodes that are locked for THIS player's unlock progress (the hard path, the sewers and junkyard exits, ...) in the
// dword at MapNode+0x168, which the checkpoint hash covers. The checkpoint therefore refused the pair at node 0 and the run sat there.
//
//   * SEEDS: the host is the authority -- it publishes every node's type and 32-byte seed, a client with the same structure overwrites its own.
//   * LOCKS: the least progress decides. Every client reports its flag dwords to the host first; the host ORs them with its own (a node locked for
//     anybody is locked for everybody), writes the result into its map, and publishes it together with the seeds; the clients overwrite theirs.
//
// A structure that does not match is not touched, and says so. Everybody waits for the other side a few seconds at most.
namespace {
struct Report {                              // a client's flags as the host holds them
    uint32_t epoch = 0, total = 0, got = 0, used_epoch = 0;
    bool     have[kMapSeedsMaxNodes] = {};
    uint8_t  type[kMapSeedsMaxNodes] = {};
    uint32_t flags[kMapSeedsMaxNodes] = {};
};
struct SeedSync {
    // client: what the host published
    uint32_t epoch = 0, total = 0, got = 0;
    bool     have[kMapSeedsMaxNodes] = {};
    MapSeedNode node[kMapSeedsMaxNodes] = {};
    uint32_t applied_epoch = 0;
    void*    applied_map = nullptr;
    int32_t  party_at = -1;                  // the node the host's party stands on (kFlagPartyHere in its publication), -1 = not said
    void*    reported_map = nullptr;         // the map this client has sent its flags for
    void*    waiting_map = nullptr;
    ULONGLONG waiting_since = 0;
    // host
    Report   rep[kMaxPeers];                 // by transport id
    uint64_t published_hash = 0;
    void*    host_map = nullptr;
    ULONGLONG host_since = 0;
    bool     merged = false;
} ss;
constexpr ULONGLONG kSeedWaitMs = tune::kSeedWaitMs;
constexpr uintptr_t kNodeFlagsDword = 0x168;
// THE HOST'S PARTY NODE RIDES ON THE PUBLICATION (0.1.139, room F499). A bit no MapNode+0x168 dword has (consumed is bit 0, the lock/hidden bytes are
// +0x169..+0x16B with small values): the host sets it on the node its marker stands on, the client takes it off before anything is compared or written.
constexpr uint32_t kFlagPartyHere = 0x80000000u;

} // namespace

void follow_seed_sync_reset() { ss = SeedSync{}; }

void follow_on_mapseeds(uint8_t from, const MapSeedsMsg& m) {
    if (!valid_mapseeds(m)) return;
    if (!g.is_client) {                                   // the host: a client's flags
        if (m.kind != kMapSeedsFromClient || from >= kMaxPeers) return;
        Report& r = ss.rep[from];
        if (m.epoch != r.epoch) { const uint32_t used = r.used_epoch; r = Report{}; r.used_epoch = used; r.epoch = m.epoch; r.total = m.total; }
        if (m.total != r.total) return;
        for (uint32_t i = 0; i < m.count; ++i) {
            const uint32_t at = m.first + i;
            if (at >= kMapSeedsMaxNodes) return;
            if (!r.have[at]) { r.have[at] = true; ++r.got; }
            r.type[at] = m.nodes[i].type; r.flags[at] = m.nodes[i].flags;
        }
        return;
    }
    if (from != kHostPeer || m.kind != kMapSeedsFromHost) return;
    if (m.epoch != ss.epoch) {                            // a new publication replaces the old one
        ss.epoch = m.epoch; ss.total = m.total; ss.got = 0; ss.party_at = -1;
        memset(ss.have, 0, sizeof(ss.have));
    }
    if (m.total != ss.total) return;
    for (uint32_t i = 0; i < m.count; ++i) {
        const uint32_t at = m.first + i;
        if (at >= kMapSeedsMaxNodes) return;
        if (!ss.have[at]) { ss.have[at] = true; ++ss.got; }
        ss.node[at] = m.nodes[i];
        if (ss.node[at].flags & kFlagPartyHere) { ss.party_at = (int32_t)at; ss.node[at].flags &= ~kFlagPartyHere; }
    }
}

namespace {

bool read_flags(void* node, uint32_t& v) { return node && mem_read((const uint8_t*)node + kNodeFlagsDword, &v, sizeof(v)); }

// The node this map's party marker stands on, while it stands (not walking, no other node selected); nullptr otherwise.
void* marker_standing(void* map) {
    void *marker = nullptr, *at = nullptr, *moving = nullptr, *selected = nullptr;
    if (!map || !mem_read((const uint8_t*)map + kMap_Marker, &marker, sizeof(marker)) || !marker ||
        !mem_read((const uint8_t*)marker + kMarker_AtNode, &at, sizeof(at)) || !at ||
        !mem_read((const uint8_t*)marker + kMarker_MovingTo, &moving, sizeof(moving)) || moving ||
        !mem_read((const uint8_t*)marker + kMarker_Selected, &selected, sizeof(selected)) || (selected && selected != at)) return nullptr;
    return at;
}

// Sends this map's per-node data in chunks. `seeds` false: only types and flags (a client's report).
bool send_map(void* map, uint32_t count, uint8_t kind, uint32_t epoch, const uint32_t* flags_override) {
    void* party = kind == kMapSeedsFromHost ? marker_standing(map) : nullptr;
    for (uint32_t first = 0; first < count; first += kMapSeedsChunk) {
        MapSeedsMsg m{};
        m.kind = kind; m.epoch = epoch; m.total = count; m.first = first;
        m.count = (uint8_t)((count - first) < kMapSeedsChunk ? (count - first) : kMapSeedsChunk);
        for (uint32_t i = 0; i < m.count; ++i) {
            void* node = node_at(map, first + i);
            uint32_t fl = 0;
            if (!node || !read_flags(node, fl)) return false;
            if (kind == kMapSeedsFromHost && !mem_read((const uint8_t*)node + kNode_Seed, m.nodes[i].seed, sizeof(m.nodes[i].seed))) return false;
            m.nodes[i].type = (uint8_t)node_type(node);
            m.nodes[i].flags = flags_override ? flags_override[first + i] : fl;
            if (party && node == party) m.nodes[i].flags |= kFlagPartyHere;
        }
        if (!net_send_mapseeds(m)) return false;
    }
    return true;
}

// THE MAP'S NODE STATES AFTER THE MAP SYNC (0.1.215, room 38C9 11-09 and 16 more uploads of 11-08/09). A node's button can be clicked only when the node is
// open to the party: its reachable byte (+0x16C) and its look's state, which the game works out from the done flags in C_MapNodeRefresh -- over every node
// when the party arrives at one, and when the map's looks are made. A done flag the map sync writes afterwards is not seen: since 0.1.204 a special_event node
// is not marked done as the client follows into it (crash 0x9BDA), the client's map came back from the event with that node open, and its done flag came
// with the host's map a moment later -- the next node stayed closed (every click on it reached no button) until the save was loaded again. After any flag
// the map sync writes, the game's own refresh runs over every node, as its arrival does.
constexpr uintptr_t kNode_Reachable = 0x16C;
using fn_node_refresh = bool (__fastcall*)(void* node);
static bool call_node_refresh(fn_node_refresh fn, void* node) {
    __try { fn(node); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void refresh_node_states(void* map, uint32_t count, const char* why) {
    const fn_node_refresh fn = (fn_node_refresh)addr_of_call(C_MapNodeRefresh);
    if (!fn) { log_line_lvl(LogLevel::Warn, "FOLLOW", "!! node states not refreshed after %s: the game's node refresh was not found -- a node may stay closed until the map is made again", why); return; }
    uint32_t refreshed = 0, opened = 0, closed = 0, faults = 0;
    char list[160] = {}; int w = 0;
    for (uint32_t i = 0; i < count; ++i) {
        void* node = node_at(map, i);
        if (!node) continue;
        uint8_t before = 0, after = 0;
        mem_read((const uint8_t*)node + kNode_Reachable, &before, 1);
        if (!call_node_refresh(fn, node)) { ++faults; continue; }
        ++refreshed;
        mem_read((const uint8_t*)node + kNode_Reachable, &after, 1);
        if (before == after) continue;
        if (after) ++opened; else ++closed;
        if (w >= 0 && (size_t)w < sizeof(list) - 16) w += _snprintf_s(list + w, sizeof(list) - w, _TRUNCATE, " %u:%u>%u", i, (unsigned)before, (unsigned)after);
    }
    log_line_lvl(faults ? LogLevel::Warn : LogLevel::Info, "FOLLOW", "%snode states refreshed after %s: %u node(s), %u now reachable, %u no longer%s%s%s", faults ? "!! " : "", why, refreshed, opened, closed,
             w > 0 ? " --" : "", list, faults ? " (the refresh FAULTED on some nodes)" : "");
}

// THE CLIENT'S PARTY MARKER STANDS WHERE THE HOST'S DOES (0.1.139, room F499). A client enters a node the host entered at once (follow) and only
// SELECTS it on its marker; the icon walks there when the map runs again. A save taken in between -- the checkpoint is staged the moment the map is
// back, right after a node -- keeps the party on the node BEFORE. Restored, the client's game had the party one node behind: the boss node, reachable
// only from the last node, was not clickable at all (no click reached the vote), while the host's could be clicked. Here, after the host's map is
// adopted: a marker standing elsewhere is sent there -- walked, the game's own way, when it can walk there; put there when it cannot.
void align_party_marker(void* map, int32_t want_idx) {
    if (want_idx < 0) return;
    void* want = node_at(map, (uint32_t)want_idx);
    void *marker = nullptr, *at = nullptr, *moving = nullptr, *selected = nullptr;
    if (!want || !mem_read((const uint8_t*)map + kMap_Marker, &marker, sizeof(marker)) || !marker ||
        !mem_read((const uint8_t*)marker + kMarker_AtNode, &at, sizeof(at)) ||
        !mem_read((const uint8_t*)marker + kMarker_MovingTo, &moving, sizeof(moving)) ||
        !mem_read((const uint8_t*)marker + kMarker_Selected, &selected, sizeof(selected))) return;
    if (at == want || moving == want) return;                               // there, or walking there
    if (moving) return;                                                     // walking somewhere else: the next publication looks again
    uint32_t at_idx = 0xFFFFFFFFu;
    if (at) index_of_node(map, at, at_idx);
    // PUT THERE, NOT WALKED (0.1.142, room L37K). 0.1.139 selected the node when C_MapNodeBlocked said it could be walked to -- but that test does not
    // look at where the party stands, and the game never walks to a done node: the selection was dropped, the party stayed a node behind and the boss
    // could not be clicked, the same as before. A pending selection of it is no better (it is what stranded F499). The marker is put on the node.
    void* none = nullptr;
    mem_write((uint8_t*)marker + kMarker_AtNode, &want, sizeof(want));
    mem_write((uint8_t*)marker + kMarker_Selected, &none, sizeof(none));
    log_line_lvl(LogLevel::Warn, "FOLLOW", "!! the party marker stood on node %d here%s, the host's party on node %d -- the marker is put on it",
                 at_idx == 0xFFFFFFFFu ? -1 : (int)at_idx, selected == want ? " (that node only selected, never walked to)" : "", want_idx);
}

// True when this map may be reported to the checkpoint now.
bool map_seed_sync(void* map) {
    if (!g.on || !map) return true;
    uint32_t count = 0; void* data = nullptr;
    if (!read_nodes(map, count, data) || count > kMapSeedsMaxNodes) return true;
    const ULONGLONG now = GetTickCount64();

    if (!g.is_client) {
        if (!net_active()) return true;
        if (ss.host_map != map) { ss.host_map = map; ss.host_since = now; ss.merged = false; }
        if (!ss.merged) {
            // THE HOST'S FLAGS ARE THE MAP'S (2026-10-03, the user's rule): a node a client has not unlocked does not lock the host -- the host leads the others in. Nothing of the
            // clients' reports is merged and nobody is waited for; the clients overwrite their own flags with the published ones (the adoption below).
            for (int k = 0; k < kMaxPeers; ++k) ss.rep[k].used_epoch = ss.rep[k].epoch;
            ss.merged = true;
            log_line("FOLLOW", "map flags: this (host) map's own node locks stand; the clients' are not merged into it");
        }
        const uint64_t h = follow_resume_checkpoint(map);
        if (!h || h == ss.published_hash) return true;
        if (!send_map(map, count, kMapSeedsFromHost, (uint32_t)GetTickCount() | 1u, nullptr)) return true;   // not recorded: the next tick tries again
        ss.published_hash = h;
        log_line("FOLLOW", "published this map's %u node seed(s) and flags to the clients (map %016llx)", count, (unsigned long long)h);
        return true;
    }

    // client: first tell the host this map's own flags (before anything is overwritten), then wait for the host's seeds and merged flags
    if (ss.reported_map != map) {
        if (!send_map(map, count, kMapSeedsFromClient, (uint32_t)GetTickCount() | 1u, nullptr)) return false;
        ss.reported_map = map;
    }
    // A map object is adopted once -- and again for every LATER publication of the host (it republishes when its map changed: a node finished, a battle
    // over). Only a new epoch re-opens it; without one the map is as agreed as it will get.
    const bool newer = ss.epoch && ss.epoch != ss.applied_epoch;
    if (map == ss.applied_map && !newer) return true;
    if (newer && ss.got == ss.total && ss.total) {
        ss.applied_epoch = ss.epoch; ss.applied_map = map; ss.waiting_map = nullptr;
        if (ss.total != count) {
            log_line("FOLLOW", "!! the map sync cannot apply: the host's map has %u node(s), this one %u", ss.total, count);
            return true;
        }
        for (uint32_t i = 0; i < count; ++i) {
            void* node = node_at(map, i);
            if (node && node_type(node) != ss.node[i].type) {
                log_line("FOLLOW", "!! the map sync cannot apply: node %u is '%s' here but '%s' on the host -- the structures differ",
                         i, node_type_name(node_type(node)), node_type_name(ss.node[i].type));
                return true;
            }
        }
        uint32_t seeds_changed = 0, flags_changed = 0, shown = 0;
        for (uint32_t i = 0; i < count; ++i) {
            void* node = node_at(map, i);
            if (!node) continue;
            uint64_t mine[4] = {};
            if (mem_read((const uint8_t*)node + kNode_Seed, mine, sizeof(mine)) && memcmp(mine, ss.node[i].seed, sizeof(mine)) != 0 &&
                mem_write((uint8_t*)node + kNode_Seed, ss.node[i].seed, sizeof(ss.node[i].seed))) ++seeds_changed;
            uint32_t fl = 0;
            if (read_flags(node, fl) && fl != ss.node[i].flags) {
                if (shown < 8) { ++shown; log_line("FOLLOW", "map sync: node %u flag dword (+0x%X: consumed, +0x169, +0x16A, +0x16B) here %08x, host %08x", i, (unsigned)kNodeFlagsDword, fl, ss.node[i].flags); }
                if (mem_write((uint8_t*)node + kNodeFlagsDword, &ss.node[i].flags, sizeof(ss.node[i].flags))) ++flags_changed;
            }
        }
        log_line_lvl(LogLevel::Warn, "FOLLOW", "ADOPTED the host's map: %u of %u node seed(s) and %u node flag word(s) were overwritten "
                                              "(same node count and types) -- the maps now agree", seeds_changed, count, flags_changed);
        align_party_marker(map, ss.party_at);
        if (flags_changed) refresh_node_states(map, count, "the host's node flags were taken");
        return true;
    }
    // The host's seeds have not (all) arrived: wait for them, a few seconds at most.
    if (ss.waiting_map != map) { ss.waiting_map = map; ss.waiting_since = now; }
    if (now - ss.waiting_since < kSeedWaitMs) return false;
    log_line_lvl(LogLevel::Warn, "FOLLOW", "the host's map did not arrive in %llu ms -- reporting this map as it is", (unsigned long long)kSeedWaitMs);
    room_sync_trouble("the host's map");
    ss.applied_map = map; ss.applied_epoch = ss.epoch;   // (the epoch too: an incomplete publication must not hold every later tick)
    return true;
}

} // namespace

void* follow_map_update(void* map_screen) {
    if (!g.faulted) unlocks_on_map(map_ready(map_screen));   // a battle / event node is over once the map is ready again
    if (g.faulted || !map_ready(map_screen)) {
        g.map_tick = 0;
        g.map_ready_since = 0;
        if (g.on && pending_any() && !g.transition_logged) {
            g.transition_logged = true;
            log_line("FOLLOW", "holding node %u -- previous node is still transitioning", pending_front().index);
        }
        return nullptr;
    }
#if defined(MGMP_WITH_SETUP)
    if (g.on && !setup_runtime_ready()) {
        setup_on_map(follow_resume_checkpoint(map_screen));
        if (!setup_runtime_ready()) return nullptr;
    }
#endif
    if (g.transition_logged) {
        log_line("FOLLOW", "map transition finished -- queued node may proceed");
        g.transition_logged = false;
    }
    // THE RUN EDIT HAPPENS ABOVE THE SESSION GATE, and that placement is the
    // whole point -- this is the third time this lesson has been paid for.
    //
    // It was originally called further down, with the rest of this function's
    // work, where `if (!g.on ...) return` comes first. `g.on` means "this process
    // has a peer role", so in a SINGLE-PLAYER run the map tick returned
    // immediately, roster_tick was never called, and the log said nothing at all:
    // measured 2026-09-22, a map arrival with `turn control ... reachable` on the
    // line above it and no edit and no refusal after it. The probe hit the same
    // trap twice (see its hand-off in mgmp_hooks.cpp) -- it needs a POINTER, not a
    // session. So does this.
    //
    // Being here is also what makes the map tick the safe point: this function IS
    // MapScreen::update, which runs only while the map is up, i.e. between nodes
    // and never during a battle.
    roster_tick();
    catsync_leave_tick();                  // after a room was left: the other players' cats go, here between nodes (no session needed)
    if (void* stranded = follow_solo_stranded(map_screen)) return stranded;     // 0.1.126: a node the party stands on and never entered, in a run played alone
    follow_unstick_marker(map_screen);                                          // 0.1.141: a party left a node behind (solo, or the host)

    if (g.on) roster_party_swap_on_map();
    roster_party_tick();

    if (!g.on || !map_screen) return nullptr;

    Guard guard;
    g.map = map_screen;

    // The map is ticking, so the run is BETWEEN nodes. This is the stamp the
    // save flush reads -- see follow_on_map.
    // 0.1.237: "up" restarts when the map's updates stopped for a while (a screen over it -- the level-up after a chapter boss): crash 0x9BDA, LAPTOP-L6424L1P
    // 11-16 21:26, the special_event right after the level-up was entered at once (the map counted as up since before it) with the marker still on node 1
    {
        const uint64_t now_t = GetTickCount64();
        if (!g.map_ready_since || (g.map_tick && now_t - g.map_tick > 250)) g.map_ready_since = now_t;
        g.map_tick = now_t;
    }

    // ★ AND THE HELD CATS LAND HERE, WHETHER OR NOT A NODE BATCH IS PENDING (2026-09-23).
    //
    // This tick was gated on `g.incoming.empty()`, and that gate is what kept the whole point
    // of the map-side sync from happening. The client now receives a cat push and folds it into
    // the node batch (the batch is what makes the node's completeness check pass, so it has to)
    // -- and because the batch is then non-empty for the whole stretch between nodes, this call
    // never ran, the push stayed held inside catsync, and the cat was only ever written when the
    // node message landed. Measured twice: every apply on the client reading "(node snapshot)",
    // and NOT ONE reading "(the map tick)". That write happens in the same instant the battle is
    // built, so a cat whose bytes changed there leaves the game holding a second ability object
    // for it -- the duplicated APPLY pairs and the doubled scene behaviour the user sees.
    //
    // The drain is separate from the publish on purpose: a held cat is one this peer knows it is
    // behind on, and applying it is exactly right, while PUBLISHING is not (the publish has its
    // own guard for held cats). The node snapshot copy is not an unapplied cat:
    // it must not prevent this peer from publishing its own edits below.
    catsync_apply_pending("the map tick (a node batch may be pending; the cats do not wait for it)");

    // ...and the same fact is what makes this a place to PUBLISH from. The client
    // follows the host into nodes, so its own edits would otherwise only leave at a
    // node the host has already entered -- too late for that node's battle. Here it
    // is between nodes, which is in time. No-op on the host, and throttled inside;
    // see catsync_map_tick.
    // An established client retains every host CATDATA in incoming until the
    // next ENTERNODE. Treating that retained copy as a publishing gate starves
    // the client's upgrades indefinitely: host upgrades first, client upgrades,
    // then host builds the next battle without the client's new abilities.
    // Drain above, then publish our edits while still between nodes. Preserve
    // the fresh-join gate (creation belongs to its first snapshot) and the real
    // ENTERNODE boundary (pending_any), including delayed node entry.
    bool established = g.have_here;
#if defined(MGMP_WITH_SETUP)
    // SETUP has already exchanged all eight cats. A resume's first queued host
    // CATDATA must not suppress local edits until after the first battle.
    established = established || setup_has_shared_roster();
#endif
    if (!pending_any() && (g.incoming.empty() || established)) catsync_map_tick();

#if defined(MGMP_WITH_CHECKPOINT)
    if (checkpoint_needs_map()) {
        if (!map_seed_sync(map_screen)) return nullptr;   // a client waits (seconds at most) for the host's node seeds
        checkpoint_on_map(follow_resume_checkpoint(map_screen));
    }
    if (!checkpoint_can_enter()) return nullptr;
#endif

    // Re-arm a destination persisted as the marker's current unconsumed node.
    // This is the recovery case where the party is already drawn on a boss (or
    // another node), so clicking that same node again never reaches EnterNode.
    if (!g.is_client && !g.host_retry_pending) {
        uint32_t selected = 0;
        if (follow_arrived_unentered(map_screen, selected)) {
            log_line("FOLLOW", "recovering arrived-but-unentered host node %u (%s), seed=%016llX",
                     selected, node_type_name(node_type(node_at(map_screen, selected))),
                     (unsigned long long)node_seed0(node_at(map_screen, selected)));
            queue_host_retry(map_screen, true, selected, "arrived node was not consumed");
        }
    }

    // If the host clicked while leaving a shop/event, EnterNode may have been
    // called during the native transition and returned before publishing.  The
    // next ready map tick is the safe equivalent of asking the player to click
    // again.  Resolve the node from the current vector so a scene rebuild can
    // never reuse a stale MapNode pointer.
    if (!g.is_client && net_active() && g.host_retry_pending && g.host_retry_map == map_screen) {
        void* retry = node_at(map_screen, g.host_retry_index);
        if (!retry) {
            log_line_lvl(LogLevel::Warn, "FOLLOW",
                         "discarding queued host node %u: map vector changed",
                         g.host_retry_index);
            g.host_retry_pending = false;
            g.host_retry_map = nullptr;
        } else {
            bool sent = false;
            // EXPERIMENTAL own random events (mgmp_indepevt): this retry is how the host usually gets into the next node when it clicked while the event screen was
            // closing (the click was held in h_EnterNode, the party icon already stands on the node) -- it waits for the clients' events here too (0.1.65).
            if (indepevt_hold_node()) return nullptr;
            if (catsync_hold_node()) return nullptr;     // 0.1.238: the same wait as h_EnterNode's
            if (follow_on_enter_node(map_screen, retry, &sent)) {
                g.host_retry_pending = false;
                g.host_retry_map = nullptr;
                return retry;
            }
            // A second transient refusal leaves the request queued.  Permanent
            // snapshot failures still latch g.faulted and stop the run.
        }
    }

    // --- a jump asked for by the debug panel --------------------------------
    //
    // Handled before the follow path and returning early, because the two are
    // different intentions and letting a jump fall through into the pending
    // follow would enter two nodes on one tick.
    if (g.jump_pending) {
        g.jump_pending = false;
        const uint32_t index = g.jump_index;

        uint32_t count = 0; void* data = nullptr;
        if (!read_nodes(map_screen, count, data)) return nullptr;   // not ready

        void* node = node_at(map_screen, index);
        if (!node) {
            log_line("FOLLOW", "!! panel asked to enter node %u but this map has"
                               " %u node(s) -- ignored", index, count);
            return nullptr;
        }

        // Loud, unconditionally, and on both peers' behalf: this is the debug
        // panel EDITING THE RUN. Everything else in this module reacts to a
        // decision a player made by clicking the map.
        log_line("FOLLOW", "!! PANEL JUMP: entering node %u (%s) seed0=%016llx by"
                           " request from the debug panel -- this is not a move the"
                           " player made on the map",
                 index, node_type_name(node_type(node)),
                 (unsigned long long)node_seed0(node));

        // Mirror h_EnterNode exactly: publish through the ordinary path first,
        // then let the caller run the original. Calling o_EnterNode alone would
        // bypass the detour and the client would never hear about it -- the same
        // property the follow path relies on in the other direction.
        bool sent = false;
        if (!follow_on_enter_node(map_screen, node, &sent)) return nullptr;
        return node;
    }

    if (!pending_any()) return nullptr;
    State::PendingNode& p = pending_front();

    // --- the deliberate late-join delay (test knob) -------------------------
    //
    // Holding the node here rather than dropping it is what makes this a test
    // of the join barrier and not just a broken client: the host walks into the
    // battle alone, this peer arrives net_follow_delay_ms later, and the barrier
    // is what has to have kept the host from playing turns in the meantime.
    // Everything downstream -- the seed check, the type check -- still runs when
    // the hold expires, so the delay changes WHEN we follow and nothing else.
    if (const uint32_t delay = config().net_follow_delay_ms) {
        const uint64_t waited = GetTickCount64() - p.at;
        if (waited < delay) {
            if (!p.held) {
                p.held = true;
                log_line("FOLLOW", "!! net_follow_delay_ms = %u -- deliberately holding"
                                   " node %u for %u ms before following. This is a TEST"
                                   " setting: it manufactures the late-join gap.",
                         delay, p.index, delay);
            }
            return nullptr;
        }
        if (p.held)
            log_line("FOLLOW", "delay elapsed (%llu ms) -- following now",
                     (unsigned long long)waited);
    }

    uint32_t count = 0; void* data = nullptr;
    if (!read_nodes(map_screen, count, data)) return nullptr;   // not ready yet

    // Same map, or the index means nothing. Both peers generate the map from
    // the same save, so a differing count is a real problem and not a race.
    if (p.count && count != p.count) {
        log_line("FOLLOW", "!! host's map has %u nodes, ours has %u -- not"
                           " following; the two runs are not the same run",
                 p.count, count);
        const uint64_t lost = p.seed;
        pending_pop();
        choice_on_node_skipped(lost);
        return nullptr;
    }

    void* node = node_at(map_screen, p.index);
    if (!node) {
        log_line("FOLLOW", "!! node %u is out of range on our map (%u nodes)",
                 p.index, count);
        const uint64_t lost = p.seed;
        pending_pop();
        choice_on_node_skipped(lost);
        return nullptr;
    }

    // Identity check, and it is the strong one: the node's stored seed is the
    // 32 bytes EnterNode is about to load into the simulation stream, so if it
    // differs the battle would start from a different RNG state and desync on
    // its first roll. Catching it here names the cause; catching it at the
    // first turn hash would only say "they disagree".
    uint64_t seed = node_seed0(node);
    if (p.seed && seed != p.seed) {
        // ADOPT OR REFUSE, and the default is refuse -- see tune::kAdoptHostSeed
        // for why. Two saves mean two maps, and two maps mean this index carries
        // different xoshiro256 state: enter with ours and the battle desyncs on
        // its first roll, which is why this check exists at all. Adoption writes
        // the host's 32 bytes into the node BEFORE o_EnterNode copies them into
        // TLS+0x178, so the game's own path then does the rest.
        const bool can_adopt = tune::kAdoptHostSeed && p.words[0] == p.seed && p.words[1];
        bool adopted = false;
        if (can_adopt)
            adopted = mem_write((uint8_t*)node + kNode_Seed, p.words, sizeof(p.words));

        if (!adopted) {
            log_line("FOLLOW", "!! node %u seed is %016llx here but %016llx on the"
                               " host -- refusing to enter; the maps differ%s",
                     p.index, (unsigned long long)seed, (unsigned long long)p.seed,
                     can_adopt ? " (and the host's seed would not write)" : "");
            const uint64_t lost = p.seed;
            pending_pop();
            choice_on_node_skipped(lost);
            return nullptr;
        }

        log_line_lvl(LogLevel::Warn, "FOLLOW",
                     "!! ADOPTED the host's node seed at node %u: ours was %016llx, the"
                     " host's is %016llx. This peer's map is not the host's, so its RNG"
                     " state has been overwritten to keep the battle deterministic --"
                     " sound only while the map's STRUCTURE agrees (count, node types).",
                     p.index, (unsigned long long)seed, (unsigned long long)p.seed);
        seed = p.seed;
    } else if (p.seed && p.words[1]) {
        // Word 0 agrees, which is the case identical saves always produce. The
        // other three are then checked too -- not to decide anything, but because
        // a difference here means one of the two reads is wrong, and the battle
        // would be rolling the host's stream only by accident.
        uint8_t mine[32] = {};
        if (mem_read((const uint8_t*)node + kNode_Seed, mine, sizeof(mine)) &&
            memcmp(mine, p.words, sizeof(mine)) != 0)
            log_line("FOLLOW", "!! node %u agrees on seed word 0 (%016llx) but its other"
                               " three words differ from the host's -- one of the two"
                               " reads is wrong",
                     p.index, (unsigned long long)seed);
    }

    uint32_t type = node_type(node);
    if (p.type != type)
        log_line("FOLLOW", "!! node %u is '%s' here but '%s' on the host --"
                           " entering anyway, the seed matched",
                 p.index, node_type_name(type), node_type_name(p.type));

    // A SPECIAL EVENT IS NOT ENTERED IN THE MAP'S FIRST MOMENTS (0.1.231). Crash Mewgenics.exe+0x9BDA4B/0x9BDABF/0x9BDBA0 -- the game's own frame draw (0x9B7F1A -> the batch
    // flush 0x140A48350 -> a freed shader), no mod frame on the stack -- in 6 of 313 client entries into a special_event over 11-13..15, and in 5 of the 6 the client had just
    // come back from the battle screen (the battle's summary on the line before, the party icon still on node 1 of a map screen just made) and entered the event in that
    // map's first frame or two. Battles, entered the same way thousands of times, do not crash. The entry waits until the map has been up for kSpecialSettleMs: a delay of
    // well under a second, never a lost entry.
    if (g.is_client && type == 11 && g.map_ready_since) {
        constexpr uint64_t kSpecialSettleMs = 600;
        const uint64_t up = GetTickCount64() - g.map_ready_since;
        if (up < kSpecialSettleMs) {
            if (g.settle_said != p.index) { g.settle_said = p.index; log_line("FOLLOW", "node %u (special_event): the map has been up %llu ms -- the entry waits until it has been up %llu ms (crash 0x9BDA)", p.index, (unsigned long long)up, (unsigned long long)kSpecialSettleMs); }
            return nullptr;
        }
    }

    // AN INSTANT NODE IS ENTERED WHEN THE PARTY HAS ARRIVED, as the host's is (0.1.120; see kNodeType_FoodBox). Until then the entry stays queued: the marker is sent to the node (the game walks
    // it), and the node's consumed byte is set so that the arrival does not call EnterNode on its own. Whatever the marker does not read as (no marker, no arrival within the time), the old
    // behaviour -- enter at once -- is what happens, so this can only delay, never lose, an entry.
    if (g.is_client && (type == kNodeType_FurnitureBox || type == kNodeType_FoodBox)) {
        void *marker = nullptr, *at = nullptr, *moving = nullptr, *sel = nullptr;
        const bool read = follow_marker(map_screen, marker, at, moving, sel);
        const bool arrived = read && at == node && !moving;
        if (read && !arrived) {
            const uint64_t now = GetTickCount64();
            if (!p.walking) {
                p.walking = true; p.walk_at = now;
                uint8_t consumed = 1;
                if (!follow_node_consumed(node) && mem_write((uint8_t*)node + kNode_Consumed, &consumed, 1)) p.walk_marked = true;
                follow_move_marker_to(node);
                log_line("FOLLOW", "node %u (%s) is an instant node: the party walks there first and the node is entered when it has arrived (the game finishes it for the node the party stands on)",
                         p.index, node_type_name(type));
            }
            if (now - p.walk_at < s_instant_walk_ms) return nullptr;
            log_line_lvl(LogLevel::Warn, "FOLLOW", "!! node %u (%s): the party has not arrived after %llu ms -- entering it now", p.index, node_type_name(type), (unsigned long long)(now - p.walk_at));
        }
        if (p.walking) {
            if (p.walk_marked) { const uint8_t zero = 0; mem_write((uint8_t*)node + kNode_Consumed, &zero, 1); p.walk_marked = false; }   // EnterNode runs as it always did: the finish consumes the node
            if (arrived) log_line("FOLLOW", "node %u (%s): the party has arrived after %llu ms -- entering it", p.index, node_type_name(type), (unsigned long long)(GetTickCount64() - p.walk_at));
            p.walking = false;
        }
    }

    const uint32_t entering = p.index;

    // Battle identity, established without negotiating anything: this is the
    // same node the host entered, so `seed` is the same 64 bits it read. The
    // host does the matching call in follow_on_enter_node. Note this is the
    // CLIENT's only chance to make it -- the follow path calls o_EnterNode, the
    // MinHook trampoline, which bypasses the h_EnterNode detour entirely.
    auto& snapshot = p.snapshot;
    bool applied = true;
    // THIS LOOP USED TO REQUIRE EVERY FAMILIAR TO ALSO APPEAR AMONG THE SNAPSHOT'S
    // CATS, AND THAT CANNOT HOLD (2026-09-22, measured). Familiars are the cats
    // appended at kDir_CatFamiliars once the party is FULL -- extra cats, NOT members
    // of the run's party list -- while the snapshot's cats are exactly the party
    // (`catsync_publish` walks run_cats). So the moment a run gained its first
    // familiar the check failed, the snapshot was declared incomplete, `g.faulted`
    // latched and the client stopped following: the host entered an event and a
    // battle, and the client stayed on the map before the battle node.
    //
    // Its job -- "this peer can resolve those familiars" -- is done by
    // catsync_apply_familiars below, which reads the live list and compares it, and
    // which no longer treats "cannot act" as failure either. Nothing is lost by
    // dropping it, and a check that is wrong by construction is worse than none:
    // this one had been failing since familiars existed and nobody had seen it
    // because no earlier run in this session had one.
    if (!roster_party_swap_release()) {
        g.faulted = true;
        log_line_lvl(LogLevel::Error, "FOLLOW", "!! shared roster restoration failed -- node %u NOT entered", entering);
        return nullptr;
    }

    // AND SAY WHAT THE SNAPSHOT CARRIED, AND TRY EVERY CAT IN IT (2026-09-23).
    //
    // Measured: the host published its OWN four cats (twice, once forced and full at its node
    // entry) and even applied the client's four -- while the client's log showed four "kept my
    // own cat ..." lines, NOT ONE apply of the host's, and a publish line that kept saying "no
    // baseline yet: the host has not sent this peer that cat". Two readings, and the loop below
    // used to hide which one it was:
    //
    //     for (uint32_t i = 0; applied && i < snapshot.cat_count; ++i)
    //
    // A refusal to revert one of THIS peer's own cats is an ORDINARY result of that loop, and if
    // it returns false then every cat after it was never even attempted -- silently, with the
    // snapshot still reported as applied. So: the ids the message carried are printed, each cat's
    // result is reported, and one refusal no longer stops the rest.
    {
        char ids[320] = {};
        int  off = 0;
        for (uint32_t i = 0; i < snapshot.cat_count && off < (int)sizeof(ids) - 24; ++i)
            off += _snprintf_s(ids + off, sizeof(ids) - off, _TRUNCATE, " %016llX",
                               (unsigned long long)snapshot.cats[i].id);
        char fam[200] = {};
        off = 0;
        for (uint32_t j = 0; j < p.message.familiar_count && off < (int)sizeof(fam) - 24; ++j)
            off += _snprintf_s(fam + off, sizeof(fam) - off, _TRUNCATE, " %016llX",
                               (unsigned long long)p.message.familiar_ids[j]);
        log_line_lvl(LogLevel::Warn, "FOLLOW",
                     "!! node %u snapshot contents: %u cat(s) [%s ] and %u familiar id(s) [%s ]",
                     entering, snapshot.cat_count, ids, p.message.familiar_count, fam);
    }

    for (uint32_t i = 0; i < snapshot.cat_count; ++i) {
        bool familiar = false;
        for (uint32_t j = 0; j < p.message.familiar_count; ++j)
            if (p.message.familiar_ids[j] == snapshot.cats[i].id) familiar = true;
        const bool ok = catsync_apply_snapshot(snapshot.cats[i], "node snapshot", familiar);
        if (!ok)
            log_line_lvl(LogLevel::Warn, "FOLLOW",
                         "!! node %u snapshot: cat %016llX did not take -- carrying on with the"
                         " rest (this line used to END the loop, and that is how the host's cats"
                         " could go unapplied with nothing in the log to say so)",
                         entering, (unsigned long long)snapshot.cats[i].id);
        applied = applied && ok;
    }
    applied = applied && catsync_apply_familiars(p.message);
    // Membership comes from the host too, not only the cats' bytes: an event on the
    // host (CatHole -> leave_party_temporarily) can take a cat out of its run, and
    // the client's restored roster would otherwise still field it.
    if (applied) {
        uint64_t ids[32] = {};
        uint32_t n = 0;
        // THE HOST'S OWN PARTY ORDER when the message carries it (proto 44). The snapshot's
        // cat list is first-arrival order and is only the fallback: adopting it put the party
        // in a different ORDER from the host's, the node hash differed on order alone, and the
        // checkpoint barrier then refused every later node (2026-09-29 19:13).
        if (p.message.party_count) {
            for (uint32_t i = 0; i < p.message.party_count && n < 32; ++i) ids[n++] = p.message.party_ids[i];
            bool differs = n != snapshot.cat_count;
            for (uint32_t i = 0; !differs && i < n; ++i) differs = ids[i] != snapshot.cats[i].id;
            if (differs)
                log_line("FOLLOW", "node %u: the host's party order comes from the message (%u cat(s)),"
                                   " not from the snapshot's arrival order (%u cat(s))",
                         entering, p.message.party_count, snapshot.cat_count);
        } else {
            for (uint32_t i = 0; i < snapshot.cat_count && n < 32; ++i) ids[n++] = snapshot.cats[i].id;
        }
        applied = roster_adopt_host_shared(ids, n, p.message.familiar_ids, p.message.familiar_count,
                                           "node snapshot");
    }
    // Gated on the flags, not on the message's `have_*`: with the module ON, a
    // snapshot that should have carried this half and did not is exactly the fault
    // the check below exists to report. See the completeness check in follow_on_message.
    applied = applied && (!tune::kInvSync || invsync_apply_snapshot(snapshot.inventory, "node snapshot"));
    applied = applied && (!tune::kRunHist || runhist_apply_snapshot(snapshot.history, "node snapshot"));
    if (!applied) {
        g.faulted = true;
        log_line_lvl(LogLevel::Error, "FOLLOW", "!! node %u snapshot incomplete -- node NOT entered; run state needs inspection", entering);
        return nullptr;
    }
    log_line("FOLLOW", "node %u snapshot applied: %u cats, inventory %016llx, history %u bytes",
             entering, snapshot.cat_count, (unsigned long long)snapshot.inventory.hash, snapshot.history.size);
    pending_pop();
    ++g.entered;
    lockstep_enter_battle(seed);
    remember_node(entering, count, type, seed);
    if (type >= 5 && type <= 11) { char why[48]; _snprintf_s(why, sizeof(why), _TRUNCATE, "node %u (%s)", entering, node_type_name(type)); unlocks_window_open(why, type >= 9); }

    // A shift-held EnterNode takes a completely different branch (the very
    // first thing it does is SDL_GetScancodeFromKey(SDLK_LSHIFT) and, if down,
    // tail-calls sub_1403923F0 instead). That is local keyboard state, so the
    // client would diverge from the host purely by holding a key. Cheap to
    // notice, so say so rather than let it be mysterious.
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
        log_line("FOLLOW", "!! LEFT SHIFT is down -- EnterNode branches on it and"
                           " this peer will not take the host's path");

    return node;
}

} // namespace mgmp
