// mgmp_net.cpp -- Winsock TCP transport. See mgmp_net.h for the design rule.

#include "mgmp_net.h"
#include "mgmp_log.h"
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
};

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

    CRITICAL_SECTION send_cs;
    bool     send_cs_ready = false;

    // Membership, host-side. Rebroadcast to everyone whenever it changes.
    PeersMsg roster;
    bool     roster_valid = false;

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

struct LevelBox { uint64_t node = 0; char name[64] = {}; uint64_t bnode = 0; uint8_t n_pending[kPendingQueues] = {}; PendingEnemy pending[kPendingQueues][kPendingMax]; uint8_t n_weather = 0; char weather[kWeatherNames][kWeatherLen] = {}; } g_level_box;
constexpr uint32_t kBoardRing = 4;
BoardAssembled g_board_ring[kBoardRing];   // the last few turns' boards, guarded by g.cs like the level box
uint32_t g_board_next = 0;
constexpr uint32_t kRollRing = 64;
RollMsg g_roll_ring[kRollRing];            // the host's last rolls, guarded by g.cs
uint32_t g_roll_next = 0;

void queue_push(const NetMsg& m) {
    EnterCriticalSection(&g.cs);
    if (m.type == MSG_UNLOCKS && m.unlocks.n_level_name) {          // the level of a battle (picked by the level picker)
        g_level_box.node = m.unlocks.level_node;
        memcpy(g_level_box.name, m.unlocks.level_name, sizeof(g_level_box.name));
    }
    if (m.type == MSG_UNLOCKS && m.unlocks.build_info && m.unlocks.level_node) {   // the build information of a battle (ANY battle node), keyed on its own
        g_level_box.bnode = m.unlocks.level_node;
        memcpy(g_level_box.n_pending, m.unlocks.n_pending, sizeof(g_level_box.n_pending));
        memcpy(g_level_box.pending, m.unlocks.pending, sizeof(g_level_box.pending));
        g_level_box.n_weather = m.unlocks.n_weather;
        memcpy(g_level_box.weather, m.unlocks.weather, sizeof(g_level_box.weather));
    }
    if (m.type == MSG_ROLL) g_roll_ring[g_roll_next++ % kRollRing] = m.roll;
    if (m.type == MSG_BOARD) {
        const BoardMsg& b = m.board;
        BoardAssembled* box = nullptr;
        for (uint32_t k = 0; k < kBoardRing && !box; ++k)
            if (g_board_ring[k].battle == b.battle && g_board_ring[k].turn == b.turn) box = &g_board_ring[k];
        if (!box) { box = &g_board_ring[g_board_next++ % kBoardRing]; *box = BoardAssembled{}; box->battle = b.battle; box->turn = b.turn; }
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
    }
    if (g.count >= kQueueCap) {
        // Drop the newest rather than the oldest. Losing the oldest would
        // reorder the action stream, and an out-of-order ACTION is a desync;
        // a dropped one is at least detectable as a stall at the turn barrier.
        ++g.stats.dropped;
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
        case MSG_SETTING:   return dec_setting(r, m.setting);
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
        case MSG_ROLL:      return dec_roll(r, m.roll);
        case MSG_PEERS:   return dec_peers(r, m.peers);
        case MSG_HALT:    return dec_halt(r, m.halt);
        case MSG_REFUSE:  r.str(m.refuse, sizeof(m.refuse)); return r.ok;
        case MSG_PING:    return true;
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
        send_framed(l.sock, from, payload, len);
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
        uint8_t p[64];
        uint32_t n = enc_peers(p, sizeof(p), mine);
        if (n) send_framed(l.sock, kHostPeer, p, n);
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

        NetMsg m{};
        if (!decode_into(payload, plen, m)) {
            log_line("NET", "!! undecodable %s frame from peer %u, %u bytes -- closing",
                     msg_name(payload[0]), (unsigned)from, plen);
            break;
        }
        m.from = from;
        g.stats.bytes_received += len + 4;
        queue_push(m);
    }

    free(buf);

    InterlockedExchange(&link.live, 0);
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

bool start_link(int slot, SOCKET s, uint8_t id) {
    Link& l = g.links[slot];
    l.sock = s;
    l.id   = id;
    InterlockedExchange(&l.live, 1);
    nodelay_on(s);
    l.thread = CreateThread(nullptr, 0, link_thread, &l, 0, nullptr);
    if (!l.thread) {
        InterlockedExchange(&l.live, 0);
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
            uint8_t p[128];
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
        if (getaddrinfo(g_rl_host, nullptr, &hints, &res) != 0 || !res) return WSAHOST_NOT_FOUND;
        inet_ntop(AF_INET, &((sockaddr_in*)res->ai_addr)->sin_addr, ip, sizeof(ip));
        freeaddrinfo(res);
    }
    (void)stop; (void)gen;
    return dial_one(ip, g_rl_port, out);
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
                        s = steam_bridge_rev_wait(kSteamRevMs, &g.stop, &rw);
                        log_line("NET", s != INVALID_SOCKET ? "the host's call came through" : "!! the host did not call back in time (%d)", rw);
                        if (s == INVALID_SOCKET) why = rw == 2 ? 2 : 3;
                    } else {
                        steam_bridge_rev_cancel();
                        log_line("NET", "!! the request to call back could not be sent");
                    }
                }
            }
            e = s != INVALID_SOCKET ? 0 : (why == 2 ? WSAETIMEDOUT : WSAECONNREFUSED);
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
uint8_t net_peer_pos() {
    if (!g.roster_valid) return 0;
    for (uint8_t i = 0; i < g.roster.count; ++i)
        if (g.roster.ids[i] == g.self) return i;
    return 0;
}

