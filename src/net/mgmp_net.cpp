// mgmp_net.cpp -- Winsock TCP transport. See mgmp_net.h for the design rule.

#include "mgmp_net.h"
#include "mgmp_log.h"
#include "mgmp_server_addr.h"   // comeow.top: the address when the name does not resolve
#include "mgmp_perf.h"
#include "mgmp_steambridge.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>

#pragma comment(lib, "ws2_32.lib")

namespace mgmp {
namespace {

// The receive queue. Fixed capacity, no allocation on the receive path.
// 256 messages is far more than a turn can produce -- run E's whole 15-turn
// battle was 49 actions -- so overflow means the game thread has stopped
// draining, which is a bug worth counting rather than growing a buffer for.
constexpr uint32_t kQueueCap = 256;

// The largest frame this build will accept. Everything except MSG_SAVEFILE is
// under 128 bytes; a save is ~45 KB, and the slack is for saves that grow.
constexpr uint32_t kMaxFrame = 1u << 20;
static_assert(kMaxFrame <= kMaxPayload, "frame cap must fit the protocol cap");

// One connection. On the host there is one per client and `id` is that client's
// peer id; on a client there is exactly one, links[0], and it is the host.
//
// Each link owns a receive thread for its whole lifetime, rather than one thread
// select()ing over all of them. Three sockets do not justify a readiness loop,
// and a thread per link keeps the per-link byte stream strictly ordered with no
// interleaving logic to get wrong -- which matters because the relay runs on
// this thread.
struct Link {
    SOCKET        sock   = INVALID_SOCKET;
    HANDLE        thread = nullptr;
    uint8_t       id     = kNoPeer;
    volatile LONG live   = 0;

    // THE OUTGOING QUEUE (0.1.50). A send used to be a blocking send() on the CALLER's thread under one lock shared by every link: the game thread stalled whenever a
    // socket's buffer was full (a slow carrier, the 32 KB/s server relay), and the host's relay of one client's frames held up its sends to everyone else. Now a frame is
    // appended here (whole, so frames never interleave) and this link's writer thread puts it on the wire; only a queue past kSendQueueCap makes the caller wait.
    CRITICAL_SECTION qcs;
    bool          qcs_ready = false;
    HANDLE        qev     = nullptr;   // auto-reset: something was queued, or the writer is told to stop
    HANDLE        writer  = nullptr;
    uint8_t*      qbuf    = nullptr;
    size_t        qcap = 0, qhead = 0, qlen = 0;
    volatile LONG wstop   = 0;
    volatile LONG wdead   = 0;         // a send failed: nothing more is written to this link
};
constexpr size_t kSendQueueCap = 16u << 20;   // bytes waiting on one link before a sender is made to wait for the writer

struct State {
    NetRole  role  = NetRole::None;
    volatile LONG state = (LONG)NetState::Idle;

    SOCKET   listener = INVALID_SOCKET;

    Link     links[kMaxPeers];
    uint8_t  self     = kNoPeer;     // our own peer id; kHostPeer on the host

    HANDLE   accept_thread = nullptr;
    volatile LONG stop = 0;

    CRITICAL_SECTION cs;
    bool     cs_ready = false;

    NetMsg   queue[kQueueCap];
    uint32_t head = 0, count = 0;
    uint32_t evicted = 0;            // voice/pointer frames pushed out of a full queue (0.1.182)
    uint8_t  dropped_last = 0;       // the type of the last message dropped because the queue was full

    CRITICAL_SECTION send_cs;
    bool     send_cs_ready = false;

    // Membership, host-side. Rebroadcast to everyone whenever it changes.
    PeersMsg roster;
    bool     roster_valid = false;
    uint8_t  seat_ids[kMaxPeers] = {};   // net_set_seat_order: the players' order, host first
    uint8_t  seat_count = 0;

    char     error[256] = {};
    NetStats stats;

