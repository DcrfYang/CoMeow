#include "mgmp_follow.h"
#include "mgmp_catsync.h"
#include "mgmp_invsync.h"
#include "mgmp_runhist.h"
#include "mgmp_choice.h"
#include "mgmp_nodehash.h"
#include "mgmp_roster.h"
#include "mgmp_lockstep.h"
#include "mgmp_config.h"
#include "mgmp_log.h"
#include "mgmp_net.h"
#include "mgmp_tuning.h"
#include <cstdio>
#include <cstring>
#include <vector>

using namespace mgmp;
namespace {
Config cfg;
NetRole role = NetRole::Client;
bool apply_ok = true, send_ok = true, roster_ok = true;
uint64_t battle = 0, remembered = 0;
unsigned checks = 0, failures = 0, publishes = 0, map_publishes = 0, hashes = 0;
unsigned sends = 0, last_peer = 0, roster_releases = 0, roster_entries = 0, roster_type = 0, roster_maps = 0;
unsigned drained = 0;
bool setup_ready = true, shared_setup = false, checkpoint_ready = true, connected = true;
uint64_t resume_checkpoint = 0;
std::vector<unsigned> sync_order;
// THE LAST ENTERNODE SENT, and it has to live HERE.
//
// It was written inside the `namespace mgmp` stub block near the bottom of the file,
// which compiled as a member of mgmp and left the case that reads it -- a few hundred
// lines ABOVE, in this anonymous namespace -- with an undeclared identifier. Moving it
// up is the whole fix; the note stays because the file's shape invites the mistake: the
// stubs are last, the cases are first, and a variable at the bottom LOOKS like it is
// visible everywhere.
EnterNodeMsg last_msg{};
std::vector<unsigned> cat_values, coins, histories;
#define CHECK(x, ...) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
template<class T> void put(void* p, size_t off, T value) { std::memcpy((char*)p + off, &value, sizeof(value)); }
struct Fixture {
    unsigned char map[0x240]{};
    unsigned char marker[0x90]{};
    unsigned char nodes[12][0x180]{};
    void* pointers[12]{};
    Fixture() {
        checkpoint_ready = connected = true;
        cfg.net_follow = true;
        cfg.net_follow_delay_ms = 0;
        role = NetRole::Client;
        apply_ok = send_ok = roster_ok = true;
        battle = remembered = 0;
        publishes = map_publishes = hashes = 0;
        drained = 0; sync_order.clear(); setup_ready = true; shared_setup = false; resume_checkpoint = 0;
        roster_releases = roster_entries = roster_type = roster_maps = 0;
        cat_values.clear(); coins.clear(); histories.clear();
        for (unsigned i = 0; i < 12; ++i) {
            pointers[i] = nodes[i];
            put(nodes[i], 0x118, uint64_t(100 + i));
            put(nodes[i], 0x138, uint32_t(9));
        }
        put(map, 124, uint32_t(12));
        put(map, 128, pointers + 0);
        follow_init();
        // follow_init deliberately does NOT reset the run -- that is what makes a
        // reconnect safe -- so each case asks for a fresh RUN explicitly, which is
        // what a save load does in the game. See the header note on follow_reset_run.
        follow_reset_run();
    }
    ~Fixture() { follow_shutdown(); }
    EnterNodeMsg node(unsigned i) {
        EnterNodeMsg m{};
        m.index = i; m.node_count = 12; m.type = 9; m.seed0 = m.seed[0] = 100 + i;
        return m;
    }
    void cat(unsigned value, uint8_t from = 0) {
        NetMsg m{}; m.type = MSG_CATDATA; m.from = from;
        m.catdata.id = 7; m.catdata.size = 1;
        m.catdata.data = (uint8_t*)std::malloc(1); m.catdata.data[0] = (uint8_t)value;
        CHECK(follow_hold_state(m));
        if (from == 0) CHECK(m.catdata.data == nullptr);
        std::free(m.catdata.data);
    }
    void snapshot(unsigned value) {
        cat(value);
        NetMsg inv{}; inv.type = MSG_INVENTORY; inv.from = 0;
        inv.inventory.coins = value; inv.inventory.hash = value;
        CHECK(follow_hold_state(inv));
        NetMsg hist{}; hist.type = MSG_RUNHIST; hist.from = 0;
        hist.runhist.size = value; hist.runhist.data = (uint8_t*)std::calloc(value, 1);
        CHECK(follow_hold_state(hist));
        CHECK(hist.runhist.data == nullptr);
    }
};
void queued_snapshots() {
    Fixture f;
    f.snapshot(11); follow_on_message(f.node(1));
    f.snapshot(22); follow_on_message(f.node(2));
    CHECK(cat_values.empty());
    CHECK(follow_map_update(f.map) == f.nodes[1]);
    CHECK(cat_values.size() == 1 && cat_values[0] == 11);
    CHECK(tune::kInvSync ? coins.size() == 1 && coins[0] == 11 : coins.empty());
    CHECK(tune::kRunHist ? histories.size() == 1 && histories[0] == 11 : histories.empty());
    CHECK(battle == 101 && remembered == 101);
    CHECK(hashes == 0);
    follow_after_enter_node(); CHECK(hashes == 1);
    f.map[0x1e8] = 1;
    CHECK(follow_map_update(f.map) == nullptr);
    CHECK(!follow_on_map());
    CHECK(cat_values.size() == 1);
    f.map[0x1e8] = 0;
    CHECK(follow_map_update(f.map) == f.nodes[2]);
    CHECK(cat_values.size() == 2 && cat_values[1] == 22);
    CHECK(tune::kInvSync ? coins.size() == 2 && coins[1] == 22 : coins.empty());
    CHECK(tune::kRunHist ? histories.size() == 2 && histories[1] == 22 : histories.empty());
    CHECK(battle == 102 && remembered == 102);
}
void coalesce_only_within_batch() {
    Fixture f;
    f.snapshot(1); f.cat(2); follow_on_message(f.node(1));
    f.snapshot(3); follow_on_message(f.node(2));
    CHECK(follow_map_update(f.map) == f.nodes[1]);
    CHECK(cat_values.size() == 1 && cat_values[0] == 2);
    CHECK(follow_map_update(f.map) == f.nodes[2]);
    CHECK(cat_values.size() == 2 && cat_values[1] == 3);
}
void gates_and_failures() {
    {
        Fixture f; f.snapshot(1); follow_on_message(f.node(1));
        f.map[0x1e4] = 1;
        CHECK(follow_map_update(f.map) == nullptr);
        CHECK(cat_values.empty());
        f.map[0x1e4] = 0; apply_ok = false;
        CHECK(follow_map_update(f.map) == nullptr);
        CHECK(battle == 0 && remembered == 0);
        apply_ok = true;
        CHECK(follow_map_update(f.map) == nullptr);
    }
    {
        Fixture f; follow_on_message(f.node(1));
        CHECK(follow_map_update(f.map) == nullptr);
        CHECK(cat_values.empty());
    }
    {
        Fixture f; f.snapshot(1);
        auto m = f.node(1); m.familiar_count = 1; m.familiar_ids[0] = 99;
        follow_on_message(m);
        CHECK(follow_map_update(f.map) == nullptr);
        CHECK(battle == 0 && remembered == 0);
    }
}
void reset_and_untrusted_sender() {
    Fixture f; f.snapshot(1); follow_on_message(f.node(1));
    follow_reset_run();
    CHECK(follow_map_update(f.map) == nullptr);
    f.cat(9, 2);
    f.snapshot(2); follow_on_message(f.node(1));
    CHECK(follow_map_update(f.map) == f.nodes[1]);
    CHECK(cat_values.size() == 1 && cat_values[0] == 2);
    f.snapshot(3); follow_on_message(f.node(1));
    CHECK(follow_map_update(f.map) == nullptr);
    CHECK(cat_values.size() == 1);
}
void overflow_and_host_failure() {
    {
        Fixture f;
        for (unsigned i = 1; i <= 9; ++i) { f.snapshot(i); follow_on_message(f.node(i)); }
        CHECK(follow_map_update(f.map) == nullptr);
        CHECK(cat_values.empty());
    }
    {
        Fixture f; follow_shutdown(); role = NetRole::Host; follow_init();
        f.map[0x1e8] = 1;
        CHECK(!follow_on_enter_node(f.map, f.nodes[1], nullptr));
        CHECK(publishes == 0);
        f.map[0x1e8] = 0; send_ok = false;
        CHECK(!follow_on_enter_node(f.map, f.nodes[1], nullptr));
        CHECK(battle == 0 && remembered == 0);
    }
}
// A RECONNECT RE-RUNS follow_init AND MUST NOT LOSE THE RUN.
//
// The handshake runs again on a reconnect -- session_shutdown -> begin ->
// WELCOME -> go_ready -- while the run was never touched, so an init that reset
// the run state threw away `have_here`. The host's catch-up then repeats the
// node the run is STANDING IN, this peer fails to recognise it ("seed == here"
// is the recognition), queues it as a new node and walks back into a node it has
// already finished. A second EnterNode is a second battle, and the two runs are
// no longer the same run.
void reconnect_keeps_the_run() {
    Fixture f; follow_shutdown(); role = NetRole::Host; follow_init();
    CHECK(follow_on_enter_node(f.map, f.nodes[3], nullptr));
    uint32_t here = 999;
    CHECK(follow_current_node(here) && here == 3);

    follow_shutdown();                       // the socket goes...
    follow_init();                           // ...and the handshake runs again
    here = 999;
    CHECK(follow_current_node(here) && here == 3);

    // ...while a NEW RUN still clears it, which is the other half of the rule and
    // what makes keeping it across a reconnect safe rather than a leak.
    follow_reset_run();
    CHECK(!follow_current_node(here));
}
// THE CATCH-UP REPEATS A NODE THIS PEER MAY ALREADY HAVE QUEUED.
//
// It is sent whenever a peer joins, and the node it names is the one the run is
// standing in -- which, for a peer that is behind, is a node already waiting in
// the queue. Queued twice, the peer enters it twice: out of one snapshot and
// back in through another taken at a different moment.
void duplicate_node_is_refused() {
    Fixture f;
    f.snapshot(1); follow_on_message(f.node(1));
    f.snapshot(2); f.cat(3); follow_on_message(f.node(1));   // the catch-up repeat
    CHECK(follow_map_update(f.map) == f.nodes[1]);
    CHECK(follow_map_update(f.map) == nullptr);              // nothing else queued
    // The batch that applies is the FIRST one: one entry per node, and the repeat's
    // state -- which is not that node's -- is discarded with it.
    CHECK(cat_values.size() == 1 && cat_values[0] == 1);
    CHECK(tune::kInvSync ? coins.size() == 1 && coins[0] == 1 : coins.empty());
}
// The catch-up itself: to that peer, and only once there is a node to name.
void catchup_names_the_current_node() {
    Fixture f; follow_shutdown(); role = NetRole::Host; follow_init();
    // The node's other seed words, which the joiner may need to ADOPT this node:
    // it may never have visited it, so this message is the only carrier.
    put(f.nodes[5], 0x120, uint64_t(0x1122334455667788ull));
    sends = 0;
    follow_catchup(2);
    CHECK(sends == 0);                       // nothing entered yet: nothing to say
    CHECK(follow_on_enter_node(f.map, f.nodes[5], nullptr));
    follow_catchup(2);
    CHECK(sends == 1 && last_peer == 2);
    CHECK(last_msg.index == 5 && last_msg.seed0 == 105);
    CHECK(last_msg.seed[0] == 105, "word 0 mirrors seed0");
    CHECK(last_msg.seed[1] == 0x1122334455667788ull, "the other words travel too");
    send_ok = false;
    follow_catchup(2);                       // a send that fails is reported, not queued
    CHECK(sends == 2);
    send_ok = true;
}
void roster_node_ordering() {
    Fixture f;
    f.snapshot(1); follow_on_message(f.node(1));
    CHECK(roster_releases == 0);
    f.map[0x1e8] = 1;
    CHECK(follow_map_update(f.map) == nullptr && roster_releases == 0);
    f.map[0x1e8] = 0;
    CHECK(follow_map_update(f.map) == f.nodes[1]);
    CHECK(roster_releases == 1 && roster_entries == 0);
    follow_after_enter_node();
    CHECK(roster_entries == 1 && roster_type == 9 && hashes == 1);
    f.snapshot(2); follow_on_message(f.node(2));
    CHECK(roster_releases == 1);
    const unsigned maps = roster_maps;
    cfg.net_follow_delay_ms = 100000;
    CHECK(follow_map_update(f.map) == nullptr);
    CHECK(roster_maps == maps + 1 && roster_releases == 1);
    cfg.net_follow_delay_ms = 0;
    roster_ok = false;
    CHECK(follow_map_update(f.map) == nullptr);
    CHECK(cat_values.size() == 1 && battle == 101);
}
void peer_updates_do_not_starve_local_publish() {
    Fixture f;
    f.snapshot(1); follow_on_message(f.node(0));
    CHECK(follow_map_update(f.map) == f.nodes[0]);
    follow_after_enter_node();
    map_publishes = drained = 0; sync_order.clear(); cat_values.clear();
    // Host upgrades first. Its CATDATA is retained for the next node, but
    // client upgrades must still be published while waiting on the map.
    f.cat(11);
    for (unsigned i = 0; i < 12; ++i) {
        CHECK(follow_map_update(f.map) == nullptr);
        CHECK(map_publishes == i + 1);
        CHECK(drained == i + 1);
    }
    CHECK(sync_order.size() == 24);
    if (sync_order.size() == 24)
        for (unsigned i = 0; i < 12; ++i)
            CHECK(sync_order[i * 2] == 1 && sync_order[i * 2 + 1] == 2);
    // A second upgrade coalesces only the host snapshot, not our outbound tick.
    f.cat(22);
    CHECK(follow_map_update(f.map) == nullptr && map_publishes == 13);
    // Once ENTERNODE arrives, preserve its batch and stop publishing across
    // that boundary, even while the follower's artificial delay is active.
    follow_on_message(f.node(1));
    cfg.net_follow_delay_ms = 100000;
    CHECK(follow_map_update(f.map) == nullptr && map_publishes == 13);
    cfg.net_follow_delay_ms = 0;
    CHECK(follow_map_update(f.map) == f.nodes[1]);
    CHECK(map_publishes == 13 && cat_values.size() == 1 && cat_values[0] == 22);
}
void first_join_still_waits_for_snapshot() {
    Fixture f;
    f.cat(11);
    CHECK(follow_map_update(f.map) == nullptr);
    CHECK(map_publishes == 0);
    follow_on_message(f.node(0));
    CHECK(follow_map_update(f.map) == f.nodes[0]);
    CHECK(cat_values.size() == 1 && cat_values[0] == 11);
    CHECK(follow_map_update(f.map) == nullptr);
    CHECK(map_publishes == 1);
}
void resume_map_gate() {
    Fixture f;
    const uint64_t original = follow_resume_checkpoint(f.map);
    CHECK(original != 0 && follow_resume_checkpoint(nullptr) == 0);
    put(f.nodes[0], 0x168, uint32_t(1));
    CHECK(original != follow_resume_checkpoint(f.map));
    put(f.nodes[0], 0x168, uint32_t(0));
    put(f.nodes[3], 0x118, uint64_t(555));
    CHECK(original != follow_resume_checkpoint(f.map));
    put(f.nodes[3], 0x118, uint64_t(103));
    CHECK(original == follow_resume_checkpoint(f.map));
    setup_ready = false;
    follow_map_update(f.map);
    CHECK(resume_checkpoint == original);
    CHECK(map_publishes == 0 && drained == 0 && roster_maps == 0);
    role = NetRole::Host; follow_init();
    const unsigned before_sends = sends;
    CHECK(!follow_on_enter_node(f.map, f.nodes[0], nullptr));
    CHECK(sends == before_sends);
    setup_ready = true; follow_map_update(f.map);
    CHECK(map_publishes == 1 && roster_maps == 1);
    role = NetRole::Client; follow_init();
    shared_setup = true;
    f.snapshot(1); // peer state arrived, but no ENTERNODE yet
    follow_map_update(f.map);
    CHECK(map_publishes == 2); // our map edits still publish before resumed battle
}

void host_click_during_map_transition_is_retried() {
    Fixture f;
    follow_shutdown();
    role = NetRole::Host;
    follow_init();
    f.map[0x1e8] = 1; // shop/event exit is still transitioning
    CHECK(!follow_on_enter_node(f.map, f.nodes[3], nullptr));
    CHECK(publishes == 0);
    f.map[0x1e8] = 0;
    CHECK(follow_map_update(f.map) == f.nodes[3]);
    CHECK(publishes == 1 && battle == 103 && remembered == 103);
}

void selected_unconsumed_node_is_rearmed_after_reload() {
    Fixture f;
    follow_shutdown();
    role = NetRole::Host;
    follow_init();
    void* marker = f.marker;
    put(f.map, 0xA0, marker);
    put(f.marker, 0x50, f.nodes[3]);
    put(f.marker, 0x60, f.nodes[3]);
    CHECK(follow_map_update(f.map) == f.nodes[3]);
    CHECK(publishes == 1 && battle == 103 && remembered == 103);
}

// Native MapMarker::Update moves +58 to +50, clears +58, calls EnterNode,
// then clears +60 even when our hook held entry. A reloaded save has no
// selection either. Reproduce both states, including adjacent native flags.
void arrived_node_with_cleared_selection_is_recovered() {
    for (bool adjacent_flags : {false, true}) {
        Fixture f; role = NetRole::Host; follow_init();
        put(f.map, 0xA0, (void*)f.marker);
        put(f.marker, 0x50, f.nodes[3]);
        put(f.nodes[3], 0x138, uint32_t(8)); // boss
        f.nodes[3][0x169] = adjacent_flags;
        f.nodes[3][0x16a] = adjacent_flags;
        setup_ready = false;
        CHECK(follow_map_update(f.map) == nullptr && publishes == 0);
        setup_ready = true; checkpoint_ready = false;
        CHECK(follow_map_update(f.map) == nullptr && publishes == 0);
        checkpoint_ready = true; connected = false;
        CHECK(follow_map_update(f.map) == nullptr && publishes == 0);
        connected = true;
        CHECK(follow_map_update(f.map) == f.nodes[3]);
        CHECK(publishes == 1 && battle == 103 && remembered == 103);
        CHECK(follow_map_update(f.map) == nullptr && publishes == 1);
    }
}

void arrived_node_recovery_respects_native_state() {
    // exit(3)/home(4)/bonus(16) moved OUT of the passive list on 2026-09-28:
    // standing unconsumed on any of them means a click-arrival happened and the
    // entry never completed, so they are recovered like battles -- see
    // exit_home_bonus_nodes_are_recovered below.
    for (unsigned scenario = 0; scenario < 9; ++scenario) {
        Fixture f; role = NetRole::Host; follow_init();
        put(f.map, 0xA0, (void*)f.marker);
        put(f.marker, 0x50, f.nodes[3]);
        put(f.marker, 0x60, f.nodes[3]);
        if (scenario == 0) f.nodes[3][0x168] = 1; // already consumed
        if (scenario == 1) put(f.marker, 0x58, f.nodes[4]); // still walking
        if (scenario == 2) put(f.marker, 0x60, f.nodes[4]); // a different destination
        if (scenario == 3) put(f.marker, 0x50, f.map); // not in node vector
        if (scenario == 4) { role = NetRole::Client; follow_init(); }
        if (scenario >= 5) {
            const uint32_t passive[] = {0, 1, 2, 17}; // none, ?1, enter(spawn), empty
            put(f.nodes[3], 0x138, passive[scenario - 5]);
        }
        CHECK(follow_map_update(f.map) == nullptr && publishes == 0);
    }
}

// 2026-09-28: the home-settlement strand left the party standing on an
// unconsumed HOME node, and the reload offered no way back in -- the
// battle-only recovery gate refused it. The marker only ever arrives at nodes
// somebody clicked, so exit/home/bonus standing unconsumed are recovered
// through the same publish + o_EnterNode chain the boss fix used, once.
void exit_home_bonus_nodes_are_recovered() {
    for (uint32_t type : {3u, 4u, 16u}) {
        Fixture f; role = NetRole::Host; follow_init();
        put(f.map, 0xA0, (void*)f.marker);
        put(f.marker, 0x50, f.nodes[3]); // arrival put us here; +60 cleared
        put(f.nodes[3], 0x138, type);
        CHECK(follow_map_update(f.map) == f.nodes[3]);
        CHECK(publishes == 1 && battle == 103 && remembered == 103);
        CHECK(follow_map_update(f.map) == nullptr && publishes == 1); // once per session
    }
}

void held_host_entry_survives_native_selection_clear() {
    Fixture f; role = NetRole::Host; follow_init();
    put(f.map, 0xA0, (void*)f.marker);
    put(f.marker, 0x50, f.nodes[3]);
    checkpoint_ready = false; // client has not left its shop
    CHECK(!follow_on_enter_node(f.map, f.nodes[3], nullptr));
    CHECK(follow_map_update(f.map) == nullptr && publishes == 0);
    checkpoint_ready = true;
    CHECK(follow_map_update(f.map) == f.nodes[3]);
    CHECK(publishes == 1);
    CHECK(follow_map_update(f.map) == nullptr && publishes == 1);
}

void consumed_mark_preserves_adjacent_flags() {
    Fixture f;
    f.nodes[3][0x169] = 1; f.nodes[3][0x16a] = 1; f.nodes[3][0x16b] = 0xA5;
    follow_mark_node_consumed(f.nodes[3]);
    CHECK(f.nodes[3][0x168] == 1);
    CHECK(f.nodes[3][0x169] == 1 && f.nodes[3][0x16a] == 1 && f.nodes[3][0x16b] == 0xA5);
}

// 2026-09-28 17:48: host left shop 15, native consume (RVA 228240) also marked its
// empty neighbour 19; the client marked only 15 and the checkpoint refused.
void consumed_mark_cascades_like_native() {
    Fixture f;
    void* links3[3] = { f.nodes[4], f.nodes[5], f.nodes[6] };
    void* links4[1] = { f.nodes[7] };
    put(f.nodes[3], 0x144, uint32_t(3)); put(f.nodes[3], 0x148, (void*)links3);
    put(f.nodes[4], 0x144, uint32_t(1)); put(f.nodes[4], 0x148, (void*)links4);
    put(f.nodes[4], 0x138, uint32_t(17));                               // empty: cascades
    put(f.nodes[5], 0x138, uint32_t(17)); f.nodes[5][0x169] = 1;        // flagged: skipped
    put(f.nodes[6], 0x138, uint32_t(8));                                // boss: skipped
    put(f.nodes[7], 0x138, uint32_t(17));                               // empty, two deep
    follow_mark_node_consumed(f.nodes[3]);
    CHECK(f.nodes[3][0x168] == 1 && f.nodes[4][0x168] == 1 && f.nodes[7][0x168] == 1);
    CHECK(f.nodes[5][0x168] == 0 && f.nodes[6][0x168] == 0);
}

}
namespace mgmp {
const Config& config() { return cfg; }
NetRole net_role() { return role; }
bool net_active() { return connected; }
uint8_t net_peer_count(){return 4;}
bool net_peer_ids(uint8_t* ids,uint8_t cap){if(cap<4)return false;for(unsigned i=0;i<4;++i)ids[i]=(uint8_t)i;return true;}
bool checkpoint_can_enter() { return checkpoint_ready; }
bool checkpoint_needs_map() { return false; }
void checkpoint_on_map(uint64_t) {}
void checkpoint_on_node(uint64_t,uint32_t) {}
bool setup_runtime_ready() { return setup_ready; }
bool setup_has_shared_roster() { return shared_setup; }
bool setup_host_can_enter() { return setup_ready; }
void setup_on_map(uint64_t checkpoint) { resume_checkpoint = checkpoint; }
uint64_t* rng_global_stream() { static uint64_t rng[4]{}; return rng; }
void log_line(const char*, const char*, ...) {}
void log_line_lvl(LogLevel, const char*, const char*, ...) {}
void roster_tick() {}
void roster_party_tick() {}
bool roster_party_swap_release() { ++roster_releases; return roster_ok; }
void roster_party_swap_reset() {}
void roster_party_swap_on_map() { ++roster_maps; }
void roster_party_swap_after_node(uint32_t type) { ++roster_entries; roster_type = type; }
void catsync_dump_lists(const char*) {}
bool roster_normalize_shared(const char*) { return true; }
bool roster_adopt_host_shared(const uint64_t*, uint32_t, const uint64_t*, uint32_t, const char*) { return true; }
void catsync_on_message(const CatDataMsg&) {}
void catsync_map_tick() { ++map_publishes; sync_order.push_back(2); }
void catsync_apply_pending(const char*) { ++drained; sync_order.push_back(1); }
bool catsync_publish(const char*, bool, bool) { ++publishes; return send_ok; }
bool invsync_publish(const char*, bool) { return send_ok; }
bool runhist_publish(const char*, bool) { return send_ok; }
bool catsync_read_familiars(EnterNodeMsg&) { return true; }
bool catsync_apply_familiars(const EnterNodeMsg& m) { return apply_ok && m.familiar_count == 0; }
bool catsync_apply_snapshot(const CatDataMsg& m, const char*, bool) { cat_values.push_back(m.data[0]); return apply_ok; }
bool invsync_apply_snapshot(const InventoryMsg& m, const char*) { coins.push_back(m.coins); return apply_ok; }
bool runhist_apply_snapshot(const RunHistMsg& m, const char*) { histories.push_back(m.size); return apply_ok; }
void lockstep_enter_battle(uint64_t seed) { battle = seed; }
void choice_on_node_entered(uint64_t seed) { remembered = seed; }
void choice_on_node_skipped(uint64_t) {}
void nodehash_on_node(uint64_t, uint32_t) { ++hashes; }
bool net_send_enter_node(const EnterNodeMsg&) { return send_ok; }
bool net_send_enter_node_to(uint8_t peer, const EnterNodeMsg& m) { ++sends; last_peer = peer; last_msg = m; return send_ok; }
}
void relayed_cat_ownership(){
 Fixture f;shared_setup=setup_ready=true;
 NetMsg m{};m.type=MSG_CATDATA;m.from=2;m.catdata.id=0x72000001ull;
 CHECK(!follow_hold_state(m));
 m.from=1;CHECK(follow_hold_state(m)); // cannot author another player's cat
 m.from=2;m.catdata.id=0x70000001ull;CHECK(follow_hold_state(m));
 m.catdata.id=0x72000001ull;setup_ready=false;CHECK(follow_hold_state(m));setup_ready=true;
 m.type=MSG_INVENTORY;CHECK(follow_hold_state(m));
 m.type=MSG_RUNHIST;CHECK(follow_hold_state(m));
}
int main() {
    relayed_cat_ownership();
    queued_snapshots(); coalesce_only_within_batch(); gates_and_failures();
    reset_and_untrusted_sender(); overflow_and_host_failure();
    reconnect_keeps_the_run(); duplicate_node_is_refused(); catchup_names_the_current_node(); roster_node_ordering();
    peer_updates_do_not_starve_local_publish();
    first_join_still_waits_for_snapshot();
    resume_map_gate();
    host_click_during_map_transition_is_retried();
    selected_unconsumed_node_is_rearmed_after_reload();
    arrived_node_with_cleared_selection_is_recovered();
    arrived_node_recovery_respects_native_state();
    exit_home_bonus_nodes_are_recovered();
    held_host_entry_survives_native_selection_clear();
    consumed_mark_preserves_adjacent_flags();
    consumed_mark_cascades_like_native();
    {
        Fixture f; role = NetRole::Host; follow_init();
        checkpoint_ready = false;
        CHECK(!follow_on_enter_node(f.map, f.nodes[0], nullptr));
        connected = false;
        CHECK(!follow_on_enter_node(f.map, f.nodes[0], nullptr));
        checkpoint_ready = connected = true;
        follow_map_update(f.map);
        CHECK(follow_on_enter_node(f.map, f.nodes[0], nullptr));
    }
    std::printf("test_follow: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