bool net_peer_ids(uint8_t* out, uint8_t cap) {
    if (!g.roster_valid || !out) return false;
    for (uint8_t i = 0; i < g.roster.count && i < cap; ++i) out[i] = g.roster.ids[i];
    return true;
}

void net_shutdown() {
    if (g.role == NetRole::None) return;
    InterlockedExchange(&g.stop, 1);

    // Shut the sockets down before waiting: every receive thread is parked in
    // recv() and will not notice a flag until the call returns. Closing the
    // listener is what releases the accept thread.
    if (g.listener != INVALID_SOCKET) { closesocket(g.listener); g.listener = INVALID_SOCKET; }
    for (int i = 0; i < kMaxPeers; ++i) {
        Link& l = g.links[i];
        if (l.sock != INVALID_SOCKET) { shutdown(l.sock, SD_BOTH); closesocket(l.sock); l.sock = INVALID_SOCKET; }
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
        if (send_framed(l.sock, g.self, payload, len)) any = true;
        else failed = true;
    }
    if (failed) log_line("NET", "!! send failed (WSA %d)", WSAGetLastError());
    return any;
}

bool net_send_peer(uint8_t peer, const uint8_t* payload, uint32_t len) {
    int slot = link_slot_of(peer);
    if (slot < 0) return false;
    return send_framed(g.links[slot].sock, g.self, payload, len);
}

// Every encoder writes into a stack buffer and hands the result to net_send.
// 512 bytes covers every phase-4 message; RUNSTATE will need its own path.
#define MGMP_SEND_WITH(encoder, arg)                       \
    uint8_t p[512];                                        \
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
    uint8_t p[1024];
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
bool net_send_cats    (const CatsMsg& m)     { MGMP_SEND_WITH(enc_cats,     m); }
// Host-authored, so it is NOT in relayed(): net_send already reaches every client.
bool net_send_savewait(const SaveWaitMsg& m) { MGMP_SEND_WITH(enc_savewait, m); }
// Peer-authored and relayed by the host (see relayed()).
bool net_send_abandon(const AbandonMsg& m) { MGMP_SEND_WITH(enc_abandon, m); }
bool net_send_roomctl(const RoomCtlMsg& m) { MGMP_SEND_WITH(enc_roomctl, m); }
// Host-authored, so it is NOT in relayed(): net_send already reaches every client.
bool net_send_mapseeds(const MapSeedsMsg& m) { MGMP_SEND_WITH(enc_mapseeds, m); }
// Host-authored, so it is NOT in relayed() either.
bool net_send_unlocks(const UnlocksMsg& m) { uint8_t p[2048]; const uint32_t n = enc_unlocks(p, sizeof(p), m); return n && net_send(p, n); }   // 2048 since proto 78 (the class names)   // bigger than the 512-byte default since proto 62