    bool     wsa_up = false;
};

State g;

void set_state(NetState s) { InterlockedExchange(&g.state, (LONG)s); }

void fail(const char* what, int err) {
    _snprintf_s(g.error, sizeof(g.error), _TRUNCATE, "%s failed (WSA %d)", what, err);
    set_state(NetState::Failed);
    log_line("NET", "!! %s", g.error);
}

bool wsa_init() {
    if (g.wsa_up) return true;
    WSADATA wsa{};
    int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) { fail("WSAStartup", rc); return false; }
    g.wsa_up = true;
    return true;
}

struct LevelBox { uint64_t node = 0; char name[64] = {}; uint64_t bnode = 0; uint8_t n_pending[kPendingQueues] = {}; PendingEnemy pending[kPendingQueues][kPendingMax]; uint32_t pending_total[kPendingQueues] = {}; uint8_t n_weather = 0; char weather[kWeatherNames][kWeatherLen] = {};
                  int8_t bdifficulty = -1;   // proto 110: the host's difficulty with the build information of battle `bnode`
                  int8_t difficulty = -1;    // ... and the latest the host sent with any UNLOCKS (a shared event's)
                  int8_t extra_events = -1, event_difficulty = -1;   // proto 112: the host's latest tag counts (T_TagCount)
                } g_level_box;
// The last few FULL boards (since proto 98 a later turn's board only comes when asked for, or streamed). Guarded by g.cs like the level box.
// 4 until 0.1.50, when a client that fell more than four boundaries behind the host found its turn's board already overwritten and waited out the whole 3 s for it, at every
// boundary, until the host stopped for a player (2026-10-20: nine boundaries in a row).
constexpr uint32_t kBoardRing = 8;
BoardAssembled g_board_ring[kBoardRing];
ULONGLONG      g_board_at[kBoardRing] = {};
uint32_t g_board_next = 0;
// CLIENT: the host's board digests (proto 98), small enough to keep a long run of them; and the latest boundary anything of the board came for (net_board_seen_past).
constexpr uint32_t kSumRing = 128;
BoardSumMsg g_sum_ring[kSumRing];
ULONGLONG   g_sum_at[kSumRing] = {};          // 0.1.246: when each arrived (net_forget_battle_before)
uint32_t g_sum_next = 0;
// CLIENT: the host's turn queues (proto 111), one per boundary, the same mailbox shape.
constexpr uint32_t kTqRing = 16;
TurnQueueMsg g_tq_ring[kTqRing];
ULONGLONG    g_tq_at[kTqRing] = {};
uint32_t g_tq_next = 0;
uint64_t g_board_latest_battle = 0;
uint32_t g_board_latest_turn = 0;
void board_note_latest(uint64_t battle, uint32_t turn) {
    if (g_board_latest_battle != battle) { g_board_latest_battle = battle; g_board_latest_turn = turn; }
    else if (turn > g_board_latest_turn) g_board_latest_turn = turn;
}
// HOST: the encoded chunks of the last kBoardKeep boundaries (each a run of [u32 length][payload]), for a client that asks (MSG_BOARDREQ, answered on its receive thread);
// and which clients asked to be sent every boundary's chunks for the rest of a battle (kBoardReqStream). Guarded by g.cs.
constexpr uint32_t kBoardKeep = 64;
struct KeptBoard { uint64_t battle = 0; uint32_t turn = 0; uint8_t* data = nullptr; uint32_t len = 0; ULONGLONG at = 0; };
KeptBoard g_kept[kBoardKeep];
uint32_t g_kept_next = 0;
uint64_t g_board_stream[kMaxPeers] = {};
constexpr uint32_t kRollRing = 64;
RollMsg g_roll_ring[kRollRing];
ULONGLONG g_roll_at[kRollRing] = {};            // the host's last rolls, guarded by g.cs
// PROTO 113, mgmp_unitstate. HOST: every unit's deep-state record of the last kUsKeep boundaries (encoded MSG_USTATE payloads), answered on a client's request from its
// receive thread. CLIENT: the records the host sent, by battle/turn/index. Both guarded by g.cs.
constexpr uint32_t kUsKeep = 6;
struct KeptUstate { uint64_t battle = 0; uint32_t turn = 0; uint8_t* blob[kBoardMax] = {}; uint32_t len[kBoardMax] = {}; ULONGLONG at = 0; };
KeptUstate g_us_kept[kUsKeep];
uint32_t g_us_kept_next = 0;
// PROTO 115 (0.1.190): units made from cat data. HOST: the battle's cat images (encoded MSG_UCAT frames), by battle/index/key; CLIENT: the frames the host sent. Both under g.cs.
constexpr uint32_t kUcatKeep = 64, kUcatBox = 16;
struct UcatFrame { uint64_t battle = 0; uint8_t index = 0; int32_t key = 0; uint8_t* data = nullptr; uint32_t len = 0; };
UcatFrame g_ucat_kept[kUcatKeep];
uint32_t g_ucat_kept_next = 0;
UcatFrame g_ucat_box[kUcatBox];
uint32_t g_ucat_box_next = 0;
// CLIENT: the host's ground maps (proto 116, mgmp_tilesync), raw payloads, one per boundary
constexpr uint32_t kTilesBox = 8;
struct TilesBox { uint64_t battle = 0; uint32_t turn = 0; uint8_t* data = nullptr; uint32_t len = 0; ULONGLONG at = 0; };
TilesBox g_tiles_box[kTilesBox];
uint32_t g_tiles_box_next = 0;
constexpr uint32_t kUsBox = 128;
struct UstateBox { uint64_t battle = 0; uint32_t turn = 0; uint8_t index = 0; uint8_t* data = nullptr; uint32_t len = 0; uint32_t seq = 0; ULONGLONG at = 0; };
UstateBox g_us_box[kUsBox];
uint32_t g_us_box_next = 0;
// CLIENT: the host's per-action checks (proto 117, MSG_ACTCHECK), raw payloads by battle/turn/action
constexpr uint32_t kActBox = 48;
struct ActBox { uint64_t battle = 0; uint32_t turn = 0, action = 0; uint8_t* data = nullptr; uint32_t len = 0; ULONGLONG at = 0; };
ActBox g_act_box[kActBox];
uint32_t g_act_box_next = 0;
// CLIENT (0.1.213): the host's last turn hashes (net_host_hash), guarded by g.cs
constexpr uint32_t kHashRing = 16;
HashMsg g_hash_ring[kHashRing];
ULONGLONG g_hash_at[kHashRing] = {};
uint32_t g_hash_next = 0;
// HOST (0.1.213): the clients whose link is slow (net_set_slow_peer, from the round trips mgmp_perf measures). Each boundary's full board is streamed to them and the
// records of the units that changed since the last boundary are pushed -- what they would otherwise ask for and wait a round trip or two to get. Guarded by g.cs.
bool g_slow_peer[kMaxPeers] = {};
uint32_t g_roll_next = 0;

// A FULL QUEUE (0.1.182). The game thread drains it once a frame; while it is stalled (a save load, a scene change: seconds) a voice frame comes ~24 times a second
// for each speaker and a pointer several times more -- 256 slots go in a few seconds, and the message then dropped was simply the newest: an ACTION, a CHOICE, a
// HASH, lost without a word. A voice frame, a pointer or an aim preview is worth nothing late, so the oldest of those makes room first (the others keep their
// order); only a queue full of game messages drops one. Called under g.cs.
bool queue_cheap(uint8_t type) { return type == MSG_VOICE || type == MSG_CURSOR || type == MSG_AIM; }
bool queue_make_room(uint8_t incoming) {
    if (queue_cheap(incoming)) return false;                       // a cheap frame never pushes out another one: it is the one that goes
    for (uint32_t k = 0; k < g.count; ++k) {
        if (!queue_cheap(g.queue[(g.head + k) % kQueueCap].type)) continue;
        for (uint32_t j = k; j + 1 < g.count; ++j)                  // close the gap (a cheap frame owns no memory; the slot left at the end is the new message's)
            g.queue[(g.head + j) % kQueueCap] = g.queue[(g.head + j + 1) % kQueueCap];
        --g.count;
        ++g.evicted;
        return true;
    }
    return false;
}

void queue_push(const NetMsg& m) {
    // A PING never reaches the game thread's queue: the receive thread takes its statistics and answers a request AT ONCE, so the round trip is the link's alone (mgmp_perf).
    if (m.type == MSG_PING) {
        perf_on_ping(m.from, m.ping);
        if (m.ping.kind == 0) {
            uint8_t p[kSendPing];
            const uint32_t n = perf_make_reply(p, sizeof(p), m.ping);
            if (n) net_send_peer(m.from, p, n);
        }
        return;
    }
    EnterCriticalSection(&g.cs);
    if (m.type == MSG_UNLOCKS && m.unlocks.n_level_name) {          // the level of a battle (picked by the level picker)
        g_level_box.node = m.unlocks.level_node;
        memcpy(g_level_box.name, m.unlocks.level_name, sizeof(g_level_box.name));
    }
    if (m.type == MSG_UNLOCKS && m.unlocks.build_info && m.unlocks.level_node) {   // the build information of a battle (ANY battle node), keyed on its own
        g_level_box.bnode = m.unlocks.level_node;
        memcpy(g_level_box.n_pending, m.unlocks.n_pending, sizeof(g_level_box.n_pending));
        memcpy(g_level_box.pending, m.unlocks.pending, sizeof(g_level_box.pending));
        memcpy(g_level_box.pending_total, m.unlocks.pending_total, sizeof(g_level_box.pending_total));
        g_level_box.n_weather = m.unlocks.n_weather;
        memcpy(g_level_box.weather, m.unlocks.weather, sizeof(g_level_box.weather));
        g_level_box.bdifficulty = m.unlocks.difficulty;
    }
    if (m.type == MSG_UNLOCKS && m.unlocks.difficulty >= 0) g_level_box.difficulty = m.unlocks.difficulty;
    if (m.type == MSG_UNLOCKS && m.unlocks.extra_events >= 0) g_level_box.extra_events = m.unlocks.extra_events;           // proto 112
    if (m.type == MSG_UNLOCKS && m.unlocks.event_difficulty >= 0) g_level_box.event_difficulty = m.unlocks.event_difficulty;
    if (m.type == MSG_ROLL) { g_roll_at[g_roll_next % kRollRing] = GetTickCount64(); g_roll_ring[g_roll_next++ % kRollRing] = m.roll; }
    // 0.1.213: the host's turn hashes, kept beside the queue -- a client's resync at a boundary waits for the host's hash of that turn without pumping the queue
    if (m.type == MSG_HASH && g.role == NetRole::Client && m.from == kHostPeer) { g_hash_at[g_hash_next % kHashRing] = GetTickCount64(); g_hash_ring[g_hash_next++ % kHashRing] = m.hash; }
    if (m.type == MSG_BOARD) {
        const BoardMsg& b = m.board;
        BoardAssembled* box = nullptr;
        for (uint32_t k = 0; k < kBoardRing && !box; ++k)
            if (g_board_ring[k].battle == b.battle && g_board_ring[k].turn == b.turn) box = &g_board_ring[k];
        if (!box) { box = &g_board_ring[g_board_next++ % kBoardRing]; *box = BoardAssembled{}; box->battle = b.battle; box->turn = b.turn; }
        // 0.1.246: a new publication of this battle and turn starts at its first chunk -- it is not merged into the last one (a mid-battle re-snapshot restarts
        // the turns under the same battle id: the new turn 0's chunks went into the battle's first turn-0 board, which already read as complete)
        if (b.first == 0 && box->got) { const uint64_t bb = box->battle; const uint32_t bt = box->turn; *box = BoardAssembled{}; box->battle = bb; box->turn = bt; }
        g_board_at[box - g_board_ring] = GetTickCount64();
        if (box->cats != b.cats || box->total != b.total) {
            *box = BoardAssembled{};   // a republication of that turn with another shape: start over
            box->battle = b.battle; box->turn = b.turn; box->cats = b.cats; box->total = b.total;
        }
        memcpy(box->rng, b.rng, sizeof(box->rng));
        for (uint32_t i = 0; i < b.count; ++i) {
            const uint32_t at = b.first + i;
            if (at >= kBoardMax) break;
            if (!box->have[at]) { box->have[at] = true; ++box->got; }
            box->unit[at] = b.units[i];
        }
        box->complete = box->got >= box->total;
        board_note_latest(b.battle, b.turn);
    }
    if (m.type == MSG_BOARDSUM) {
        const BoardSumMsg& s = m.boardsum;
        BoardSumMsg* slot = nullptr;
        for (uint32_t k = 0; k < kSumRing && !slot; ++k) if (g_sum_ring[k].battle == s.battle && g_sum_ring[k].turn == s.turn) slot = &g_sum_ring[k];
        if (!slot) slot = &g_sum_ring[g_sum_next++ % kSumRing];
        *slot = s;
        g_sum_at[slot - g_sum_ring] = GetTickCount64();
        board_note_latest(s.battle, s.turn);
    }
    if (m.type == MSG_TURNQUEUE) {
        const TurnQueueMsg& q = m.turnqueue;
        TurnQueueMsg* slot = nullptr;
        for (uint32_t k = 0; k < kTqRing && !slot; ++k) if (g_tq_ring[k].battle == q.battle && g_tq_ring[k].turn == q.turn) slot = &g_tq_ring[k];
        if (!slot) slot = &g_tq_ring[g_tq_next++ % kTqRing];
        *slot = q;
        g_tq_at[slot - g_tq_ring] = GetTickCount64();
    }
    // The board's frames live in their mailboxes only (the game thread waits on those inside the turn boundary): queuing them as well cost five slots of the 256 per boundary
    // and nothing ever read them.
    if (m.type == MSG_BOARD || m.type == MSG_BOARDSUM || m.type == MSG_TURNQUEUE) {
        ++g.stats.received;
        LeaveCriticalSection(&g.cs);
        return;
    }
    if (g.count >= kQueueCap && !queue_make_room(m.type)) {
        // Drop the newest rather than the oldest. Losing the oldest would
        // reorder the action stream, and an out-of-order ACTION is a desync;
        // a dropped one is at least detectable as a stall at the turn barrier.
        ++g.stats.dropped;
        g.dropped_last = m.type;
        LeaveCriticalSection(&g.cs);
        // The frame is ours by the time it reaches here, so dropping it means
        // freeing it -- a dropped SAVEFILE that leaked would be 45 KB gone with
        // nothing to say so.
        NetMsg dead = m;
        net_msg_release(dead);
        return;
    }
    g.queue[(g.head + g.count) % kQueueCap] = m;
    ++g.count;
    ++g.stats.received;
    LeaveCriticalSection(&g.cs);
}

// --- link table -------------------------------------------------------------
//
// Guarded by link_cs. Held only around table reads and writes, never across a
// send or a recv: a slow peer must not be able to stall the accept thread or
// another peer's relay.

int link_slot_of(uint8_t peer) {
    for (int i = 0; i < kMaxPeers; ++i)
        if (InterlockedCompareExchange(&g.links[i].live, 0, 0) && g.links[i].id == peer)
            return i;
    return -1;
}

int free_link_slot() {
    for (int i = 0; i < kMaxPeers; ++i)
        if (!InterlockedCompareExchange(&g.links[i].live, 0, 0)) return i;
    return -1;
}

// Read exactly n bytes, or fail. Returns false on close/error/stop.
bool recv_exact(SOCKET s, uint8_t* p, uint32_t n) {
    uint32_t got = 0;
    while (got < n) {
        if (InterlockedCompareExchange(&g.stop, 0, 0)) return false;
        int r = recv(s, (char*)(p + got), (int)(n - got), 0);
        if (r == 0)  return false;                  // orderly shutdown
        if (r < 0) {
            int e = WSAGetLastError();
            if (e == WSAEINTR) continue;
            return false;
        }
        got += (uint32_t)r;
    }
    return true;
}

bool decode_into(const uint8_t* buf, uint32_t len, NetMsg& m) {
    if (len < 1) return false;
    Reader r(buf, len);
    m.type = r.u8v();
    switch (m.type) {
        case MSG_HELLO:   return dec_hello(r, m.hello);
        case MSG_WELCOME: return dec_welcome(r, m.welcome);
        case MSG_ACTION:  return dec_action(r, m.action);
        case MSG_HASH:    return dec_hash(r, m.hash);
        case MSG_CONTROL: return dec_control(r, m.control);
        case MSG_CURSOR:  return dec_cursor(r, m.cursor);
        case MSG_AIM:     return dec_aim(r, m.aim);
        case MSG_ENTERNODE: return dec_enter_node(r, m.enter_node);
        case MSG_CHOICE:    return dec_choice(r, m.choice);
        case MSG_DEBUGHIT:  return dec_debughit(r, m.debughit);
        case MSG_PARTY:     return dec_party(r, m.party);
        case MSG_SETUP:     return dec_setup(r, m.setup);
        case MSG_CHAPTER:   return dec_chapter(r, m.chapter);
        case MSG_CHAPTERSEED: return dec_chapterseed(r, m.chapterseed);
        case MSG_CHECKPOINT:return dec_checkpoint(r, m.checkpoint);
        case MSG_CHAPTERMAP: return dec_chaptermap(r, m.chaptermap);
        case MSG_SAVEFILE: return dec_savefile(r, m.savefile);
        case MSG_CATDATA:  return dec_catdata(r, m.catdata);
        case MSG_CATDIGEST: return dec_catdigest(r, m.catdigest);
        case MSG_INVENTORY: return dec_inventory(r, m.inventory);
        case MSG_RUNHIST:   return dec_runhist(r, m.runhist);
        case MSG_STATEDUMP: return dec_statedump(r, m.statedump);
        case MSG_PEERLOG:   return dec_peerlog(r, m.peerlog);
        case MSG_AUDIT:     return dec_audit(r, m.audit);
        case MSG_UQD:       return dec_uqd(r, m.uqd);
        case MSG_PROPS:     return dec_props(r, m.props);
        case MSG_RNGL:      return dec_rngl(r, m.rngl);
        case MSG_CHAT:      return dec_chat(r, m.chat);
        case MSG_DRAW:      return dec_draw(r, m.draw);
        case MSG_SETTING:   return dec_setting(r, m.setting);
        case MSG_EQUIP:     return dec_equip(r, m.equip);
        case MSG_TREASURE:  return dec_treasure(r, m.treasure);
        case MSG_HOUSECOOP: return dec_housecoop(r, m.housecoop);
        case MSG_DEEP:      return dec_deep(r, m.deep);
        case MSG_NODEHASH:  return dec_nodehash(r, m.nodehash);
        case MSG_HOSTLEFT:  return dec_hostleft(r, m.hostleft);
        case MSG_PAGE:      return dec_page(r, m.page);
        case MSG_CATS:      return dec_cats(r, m.cats);
        case MSG_SAVEWAIT:  return dec_savewait(r, m.savewait);
        case MSG_ABANDON:   return dec_abandon(r, m.abandon);
        case MSG_ROOMCTL:   return dec_roomctl(r, m.roomctl);
        case MSG_MAPSEEDS:  return dec_mapseeds(r, m.mapseeds);
        case MSG_UNLOCKS:   return dec_unlocks(r, m.unlocks);
        case MSG_BOARD:     return dec_board(r, m.board);
        case MSG_BOARDSUM:  return dec_boardsum(r, m.boardsum);
        case MSG_BOARDREQ:  return dec_boardreq(r, m.boardreq);
        case MSG_TURNQUEUE: return dec_turnqueue(r, m.turnqueue);
        case MSG_SOULCAT:   return dec_soulcat(r, m.soulcat);
        case MSG_NODEVOTE:  return dec_nodevote(r, m.nodevote);
        case MSG_VOICE:     return dec_voice(r, m.voice);
        case MSG_ROLL:      return dec_roll(r, m.roll);
        case MSG_PEERS:   return dec_peers(r, m.peers);
        case MSG_HALT:    return dec_halt(r, m.halt);
        case MSG_REFUSE:  r.str(m.refuse, sizeof(m.refuse)); return r.ok;
        case MSG_PING:    return dec_ping(r, m.ping);
        default:          return false;
    }
}

// Write one framed message on one socket. `from` is stamped into the envelope:
// our own id when we author a message, the ORIGINATOR's id when the host relays
// one.
bool send_framed(SOCKET s, uint8_t from, const uint8_t* payload, uint32_t len) {
    if (s == INVALID_SOCKET || len == 0) return false;

    uint32_t n = len + kEnvelopeBytes;
    EnterCriticalSection(&g.send_cs);
    bool ok = true;
    const char* parts[3] = { (const char*)&n, (const char*)&from, (const char*)payload };
    int         sizes[3] = { 4, (int)kEnvelopeBytes, (int)len };
    for (int i = 0; i < 3 && ok; ++i) {
        int off = 0;
        while (off < sizes[i]) {
            int r = send(s, parts[i] + off, sizes[i] - off, 0);
            if (r <= 0) { ok = false; break; }
            off += r;
        }
    }
    if (ok) { ++g.stats.sent; g.stats.bytes_sent += n + 4; }
    LeaveCriticalSection(&g.send_cs);
    return ok;
}

// Queue one framed message on a link (see Link: the outgoing queue). Any thread. False when the link is down or its writer has failed.
bool link_send(Link& l, uint8_t from, const uint8_t* payload, uint32_t len) {
    if (len == 0 || !l.qcs_ready) return false;
    if (!InterlockedCompareExchange(&l.live, 0, 0) || InterlockedCompareExchange(&l.wdead, 0, 0)) return false;
    const uint32_t n = len + kEnvelopeBytes;
    const size_t total = 4 + (size_t)n;
    for (;;) {
        EnterCriticalSection(&l.qcs);
        if (l.qlen == 0 || l.qlen + total <= kSendQueueCap) break;
        LeaveCriticalSection(&l.qcs);
        // backpressure: only a link this far behind makes its sender wait, as every send used to
        if (!InterlockedCompareExchange(&l.live, 0, 0) || InterlockedCompareExchange(&l.wdead, 0, 0)) return false;
        Sleep(1);
    }
    if (l.qhead + l.qlen + total > l.qcap) {
        if (l.qhead) { memmove(l.qbuf, l.qbuf + l.qhead, l.qlen); l.qhead = 0; }
        if (l.qlen + total > l.qcap) {
            size_t cap = l.qcap ? l.qcap : 64 * 1024;
            while (cap < l.qlen + total) cap *= 2;
            uint8_t* grown = (uint8_t*)realloc(l.qbuf, cap);
            if (!grown) { LeaveCriticalSection(&l.qcs); log_line("NET", "!! out of memory queuing %u bytes for peer %u", n, (unsigned)l.id); return false; }
            l.qbuf = grown; l.qcap = cap;
        }
    }
    uint8_t* at = l.qbuf + l.qhead + l.qlen;
    memcpy(at, &n, 4); memcpy(at + 4, &from, kEnvelopeBytes); memcpy(at + 4 + kEnvelopeBytes, payload, len);
    l.qlen += total;
    LeaveCriticalSection(&l.qcs);
    SetEvent(l.qev);
    EnterCriticalSection(&g.send_cs);
    ++g.stats.sent; g.stats.bytes_sent += total;
    LeaveCriticalSection(&g.send_cs);
    return true;
}

// The writer of one link: takes what is queued, a block at a time, and puts it on the wire. Stops when told to (after draining), or at the first failed send -- then it shuts
// the socket down so the receive thread sees the link go, as it would have from the other side.
DWORD WINAPI link_writer(LPVOID param) {
    Link& l = *(Link*)param;
    static constexpr size_t kBlock = 64 * 1024;
    uint8_t* block = (uint8_t*)malloc(kBlock);
    if (!block) { InterlockedExchange(&l.wdead, 1); return 0; }
    for (;;) {
        EnterCriticalSection(&l.qcs);
        const size_t take = l.qlen < kBlock ? l.qlen : kBlock;
        if (take) { memcpy(block, l.qbuf + l.qhead, take); l.qhead += take; l.qlen -= take; if (!l.qlen) l.qhead = 0; }
        LeaveCriticalSection(&l.qcs);
        if (!take) {
            if (InterlockedCompareExchange(&l.wstop, 0, 0)) break;
            WaitForSingleObject(l.qev, 200);
            continue;
        }
        size_t off = 0;
        while (off < take) {
            const int r = send(l.sock, (const char*)block + off, (int)(take - off), 0);
            if (r <= 0) {
                const int e = WSAGetLastError();
                InterlockedExchange(&l.wdead, 1);
                if (!InterlockedCompareExchange(&g.stop, 0, 0) && !InterlockedCompareExchange(&l.wstop, 0, 0))
                    log_line("NET", "!! send to peer %u failed (WSA %d) -- nothing more is written to it", (unsigned)l.id, e);
                shutdown(l.sock, SD_BOTH);
                EnterCriticalSection(&l.qcs); l.qhead = l.qlen = 0; LeaveCriticalSection(&l.qcs);
                free(block);
                return 0;
            }
            off += (size_t)r;
        }
    }
    free(block);
    return 0;
}

// Wait (bounded) until every live link's queue is on the wire: before a deliberate close, so the last frames (HOSTLEFT, a refusal) are not lost with the socket.
void links_drain(DWORD max_ms) {
    const ULONGLONG t0 = GetTickCount64();
    for (;;) {
        bool busy = false;
        for (int i = 0; i < kMaxPeers; ++i) {
            Link& l = g.links[i];
            if (!l.qcs_ready || !InterlockedCompareExchange(&l.live, 0, 0) || InterlockedCompareExchange(&l.wdead, 0, 0)) continue;
            EnterCriticalSection(&l.qcs); if (l.qlen) busy = true; LeaveCriticalSection(&l.qcs);
        }
        if (!busy || GetTickCount64() - t0 >= max_ms) return;
        Sleep(5);
    }
}

// HOST, receive thread of the asking client (proto 98): the kept chunks of that boundary, to that client only; with kBoardReqStream it is sent every later boundary's chunks
// of this battle as well (net_board_publish).
void host_answer_ustatereq(Link& l, const uint8_t* payload, uint32_t plen) {
    UstateReqMsg q{};
    if (!g.cs_ready || l.id >= kMaxPeers || !dec_ustatereq(payload, plen, q)) return;
    uint32_t sent = 0, missing = 0;
    for (uint32_t i = 0; i < q.n; ++i) {
        uint8_t* copy = nullptr; uint32_t len = 0;
        EnterCriticalSection(&g.cs);
        for (uint32_t k = 0; k < kUsKeep && !copy; ++k) {
            const KeptUstate& s = g_us_kept[k];
            if (s.battle == q.battle && s.turn == q.turn && s.blob[q.index[i]]) {
                len = s.len[q.index[i]];
                copy = (uint8_t*)malloc(len);
                if (copy) memcpy(copy, s.blob[q.index[i]], len);
            }
        }
        LeaveCriticalSection(&g.cs);
        if (!copy) { ++missing; continue; }
        if (link_send(l, kHostPeer, copy, len)) ++sent;
        free(copy);
    }
    log_line("USYNC", "-> peer %u asked for %u unit record(s) of turn %u (their deep state differed there): %u sent%s", (unsigned)l.id, (unsigned)q.n, (unsigned)q.turn, sent,
             missing ? " -- some are no longer kept here" : "");
}
// HOST, receive thread (proto 115): the cat image of the unit a client has to make, or "none".
void host_answer_ucatreq(Link& l, const uint8_t* payload, uint32_t plen) {
    UcatReqMsg q{};
    if (!g.cs_ready || !dec_ucatreq(payload, plen, q)) return;
    uint8_t* copy = nullptr; uint32_t len = 0;
    EnterCriticalSection(&g.cs);
    for (uint32_t k = 0; k < kUcatKeep && !copy; ++k) {
        const UcatFrame& f = g_ucat_kept[k];
        if (f.data && f.battle == q.battle && f.index == q.index && f.key == q.key) { copy = (uint8_t*)malloc(f.len); if (copy) { memcpy(copy, f.data, f.len); len = f.len; } }
    }
    LeaveCriticalSection(&g.cs);
    if (copy) {
        const bool ok = link_send(l, kHostPeer, copy, len);
        log_line("UCAT", "-> peer %u asked for the cat data of unit %u (key %08x) of battle %016llx: %u bytes %s", (unsigned)l.id, (unsigned)q.index, (unsigned)q.key,
                 (unsigned long long)q.battle, len, ok ? "sent" : "NOT sent (the link refused it)");
        free(copy);
        return;
    }
    UcatMsg none{};
    none.battle = q.battle; none.index = q.index; none.key = q.key; none.status = kUcatNone;
    uint8_t p[64];
    const uint32_t n = enc_ucat(p, sizeof(p), none);
    if (n) link_send(l, kHostPeer, p, n);
    log_line("UCAT", "-> peer %u asked for the cat data of unit %u (key %08x) of battle %016llx: the host keeps none (answered 'none')", (unsigned)l.id, (unsigned)q.index,
             (unsigned)q.key, (unsigned long long)q.battle);
}
void client_store_ucat(const uint8_t* payload, uint32_t plen) {
    UcatMsg m{};
    if (!g.cs_ready || !dec_ucat(payload, plen, m)) return;
    UcatFrame f{};
    f.battle = m.battle; f.index = m.index; f.key = m.key;
    f.data = (uint8_t*)malloc(plen);
    if (!f.data) return;
    memcpy(f.data, payload, plen); f.len = plen;
    EnterCriticalSection(&g.cs);
    UcatFrame& slot = g_ucat_box[g_ucat_box_next++ % kUcatBox];
    free(slot.data);
    slot = f;
    LeaveCriticalSection(&g.cs);
}
void client_store_tiles(const uint8_t* payload, uint32_t plen) {
    if (!g.cs_ready || plen < 1 + 8 + 4 || plen > kSendTiles) return;
    TilesBox b{};
    memcpy(&b.battle, payload + 1, 8); memcpy(&b.turn, payload + 9, 4);
    b.data = (uint8_t*)malloc(plen);
    if (!b.data) return;
    memcpy(b.data, payload, plen); b.len = plen; b.at = GetTickCount64();
    EnterCriticalSection(&g.cs);
    TilesBox* slot = nullptr;
    for (uint32_t k = 0; k < kTilesBox && !slot; ++k) if (g_tiles_box[k].battle == b.battle && g_tiles_box[k].turn == b.turn) slot = &g_tiles_box[k];
    if (!slot) slot = &g_tiles_box[g_tiles_box_next++ % kTilesBox];
    free(slot->data);
    *slot = b;
    LeaveCriticalSection(&g.cs);
}
void client_store_actcheck(const uint8_t* payload, uint32_t plen) {
    if (!g.cs_ready || plen < 1 + 8 + 4 + 4 + 1 || plen > kSendActCheck) return;
    ActBox b{};
    memcpy(&b.battle, payload + 1, 8); memcpy(&b.turn, payload + 9, 4); memcpy(&b.action, payload + 13, 4);
    b.data = (uint8_t*)malloc(plen);
    if (!b.data) return;
    memcpy(b.data, payload, plen); b.len = plen; b.at = GetTickCount64();
    EnterCriticalSection(&g.cs);
    ActBox* slot = nullptr;
    for (uint32_t k = 0; k < kActBox && !slot; ++k) if (g_act_box[k].data && g_act_box[k].battle == b.battle && g_act_box[k].turn == b.turn && g_act_box[k].action == b.action) slot = &g_act_box[k];
    if (!slot) slot = &g_act_box[g_act_box_next++ % kActBox];
    free(slot->data);
    *slot = b;
    LeaveCriticalSection(&g.cs);
}
void client_store_ustate(const uint8_t* payload, uint32_t plen) {
    if (!g.cs_ready || plen < 1 + 8 + 4 + 1) return;
    UstateBox b{};
    memcpy(&b.battle, payload + 1, 8); memcpy(&b.turn, payload + 9, 4); b.index = payload[13];
    b.data = (uint8_t*)malloc(plen);
    if (!b.data) return;
    memcpy(b.data, payload, plen); b.len = plen;
    EnterCriticalSection(&g.cs);
    b.seq = g_us_box_next + 1;          // 0.1.239: the newest of several records of one unit and boundary is the one read
    b.at = GetTickCount64();
    UstateBox& slot = g_us_box[g_us_box_next++ % kUsBox];
    free(slot.data);
    slot = b;
    LeaveCriticalSection(&g.cs);
}

void host_answer_boardreq(Link& l, const BoardReqMsg& q) {
    if (!g.cs_ready || l.id >= kMaxPeers) return;
    uint8_t* copy = nullptr; uint32_t len = 0;
    EnterCriticalSection(&g.cs);
    if (q.flags & kBoardReqStream) g_board_stream[l.id] = q.battle;
    for (uint32_t k = 0; k < kBoardKeep; ++k) {
        const KeptBoard& kb = g_kept[k];
        if (kb.data && kb.battle == q.battle && kb.turn == q.turn) {
            copy = (uint8_t*)malloc(kb.len);
            if (copy) { memcpy(copy, kb.data, kb.len); len = kb.len; }
            break;
        }
    }
    LeaveCriticalSection(&g.cs);
    if (!copy) {
        log_line("BOARD", "!! peer %u asked for the board of battle %016llx turn %u, which is no longer kept here%s", (unsigned)l.id, (unsigned long long)q.battle, (unsigned)q.turn,
                 (q.flags & kBoardReqStream) ? " (it is streamed every later boundary's board from now on)" : "");
        return;
    }
    uint32_t chunks = 0;
    for (uint32_t at = 0; at + 4 <= len; ) {
        uint32_t n = 0; memcpy(&n, copy + at, 4); at += 4;
        if (!n || at + n > len) break;
        if (link_send(l, kHostPeer, copy + at, n)) ++chunks;
        at += n;
    }
    free(copy);
    log_line("BOARD", "-> peer %u asked for the board of turn %u (its digest differed): %u chunk(s) sent%s", (unsigned)l.id, (unsigned)q.turn, chunks,
             (q.flags & kBoardReqStream) ? " -- and it asked to be sent the whole board at every boundary of this battle from now on" : "");
}

// Stop a link's writer (it drains first) and wait for it. Called before the socket is closed.
void link_writer_stop(Link& l, DWORD max_ms) {
    if (!l.writer) return;
    InterlockedExchange(&l.wstop, 1);
    if (l.qev) SetEvent(l.qev);
    if (WaitForSingleObject(l.writer, max_ms) == WAIT_TIMEOUT) log_line("NET", "!! the writer for peer %u did not stop in %lu ms", (unsigned)l.id, (unsigned long)max_ms);
    CloseHandle(l.writer);
    l.writer = nullptr;
}

// Which messages the host passes on to the other clients.
//
// Only the ones a CLIENT can author and another client needs. Everything else
// is either host-authored and already going to everyone (SAVEFILE, CATDATA,
// INVENTORY, ENTERNODE, PEERS, WELCOME) or strictly point to point (HELLO,
// REFUSE). Relaying a host-authored message would deliver it twice.
bool relayed(uint8_t type) {
    switch (type) {
        case MSG_ACTION:
        case MSG_HASH:
        case MSG_CONTROL:
        case MSG_CURSOR:
        // AIM, like CURSOR, is authored by whichever peer is aiming and is for
        // everyone else to look at.
        case MSG_AIM:
        case MSG_CHOICE:
        // Independent party owners publish their own CATDATA. In a 3/4-player
        // session those snapshots must also reach the other clients; the host
        // never republishes cats owned by someone else.
        case MSG_CATDATA:
        case MSG_HALT:
        // UQD: every peer authors its own digest of each action, for the others to compare
        case MSG_UQD:
        case MSG_RNGL:
        case MSG_DEEP:
        // CHAT: every peer authors its own lines, and the others (in a room of three or four, the other clients too) all show them
        case MSG_CHAT:
        // DRAW: every peer draws its own lines, and every other screen shows them
        case MSG_DRAW:
        // EQUIP: authored by the cat's OWNER, which may be a client -- in a room of three or four the other clients equip the same item or their copy
        // of the cat waits for it forever (2026-10-12 review: it reached the host only, and the next turn hash halted)
        case MSG_EQUIP:
        // SOULCAT (proto 100): authored by the Soul Jar's owner, which may be a client -- every other peer summons from it
        case MSG_SOULCAT:
        // NODEVOTE (proto 106): every player's pick of the next map node, for every other screen
        case MSG_NODEVOTE:
        // VOICE (proto 107): every player's own microphone, for every other player's speakers
        case MSG_VOICE:
        // NODEHASH is symmetric -- every peer authors its own -- so with more
        // than two players a client's has to reach the other clients, exactly
        // like the per-turn HASH above it.
        case MSG_NODEHASH:
        // CATDIGEST is authored by every peer: an owner that is another client
        // must see the digest to heal a cat it is authoritative for.
        case MSG_CATDIGEST:
        // Authored by every peer, for everyone else to display.
        case MSG_PAGE:
        case MSG_CATS:
        // Authored by every peer and read by every peer -- the vote has no arbiter.
        case MSG_ABANDON:
        case MSG_ROOMCTL:
        // DEBUGHIT (developer tools): authored by whichever peer clicks, and every peer must write the same hp or the next turn's hash halts. Without this a client's hit reached the host only,
        // and the second client fought on against enemies the other two had already struck down (2026-10-05, three players).
        case MSG_DEBUGHIT:
        // HOUSECOOP (0.1.237): every player authors its own status, ask, vote and cancel for every other player. Not relayed, a client never heard the other
        // clients in a room of three or four: room KD8E (11-16 21:08) -- MetLouis had the house boss today and waited for "peer 2: no recent status" while that
        // player stood in its warehouse; nobody was ever asked, and both boss days were fought alone.
        case MSG_HOUSECOOP:
            return true;
        default:
            return false;
    }
}

void host_relay(uint8_t from, const uint8_t* payload, uint32_t len) {
    for (int i = 0; i < kMaxPeers; ++i) {
        Link& l = g.links[i];
        if (!InterlockedCompareExchange(&l.live, 0, 0)) continue;
        if (l.id == from) continue;              // not back to its author
        link_send(l, from, payload, len);
    }
}

// Build the current membership and tell everyone, including ourselves.
//
// Each client gets its own copy because `you` differs; the host queues one
// locally so the session layer learns the membership through exactly the same
// path on both sides rather than by reaching into the transport.
//
// Called on the accept thread when a peer arrives and on a link's own thread
// when it leaves. Both are transport threads, never the game thread.
void host_publish_roster() {
    if (g.role != NetRole::Host) return;

    PeersMsg roster{};
    roster.ids[roster.count++] = kHostPeer;
    for (uint8_t want = 0; want < 255 && roster.count < kMaxPeers; ++want) {
        for (int i = 0; i < kMaxPeers; ++i) {
            Link& l = g.links[i];
            if (!InterlockedCompareExchange(&l.live, 0, 0)) continue;
            if (l.id != want || l.id == kHostPeer) continue;
            roster.ids[roster.count++] = l.id;   // ascending by construction
        }
    }

    g.roster = roster;
    g.roster_valid = true;

    for (int i = 0; i < kMaxPeers; ++i) {
        Link& l = g.links[i];
        if (!InterlockedCompareExchange(&l.live, 0, 0)) continue;
        PeersMsg mine = roster;
        mine.you = l.id;
        uint8_t p[kSendPeers];
        uint32_t n = enc_peers(p, sizeof(p), mine);
        if (n) link_send(l, kHostPeer, p, n);
    }

    NetMsg self{};
    self.type      = MSG_PEERS;
    self.from      = kHostPeer;
    self.peers     = roster;
    self.peers.you = kHostPeer;
    queue_push(self);
}

// The receive loop for ONE link. Owns that link's socket for its lifetime.
void recv_loop(Link& link) {
    // Sized for MSG_SAVEFILE, which is the only frame that is not a few hundred
    // bytes. Heap rather than stack: a receive thread's stack is not the place
    // for a megabyte, and now there is one of these per peer, so it cannot be
    // `static` any more either.
    uint8_t* buf = (uint8_t*)malloc(kMaxFrame);
    if (!buf) { log_line("NET", "!! could not allocate a receive buffer"); return; }

    for (;;) {
        if (InterlockedCompareExchange(&g.stop, 0, 0)) break;

        uint32_t len = 0;
        if (!recv_exact(link.sock, (uint8_t*)&len, 4)) break;
        if (len < kEnvelopeBytes || len > kMaxFrame) {
            log_line("NET", "!! bad frame length %u from peer %u -- closing",
                     len, (unsigned)link.id);
            break;
        }
        if (!recv_exact(link.sock, buf, len)) break;

        uint8_t         from    = buf[0];
        const uint8_t*  payload = buf + kEnvelopeBytes;
        uint32_t        plen    = len - kEnvelopeBytes;
        if (plen == 0) continue;                 // envelope with no message

        // A client may only speak for itself. Believing a forged `from` would
        // let one peer inject another's decisions, so the host overwrites it
        // with the id it handed that socket rather than trusting what arrived.
        if (g.role == NetRole::Host) from = link.id;

        // Relay BEFORE decoding and queuing. Decoding can allocate and the game
        // thread may be mid-frame; the other clients should not wait on either.
        if (g.role == NetRole::Host && relayed(payload[0]))
            host_relay(from, payload, plen);

        // proto 113 (mgmp_unitstate): a unit-record request is answered here, from the kept records; a record is put in the mailbox. Neither goes through the game thread.
        if (payload[0] == MSG_USTATEREQ) { if (g.role == NetRole::Host) host_answer_ustatereq(link, payload, plen); continue; }
        if (payload[0] == MSG_USTATE)    { if (g.role != NetRole::Host) client_store_ustate(payload, plen); continue; }
        // proto 115: a unit's cat data, asked for and answered the same way
        if (payload[0] == MSG_UCATREQ)   { if (g.role == NetRole::Host) host_answer_ucatreq(link, payload, plen); continue; }
        if (payload[0] == MSG_UCAT)      { if (g.role != NetRole::Host) client_store_ucat(payload, plen); continue; }
        // proto 116: the host's ground map of a boundary, a mailbox like the unit records
        if (payload[0] == MSG_TILES)     { if (g.role != NetRole::Host) client_store_tiles(payload, plen); continue; }
        // proto 117: the host's per-action check, a mailbox too
        if (payload[0] == MSG_ACTCHECK)  { if (g.role != NetRole::Host) client_store_actcheck(payload, plen); continue; }

        NetMsg m{};
        if (!decode_into(payload, plen, m)) {
            // A PREVIEW IS NOT WORTH A CONNECTION (2026-10-20, a client's cat swallowed by the Spewer: its aim carried a direction outside the
            // bounds, and the host closed the link -- the client "dropped out" every time it chose to struggle). The aim and the pointer only
            // draw something; one that cannot be read is dropped. Everything else still closes: a broken game message is a broken session.
            if (payload[0] == MSG_AIM || payload[0] == MSG_CURSOR) {
                static ULONGLONG said = 0;
                if (GetTickCount64() - said > 10000) { said = GetTickCount64(); log_line("NET", "!! undecodable %s frame from peer %u, %u bytes -- dropped (a preview only)", msg_name(payload[0]), (unsigned)from, plen); }
                continue;
            }
            log_line("NET", "!! undecodable %s frame from peer %u, %u bytes -- closing",
                     msg_name(payload[0]), (unsigned)from, plen);
            break;
        }
        m.from = from;
        g.stats.bytes_received += len + 4;
        // proto 98: a client asking for a boundary's full board is answered HERE, from the kept chunks -- not through the game thread, which may be busy or parked.
        if (m.type == MSG_BOARDREQ) {
            if (g.role == NetRole::Host) host_answer_boardreq(link, m.boardreq);
            continue;
        }
        queue_push(m);
    }

    free(buf);

    InterlockedExchange(&link.live, 0);
    // the writer first: shut the socket down (a writer blocked in send() returns), let it go, and only then close the handle it uses
    if (link.sock != INVALID_SOCKET) shutdown(link.sock, SD_BOTH);
    link_writer_stop(link, 2000);
    if (link.sock != INVALID_SOCKET) { closesocket(link.sock); link.sock = INVALID_SOCKET; }

    if (!InterlockedCompareExchange(&g.stop, 0, 0)) {
        log_line("NET", "peer %u disconnected", (unsigned)link.id);
        if (g.role == NetRole::Host) {
            // One client leaving is not the end of the session for the others.
            // Whether the RUN can continue is the session layer's call; the
            // transport only reports the membership change.
            host_publish_roster();
        } else {
            set_state(NetState::Closed);
        }
    }
}

DWORD WINAPI link_thread(LPVOID param) {
    recv_loop(*(Link*)param);
    return 0;
}

void nodelay_on(SOCKET s) {
    // Turn off Nagle. A turn-based game sends one small message and then waits
    // for the reply, which is precisely the pattern Nagle's 200 ms delayed-ACK
    // interaction punishes hardest -- it would add latency to every decision
    // for no throughput gain.
    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
}

// A LINK IS WAITED FOR, NOT CLOSED (0.1.184, see mgmp_steambridge kWaitTimeoutMs). Windows gives up on a TCP connection whose data goes unacknowledged
// after its retransmission limit (some tens of seconds); TCP_MAXRT -1 retransmits for as long as the socket lives (a day where -1 is refused).
void wait_forever(SOCKET s) {
#ifndef TCP_MAXRT
#define TCP_MAXRT 5
#endif
    static LONG said = 0;
    DWORD v = (DWORD)-1;
    bool ok = setsockopt(s, IPPROTO_TCP, TCP_MAXRT, (const char*)&v, sizeof(v)) == 0;
    if (!ok) { v = 24 * 3600; ok = setsockopt(s, IPPROTO_TCP, TCP_MAXRT, (const char*)&v, sizeof(v)) == 0; }
    if (!InterlockedExchange(&said, 1))
        log_line("NET", "a link whose other end goes quiet is waited for, not closed: TCP retransmits %s", !ok ? "with Windows' default limit (TCP_MAXRT refused)" : v == (DWORD)-1 ? "without a time limit" : "for a day");
}

bool start_link(int slot, SOCKET s, uint8_t id) {
    Link& l = g.links[slot];
    wait_forever(s);
    l.sock = s;
    l.id   = id;
    // the outgoing queue of a fresh link: empty, with its own writer (a slot reused after a peer left starts clean; the old writer was stopped by its receive thread)
    if (!l.qcs_ready) { InitializeCriticalSection(&l.qcs); l.qcs_ready = true; }
    if (!l.qev) l.qev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (l.writer) link_writer_stop(l, 2000);
    EnterCriticalSection(&l.qcs); l.qhead = l.qlen = 0; LeaveCriticalSection(&l.qcs);
    InterlockedExchange(&l.wstop, 0);
    InterlockedExchange(&l.wdead, 0);
    l.writer = l.qev ? CreateThread(nullptr, 0, link_writer, &l, 0, nullptr) : nullptr;
    if (!l.writer) {
        closesocket(s);
        l.sock = INVALID_SOCKET;
        fail("CreateThread (writer)", (int)GetLastError());
        return false;
    }
    InterlockedExchange(&l.live, 1);
    nodelay_on(s);
    l.thread = CreateThread(nullptr, 0, link_thread, &l, 0, nullptr);
    if (!l.thread) {
        InterlockedExchange(&l.live, 0);
        shutdown(s, SD_BOTH);
        link_writer_stop(l, 2000);
        closesocket(s);
        l.sock = INVALID_SOCKET;
        fail("CreateThread", (int)GetLastError());
        return false;
    }
    return true;
}

DWORD WINAPI accept_thread(LPVOID) {
    set_state(NetState::Listening);
    log_line("NET", "listening for up to %u peer(s) on the host", (unsigned)(kMaxPeers - 1));

    for (;;) {
        if (InterlockedCompareExchange(&g.stop, 0, 0)) break;

        SOCKET c = accept(g.listener, nullptr, nullptr);
        if (c == INVALID_SOCKET) {
            if (!InterlockedCompareExchange(&g.stop, 0, 0))
                fail("accept", WSAGetLastError());
            break;
        }

        // Lowest free id rather than a monotonic counter, so an id is always
        // below kMaxPeers and every per-peer table in the mod can be a flat
        // array indexed by it. Reuse is safe because the roster is republished
        // on every membership change, and because position -- not id -- is what
        // the control split is derived from.
        uint8_t id = kNoPeer;
        for (uint8_t cand = kHostPeer + 1; cand < kMaxPeers; ++cand) {
            if (link_slot_of(cand) < 0) { id = cand; break; }
        }

        int slot = free_link_slot();
        if (slot < 0 || id == kNoPeer) {
            // Refuse politely rather than dropping the socket: a player who
            // turns up to a full session should be told, not left staring at a
            // connect that appears to succeed and then does nothing.
            uint8_t p[kSendRefuseFull];
            uint32_t n = enc_refuse(p, sizeof(p), "session is full");
            if (n) send_framed(c, kHostPeer, p, n);
            log_line("NET", "!! refused a connection -- the session already has "
                            "%u peer(s), the maximum", (unsigned)kMaxPeers);
            closesocket(c);
            continue;
        }

        if (!start_link(slot, c, id)) continue;

        log_line("NET", "peer %u connected", (unsigned)id);
        set_state(NetState::Connected);
        host_publish_roster();
    }
    return 0;
}

void ensure_cs() {
    if (!g.cs_ready)      { InitializeCriticalSection(&g.cs);      g.cs_ready = true; }
    if (!g.send_cs_ready) { InitializeCriticalSection(&g.send_cs); g.send_cs_ready = true; }
}

} // namespace

