// mgmp_indepevt.cpp -- EXPERIMENTAL: the random "?" events are each player's own (see the header).
#include "mgmp_indepevt.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_catsync.h"
#include "mgmp_event_class.h"
#include "mgmp_equipment_guard.h"
#include "mgmp_follow.h"
#include "mgmp_i18n.h"
#include "mgmp_lockstep.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_room.h"
#include "mgmp_roster.h"
#include "mgmp_session.h"

namespace mgmp {
namespace {

constexpr uint32_t kTypeEvent = 9, kTypeOptionalEvent = 10;   // MapNode::str_to_type order
constexpr ULONGLONG kQuietAfterEventMs = 300;    // the event screen has not updated for this long (and the map is up again): it is closed
constexpr ULONGLONG kToastGraceMs     = 1500;   // the host's "other players are still on their event" notice appears only after waiting this long
constexpr ULONGLONG kNoEventGraceMs   = 4000;   // on the map this long after entering an event node without any event: there was none
constexpr ULONGLONG kPublishRetryMs   = 10000;  // the cats could not be published before DONE: told anyway after this long
constexpr ULONGLONG kHoldMaxMs        = 120000; // the host waits for a client's DONE at most this long
constexpr ULONGLONG kToastEveryMs     = 3000;
constexpr uintptr_t kDir_NextEventBonus = 0x6D0;   // int: added by `next_event_bonus`, read and zeroed by the next WorldEvent constructor

struct State {
    // the constructor in flight (h_EvSetup), and this peer's own event
    const void* ctor_we = nullptr;
    const void* own_we = nullptr;
    uint64_t own_seed = 0;
    char own_name[64] = {};
    // an own event drawn by the node itself, waiting for the constructor that gets its name
    uint64_t pending_seed = 0;
    char pending_name[64] = {};
    uint64_t own_node = 0;           // the last node an own event was drawn on (kept after the screen is gone, for NODEHASH)
    // the next event's difficulty bonus while an own event is up (restored when a result changes it)
    bool bonus_guard = false;
    int32_t bonus_value = 0;
    ULONGLONG own_frame_ms = 0;
    uint32_t suppressed_said = 0;
    bool claim_in_ctor = false;      // an own event drawn by its constructor: the bonus guard starts at that constructor's end
    // CLIENT: the event node this peer stands on and what it has said about it
    uint64_t node_seed = 0;          // the event node last seen
    ULONGLONG node_seen_ms = 0;      // first map tick on it
    uint64_t event_seed = 0;         // an event was constructed on that node
    ULONGLONG last_event_ms = 0;     // the event screen's last update
    uint64_t done_sent = 0;
    ULONGLONG publish_first_fail_ms = 0;
    // HOST: each client's last DONE, and the node held for them
    uint64_t done[kMaxPeers] = {};
    uint64_t hold_seed = 0;
    ULONGLONG hold_since = 0;
    ULONGLONG last_toast = 0;
    bool valve_said = false;
};
State g;

// THE HOST'S DRAW, as the receive thread leaves it for the game thread: (low 32 bits of the node seed << 32) | hash of the event name. One 64-bit word, so it is read whole.
std::atomic<uint64_t> g_host_draw{0};
constexpr ULONGLONG kNameWaitMs = 800;           // a client's node draw waits this long for the host's name
uint32_t seed_lo32(uint64_t seed) { return (uint32_t)seed ? (uint32_t)seed : 1u; }

typedef void* (__fastcall* fn_str_assign)(void* s, const char* p, size_t n);
bool call_assign(fn_str_assign f, void* s, const char* p, size_t n) {
    __try { f(s, p, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool is_own_class(const char* name) { return name && name[0] && strncmp(name, "Quest_", 6) != 0 && event_class_independent(name); }

constexpr uint32_t kTypeSpecialEvent = 11;   // a map's quest node (`type special_event`, `level Quest_xxx`)
bool event_node_type(uint32_t t) { return t == kTypeEvent || t == kTypeOptionalEvent || t == kTypeSpecialEvent; }

bool room_on() { return session_exp_indep_events() && net_active() && net_peer_count() >= 2; }

int32_t* next_event_bonus() {
    const uintptr_t at = addr_of_data(D_MewDirectorPtr);
    uint8_t* dir = nullptr;
    if (!at || !mem_read((const void*)at, &dir, sizeof(dir)) || !dir) return nullptr;
    return (int32_t*)(dir + kDir_NextEventBonus);
}

// a result of the own event changed the next event's bonus: put it back (the next event is drawn and built the same on every peer)
void bonus_check(const char* when) {
    if (!g.bonus_guard) return;
    int32_t* p = next_event_bonus();
    int32_t now = 0;
    if (!p || !mem_read(p, &now, sizeof(now)) || now == g.bonus_value) return;
    const bool ok = mem_write(p, &g.bonus_value, sizeof(g.bonus_value));
    log_line_lvl(LogLevel::Warn, "INDEPEVT", "next_event_bonus inside this player's own event (%s): the next event's bonus %d -> %d %s -- the next event must be the same for every player",
                 when, g.bonus_value, now, ok ? "put back" : "!! could NOT be put back");
}

} // namespace

bool indepevt_active() { return room_on() && event_node_type(follow_here_type()); }

namespace {
// Does a cat of the SHARED party (the host's cats, before any peer swaps its own in) wear a main-story item? Answered with the game's own predicate over each cat's five
// slots. 0 = none, 1 = one is worn (`item` names it), -1 = unreadable (the caller treats that as "yes": a quest event follows the host unless it is certain).
int shared_party_story_item(char (&item)[48]) {
    item[0] = 0;
    __try {
        const uintptr_t at = addr_of_data(D_MewDirectorPtr);
        auto by_id = (void* (__fastcall*)(void*, uint64_t))addr_of_call(C_CatDataById);
        auto legacy = (EquipmentLegacy)addr_of_call(C_EquipmentLegacyQuest);
        uint8_t* dir = nullptr; void* registry = nullptr; uint8_t* data = nullptr; uint32_t n = 0;
        if (!at || !by_id || !legacy || !mem_read((const void*)at, &dir, sizeof(dir)) || !dir ||
            !mem_read(dir + kDir_CatRegistry, &registry, sizeof(registry)) || !registry ||
            !mem_read(dir + kDir_CatIdCount, &n, sizeof(n)) || n == 0 || n > 16 ||
            !mem_read(dir + kDir_CatIdData, &data, sizeof(data)) || !data) return -1;
        for (uint32_t i = 0; i < n; ++i) {
            uint64_t id = 0;
            if (!mem_read(data + (uintptr_t)i * 8, &id, sizeof(id)) || !id) return -1;
            void* cat = by_id(registry, id);
            if (!cat) return -1;
            if (cat_wears_story_item(cat, legacy, item, sizeof(item))) return 1;
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
} // namespace

void indepevt_ctor_begin(void* world_event, const void* name_string) {
    g.ctor_we = world_event;
    // A MAIN-QUEST EVENT (0.1.66). Its options that need a quest item cannot come up when nobody wears one; the others (a mutation, an injury, nothing at all) are the
    // player's own to take. The rule is the host's: while its party wears no main-story item the event is each player's own, otherwise it follows the host as ever
    // (a client never brings a main-story item into a room: the departure check refuses it). Area entrances (obelisks, the rift, the time machines) and the events
    // whose open options end the run or kill a cat are not in the table (scratch/gen_event_class.py).
    if (!name_string || !room_on() || follow_here_type() != kTypeSpecialEvent) return;
    char name[64] = {};
    if (!mem_read_std_string(name_string, name, sizeof(name)) || strncmp(name, "Quest_", 6) != 0) return;
    if (!event_class_quest_independent(name)) {
        log_line("INDEPEVT", "quest event '%s' on node %016llx follows the host (an area entrance, an ending, or an option that kills a cat)", name, (unsigned long long)follow_here_seed());
        return;
    }
    char item[48] = {};
    const int carried = shared_party_story_item(item);
    if (carried != 0) {
        log_line("INDEPEVT", "quest event '%s' follows the host: %s", name, carried > 0 ? "its party wears the main-story item" : "the party's items could not be read");
        if (carried > 0) log_line("INDEPEVT", "   (item: %s)", item);
        return;
    }
    bool swapped = true;
    if (net_role() == NetRole::Client) swapped = roster_party_swap_for_own_event();
    g.own_node = follow_here_seed();
    g.own_we = world_event;
    g.own_seed = follow_here_seed();
    strncpy_s(g.own_name, sizeof(g.own_name), name, _TRUNCATE);
    g.pending_seed = 0;
    g.bonus_guard = false;
    g.claim_in_ctor = true;
    log_line("INDEPEVT", "quest event '%s' on node %016llx is this player's OWN: the host's party wears no main-story item -- %s, this player chooses, nothing crosses the wire%s", name,
             (unsigned long long)g.own_seed, net_role() == NetRole::Client ? "this player's cats take it" : "the host's cats take it",
             swapped ? "" : " (!! the local roster could not be put in place)");
}

void indepevt_ctor_end(void* world_event) {
    if (world_event == g.ctor_we) g.ctor_we = nullptr;
    if (g.claim_in_ctor && world_event && world_event == g.own_we) {
        g.claim_in_ctor = false;
        const int32_t* p = next_event_bonus();
        g.bonus_guard = p && mem_read(p, &g.bonus_value, sizeof(g.bonus_value));
        g.own_frame_ms = GetTickCount64();
    }
    // the event the node drew as this player's own: the first constructor on that node that got its name is it
    if (g.pending_seed && world_event) {
        char name[64] = {};
        const bool read = mem_read_std_string((const uint8_t*)world_event + kEvt_EventName, name, sizeof(name)) && name[0];
        if (g.pending_seed == follow_here_seed() && read && strcmp(name, g.pending_name) == 0) {
            g.own_we = world_event;
            g.own_seed = g.pending_seed;
            strncpy_s(g.own_name, sizeof(g.own_name), name, _TRUNCATE);
            log_line("INDEPEVT", "the event screen of '%s' is up: this player's own", name);
            // the constructor has just taken (and zeroed) the bonus: from here on a result of this event may not change it
            const int32_t* p = next_event_bonus();
            g.bonus_guard = p && mem_read(p, &g.bonus_value, sizeof(g.bonus_value));
            g.own_frame_ms = GetTickCount64();
        } else {
            log_line_lvl(LogLevel::Warn, "INDEPEVT", "!! the node drew '%s' as this player's own, but the event constructed is '%s'%s -- it follows the host",
                         g.pending_name, read ? name : "?", g.pending_seed == follow_here_seed() ? "" : " on another node");
        }
        g.pending_seed = 0; g.pending_name[0] = '\0';
    }
    if (indepevt_active() && net_role() == NetRole::Client) {
        g.event_seed = follow_here_seed();
        g.last_event_ms = GetTickCount64();
    }
}

void indepevt_on_host_event(uint8_t from, uint32_t seed_lo, uint32_t name_hash) {
    if (from != kHostPeer || net_role() != NetRole::Client) return;
    g_host_draw.store(((uint64_t)seed_lo << 32) | name_hash);
}

namespace {
// CLIENT, the node's draw (not a constructor's): the host's drawn event, waited for briefly. When this peer's draw differs and EITHER event follows the host, this peer
// is put on the HOST'S event (the drawn name is overwritten before the game builds the event): a follow-class event's shared effects (the next fight's spawns, familiars,
// a fight, a shop ...) must run on every peer, and a click on an event the host is not on would run them here alone. When both events are the players' own, the draw stays
// as it came out (a local special case, as asked).
// `name` is updated when the draw was replaced by the host's.
void align_with_host(void* out_name, char (&name)[64]) {
    const uint64_t seed = follow_here_seed();
    if (!seed) return;
    const uint32_t lo = seed_lo32(seed);
    const ULONGLONG t0 = GetTickCount64();
    while ((uint32_t)(g_host_draw.load() >> 32) != lo && GetTickCount64() - t0 < kNameWaitMs) Sleep(5);
    const uint64_t got = g_host_draw.load();
    if ((uint32_t)(got >> 32) != lo) {
        log_line_lvl(LogLevel::Warn, "INDEPEVT", "!! the host's drawn event for node %016llx did not arrive within %llu ms -- this peer keeps its own draw '%s'",
                     (unsigned long long)seed, (unsigned long long)kNameWaitMs, name);
        return;
    }
    const uint32_t hh = (uint32_t)got;
    if (hh == event_class_hash(name)) return;
    const char* hn = event_name_by_hash(hh);
    if (!hn) {
        log_line_lvl(LogLevel::Warn, "INDEPEVT", "!! the host drew an event (hash %08x) this peer does not know; it drew '%s' -- left as it is", hh, name);
        return;
    }
    const bool mine_own = is_own_class(name), host_own = is_own_class(hn);
    if (mine_own && host_own) {
        log_line("INDEPEVT", "this peer drew '%s', the host '%s': both are the players' own events -- the draws stay different", name, hn);
        return;
    }
    const auto assign = (fn_str_assign)addr_of_call(C_StrAssign);
    if (!assign || !call_assign(assign, out_name, hn, strlen(hn))) {
        log_line_lvl(LogLevel::Error, "INDEPEVT", "!! this peer drew '%s', the host '%s' (%s): the draw could NOT be replaced -- the shared effects may run here alone", name, hn,
                     host_own ? "this one follows the host" : "the host's follows the host");
        return;
    }
    char again[64] = {};
    log_line_lvl(LogLevel::Warn, "INDEPEVT", "this peer drew '%s' (%s), the host '%s' (%s): put on the host's event so the shared effects run on every peer",
                 name, mine_own ? "own" : "follows the host", hn, host_own ? "own" : "follows the host");
    if (mem_read_std_string(out_name, again, sizeof(again)) && again[0]) memcpy(name, again, sizeof(again));
}
} // namespace

void indepevt_after_pick(void* out_name, bool in_ctor) {
    if ((in_ctor && !g.ctor_we) || !out_name || !indepevt_active()) return;
    char name[64] = {};
    if (!mem_read_std_string(out_name, name, sizeof(name)) || !name[0]) return;
    if (!in_ctor) {
        if (net_role() == NetRole::Host) {          // tell the clients what this node drew (proto 103)
            ChapterMsg m{};
            m.kind = kChapterEventName; m.generation = seed_lo32(follow_here_seed()); m.mapflags = event_class_hash(name);
            if (!net_send_chapter(m)) log_line_lvl(LogLevel::Warn, "INDEPEVT", "!! the drawn event '%s' could not be sent to the clients", name);
        } else if (net_role() == NetRole::Client) {
            align_with_host(out_name, name);
        }
    }
    const bool own = is_own_class(name);
    if (!own) {
        log_line("INDEPEVT", "event '%s' drawn on node %016llx: its effects reach the shared run -- it follows the host as before", name,
                 (unsigned long long)follow_here_seed());
        return;
    }
    // A CLIENT'S OWN CATS, from here on: the draw above was made with the shared party (the same event as the host's), the subject the
    // constructor picks next must be one of this player's cats.
    bool swapped = true;
    if (net_role() == NetRole::Client) swapped = roster_party_swap_for_own_event();
    g.own_node = follow_here_seed();
    if (in_ctor) {
        g.own_we = g.ctor_we;
        g.own_seed = follow_here_seed();
        strncpy_s(g.own_name, sizeof(g.own_name), name, _TRUNCATE);
        g.bonus_guard = false;        // taken at the constructor's end, after it zeroed the bonus
        g.pending_seed = 0;
        g.claim_in_ctor = true;
    } else {                     // the constructor comes later (the event scene): claimed in indepevt_ctor_end by the name
        g.own_we = nullptr;
        g.own_seed = 0;
        g.pending_seed = follow_here_seed();
        strncpy_s(g.pending_name, sizeof(g.pending_name), name, _TRUNCATE);
    }
    if (swapped)
        log_line("INDEPEVT", "event '%s' drawn on node %016llx (%s) is this player's OWN: %s, this player chooses, nothing crosses the wire", name,
                 (unsigned long long)follow_here_seed(), in_ctor ? "by its constructor" : "by the node",
                 net_role() == NetRole::Client ? "this player's cats take it" : "the host's cats take it");
    else
        log_line_lvl(LogLevel::Warn, "INDEPEVT", "!! event '%s' is this player's own, but the local roster could not be put in place -- the subject is"
                     " from the shared party; this player still chooses (changes to another player's cat are overwritten by its owner)", name);
}

bool indepevt_own_node(uint64_t seed) { return seed && seed == g.own_node && room_on(); }

bool indepevt_is_own(const void* world_event) {
    return world_event && world_event == g.own_we && g.own_seed && g.own_seed == follow_here_seed() && room_on();
}

bool indepevt_suppress(const void* world_event, const char* what) {
    if (!indepevt_is_own(world_event)) return false;
    uint32_t bit = 0;
    for (const char* c = what ? what : ""; *c; ++c) bit = bit * 31 + (unsigned char)*c;
    bit = 1u << (bit % 31);
    if (!(g.suppressed_said & bit)) {
        g.suppressed_said |= bit;
        log_line_lvl(LogLevel::Warn, "INDEPEVT", "inside this player's own event '%s': %s SKIPPED on every player -- the next event must be the same for everyone",
                     g.own_name, what ? what : "?");
    }
    return true;
}

void indepevt_on_event_frame(const void* world_event) {
    if (indepevt_is_own(world_event)) { g.own_frame_ms = GetTickCount64(); bonus_check("while the event is up"); }
    if (net_role() == NetRole::Client && indepevt_active()) {
        g.last_event_ms = GetTickCount64();
        if (!g.event_seed) g.event_seed = follow_here_seed();
    }
}

void indepevt_tick() {
    if (!net_active()) {
        if (g.own_we || g.node_seed || g.hold_seed) g = State{};
        return;
    }
    const ULONGLONG now = GetTickCount64();
    // the own event's screen is gone (no update for a moment): a last look at the bonus, then the guard ends
    if (g.bonus_guard && now - g.own_frame_ms > 500) { bonus_check("after the event"); g.bonus_guard = false; }
    if (net_role() == NetRole::Host) {
        // forget players who left (a newcomer in the same slot reports for itself)
        uint8_t ids[kMaxPeers] = {};
        bool present[kMaxPeers] = {};
        if (net_peer_ids(ids, kMaxPeers)) for (uint8_t i = 0; i < net_peer_count() && i < kMaxPeers; ++i) if (ids[i] < kMaxPeers) present[ids[i]] = true;
        for (uint8_t p = 0; p < kMaxPeers; ++p) if (g.done[p] && !present[p]) g.done[p] = 0;
        return;
    }
    // NOT lockstep_in_battle() (0.1.73, room QVST): that is "a roster has been snapshotted and not replaced yet", which stays TRUE on the map after the run's first fight --
    // from then on no client ever said DONE and the host sat at the boss node until its 120 s valve. follow_on_map() below is the real test: the map screen is ticking.
    if (net_role() != NetRole::Client || !indepevt_active()) return;
    const uint64_t seed = follow_here_seed();
    if (!seed || seed == g.done_sent || !follow_on_map()) return;
    if (seed != g.node_seed) { g.node_seed = seed; g.node_seen_ms = now; g.publish_first_fail_ms = 0; }
    // the event screen is closed (it stopped updating), or there was no event on this node at all
    if (g.event_seed == seed) { if (now - g.last_event_ms < kQuietAfterEventMs) return; }
    else if (now - g.node_seen_ms < kNoEventGraceMs) return;
    // this player's cats first, so the host has them before its next node is built from them
    if (!catsync_publish("this player's random event is done", false, true)) {
        if (!g.publish_first_fail_ms) g.publish_first_fail_ms = now;
        if (now - g.publish_first_fail_ms < kPublishRetryMs) return;
        log_line_lvl(LogLevel::Warn, "INDEPEVT", "!! the cats could not be published for %llu ms -- telling the host the event is done anyway",
                     (unsigned long long)(now - g.publish_first_fail_ms));
    }
    ChapterMsg m{};
    m.kind = kChapterEventDone; m.act = 0; m.difficulty = 0;
    m.generation = (uint32_t)seed ? (uint32_t)seed : 1u;
    m.mapflags = (uint32_t)(seed >> 32);
    if (!net_send_chapter(m)) return;
    g.done_sent = seed;
    log_line("INDEPEVT", "-> done with the event node %016llx (%s) -- the host may go on", (unsigned long long)seed,
             g.event_seed == seed ? "its screen closed" : "no event came up");
}

void indepevt_on_done(uint8_t from, uint64_t seed) {
    if (net_role() != NetRole::Host || from >= kMaxPeers || from == net_self()) return;
    g.done[from] = seed;
    log_line("INDEPEVT", "<- peer %u is done with the event node %016llx", (unsigned)from, (unsigned long long)seed);
}

bool indepevt_hold_node() {
    if (net_role() != NetRole::Host || !indepevt_active()) return false;
    const uint64_t seed = follow_here_seed();
    if (!seed) return false;
    // Only a node where the HOST drew an own event: when its event follows the host (or either one does), the draws were aligned and every peer is on the same event --
    // nothing a client does there reaches the host's next node.
    if (g.own_node != seed) return false;
    const uint32_t lo = (uint32_t)seed ? (uint32_t)seed : 1u;
    const uint64_t want = ((uint64_t)(uint32_t)(seed >> 32) << 32) | lo;
    uint8_t ids[kMaxPeers] = {};
    if (!net_peer_ids(ids, kMaxPeers)) return false;
    char waiting[64] = {};
    int off = 0;
    for (uint8_t i = 0; i < net_peer_count() && i < kMaxPeers; ++i) {
        const uint8_t p = ids[i];
        if (p >= kMaxPeers || p == net_self() || g.done[p] == want) continue;
        off += _snprintf_s(waiting + off, sizeof(waiting) - off, _TRUNCATE, "%s%u", off ? " " : "", (unsigned)p);
    }
    if (!off) return false;
    const ULONGLONG now = GetTickCount64();
    if (g.hold_seed != seed) {
        g.hold_seed = seed; g.hold_since = now; g.valve_said = false;
        log_line("INDEPEVT", "HELD: the host wants to leave the event node %016llx, peer(s) [%s] are still on their own event", (unsigned long long)seed, waiting);
    }
    if (now - g.hold_since >= kHoldMaxMs) {
        if (!g.valve_said) {
            g.valve_said = true;
            log_line_lvl(LogLevel::Warn, "INDEPEVT", "!! peer(s) [%s] said nothing for %llu s about the event node %016llx -- the host goes on without them",
                         waiting, (unsigned long long)(kHoldMaxMs / 1000), (unsigned long long)seed);
        }
        return false;
    }
    // The notice only when the wait is real: a client's "done" follows its event screen closing within a moment, and a flash of the notice for that moment is noise.
    if (now - g.hold_since >= kToastGraceMs && now - g.last_toast >= kToastEveryMs) { g.last_toast = now; room_toast(tr(Tx::IE_WAIT_OTHERS)); }
    return true;
}

} // namespace mgmp