// Host-authored, so it is NOT in relayed() either. Bigger than the 512-byte default: a chunk is up to 16 units.
bool net_send_board(const BoardMsg& m) { uint8_t p[1024]; const uint32_t n = enc_board(p, sizeof(p), m); return n && net_send(p, n); }
bool net_send_board_to(uint8_t peer, const BoardMsg& m) { uint8_t p[1024]; const uint32_t n = enc_board(p, sizeof(p), m); return n && net_send_peer(peer, p, n); }
bool net_host_weather(uint64_t node_id, char (*names)[kWeatherLen], uint8_t& n) {
    n = 0;
    if (!g.cs_ready || !names || !node_id) return false;
    EnterCriticalSection(&g.cs);
    const bool ok = g_level_box.bnode == node_id;
    if (ok) { n = g_level_box.n_weather; for (uint8_t i = 0; i < n && i < kWeatherNames; ++i) memcpy(names[i], g_level_box.weather[i], kWeatherLen); }
    LeaveCriticalSection(&g.cs);
    return ok;
}
bool net_send_roll(const RollMsg& m) { uint8_t p[64]; const uint32_t n = enc_roll(p, sizeof(p), m); return n && net_send(p, n); }
bool net_host_roll(uint64_t battle, uint32_t seq, uint8_t site, RollMsg& out) {
    if (!g.cs_ready || !battle) return false;
    EnterCriticalSection(&g.cs);
    bool ok = false;
    for (uint32_t k = 0; k < kRollRing && !ok; ++k)
        if (g_roll_ring[k].battle == battle && g_roll_ring[k].seq == seq && g_roll_ring[k].site == site) { out = g_roll_ring[k]; ok = true; }
    LeaveCriticalSection(&g.cs);
    return ok;
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

bool net_host_pending(uint64_t node_id, PendingEnemy (*out)[kPendingMax], uint8_t* n) {
    for (uint32_t q = 0; q < kPendingQueues; ++q) n[q] = 0;
    if (!g.cs_ready || !out || !node_id) return false;
    EnterCriticalSection(&g.cs);
    const bool ok = g_level_box.bnode == node_id;
    if (ok) for (uint32_t q = 0; q < kPendingQueues; ++q) { n[q] = g_level_box.n_pending[q]; for (uint8_t i = 0; i < n[q] && i < kPendingMax; ++i) out[q][i] = g_level_box.pending[q][i]; }
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
    uint8_t p[512];
    uint32_t n = enc_hello(p, sizeof(p), h);
    return n && net_send_peer(peer, p, n);
}

// To one peer, for replaying a battle already under way to a joiner. A
// broadcast would re-inject decisions into peers that already applied them --
// pend_take cannot tell a replay from a live decision, correctly, because
// there is no difference except who still needs it.
bool net_send_action_to(uint8_t peer, const ActionMsg& a) {
    uint8_t p[512];
    uint32_t n = enc_action(p, sizeof(p), a);
    return n && net_send_peer(peer, p, n);
}

// To one peer: telling a joiner where the run is standing. A broadcast would
// re-drive peers that are already in that node back into it.
bool net_send_enter_node_to(uint8_t peer, const EnterNodeMsg& m) {
    uint8_t p[1024];
    uint32_t n = enc_enter_node(p, sizeof(p), m);
    return n && net_send_peer(peer, p, n);
}

bool net_send_welcome_to(uint8_t peer, const Welcome& w) {
    uint8_t p[512];
    uint32_t n = enc_welcome(p, sizeof(p), w);
    return n && net_send_peer(peer, p, n);
}

bool net_send_refuse_to(uint8_t peer, const char* reason) {
    uint8_t p[512];
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

bool net_send_uqd(const UqdMsg& m) { uint8_t p[128]; const uint32_t n = enc_uqd(p, sizeof(p), m); return n && net_send(p, n); }
bool net_send_setting(const SettingMsg& m) { uint8_t p[16]; const uint32_t n = enc_setting(p, sizeof(p), m); return n && net_send(p, n); }
bool net_send_chat(const ChatMsg& m) { uint8_t p[320]; const uint32_t n = enc_chat(p, sizeof(p), m); return n && net_send(p, n); }
bool net_send_rngl(const RnglMsg& m) { uint8_t p[512]; const uint32_t n = enc_rngl(p, sizeof(p), m); return n && net_send(p, n); }
bool net_send_deep(const DeepMsg& m) { uint8_t p[384]; const uint32_t n = enc_deep(p, sizeof(p), m); return n && net_send(p, n); }
bool net_send_props(const PropsMsg& m) { uint8_t p[1024]; const uint32_t n = enc_props(p, sizeof(p), m); return n && net_send(p, n); }
bool net_send_audit(const AuditMsg& m) {
    uint8_t* p = (uint8_t*)malloc(4096);
    if (!p) return false;
    const uint32_t n = enc_audit(p, 4096, m);
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
    for (uint32_t i = 0; i < kInvBuckets; ++i)
        if (m.inventory.data[i]) { free(m.inventory.data[i]); m.inventory.data[i] = nullptr; }
}

bool net_poll(NetMsg& out) {
    if (!g.cs_ready) return false;
    EnterCriticalSection(&g.cs);
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

NetStats net_stats() {
    NetStats s{};
    if (!g.cs_ready) return s;
    EnterCriticalSection(&g.cs);
    s = g.stats;
    LeaveCriticalSection(&g.cs);
    return s;
}

} // namespace mgmp