// ---------------------------------------------------------------------------

bool net_host(uint16_t port) {
    if (g.role != NetRole::None) return false;
    ensure_cs();
    if (!wsa_init()) return false;

    g.listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g.listener == INVALID_SOCKET) { fail("socket", WSAGetLastError()); return false; }

    // SO_REUSEADDR so a crashed session's TIME_WAIT does not lock the port for
    // the next run. During desync hunting the game gets restarted constantly.
    BOOL reuse = TRUE;
    setsockopt(g.listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port        = htons(port);
    if (bind(g.listener, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) {
        fail("bind", WSAGetLastError()); closesocket(g.listener);
        g.listener = INVALID_SOCKET; return false;
    }
    if (listen(g.listener, kMaxPeers) == SOCKET_ERROR) {
        fail("listen", WSAGetLastError()); closesocket(g.listener);
        g.listener = INVALID_SOCKET; return false;
    }

    g.role = NetRole::Host;
    g.self = kHostPeer;
    log_line("NET", "hosting on port %u", (unsigned)port);

    InterlockedExchange(&g.stop, 0);
    g.accept_thread = CreateThread(nullptr, 0, accept_thread, nullptr, 0, nullptr);
    if (!g.accept_thread) { fail("CreateThread", (int)GetLastError()); return false; }
    return true;
}

// --- dialling the host ------------------------------------------------------------------------------------------------
//
// OFF THE GAME THREAD, with a timeout per address, and over a LIST of addresses. A blocking connect() to a host that does not
// answer (a router that does not forward the port, a firewall that drops) holds the frame for twenty seconds; the lobby may
// now hand several candidates -- the host's private address first for a player behind the same router, then the others -- and
// the first that answers wins. net_join returns at once with the state Connecting; the thread leaves it Connected, or Failed
// with the last error, and bumps net_dial_seq so the menu can say so.

constexpr DWORD kDialMs = 5000;     // per address
constexpr int   kMaxCands = 6;
constexpr DWORD kSteamDialMs = 15000;   // a Steam connection may need to find a route or a relay
constexpr DWORD kSteamRevMs = 15000;    // how long a joiner waits for the host to call it back after its own dial was refused
bool (*g_steam_reverse)(uint64_t host_steamid) = nullptr;

struct DialJob {
    char     cands[kMaxCands][64] = {};
    int      n = 0;
    uint16_t port = 0;
};
DialJob g_dial;

// --- the server as the carrier ------------------------------------------------------------------------------------------------
// The lobby server pairs two connections ("pipe") and then moves their bytes unchanged. The joiner's end IS the game socket (the same protocol
// as a direct TCP link); the host's end is bridged to its own game port, the way the Steam bridge does it.
int dial_one(const char* addr, uint16_t port, SOCKET& out);   // below
namespace {
char     g_rl_host[64] = {};
uint16_t g_rl_port = 0;
volatile LONG g_rl_gen = 0;

// A connection to the relay server (a name is resolved). 0 = connected (blocking socket in `out`), else a WSA error.
int rl_connect(SOCKET& out, volatile LONG* stop, LONG gen) {
    out = INVALID_SOCKET;
    if (!g_rl_host[0] || !g_rl_port) return WSAEINVAL;
    char ip[64] = {};
    in_addr tmp{};
    if (inet_pton(AF_INET, g_rl_host, &tmp) == 1) {
        strncpy_s(ip, sizeof(ip), g_rl_host, _TRUNCATE);
    } else {
        addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if ((getaddrinfo(g_rl_host, nullptr, &hints, &res) != 0 || !res) &&
            (!official_fallback(g_rl_host) || getaddrinfo(official_fallback(g_rl_host), nullptr, &hints, &res) != 0 || !res)) return WSAHOST_NOT_FOUND;
        inet_ntop(AF_INET, &((sockaddr_in*)res->ai_addr)->sin_addr, ip, sizeof(ip));
        freeaddrinfo(res);
    }
    (void)stop; (void)gen;
    const int e = dial_one(ip, g_rl_port, out);
    if (e == 0 && out != INVALID_SOCKET) wait_forever(out);     // the relay pipe's own hop (0.1.184)
    return e;
}

// One line (up to 300 bytes), read byte by byte so that nothing after it is consumed. False on a timeout, a stop or a closed connection.
bool rl_read_line(SOCKET s, char* out, size_t cap, DWORD timeout_ms, volatile LONG* stop, LONG gen) {
    size_t n = 0;
    const DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < timeout_ms) {
        if ((stop && InterlockedCompareExchange(stop, 0, 0)) || InterlockedCompareExchange(&g_rl_gen, 0, 0) != gen) return false;
        fd_set rd; FD_ZERO(&rd); FD_SET(s, &rd);
        timeval tv{ 0, 100000 };
        const int r = select(0, &rd, nullptr, nullptr, &tv);
        if (r < 0) return false;
        if (r == 0) continue;
        char ch;
        if (recv(s, &ch, 1, 0) != 1) return false;
        if (ch == '\n') { out[n] = 0; return true; }
        if (n + 1 < cap) out[n++] = ch;
    }
    return false;
}

bool rl_send_line(SOCKET s, const char* line) {
    const int len = (int)strlen(line);
    return send(s, line, len, 0) == len;
}

// Joiner: connect, ask for a pipe, wait for the host's side. The returned socket carries the game protocol from its first byte.
SOCKET rl_dial(const char* tok, int* why) {
    *why = 0;
    SOCKET s = INVALID_SOCKET;
    const LONG gen = InterlockedCompareExchange(&g_rl_gen, 0, 0);
    const int e = rl_connect(s, &g.stop, gen);
    if (e != 0) { *why = e; return INVALID_SOCKET; }
    char msg[96];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE, "{\"t\":\"pipe\",\"tok\":\"%s\"}\n", tok);
    char line[320] = {};
    if (!rl_send_line(s, msg) || !rl_read_line(s, line, sizeof(line), 20000, &g.stop, gen)) {
        closesocket(s); *why = WSAETIMEDOUT; return INVALID_SOCKET;
    }
    if (!strstr(line, "\"piped\"")) {
        log_line("NET", "!! the relay refused the pipe: %.200s", line);
        closesocket(s); *why = WSAECONNREFUSED; return INVALID_SOCKET;
    }
    BOOL on = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof(on));
    return s;
}

struct RlServe { char tok[24]; uint32_t id; uint16_t port; LONG gen; };

DWORD WINAPI rl_serve_thread(LPVOID p) {
    RlServe job = *(RlServe*)p;
    delete (RlServe*)p;
    SOCKET a = INVALID_SOCKET, b = INVALID_SOCKET;
    auto done = [&](const char* why) {
        if (a != INVALID_SOCKET) closesocket(a);
        if (b != INVALID_SOCKET) closesocket(b);
        log_line("NET", "relay pipe %u closed (%s)", (unsigned)job.id, why);
    };
    const int e = rl_connect(a, nullptr, job.gen);
    if (e != 0) { a = INVALID_SOCKET; done("could not reach the server"); return 0; }
    char msg[112];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE, "{\"t\":\"pipe\",\"tok\":\"%s\",\"id\":%u}\n", job.tok, (unsigned)job.id);
    char line[320] = {};
    if (!rl_send_line(a, msg) || !rl_read_line(a, line, sizeof(line), 15000, nullptr, job.gen) || !strstr(line, "\"piped\"")) { done("the server did not pair it"); return 0; }
    // the joiner is on the other end: connect to the game's own port
    b = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in sa{};
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); sa.sin_port = htons(job.port);
    if (b == INVALID_SOCKET || connect(b, (sockaddr*)&sa, sizeof(sa)) != 0) { if (b != INVALID_SOCKET) { closesocket(b); b = INVALID_SOCKET; } done("the game is not listening"); return 0; }
    BOOL on = TRUE;
    setsockopt(a, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof(on));
    setsockopt(b, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof(on));
    log_line("NET", "relay pipe %u up", (unsigned)job.id);
    static const int kBuf = 16384;
    char* buf = new char[kBuf];
    const char* why = "ended";
    for (;;) {
        if (InterlockedCompareExchange(&g_rl_gen, 0, 0) != job.gen) { why = "the room ended"; break; }
        fd_set rd; FD_ZERO(&rd); FD_SET(a, &rd); FD_SET(b, &rd);
        timeval tv{ 0, 100000 };
        const int r = select(0, &rd, nullptr, nullptr, &tv);
        if (r < 0) { why = "select failed"; break; }
        if (r == 0) continue;
        bool dead = false;
        for (int side = 0; side < 2 && !dead; ++side) {
            SOCKET from = side ? b : a, to = side ? a : b;
            if (!FD_ISSET(from, &rd)) continue;
            const int n = recv(from, buf, kBuf, 0);
            if (n <= 0) { dead = true; why = side ? "the game closed it" : "the joiner left"; break; }
            for (int off = 0; off < n;) {
                const int w = send(to, buf + off, n - off, 0);
                if (w <= 0) { dead = true; why = "send failed"; break; }
                off += w;
            }
        }
        if (dead) break;
    }
    delete[] buf;
    done(why);
    return 0;
}

} // namespace

void net_relay_set_server(const char* host, uint16_t port) {
    strncpy_s(g_rl_host, sizeof(g_rl_host), host ? host : "", _TRUNCATE);
    g_rl_port = port;
}

bool net_relay_serve(const char* host_token, uint32_t id, uint16_t game_port) {
    if (!wsa_init() || !host_token || !host_token[0] || strlen(host_token) >= 24) return false;
    RlServe* job = new RlServe{};
    strncpy_s(job->tok, sizeof(job->tok), host_token, _TRUNCATE);
    job->id = id; job->port = game_port;
    job->gen = InterlockedCompareExchange(&g_rl_gen, 0, 0);
    HANDLE h = CreateThread(nullptr, 0, rl_serve_thread, job, 0, nullptr);
    if (!h) { delete job; return false; }
    CloseHandle(h);
    return true;
}

void net_relay_stop() { InterlockedIncrement(&g_rl_gen); }

HANDLE  g_dial_thread = nullptr;
volatile LONG g_dial_seq = 0;
volatile LONG g_dial_err = 0;
volatile LONG g_dial_steam_failed = 0;   // 0.1.193: see net_dial_steam_failed
volatile LONG g_dial_steam_tries = 0;    // 0.1.219: Steam calls of this dial that failed (net_dial_steam_status)
volatile LONG g_dial_steam_back  = 0;    // 0.1.219: the dial waits for the host's call-back

// One address: a non-blocking connect that is waited for in 100 ms slices (so a shutdown is noticed). 0 = connected, else a WSA error.
int dial_one(const char* addr, uint16_t port, SOCKET& out) {
    out = INVALID_SOCKET;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return WSAGetLastError();
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    if (inet_pton(AF_INET, addr, &a.sin_addr) != 1) { closesocket(s); return WSAEINVAL; }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    int err = 0;
    if (connect(s, (sockaddr*)&a, sizeof(a)) == 0) {
        err = 0;
    } else if (WSAGetLastError() != WSAEWOULDBLOCK) {
        err = WSAGetLastError();
    } else {
        err = WSAETIMEDOUT;
        const DWORD t0 = GetTickCount();
        while (!InterlockedCompareExchange(&g.stop, 0, 0) && GetTickCount() - t0 < kDialMs) {
            fd_set wr, ex;
            FD_ZERO(&wr); FD_ZERO(&ex);
            FD_SET(s, &wr); FD_SET(s, &ex);
            timeval tv{ 0, 100000 };
            if (select(0, nullptr, &wr, &ex, &tv) <= 0) continue;
            int so = 0; int sl = sizeof(so);
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&so, &sl);
            err = (FD_ISSET(s, &wr) && so == 0) ? 0 : (so ? so : WSAECONNREFUSED);
            break;
        }
    }
    if (err != 0) { closesocket(s); return err; }
    u_long blocking = 0;
    ioctlsocket(s, FIONBIO, &blocking);       // the link threads use plain blocking recv
    out = s;
    return 0;
}

DWORD WINAPI dial_thread(LPVOID) {
    int last = WSAETIMEDOUT;
    InterlockedExchange(&g_dial_steam_failed, 0);
    InterlockedExchange(&g_dial_steam_tries, 0);
    InterlockedExchange(&g_dial_steam_back, 0);
    for (int i = 0; i < g_dial.n && !InterlockedCompareExchange(&g.stop, 0, 0); ++i) {
        log_line("NET", "connecting to %s:%u (%d of %d)", g_dial.cands[i], (unsigned)g_dial.port, i + 1, g_dial.n);
        SOCKET s = INVALID_SOCKET;
        int e = 0;
        if (!strncmp(g_dial.cands[i], "steam:", 6)) {
            // The same TCP link, carried by Steam: the bridge hands back a loopback socket that is pumped to the host's Steam
            // connection (see mgmp_steambridge.h).
            // A Steam connection that is refused straight away is usually the host's side not being ready yet (a room that was just rebuilt): try a
            // couple more times before giving the address up.
            int why = 0;
            for (int attempt = 0; attempt < 3; ++attempt) {
                if (attempt) {
                    log_line("NET", "Steam connection failed -- trying again (%d of 3)", attempt + 1);
                    for (int w = 0; w < 30 && !InterlockedCompareExchange(&g.stop, 0, 0); ++w) Sleep(50);
                }
                s = steam_bridge_dial(strtoull(g_dial.cands[i] + 6, nullptr, 10), kSteamDialMs, &g.stop, &why);
                if (s == INVALID_SOCKET) InterlockedIncrement(&g_dial_steam_tries);
                if (s != INVALID_SOCKET || why == 1 || why == 2 || InterlockedCompareExchange(&g.stop, 0, 0)) break;
            }
            if (s == INVALID_SOCKET && why == 3 && g_steam_reverse && !InterlockedCompareExchange(&g.stop, 0, 0)) {
                // REVERSE DIALLING. Steam refused our call (2026-10-02: "Bad cert: CA key ... is not known to us" -- the host's Steam client could not accept our newer
                // certificate). The check is asymmetric, so ask the HOST to dial US: it presents its own certificate, which we can verify.
                int rf = 0; char dbg[132] = {};
                steam_bridge_last_failure(&rf, dbg, sizeof(dbg));
                log_line("NET", "Steam refused the call (end reason %d '%s') -- asking the host to call us back over Steam", rf, dbg);
                const uint64_t host_id = strtoull(g_dial.cands[i] + 6, nullptr, 10);
                if (steam_bridge_rev_arm()) {
                    if (g_steam_reverse(host_id)) {
                        int rw = 0;
                        InterlockedExchange(&g_dial_steam_back, 1);
                        s = steam_bridge_rev_wait(kSteamRevMs, &g.stop, &rw);
                        InterlockedExchange(&g_dial_steam_back, 0);
                        if (s == INVALID_SOCKET) InterlockedIncrement(&g_dial_steam_tries);
                        log_line("NET", s != INVALID_SOCKET ? "the host's call came through" : "!! the host did not call back in time (%d)", rw);
                        if (s == INVALID_SOCKET) why = rw == 2 ? 2 : 3;
                    } else {
                        steam_bridge_rev_cancel();
                        log_line("NET", "!! the request to call back could not be sent");
                    }
                }
            }
            e = s != INVALID_SOCKET ? 0 : (why == 2 ? WSAETIMEDOUT : WSAECONNREFUSED);
            if (s == INVALID_SOCKET) InterlockedExchange(&g_dial_steam_failed, 1);
        } else if (!strncmp(g_dial.cands[i], "relay:", 6)) {
            // The same TCP link, carried by the lobby server (the host chose "server relay"): the socket the server hands over IS the game socket.
            int why = 0;
            s = rl_dial(g_dial.cands[i] + 6, &why);
            e = s != INVALID_SOCKET ? 0 : (why ? why : WSAECONNREFUSED);
        } else {
            e = dial_one(g_dial.cands[i], g_dial.port, s);
        }
        if (e == 0) {
            g.self = kNoPeer;     // our own id is not known until PEERS arrives; the host stamps `from` regardless
            set_state(NetState::Connected);
            log_line("NET", "connected to %s", g_dial.cands[i]);
            start_link(0, s, kHostPeer);
            return 0;
        }
        last = e;
        log_line("NET", "!! %s:%u did not answer (WSA %d)", g_dial.cands[i], (unsigned)g_dial.port, e);
    }
    if (InterlockedCompareExchange(&g.stop, 0, 0)) return 0;      // shut down while dialling: not a failure
    InterlockedExchange(&g_dial_err, last);
    fail("connect", last);
    InterlockedIncrement(&g_dial_seq);
    return 0;
}

void net_set_steam_reverse_hook(bool (*hook)(uint64_t host_steamid)) { g_steam_reverse = hook; }

// `addr` is one address or a comma-separated list of up to four, in the order to try them.
bool net_join(const char* addr, uint16_t port) {
    if (g.role != NetRole::None) return false;
    ensure_cs();
    if (!wsa_init()) return false;

    g_dial = DialJob{};
    g_dial.port = port;
    for (const char* p = addr ? addr : ""; *p && g_dial.n < kMaxCands;) {
        const char* e = strchr(p, ',');
        const size_t len = e ? (size_t)(e - p) : strlen(p);
        char one[64] = {};
        if (len > 0 && len < sizeof(one)) {
            memcpy(one, p, len);
            in_addr tmp{};
            bool steam_ok = !strncmp(one, "steam:", 6) && len > 6;
            for (size_t k = 6; steam_ok && k < len; ++k) if (one[k] < '0' || one[k] > '9') steam_ok = false;
            bool relay_ok = !strncmp(one, "relay:", 6) && len > 6;
            for (size_t k = 6; relay_ok && k < len; ++k) if (!isxdigit((unsigned char)one[k])) relay_ok = false;
            if (steam_ok || relay_ok || inet_pton(AF_INET, one, &tmp) == 1) {
                bool dup = false;
                for (int i = 0; i < g_dial.n; ++i) if (!strcmp(g_dial.cands[i], one)) dup = true;
                if (!dup) strncpy_s(g_dial.cands[g_dial.n++], sizeof(g_dial.cands[0]), one, _TRUNCATE);
            }
        }
        if (!e) break;
        p = e + 1;
    }
    if (g_dial.n == 0) {
        _snprintf_s(g.error, sizeof(g.error), _TRUNCATE, "bad address '%s'", addr ? addr : "");
        set_state(NetState::Failed);
        log_line("NET", "!! %s", g.error);
        return false;
    }

    g.role = NetRole::Client;
    g.self = kNoPeer;
    InterlockedExchange(&g.stop, 0);
    set_state(NetState::Connecting);
    g_dial_thread = CreateThread(nullptr, 0, dial_thread, nullptr, 0, nullptr);
    if (!g_dial_thread) {
        g.role = NetRole::None;
        fail("CreateThread", (int)GetLastError());
        return false;
    }
    return true;
}

uint32_t net_dial_seq() { return (uint32_t)InterlockedCompareExchange(&g_dial_seq, 0, 0); }
bool net_dial_steam_failed() { return InterlockedCompareExchange(&g_dial_steam_failed, 0, 0) != 0; }
void net_dial_steam_status(uint32_t* failures, bool* calling_back) {
    if (failures) *failures = (uint32_t)InterlockedCompareExchange(&g_dial_steam_tries, 0, 0);
    if (calling_back) *calling_back = InterlockedCompareExchange(&g_dial_steam_back, 0, 0) != 0;
}
int      net_dial_error() { return (int)InterlockedCompareExchange(&g_dial_err, 0, 0); }

// The transport learns our own id from the same PEERS message the session layer
// sees; net_poll calls this so the two can never disagree.
void net_note_self(uint8_t id) { g.self = id; }
uint8_t net_self() { return g.self; }
uint8_t net_peer_count() { return g.roster_valid ? g.roster.count : 0; }

// Our INDEX in the sorted membership list, not our id.
//
// The two differ once anyone has disconnected: ids are never reused, so a
// session that loses peer 1 keeps ids {0,2,3} while the positions stay {0,1,2}.
// The control split is computed from position precisely so that the departure
// of one player does not silently hand another player somebody else's cats.
// The members in seat order: the seated ones that are still here, in the seat order, then the rest as the roster has them (ascending ids).
static uint8_t ordered_members(uint8_t* out) {
    if (!g.roster_valid) return 0;
    const uint8_t n = g.roster.count < kMaxPeers ? g.roster.count : (uint8_t)kMaxPeers;
    bool taken[kMaxPeers] = {};
    uint8_t k = 0;
    for (uint8_t s = 0; s < g.seat_count; ++s)
        for (uint8_t i = 0; i < n; ++i)
            if (!taken[i] && g.roster.ids[i] == g.seat_ids[s]) { out[k++] = g.roster.ids[i]; taken[i] = true; }
    for (uint8_t i = 0; i < n; ++i) if (!taken[i]) out[k++] = g.roster.ids[i];
    return k;
}

// The place among the members who are HERE (host 0, then the others in seat order): 0..count-1. Only what has to split something evenly over the present players
// wants this; a session cat's id carries the SEAT (net_peer_pos), which must not move when somebody leaves.
uint8_t net_peer_rank() {
    uint8_t ids[kMaxPeers] = {};
    const uint8_t n = ordered_members(ids);
    for (uint8_t i = 0; i < n; ++i)
        if (ids[i] == g.self) return i;
    return 0;
}

// THE SEAT, STABLE (0.1.115; room 3KGX 2026-10-26, three players: the one in seat 1 dropped out and the player in seat 2 became "position 1" -- the dropped player's cats
// (71......) were taken for its own, its real cats (72......) were taken out of the run "as another player's", and the settlement brought the OTHER player's cats home).
// The index in the seat order the checkpoint fixed (net_set_seat_order), whoever has left since; before a seat order exists, the place among the present members.
uint8_t net_peer_pos() {
    if (g.roster_valid && g.seat_count)
        for (uint8_t s = 0; s < g.seat_count; ++s)
            if (g.seat_ids[s] == g.self) return s;
    return net_peer_rank();
}

// How many positions exist: the seats of the session, or the players present when there is no seat order. A position below this may belong to somebody who has left.
uint8_t net_seat_span() {
    const uint8_t present = g.roster_valid ? (g.roster.count < kMaxPeers ? g.roster.count : (uint8_t)kMaxPeers) : 0;
    return g.seat_count > present ? g.seat_count : present;
}

// WHO HOLDS A SEAT (0.1.119). A seat is the index in the seat order the checkpoint fixed from the players' identities (Steam id / install fingerprint), so "seat s" names a person for the whole run; the peer id
// is only the connection that person has now. Without a seat order the place among the present members stands in for it.
bool net_seat_peer(uint8_t pos, uint8_t& id) {
    if (!g.roster_valid) return false;
    uint8_t ids[kMaxPeers] = {};
    const uint8_t n = ordered_members(ids);
    if (g.seat_count) {
        if (pos >= g.seat_count) return false;
        for (uint8_t i = 0; i < n; ++i) if (ids[i] == g.seat_ids[pos]) { id = ids[i]; return true; }
        return false;
    }
    if (pos >= n) return false;
    id = ids[pos];
    return true;
}

// The seats whose player has left the room. Level, not edge: it needs no memory of who was here when, so a player who left before anybody looked is found just the same.
uint32_t net_absent_seats() {
    if (!g.roster_valid || !g.seat_count) return 0;
    uint32_t mask = 0;
    for (uint8_t s = 0; s < g.seat_count && s < 32; ++s) {
        bool here = false;
        for (uint8_t i = 0; i < g.roster.count && i < kMaxPeers; ++i) if (g.roster.ids[i] == g.seat_ids[s]) here = true;
        if (!here) mask |= 1u << s;
    }
    return mask;
}

bool net_peer_ids(uint8_t* out, uint8_t cap) {
    if (!g.roster_valid || !out) return false;
    uint8_t ids[kMaxPeers] = {};
    const uint8_t n = ordered_members(ids);
    for (uint8_t i = 0; i < n && i < cap; ++i) out[i] = ids[i];
    return true;
}

void net_set_seat_order(const uint8_t* ids, uint8_t n) {
    if (!ids || n > kMaxPeers || (n && ids[0] != kHostPeer)) return;
    uint8_t was[kMaxPeers] = {}; const uint8_t had = g.seat_count;
    memcpy(was, g.seat_ids, had);
    memcpy(g.seat_ids, ids, n); g.seat_count = n;
    if (had != n || memcmp(was, ids, n) != 0) {
        char text[64] = {}; size_t at = 0;
        for (uint8_t i = 0; i < n && at + 6 < sizeof(text); ++i) at += _snprintf_s(text + at, sizeof(text) - at, _TRUNCATE, "%s%u", i ? "," : "", (unsigned)ids[i]);
        log_line("NET", "seat order set: positions are peers [%s] (host first, then by player id), position here %u", text, (unsigned)net_peer_pos());
    }
}

void net_shutdown() {
    if (g.role == NetRole::None) return;
    // What is still queued goes out first (the last frames before a deliberate close -- HOSTLEFT, a refusal -- used to be on the wire by the time send() returned).
    links_drain(1500);
    InterlockedExchange(&g.stop, 1);

    // Shut the sockets down before waiting: every receive thread is parked in
    // recv() and will not notice a flag until the call returns. Closing the
    // listener is what releases the accept thread. A link's socket is CLOSED by its
    // receive thread once its writer has stopped (or below, for one that did not exit).
    if (g.listener != INVALID_SOCKET) { closesocket(g.listener); g.listener = INVALID_SOCKET; }
    for (int i = 0; i < kMaxPeers; ++i) {
        Link& l = g.links[i];
        if (l.sock != INVALID_SOCKET) shutdown(l.sock, SD_BOTH);
    }

    if (g_dial_thread) {
        if (WaitForSingleObject(g_dial_thread, 3000) == WAIT_TIMEOUT)
            log_line("NET", "!! dial thread did not exit in 3 s");
        CloseHandle(g_dial_thread);
        g_dial_thread = nullptr;
    }
    if (g.accept_thread) {
        if (WaitForSingleObject(g.accept_thread, 2000) == WAIT_TIMEOUT)
            log_line("NET", "!! accept thread did not exit in 2 s");
        CloseHandle(g.accept_thread);
        g.accept_thread = nullptr;
    }
    for (int i = 0; i < kMaxPeers; ++i) {
        Link& l = g.links[i];
        if (!l.thread) continue;
        if (WaitForSingleObject(l.thread, 2000) == WAIT_TIMEOUT)
            log_line("NET", "!! receive thread for peer %u did not exit in 2 s",
                     (unsigned)l.id);
        CloseHandle(l.thread);
        l.thread = nullptr;
        InterlockedExchange(&l.live, 0);
    }
    for (int i = 0; i < kMaxPeers; ++i) {
        Link& l = g.links[i];
        link_writer_stop(l, 1000);
        if (l.sock != INVALID_SOCKET) { closesocket(l.sock); l.sock = INVALID_SOCKET; }
        if (l.qcs_ready) { EnterCriticalSection(&l.qcs); l.qhead = l.qlen = 0; LeaveCriticalSection(&l.qcs); }
    }

    NetStats s = net_stats();
    log_line("NET", "closed: %u sent / %u received / %u dropped, %llu B out / %llu B in",
             s.sent, s.received, s.dropped,
             (unsigned long long)s.bytes_sent, (unsigned long long)s.bytes_received);

    // Anything still queued is ours to free. Only SAVEFILE owns memory, but
    // draining unconditionally keeps that fact in one place.
    if (g.cs_ready) {
        EnterCriticalSection(&g.cs);
        while (g.count) {
            net_msg_release(g.queue[g.head]);
            g.head = (g.head + 1) % kQueueCap;
            --g.count;
        }
        LeaveCriticalSection(&g.cs);
    }

    if (g.cs_ready)      { DeleteCriticalSection(&g.cs);      g.cs_ready = false; }
    if (g.send_cs_ready) { DeleteCriticalSection(&g.send_cs); g.send_cs_ready = false; }
    if (g.wsa_up)        { WSACleanup(); g.wsa_up = false; }

    g.role = NetRole::None;
    g.self = kNoPeer;
    g.roster_valid = false;
    g.seat_count = 0;
    for (uint32_t p = 0; p < kMaxPeers; ++p) g_slow_peer[p] = false;      // 0.1.213: the next room's peer with this id is measured afresh
    set_state(NetState::Idle);
}

NetState    net_state() { return (NetState)InterlockedCompareExchange(&g.state, 0, 0); }
NetRole     net_role()  { return g.role; }
const char* net_error() { return g.error; }

bool net_active() {
    if (g.role == NetRole::None) return false;
    NetState s = net_state();
    return s != NetState::Failed && s != NetState::Closed && s != NetState::Idle;
}

// Send to everyone we are connected to.
//
// On a client that is one socket, the host, and this is the old behaviour
// unchanged. On the host it is every live client -- which is what makes every
// existing host-authored call site (SAVEFILE, CATDATA, INVENTORY, ENTERNODE,
// HASH, CONTROL) reach all the players without any of them being edited.
//
// Returns true if it reached at least one peer. A host with no clients yet
// returns false, which every caller already treats as "nothing sent".
bool net_send(const uint8_t* payload, uint32_t len) {
    if (len == 0) return false;
    bool any = false, failed = false;
    for (int i = 0; i < kMaxPeers; ++i) {
        Link& l = g.links[i];
        if (!InterlockedCompareExchange(&l.live, 0, 0)) continue;
        if (link_send(l, g.self, payload, len)) any = true;
        else failed = true;
    }
    if (failed) {
        static ULONGLONG said = 0;
        if (GetTickCount64() - said > 10000) { said = GetTickCount64(); log_line("NET", "!! a %s frame could not be queued for every peer (a link is closing)", msg_name(payload[0])); }
    }
    return any;
}

bool net_send_peer(uint8_t peer, const uint8_t* payload, uint32_t len) {
    int slot = link_slot_of(peer);
    if (slot < 0) return false;
    return link_send(g.links[slot], g.self, payload, len);
}

// Every encoder writes into a stack buffer and hands the result to net_send.
// 512 bytes covers every phase-4 message; RUNSTATE will need its own path.
#define MGMP_SEND_WITH(encoder, arg)                       \
    uint8_t p[kSendSmall];                                 \
    uint32_t n = encoder(p, sizeof(p), arg);               \
    return n && net_send(p, n)

bool net_send_hello  (const Hello& h)     { MGMP_SEND_WITH(enc_hello,   h); }
bool net_send_welcome(const Welcome& w)   { MGMP_SEND_WITH(enc_welcome, w); }
bool net_send_action (const ActionMsg& a) { MGMP_SEND_WITH(enc_action,  a); }
bool net_send_hash   (const HashMsg& h)   { MGMP_SEND_WITH(enc_hash,    h); }
bool net_send_halt   (const HaltMsg& h)   { MGMP_SEND_WITH(enc_halt,    h); }
bool net_send_control(const ControlMsg& c) { MGMP_SEND_WITH(enc_control, c); }
bool net_send_cursor (const CursorMsg& c)  { MGMP_SEND_WITH(enc_cursor,  c); }
bool net_send_aim    (const AimMsg& m)     { MGMP_SEND_WITH(enc_aim,     m); }
bool net_send_enter_node(const EnterNodeMsg& m) {
    uint8_t p[kSendEnterNode];
    const uint32_t n = enc_enter_node(p, sizeof(p), m);
    return n && net_send(p, n);
}
bool net_send_choice(const ChoiceMsg& m) { MGMP_SEND_WITH(enc_choice, m); }
bool net_send_debughit(const DebugHitMsg& m) { MGMP_SEND_WITH(enc_debughit, m); }
bool net_send_party(const PartyMsg& m) { MGMP_SEND_WITH(enc_party, m); }
bool net_send_chapter(const ChapterMsg& m) { MGMP_SEND_WITH(enc_chapter, m); }
bool net_send_chapterseed(const ChapterSeedMsg& m) { MGMP_SEND_WITH(enc_chapterseed, m); }
bool net_send_checkpoint(const CheckpointMsg& m) {
    // proto 76: an offer carries up to kCheckpointCandidates saves (33 bytes each) -- past the 512-byte stack buffer of the small messages
    static_assert(kCheckpointCandidates * 33 + 512 <= 8192, "checkpoint frame buffer");
    uint8_t* p = (uint8_t*)malloc(8192);
    if (!p) return false;
    const uint32_t n = enc_checkpoint(p, 8192, m);
    const bool ok = n && net_send(p, n);
    free(p);
    return ok;
}
bool net_send_setup(const SetupMsg& m) {
    uint32_t need = setup_frame_size(m);
    if (need > kMaxFrame) return false;
    uint8_t* p = (uint8_t*)malloc(need);
    if (!p) return false;
    uint32_t n = enc_setup(p, need, m);
    bool ok = n && net_send(p, n);
    free(p);
    return ok;
}
bool net_send_nodehash(const NodeHashMsg& m) { MGMP_SEND_WITH(enc_nodehash, m); }
// 64 cats x 20 bytes does not fit the 512-byte stack frame above.
bool net_send_catdigest(const CatDigestMsg& m) {
    uint8_t p[2 + kCatDigestMax * 20];
    const uint32_t n = enc_catdigest(p, sizeof(p), m);
    return n && net_send(p, n);
}
// Not in relayed() above: host-authored, so net_send already reaches every
// client and a relay would deliver it twice.
bool net_send_hostleft(const HostLeftMsg& m) { MGMP_SEND_WITH(enc_hostleft, m); }
bool net_send_page    (const PageMsg& m)     { MGMP_SEND_WITH(enc_page,     m); }
bool net_send_nodevote(const NodeVoteMsg& m) { MGMP_SEND_WITH(enc_nodevote, m); }
bool net_send_voice(const VoiceMsg& m) { MGMP_SEND_WITH(enc_voice, m); }
bool net_send_cats    (const CatsMsg& m)     { uint8_t p[kSendCats]; const uint32_t n = enc_cats(p, sizeof(p), m); return n && net_send(p, n); }   // 8 long names > 512 (0.1.182)
// Host-authored, so it is NOT in relayed(): net_send already reaches every client.
bool net_send_savewait(const SaveWaitMsg& m) { MGMP_SEND_WITH(enc_savewait, m); }
// Peer-authored and relayed by the host (see relayed()).
bool net_send_abandon(const AbandonMsg& m) { MGMP_SEND_WITH(enc_abandon, m); }
bool net_send_roomctl(const RoomCtlMsg& m) { MGMP_SEND_WITH(enc_roomctl, m); }
// Host-authored, so it is NOT in relayed(): net_send already reaches every client.
bool net_send_mapseeds(const MapSeedsMsg& m) { MGMP_SEND_WITH(enc_mapseeds, m); }
// Host-authored, so it is NOT in relayed() either.
bool net_send_unlocks(const UnlocksMsg& m) { static thread_local uint8_t p[kSendUnlocks]; const uint32_t n = enc_unlocks(p, sizeof(p), m); return n && net_send(p, n); }   // 2048 since proto 78 (the class names)   // bigger than the 512-byte default since proto 62

// Host-authored, so it is NOT in relayed() either. Bigger than the 512-byte default: a chunk is up to 16 units.
bool net_send_board(const BoardMsg& m) { uint8_t p[kSendBoard]; const uint32_t n = enc_board(p, sizeof(p), m); return n && net_send(p, n); }
bool net_send_board_to(uint8_t peer, const BoardMsg& m) { uint8_t p[kSendBoard]; const uint32_t n = enc_board(p, sizeof(p), m); return n && net_send_peer(peer, p, n); }
bool net_host_weather(uint64_t node_id, char (*names)[kWeatherLen], uint8_t& n) {
    n = 0;
    if (!g.cs_ready || !names || !node_id) return false;
    EnterCriticalSection(&g.cs);
    const bool ok = g_level_box.bnode == node_id;
    if (ok) { n = g_level_box.n_weather; for (uint8_t i = 0; i < n && i < kWeatherNames; ++i) memcpy(names[i], g_level_box.weather[i], kWeatherLen); }
    LeaveCriticalSection(&g.cs);
    return ok;
}
int net_host_tag_count(bool extra_events) {
    if (!g.cs_ready) return -1;
    EnterCriticalSection(&g.cs);
    const int v = extra_events ? g_level_box.extra_events : g_level_box.event_difficulty;
    LeaveCriticalSection(&g.cs);
    return v;
}
int net_host_difficulty(uint64_t node_id) {
    if (!g.cs_ready) return -1;
    EnterCriticalSection(&g.cs);
    const int d = node_id ? (g_level_box.bnode == node_id ? g_level_box.bdifficulty : -1) : g_level_box.difficulty;
    LeaveCriticalSection(&g.cs);
    return d;
}
bool net_send_roll(const RollMsg& m) { uint8_t p[kSendRoll]; const uint32_t n = enc_roll(p, sizeof(p), m); return n && net_send(p, n); }
bool net_host_roll(uint64_t battle, uint32_t seq, uint8_t site, RollMsg& out) {
    if (!g.cs_ready || !battle) return false;
    EnterCriticalSection(&g.cs);
    bool ok = false;
    for (uint32_t k = 0; k < kRollRing && !ok; ++k)
        if (g_roll_ring[k].battle == battle && g_roll_ring[k].seq == seq && g_roll_ring[k].site == site) { out = g_roll_ring[k]; ok = true; }
    LeaveCriticalSection(&g.cs);
    return ok;
}
void net_clear_battle_caches() {
    if (!g.cs_ready) return;
    EnterCriticalSection(&g.cs);
    for (uint32_t k = 0; k < kBoardRing; ++k) g_board_ring[k] = BoardAssembled{};
    g_board_next = 0;
    for (uint32_t k = 0; k < kRollRing; ++k) g_roll_ring[k] = RollMsg{};
    g_roll_next = 0;
    g_level_box = LevelBox{};
    for (uint32_t k = 0; k < kSumRing; ++k) g_sum_ring[k] = BoardSumMsg{};
    g_sum_next = 0;
    for (uint32_t k = 0; k < kTqRing; ++k) g_tq_ring[k] = TurnQueueMsg{};
    g_tq_next = 0;
    for (uint32_t k = 0; k < kTilesBox; ++k) { free(g_tiles_box[k].data); g_tiles_box[k] = TilesBox{}; }
    g_tiles_box_next = 0;
    g_board_latest_battle = 0; g_board_latest_turn = 0;
    for (uint32_t k = 0; k < kBoardKeep; ++k) { free(g_kept[k].data); g_kept[k] = KeptBoard{}; }
    g_kept_next = 0;
    for (uint32_t p = 0; p < kMaxPeers; ++p) g_board_stream[p] = 0;
    for (uint32_t k = 0; k < kActBox; ++k) { free(g_act_box[k].data); g_act_box[k] = ActBox{}; }      // 0.1.213
    g_act_box_next = 0;
    for (uint32_t k = 0; k < kHashRing; ++k) g_hash_ring[k] = HashMsg{};      // a battle entered again keeps its id: the last entry's turn hashes must not answer a resync
    g_hash_next = 0;
    for (uint32_t k = 0; k < kUcatBox; ++k) { free(g_ucat_box[k].data); g_ucat_box[k] = UcatFrame{}; }      // a battle entered again keeps its id: an old fight's cat must not be taken
    g_ucat_box_next = 0;
    for (uint32_t k = 0; k < kUcatKeep; ++k) { free(g_ucat_kept[k].data); g_ucat_kept[k] = UcatFrame{}; }
    g_ucat_kept_next = 0;
    // 0.1.241 (room KD8E 11-19 22:36, battle fa48c212 entered twice after a restore): the unit records were the one store this left out -- the host answered turn 0 of
    // the second entry with the first entry's records (other units at the same index: 0.1.239's check refused all 13, the halt followed at turn 6). Both are by battle and turn only.
    for (uint32_t k = 0; k < kUsKeep; ++k) { for (uint32_t i = 0; i < kBoardMax; ++i) { free(g_us_kept[k].blob[i]); } g_us_kept[k] = KeptUstate{}; }
    g_us_kept_next = 0;
    for (uint32_t k = 0; k < kUsBox; ++k) { free(g_us_box[k].data); g_us_box[k] = UstateBox{}; }
    g_us_box_next = 0;
    LeaveCriticalSection(&g.cs);
}
// 0.1.246: everything of `battle` that arrived (or was kept) before `before` (GetTickCount64) is let go: boards, digests, turn queues, tiles, the host's
// hashes and rolls, unit records, action checks. For a mid-battle re-snapshot (the game replaced the battle's character list): the battle id stays, the
// turns start again at 0, and every store is keyed by battle and turn -- the battle's first turn 0..N answered the new turn 0..N (room XU 11-20 23:59:
// the stale turn-0 board was applied, its old stream gave an rng mismatch, the stale unit records were refused, the battle halted at turn 2).
void net_forget_battle_before(uint64_t battle, ULONGLONG before) {
    if (!g.cs_ready || !battle) return;
    uint32_t n = 0;
    EnterCriticalSection(&g.cs);
    for (uint32_t k = 0; k < kBoardRing; ++k) if (g_board_ring[k].battle == battle && g_board_at[k] < before) { g_board_ring[k] = BoardAssembled{}; ++n; }
    for (uint32_t k = 0; k < kSumRing; ++k)   if (g_sum_ring[k].battle == battle && g_sum_at[k] < before) { g_sum_ring[k] = BoardSumMsg{}; ++n; }
    for (uint32_t k = 0; k < kTqRing; ++k)    if (g_tq_ring[k].battle == battle && g_tq_at[k] < before) { g_tq_ring[k] = TurnQueueMsg{}; ++n; }
    for (uint32_t k = 0; k < kHashRing; ++k)  if (g_hash_ring[k].battle_id == battle && g_hash_at[k] < before) { g_hash_ring[k] = HashMsg{}; ++n; }
    for (uint32_t k = 0; k < kRollRing; ++k)  if (g_roll_ring[k].battle == battle && g_roll_at[k] < before) { g_roll_ring[k] = RollMsg{}; ++n; }
    for (uint32_t k = 0; k < kTilesBox; ++k)  if (g_tiles_box[k].data && g_tiles_box[k].battle == battle && g_tiles_box[k].at < before) { free(g_tiles_box[k].data); g_tiles_box[k] = TilesBox{}; ++n; }
    for (uint32_t k = 0; k < kUsBox; ++k)     if (g_us_box[k].data && g_us_box[k].battle == battle && g_us_box[k].at < before) { free(g_us_box[k].data); g_us_box[k] = UstateBox{}; ++n; }
    for (uint32_t k = 0; k < kActBox; ++k)    if (g_act_box[k].data && g_act_box[k].battle == battle && g_act_box[k].at < before) { free(g_act_box[k].data); g_act_box[k] = ActBox{}; ++n; }
    for (uint32_t k = 0; k < kBoardKeep; ++k) if (g_kept[k].data && g_kept[k].battle == battle && g_kept[k].at < before) { free(g_kept[k].data); g_kept[k] = KeptBoard{}; ++n; }
    for (uint32_t k = 0; k < kUsKeep; ++k)
        if (g_us_kept[k].battle == battle && g_us_kept[k].at < before) { for (uint32_t i = 0; i < kBoardMax; ++i) free(g_us_kept[k].blob[i]); g_us_kept[k] = KeptUstate{}; ++n; }
    if (g_board_latest_battle == battle) { g_board_latest_battle = 0; g_board_latest_turn = 0; }    // "the host is already past turn N" belonged to the old numbering
    LeaveCriticalSection(&g.cs);
    log_line("NET", "battle %016llx re-snapshotted: %u stored board/hash/record entr(ies) from before it let go (the turns start again at 0)", (unsigned long long)battle, n);
}

bool net_host_board(uint64_t battle, uint32_t turn, BoardAssembled& out) {
    if (!g.cs_ready || !battle) return false;
    EnterCriticalSection(&g.cs);
    bool ok = false;
    for (uint32_t k = 0; k < kBoardRing && !ok; ++k)
        if (g_board_ring[k].battle == battle && g_board_ring[k].turn == turn && g_board_ring[k].complete) { out = g_board_ring[k]; ok = true; }
    LeaveCriticalSection(&g.cs);
    return ok;
}

uint32_t net_board_publish(const BoardMsg* chunks, uint32_t n, const BoardSumMsg* sum) {
    if (!g.cs_ready || !chunks || !n || g.role != NetRole::Host) return 0;
    // the chunks, encoded once: [u32 length][payload] each
    uint8_t* data = (uint8_t*)malloc((size_t)n * (4 + 1536));
    if (!data) return 0;
    uint32_t len = 0;
    for (uint32_t c = 0; c < n; ++c) {
        const uint32_t k = enc_board(data + len + 4, 1536, chunks[c]);
        if (!k) continue;
        memcpy(data + len, &k, 4);
        len += 4 + k;
    }
    const uint64_t battle = chunks[0].battle;
    const uint32_t turn = chunks[0].turn;
    bool stream[kMaxPeers] = {};
    EnterCriticalSection(&g.cs);
    KeptBoard* slot = nullptr;
    for (uint32_t k = 0; k < kBoardKeep && !slot; ++k) if (g_kept[k].data && g_kept[k].battle == battle && g_kept[k].turn == turn) slot = &g_kept[k];
    if (!slot) slot = &g_kept[g_kept_next++ % kBoardKeep];
    slot->at = GetTickCount64();
    free(slot->data);
    slot->battle = battle; slot->turn = turn; slot->data = data; slot->len = len;
    for (uint32_t p = 0; p < kMaxPeers; ++p) stream[p] = g_board_stream[p] == battle || g_slow_peer[p];     // 0.1.213: a slow link gets every board whole, unasked
    // the copy sent below is taken under the lock: a request answered on a receive thread may replace the slot meanwhile
    uint8_t* mine = (uint8_t*)malloc(len ? len : 1);
    if (mine) memcpy(mine, data, len);
    LeaveCriticalSection(&g.cs);
    if (!mine) return 0;

    uint8_t sb[16 + 4 + 4 + 32 + 4 * kBoardMax];
    const uint32_t sn = sum ? enc_boardsum(sb, sizeof(sb), *sum) : 0;
    uint32_t reached = 0;
    for (int i = 0; i < kMaxPeers; ++i) {
        Link& l = g.links[i];
        if (!InterlockedCompareExchange(&l.live, 0, 0)) continue;
        bool any = false;
        if (sn && link_send(l, g.self, sb, sn)) any = true;
        if (!sum || (l.id < kMaxPeers && stream[l.id])) {
            for (uint32_t at = 0; at + 4 <= len; ) {
                uint32_t k = 0; memcpy(&k, mine + at, 4); at += 4;
                if (link_send(l, g.self, mine + at, k)) any = true;
                at += k;
            }
        }
        if (any) ++reached;
    }
    free(mine);
    return reached;
}

bool net_host_boardsum(uint64_t battle, uint32_t turn, BoardSumMsg& out) {
    if (!g.cs_ready || !battle) return false;
    EnterCriticalSection(&g.cs);
    bool ok = false;
    for (uint32_t k = 0; k < kSumRing && !ok; ++k)
        if (g_sum_ring[k].battle == battle && g_sum_ring[k].turn == turn) { out = g_sum_ring[k]; ok = true; }
    LeaveCriticalSection(&g.cs);
    return ok;
}

bool net_board_seen_past(uint64_t battle, uint32_t turn) {
    if (!g.cs_ready || !battle) return false;
    EnterCriticalSection(&g.cs);
    const bool past = g_board_latest_battle == battle && g_board_latest_turn > turn;
    LeaveCriticalSection(&g.cs);
    return past;
}

void net_ustate_publish(uint64_t battle, uint32_t turn, uint8_t* const* blobs, const uint32_t* lens, uint32_t n) {
    if (!g.cs_ready || g.role != NetRole::Host) { for (uint32_t i = 0; i < n; ++i) free(blobs[i]); return; }
    EnterCriticalSection(&g.cs);
    KeptUstate* s = nullptr;
    for (uint32_t k = 0; k < kUsKeep && !s; ++k) if (g_us_kept[k].battle == battle && g_us_kept[k].turn == turn) s = &g_us_kept[k];
    if (!s) s = &g_us_kept[g_us_kept_next++ % kUsKeep];
    s->at = GetTickCount64();
    for (uint32_t i = 0; i < kBoardMax; ++i) { free(s->blob[i]); s->blob[i] = nullptr; s->len[i] = 0; }
    s->battle = battle; s->turn = turn;
    for (uint32_t i = 0; i < n && i < kBoardMax; ++i) { s->blob[i] = blobs[i]; s->len[i] = lens[i]; }
    // 0.1.213: A SLOW LINK IS SENT WHAT CHANGED, UNASKED. The records of the previous boundary are kept beside these; a unit whose record differs from it (past the
    // 14-byte header: type, battle, turn, index) went through something this turn, and a client whose link is slow gets it now -- its repair finds it in its mailbox
    // instead of asking and waiting (room DBJ3, 11-08: the records came after 3 s and were too late). Copies taken under the lock, sent after it.
    bool any_slow = false;
    for (uint32_t p = 0; p < kMaxPeers; ++p) any_slow = any_slow || g_slow_peer[p];
    const KeptUstate* prev = nullptr;
    if (any_slow && turn > 0)
        for (uint32_t k = 0; k < kUsKeep && !prev; ++k) if (&g_us_kept[k] != s && g_us_kept[k].battle == battle && g_us_kept[k].turn + 1 == turn) prev = &g_us_kept[k];
    uint8_t* push[kBoardMax] = {}; uint32_t push_len[kBoardMax] = {}; uint32_t npush = 0;
    if (any_slow)
        for (uint32_t i = 0; i < n && i < kBoardMax; ++i) {
            if (!s->blob[i] || s->len[i] <= 14) continue;
            const bool same = prev && prev->blob[i] && prev->len[i] == s->len[i] && memcmp(prev->blob[i] + 14, s->blob[i] + 14, s->len[i] - 14) == 0;
            if (same) continue;
            push[npush] = (uint8_t*)malloc(s->len[i]);
            if (push[npush]) { memcpy(push[npush], s->blob[i], s->len[i]); push_len[npush] = s->len[i]; ++npush; }
        }
    bool slow[kMaxPeers] = {};
    for (uint32_t p = 0; p < kMaxPeers; ++p) slow[p] = g_slow_peer[p];
    LeaveCriticalSection(&g.cs);
    for (uint32_t i = kBoardMax; i < n; ++i) free(blobs[i]);
    if (npush) {
        uint32_t reached = 0;
        for (int li = 0; li < kMaxPeers; ++li) {
            Link& l = g.links[li];
            if (!InterlockedCompareExchange(&l.live, 0, 0) || l.id >= kMaxPeers || !slow[l.id]) continue;
            for (uint32_t k = 0; k < npush; ++k) link_send(l, kHostPeer, push[k], push_len[k]);
            ++reached;
        }
        static uint32_t said = 0;
        if (reached && said < 40) { ++said; log_line("USYNC", "-> turn %u: %u unit record(s) that changed this turn pushed to %u slow client(s), unasked", (unsigned)turn, npush, reached); }
        for (uint32_t k = 0; k < npush; ++k) free(push[k]);
    }
}
void net_set_slow_peer(uint8_t peer, bool slow) {
    if (!g.cs_ready || peer >= kMaxPeers) return;
    EnterCriticalSection(&g.cs);
    const bool was = g_slow_peer[peer];
    g_slow_peer[peer] = slow;
    LeaveCriticalSection(&g.cs);
    if (was != slow) log_line("BOARD", "%s peer %u: %s", slow ? "!!" : "", (unsigned)peer,
                              slow ? "its link is slow -- every boundary's full board and the changed unit records go to it unasked" : "its link is quick again -- boards and records go to it when it asks");
}
bool net_slow_peer(uint8_t peer) {
    if (!g.cs_ready || peer >= kMaxPeers) return false;
    EnterCriticalSection(&g.cs);
    const bool s = g_slow_peer[peer];
    LeaveCriticalSection(&g.cs);
    return s;
}
bool net_send_actcheck(const ActCheckMsg& m) {
    if (g.role != NetRole::Host) return false;
    uint8_t p[kSendActCheck];
    const uint32_t n = enc_actcheck(p, sizeof(p), m);
    return n && net_send(p, n);
}
bool net_host_hash(uint64_t battle, uint32_t turn, HashMsg& out) {
    if (!g.cs_ready || !battle) return false;
    bool ok = false;
    EnterCriticalSection(&g.cs);
    for (uint32_t k = 0; k < kHashRing; ++k) {                      // oldest to newest: the newest of a turn wins (a resynced client may be sent a second one)
        const HashMsg& h = g_hash_ring[(g_hash_next + k) % kHashRing];
        if (h.battle_id == battle && h.turn == turn) { out = h; ok = true; }
    }
    LeaveCriticalSection(&g.cs);
    return ok;
}
bool net_host_actcheck(uint64_t battle, uint32_t turn, uint32_t action, ActCheckMsg& out) {
    if (!g.cs_ready || !battle) return false;
    uint8_t buf[kSendActCheck]; uint32_t len = 0;
    EnterCriticalSection(&g.cs);
    for (uint32_t k = 0; k < kActBox && !len; ++k) {
        const ActBox& b = g_act_box[k];
        if (b.data && b.battle == battle && b.turn == turn && b.action == action && b.len <= sizeof(buf)) { memcpy(buf, b.data, b.len); len = b.len; }
    }
    LeaveCriticalSection(&g.cs);
    return len && dec_actcheck(buf, len, out);
}
bool net_ucat_keep(uint64_t battle, uint8_t index, int32_t key, uint64_t save_id, const uint8_t* image, uint32_t size, uint64_t hash) {
    if (!g.cs_ready || g.role != NetRole::Host || !image || !size) return false;
    UcatMsg m{};
    m.battle = battle; m.index = index; m.key = key; m.save_id = save_id; m.status = kUcatImage; m.size = size; m.hash = hash; m.data = image;
    const uint32_t cap = ucat_frame_size(m);
    uint8_t* p = (uint8_t*)malloc(cap);
    if (!p) return false;
    const uint32_t n = enc_ucat(p, cap, m);
    if (!n) { free(p); return false; }
    EnterCriticalSection(&g.cs);
    UcatFrame* f = nullptr;
    for (uint32_t k = 0; k < kUcatKeep && !f; ++k) if (g_ucat_kept[k].data && g_ucat_kept[k].battle == battle && g_ucat_kept[k].index == index) f = &g_ucat_kept[k];
    if (!f) f = &g_ucat_kept[g_ucat_kept_next++ % kUcatKeep];
    free(f->data);
    f->battle = battle; f->index = index; f->key = key; f->data = p; f->len = n;
    LeaveCriticalSection(&g.cs);
    return true;
}
bool net_send_tiles(const TilesMsg& m) {
    if (g.role != NetRole::Host) return false;
    static uint8_t p[kSendTiles];
    const uint32_t n = enc_tiles(p, sizeof(p), m);
    return n && net_send(p, n);
}
uint32_t net_host_tiles(uint64_t battle, uint32_t turn, uint8_t* out, uint32_t cap) {
    if (!g.cs_ready || !battle || !out) return 0;
    uint32_t got = 0;
    EnterCriticalSection(&g.cs);
    for (uint32_t k = 0; k < kTilesBox && !got; ++k) {
        const TilesBox& b = g_tiles_box[k];
        if (b.data && b.battle == battle && b.turn == turn && b.len <= cap) { memcpy(out, b.data, b.len); got = b.len; }
    }
    LeaveCriticalSection(&g.cs);
    return got;
}
bool net_send_ucatreq(const UcatReqMsg& m) { uint8_t p[32]; const uint32_t n = enc_ucatreq(p, sizeof(p), m); return n && net_send(p, n); }
uint32_t net_host_ucat(uint64_t battle, uint8_t index, int32_t key, uint8_t* out, uint32_t cap) {
    if (!g.cs_ready || !battle || !out) return 0;
    uint32_t got = 0;
    EnterCriticalSection(&g.cs);
    for (uint32_t k = 0; k < kUcatBox && !got; ++k) {
        const UcatFrame& f = g_ucat_box[k];
        if (f.data && f.battle == battle && f.index == index && f.key == key && f.len <= cap) { memcpy(out, f.data, f.len); got = f.len; }
    }
    LeaveCriticalSection(&g.cs);
    return got;
}
bool net_send_ustatereq(const UstateReqMsg& m) { uint8_t p[32 + kUstateReqMax]; const uint32_t n = enc_ustatereq(p, sizeof(p), m); return n && net_send(p, n); }
uint32_t net_host_ustate(uint64_t battle, uint32_t turn, uint8_t index, uint8_t* out, uint32_t cap) {
    if (!g.cs_ready || !battle) return 0;
    uint32_t got = 0;
    EnterCriticalSection(&g.cs);
    const UstateBox* best = nullptr;
    for (uint32_t k = 0; k < kUsBox; ++k) {
        const UstateBox& b = g_us_box[k];
        if (b.data && b.battle == battle && b.turn == turn && b.index == index && b.len <= cap && (!best || b.seq > best->seq)) best = &b;
    }
    if (best) { memcpy(out, best->data, best->len); got = best->len; }
    LeaveCriticalSection(&g.cs);
    return got;
}
// 0.1.239: every record of this unit and boundary is let go (one that did not match the host's board row: the next one asked for is read instead)
void net_drop_ustate(uint64_t battle, uint32_t turn, uint8_t index) {
    if (!g.cs_ready) return;
    EnterCriticalSection(&g.cs);
    for (uint32_t k = 0; k < kUsBox; ++k) {
        UstateBox& b = g_us_box[k];
        if (b.data && b.battle == battle && b.turn == turn && b.index == index) { free(b.data); b = UstateBox{}; }
    }
    LeaveCriticalSection(&g.cs);
}

bool net_send_boardreq(const BoardReqMsg& m) { uint8_t p[kSendBoardReq]; const uint32_t n = enc_boardreq(p, sizeof(p), m); return n && net_send(p, n); }

bool net_send_turnqueue(const TurnQueueMsg& m) {
    if (g.role != NetRole::Host) return false;
    uint8_t p[16 + 2 * kTurnQueueMax];
    const uint32_t n = enc_turnqueue(p, sizeof(p), m);
    return n && net_send(p, n);
}
bool net_host_turnqueue(uint64_t battle, uint32_t turn, TurnQueueMsg& out) {
    if (!g.cs_ready || !battle) return false;
    EnterCriticalSection(&g.cs);
    bool ok = false;
    for (uint32_t k = 0; k < kTqRing && !ok; ++k)
        if (g_tq_ring[k].battle == battle && g_tq_ring[k].turn == turn) { out = g_tq_ring[k]; ok = true; }
    LeaveCriticalSection(&g.cs);
    return ok;
}

bool net_host_pending(uint64_t node_id, PendingEnemy (*out)[kPendingMax], uint8_t* n, uint32_t* total) {
    for (uint32_t q = 0; q < kPendingQueues; ++q) { n[q] = 0; if (total) total[q] = 0; }
    if (!g.cs_ready || !out || !node_id) return false;
    EnterCriticalSection(&g.cs);
    const bool ok = g_level_box.bnode == node_id;
    if (ok) for (uint32_t q = 0; q < kPendingQueues; ++q) {
        n[q] = g_level_box.n_pending[q];
        for (uint8_t i = 0; i < n[q] && i < kPendingMax; ++i) out[q][i] = g_level_box.pending[q][i];
        if (total) total[q] = g_level_box.pending_total[q] > n[q] ? g_level_box.pending_total[q] : n[q];
    }
    LeaveCriticalSection(&g.cs);
    return ok;
}

bool net_host_level(uint64_t node_id, char* out, size_t cap) {
    if (!g.cs_ready || !out || !cap) return false;
    EnterCriticalSection(&g.cs);
    const bool ok = node_id && g_level_box.node == node_id && g_level_box.name[0];
    if (ok) { strncpy_s(out, cap, g_level_box.name, _TRUNCATE); }
    LeaveCriticalSection(&g.cs);
    return ok;
}
bool net_send_refuse (const char* reason) { MGMP_SEND_WITH(enc_refuse,  reason); }

#undef MGMP_SEND_WITH

bool net_send_hello_to(uint8_t peer, const Hello& h) {
    uint8_t p[kSendSmall];
    uint32_t n = enc_hello(p, sizeof(p), h);
    return n && net_send_peer(peer, p, n);
}

// To one peer, for replaying a battle already under way to a joiner. A
// broadcast would re-inject decisions into peers that already applied them --
// pend_take cannot tell a replay from a live decision, correctly, because
// there is no difference except who still needs it.
bool net_send_action_to(uint8_t peer, const ActionMsg& a) {
    uint8_t p[kSendSmall];
    uint32_t n = enc_action(p, sizeof(p), a);
    return n && net_send_peer(peer, p, n);
}

// To one peer: telling a joiner where the run is standing. A broadcast would
// re-drive peers that are already in that node back into it.
bool net_send_enter_node_to(uint8_t peer, const EnterNodeMsg& m) {
    uint8_t p[kSendEnterNode];
    uint32_t n = enc_enter_node(p, sizeof(p), m);
    return n && net_send_peer(peer, p, n);
}

bool net_send_welcome_to(uint8_t peer, const Welcome& w) {
    uint8_t p[kSendSmall];
    uint32_t n = enc_welcome(p, sizeof(p), w);
    return n && net_send_peer(peer, p, n);
}

bool net_send_refuse_to(uint8_t peer, const char* reason) {
    uint8_t p[kSendSmall];
    uint32_t n = enc_refuse(p, sizeof(p), reason);
    return n && net_send_peer(peer, p, n);
}

namespace {
// Shared by the broadcast and the point-to-point form so the frame cap check
// and the malloc live in exactly one place.
bool send_savefile_frame(const SaveFileMsg& m, bool broadcast, uint8_t peer) {
    uint32_t need = savefile_frame_size(m);
    if (need > kMaxFrame) {
        log_line("NET", "!! save file is %u bytes, over the %u-byte frame cap",
                 m.size, kMaxFrame);
        return false;
    }
    uint8_t* p = (uint8_t*)malloc(need);
    if (!p) return false;
    uint32_t n = enc_savefile(p, need, m);
    bool ok = n && (broadcast ? net_send(p, n) : net_send_peer(peer, p, n));
    free(p);
    return ok;
}
} // namespace

bool net_send_savefile(const SaveFileMsg& m) {
    return send_savefile_frame(m, true, kNoPeer);
}

// Host to one peer, once, during the fresh-session handshake -- before either
// side has loaded its save. Own frame size check rather than sharing
// send_savefile_frame so the two messages can evolve independently.
bool net_send_chaptermap_to(uint8_t peer, const ChapterMapMsg& m) {
    uint32_t need = chaptermap_frame_size(m);
    if (need > kMaxFrame) {
        log_line("NET", "!! chapter map row is %u bytes, over the %u-byte frame cap",
                 m.size, kMaxFrame);
        return false;
    }
    uint8_t* p = (uint8_t*)malloc(need);
    if (!p) return false;
    uint32_t n = enc_chaptermap(p, need, m);
    bool ok = n && net_send_peer(peer, p, n);
    free(p);
    return ok;
}

// To one peer. A player who joins after the host has already published needs
// the save, but the peers already in the run must NOT be sent it again -- on a
// client that arrives as a fresh redirect-and-load of the whole run.
bool net_send_savefile_to(uint8_t peer, const SaveFileMsg& m) {
    return send_savefile_frame(m, false, peer);
}

bool net_send_catdata(const CatDataMsg& m) {
    uint32_t need = catdata_frame_size(m);
    if (need > kMaxFrame) {
        log_line("NET", "!! serialized cat is %u bytes, over the %u-byte frame cap",
                 m.size, kMaxFrame);
        return false;
    }
    uint8_t* p = (uint8_t*)malloc(need);
    if (!p) return false;
    uint32_t n = enc_catdata(p, need, m);
    bool ok = n && net_send(p, n);
    free(p);
    return ok;
}

// The point-to-point forms of the three run-state pushes. Each is the broadcast
// body with net_send_peer in place of net_send, and they exist for one caller:
// the catch-up burst sent to a peer that just joined. See the note in
// mgmp_net.h for why that burst must not be a broadcast.
bool net_send_catdata_to(uint8_t peer, const CatDataMsg& m) {
    uint32_t need = catdata_frame_size(m);
    if (need > kMaxFrame) {
        log_line("NET", "!! serialized cat is %u bytes, over the %u-byte frame cap",
                 m.size, kMaxFrame);
        return false;
    }
    uint8_t* p = (uint8_t*)malloc(need);
    if (!p) return false;
    uint32_t n = enc_catdata(p, need, m);
    bool ok = n && net_send_peer(peer, p, n);
    free(p);
    return ok;
}

bool net_send_inventory_to(uint8_t peer, const InventoryMsg& m) {
    uint32_t need = inventory_frame_size(m);
    if (need > kMaxFrame) {
        log_line("NET", "!! serialized inventory is %u bytes, over the %u-byte "
                        "frame cap", need, kMaxFrame);
        return false;
    }
    uint8_t* p = (uint8_t*)malloc(need);
    if (!p) return false;
    uint32_t n = enc_inventory(p, need, m);
    bool ok = n && net_send_peer(peer, p, n);
    free(p);
    return ok;
}

bool net_send_runhist_to(uint8_t peer, const RunHistMsg& m) {
    uint32_t need = runhist_frame_size(m);
    if (need > kMaxFrame) {
        log_line("NET", "!! serialized run history is %u bytes, over the %u-byte "
                        "frame cap", m.size, kMaxFrame);
        return false;
    }
    uint8_t* p = (uint8_t*)malloc(need);
    if (!p) return false;
    uint32_t n = enc_runhist(p, need, m);
    bool ok = n && net_send_peer(peer, p, n);
    free(p);
    return ok;
}

bool net_send_runhist(const RunHistMsg& m) {
    uint32_t need = runhist_frame_size(m);
    if (need > kMaxFrame) {
        log_line("NET", "!! serialized run history is %u bytes, over the %u-byte "
                        "frame cap", m.size, kMaxFrame);
        return false;
    }
    uint8_t* p = (uint8_t*)malloc(need);
    if (!p) return false;
    uint32_t n = enc_runhist(p, need, m);
    bool ok = n && net_send(p, n);
    free(p);
    return ok;
}

bool net_send_statedump(const StateDumpMsg& m) {
    uint32_t need = statedump_frame_size(m);
    if (need > kMaxFrame) {
        log_line("NET", "!! state dump is %u bytes, over the %u-byte frame cap",
                 need, kMaxFrame);
        return false;
    }
    uint8_t* p = (uint8_t*)malloc(need);
    if (!p) return false;
    uint32_t n = enc_statedump(p, need, m);
    bool ok = n && net_send(p, n);
    free(p);
    return ok;
}

bool net_send_uqd(const UqdMsg& m) { uint8_t p[kSendUqd]; const uint32_t n = enc_uqd(p, sizeof(p), m); return n && net_send(p, n); }
bool net_send_ping(const PingMsg& m) { uint8_t p[kSendPing]; const uint32_t n = enc_ping(p, sizeof(p), m); return n && net_send(p, n); }
bool net_send_setting(const SettingMsg& m) { uint8_t p[kSendSetting]; const uint32_t n = enc_setting(p, sizeof(p), m); return n && net_send(p, n); }
bool net_send_equip(const EquipMsg& m) { uint8_t p[kSendEquip]; const uint32_t n = enc_equip(p, sizeof(p), m); return n && net_send(p, n); }
bool net_send_soulcat(const SoulCatMsg& m) {
    const uint32_t need = soulcat_frame_size(m);
    if (need > kMaxFrame) { log_line("NET", "!! a soul jar's cat is %u bytes, over the %u-byte frame cap", m.size, kMaxFrame); return false; }
    uint8_t* p = (uint8_t*)malloc(need);
    if (!p) return false;
    const uint32_t n = enc_soulcat(p, need, m);
    const bool ok = n && net_send(p, n);
    free(p);
    return ok;
}
bool net_send_chat(const ChatMsg& m) { uint8_t p[kSendChat]; const uint32_t n = enc_chat(p, sizeof(p), m); return n && net_send(p, n); }
bool net_send_draw(const DrawMsg& m) { uint8_t p[kSendDraw]; const uint32_t n = enc_draw(p, sizeof(p), m); return n && net_send(p, n); }
bool net_send_treasure(const TreasureMsg& m) { uint8_t p[kSendTreasure]; const uint32_t n = enc_treasure(p, sizeof(p), m); return n && net_send(p, n); }
bool net_send_housecoop(const HouseCoopMsg& m) { uint8_t p[kSendHouseCoop]; const uint32_t n = enc_housecoop(p, sizeof(p), m); return n && net_send(p, n); }
bool net_send_rngl(const RnglMsg& m) { uint8_t p[kSendSmall]; const uint32_t n = enc_rngl(p, sizeof(p), m); return n && net_send(p, n); }
bool net_send_deep(const DeepMsg& m) { uint8_t p[kSendDeep]; const uint32_t n = enc_deep(p, sizeof(p), m); return n && net_send(p, n); }
bool net_send_props(const PropsMsg& m) { uint8_t p[kSendProps]; const uint32_t n = enc_props(p, sizeof(p), m); return n && net_send(p, n); }
bool net_send_audit(const AuditMsg& m) {
    uint8_t* p = (uint8_t*)malloc(8192);
    if (!p) return false;
    const uint32_t n = enc_audit(p, 8192, m);
    const bool ok = n && net_send(p, n);
    free(p);
    return ok;
}

bool net_send_peerlog(const PeerLogMsg& m) {
    const uint32_t need = peerlog_frame_size(m);
    if (need > kMaxFrame) return false;
    uint8_t* p = (uint8_t*)malloc(need);
    if (!p) return false;
    const uint32_t n = enc_peerlog(p, need, m);
    const bool ok = n && net_send(p, n);
    free(p);
    return ok;
}

bool net_send_inventory(const InventoryMsg& m) {
    uint32_t need = inventory_frame_size(m);
    if (need > kMaxFrame) {
        log_line("NET", "!! serialized inventory is %u bytes, over the %u-byte "
                        "frame cap", need, kMaxFrame);
        return false;
    }
    uint8_t* p = (uint8_t*)malloc(need);
    if (!p) return false;
    uint32_t n = enc_inventory(p, need, m);
    bool ok = n && net_send(p, n);
    free(p);
    return ok;
}

void net_msg_release(NetMsg& m) {
    if (m.savefile.data) { free(m.savefile.data); m.savefile.data = nullptr; }
    if (m.chaptermap.data) { free(m.chaptermap.data); m.chaptermap.data = nullptr; }
    if (m.catdata.data)  { free(m.catdata.data);  m.catdata.data  = nullptr; }
    for (uint8_t i = 0; i < kSetupMaxCats; ++i)
        if (m.setup.cats[i].data) { free(m.setup.cats[i].data); m.setup.cats[i].data = nullptr; }
    if (m.runhist.data)  { free(m.runhist.data);  m.runhist.data  = nullptr; }
    if (m.statedump.data){ free(m.statedump.data);m.statedump.data= nullptr; }
    if (m.peerlog.data)  { free(m.peerlog.data);  m.peerlog.data  = nullptr; }
    if (m.soulcat.data)  { free(m.soulcat.data);  m.soulcat.data  = nullptr; }
    for (uint32_t i = 0; i < kInvBuckets; ++i)
        if (m.inventory.data[i]) { free(m.inventory.data[i]); m.inventory.data[i] = nullptr; }
}

bool net_poll(NetMsg& out) {
    if (!g.cs_ready) return false;
    EnterCriticalSection(&g.cs);
    {   // 0.1.182: what a full queue cost, said where it can be seen (it used to be a count in the closing line only)
        static uint32_t s_dropped = 0, s_evicted = 0;
        static ULONGLONG s_said = 0;
        if ((g.stats.dropped != s_dropped || g.evicted != s_evicted) && GetTickCount64() - s_said > 5000) {
            const uint32_t d = g.stats.dropped - s_dropped, e = g.evicted - s_evicted;
            const uint8_t last = g.dropped_last;
            s_dropped = g.stats.dropped; s_evicted = g.evicted; s_said = GetTickCount64();
            LeaveCriticalSection(&g.cs);
            if (d) log_line_lvl(LogLevel::Warn, "NET", "!! receive queue FULL (%u slots, the game thread stopped draining): %u message(s) DROPPED, the last a %s; %u voice/pointer frame(s) pushed out",
                                kQueueCap, d, msg_name(last), e);
            else   log_line("NET", "receive queue full (%u slots, the game thread stopped draining): %u voice/pointer frame(s) pushed out to keep every game message", kQueueCap, e);
            EnterCriticalSection(&g.cs);
        }
    }
    bool got = g.count > 0;
    if (got) {
        out = g.queue[g.head];
        // Hand the allocation over rather than sharing it: the queue slot is
        // reused, and two owners of one pointer is a double free waiting for a
        // busy session.
        g.queue[g.head].savefile.data = nullptr;
        g.queue[g.head].catdata.data  = nullptr;
        for (uint8_t i = 0; i < kSetupMaxCats; ++i)
            g.queue[g.head].setup.cats[i].data = nullptr;
        g.queue[g.head].runhist.data  = nullptr;
        g.queue[g.head].statedump.data = nullptr;
        g.queue[g.head].peerlog.data   = nullptr;
        g.queue[g.head].soulcat.data   = nullptr;
        for (uint32_t i = 0; i < kInvBuckets; ++i)
            g.queue[g.head].inventory.data[i] = nullptr;
        g.head = (g.head + 1) % kQueueCap;
        --g.count;
    }
    LeaveCriticalSection(&g.cs);

    // Learn our own id here rather than in the session layer, so the transport's
    // idea of `self` -- which it stamps into every outgoing envelope -- can
    // never lag behind what the rest of the mod believes.
    if (got && out.type == MSG_PEERS && out.peers.you != kNoPeer) {
        g.self = out.peers.you;
        g.roster = out.peers;
        g.roster_valid = true;
    }
    return got;
}

size_t net_queued_bytes(uint8_t peer) {
    size_t n = 0;
    for (int i = 0; i < kMaxPeers; ++i) {
        Link& l = g.links[i];
        if (!l.qcs_ready || l.id != peer || !InterlockedCompareExchange(&l.live, 0, 0)) continue;
        EnterCriticalSection(&l.qcs); n += l.qlen; LeaveCriticalSection(&l.qcs);
    }
    return n;
}

NetStats net_stats() {
    NetStats s{};
    if (!g.cs_ready) return s;
    EnterCriticalSection(&g.cs);
    s = g.stats;
    LeaveCriticalSection(&g.cs);
    return s;
}

} // namespace mgmp
