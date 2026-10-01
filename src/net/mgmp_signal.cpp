// mgmp_signal.cpp -- see mgmp_signal.h for why this exists and what it is not.
//
// WHERE THE THREADS MEET, exactly. Getting this wrong is the one way this
// module can corrupt something, so it is stated once and the code follows it:
//
//   WORKER THREAD owns: the socket, the dial, the receive buffer, and the ring
//                       of complete lines. It writes g.sock, g.state, g.stop and
//                       the ring, and it reads g.addr/g.port/g.name -- all of
//                       which the game thread only writes while no worker is
//                       alive (a connect tears the old one down first).
//
//   GAME THREAD owns:   everything the panel reads. Parsing happens here, not on
//                       the worker, so the room list, the member list and the
//                       lobby's status strings are single-threaded state with no
//                       locking at all. The ring is the handover.
//
// So: the game thread never blocks (no dial, no recv, no wait), and the worker
// never touches a string the panel is reading.

#include "mgmp_signal.h"
#include "mgmp_config.h"
#include "mgmp_log.h"
#include "mgmp_session.h"

#include "json.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include <bcrypt.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace mgmp {
namespace {

using nlohmann::json;

// Lines the worker may run ahead of the game thread before it starts dropping.
// The lobby is a handful of messages a second, so overflow means the game
// thread stopped draining -- a bug worth counting, not a buffer worth growing.
constexpr uint32_t kRing       = 64;
constexpr uint32_t kLineMax    = 1024;
constexpr uint32_t kMaxRooms   = 32;
constexpr uint32_t kMaxMembers = 8;

// A dial that has not connected by now is not going to. Five seconds is long
// enough for a slow network and short enough that a wrong address typed into
// the panel reads as an error rather than as a hang.
constexpr DWORD kDialMs = 5000;

// The receive timeout, and the only reason it exists: recv must return often
// enough for the worker to notice g.stop. Without it, shutting the lobby down
// waits for a message that may never come.
constexpr DWORD kRecvPollMs = 500;

// How many times the auto-room rule may act before it gives up. Three covers
// the one race that matters -- two peers with the same room name trying to
// create it at the same moment -- and stops a name that can never be resolved
// from looping forever.
constexpr int kAutoRoomTries = 3;

enum class Req { None, Connect, Disconnect, List, Create, Join, Leave };

struct Request {
    Req      kind = Req::None;
    char     addr[64] = {};
    uint16_t port = 0;
    char     name[32] = {};
    char     arg[48]  = {};   // room name to create, or room id to join
    char     pw[72]   = {};   // the password's digest ("" = none)
};

struct State {
    // --- shared with the worker ---------------------------------------------
    volatile LONG state = (LONG)SignalState::Off;
    volatile LONG stop  = 0;
    SOCKET        sock  = INVALID_SOCKET;
    HANDLE        thread = nullptr;

    // The ring of complete lines, worker -> game thread.
    CRITICAL_SECTION ring_cs;
    bool     ring_cs_ready = false;
    char     lines[kRing][kLineMax];
    uint16_t len[kRing];
    uint32_t head = 0, count = 0, dropped = 0;

    // Serializes a send against the worker clearing g.sock.
    CRITICAL_SECTION send_cs;
    bool     send_cs_ready = false;

    // --- game thread only ---------------------------------------------------
    Request  request;
    char     addr[64] = "127.0.0.1";
    uint16_t port     = 27700;
    char     name[32] = {};
    char     server[80] = {};
    char     error[192] = {};
    char     status[192] = {};
    char     room[16]  = {};
    char     role[8]   = {};
    char     host_addr[64] = {};
    uint16_t host_port = 0;
    char     event[160] = {};
    bool     server_pw = false;        // the server's welcome said it knows room passwords
    bool     room_pw   = false;        // the room we are in has one
    char     error_code[24] = {};      // see signal_error_code
    uint32_t error_seq = 0;

    SignalRoom rooms[kMaxRooms];
    uint32_t   room_count  = 0;
    bool       rooms_valid = false;

    SignalPeer peers[kMaxMembers];
    uint32_t   peer_count = 0;

    bool locked = false;          // the server's word for the room we are in
    int  want_lock = -1;          // -1 none, 0 unlock, 1 lock -- sent on the next update
    bool away = false;            // this player is away; re-sent whenever it could have been lost
    bool away_dirty = false;

    char auto_room[32] = {};
    int  auto_tries    = 0;
    bool auto_done     = false;
    // Set while an auto-room action is waiting for its answer. Without it the
    // rule fires again on the next frame -- the reply has not arrived, the room
    // is still not in the stale list -- and the server answers the second
    // request with "you are already in a room". Found by running it, not by
    // reading it: both peers logged the request twice and the refusal once.
    bool auto_waiting  = false;

    bool wsa_up = false;
};

State g;

// --- shared state helpers ----------------------------------------------------

void set_state(SignalState s) { InterlockedExchange(&g.state, (LONG)s); }
SignalState state() { return (SignalState)InterlockedCompareExchange(&g.state, 0, 0); }

void set_status(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(g.status, sizeof(g.status), _TRUNCATE, fmt, ap);
    va_end(ap);
}

void set_error(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(g.error, sizeof(g.error), _TRUNCATE, fmt, ap);
    va_end(ap);
}

bool wsa_init() {
    if (g.wsa_up) return true;
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    g.wsa_up = true;
    return true;
}

// --- the ring ---------------------------------------------------------------

void ring_push(const char* line, uint32_t n) {
    while (n && (line[n - 1] == '\r' || line[n - 1] == ' ')) --n;   // tolerant of CRLF
    if (n == 0) return;
    if (n >= kLineMax) n = kLineMax - 1;

    EnterCriticalSection(&g.ring_cs);
    if (g.count >= kRing) {
        // Drop the NEWEST and count it, like the transport does and for the
        // same reason: reordering a lobby message would show a stale member
        // list, and a dropped one is visible as a count.
        ++g.dropped;
        LeaveCriticalSection(&g.ring_cs);
        return;
    }
    const uint32_t slot = (g.head + g.count) % kRing;
    memcpy(g.lines[slot], line, n);
    g.lines[slot][n] = 0;
    g.len[slot]      = (uint16_t)n;
    ++g.count;
    LeaveCriticalSection(&g.ring_cs);
}

bool ring_pop(char* out, uint32_t cap) {
    bool got = false;
    EnterCriticalSection(&g.ring_cs);
    if (g.count) {
        const uint32_t i = g.head;
        const uint16_t n = g.len[i];
        if ((uint32_t)n < cap) { memcpy(out, g.lines[i], n); out[n] = 0; got = true; }
        g.head = (g.head + 1) % kRing;
        --g.count;
    }
    LeaveCriticalSection(&g.ring_cs);
    return got;
}

// --- sending ----------------------------------------------------------------

bool send_line(const std::string& line) {
    if (state() != SignalState::Connected) return false;
    std::string out = line;
    if (out.empty() || out.back() != '\n') out.push_back('\n');

    EnterCriticalSection(&g.send_cs);
    bool ok = false;
    SOCKET s = g.sock;
    if (s != INVALID_SOCKET) {
        ok = true;
        size_t off = 0;
        while (off < out.size()) {
            const int n = send(s, out.c_str() + off, (int)(out.size() - off), 0);
            if (n <= 0) { ok = false; break; }
            off += (size_t)n;
        }
    }
    LeaveCriticalSection(&g.send_cs);
    return ok;
}

bool send_obj(const json& o) { return send_line(o.dump(-1, ' ', false, json::error_handler_t::replace)); }

// The two shapes the lobby sends. Built explicitly rather than with brace
// initialisation, because nlohmann reads `{{"k","v"}}` as an object only by a
// special case, and a message that silently went out as an array would fail as
// "unknown message type" at the other end.
bool send_cmd(const char* t) {
    json o = json::object();
    o["t"] = t;
    return send_obj(o);
}

bool send_cmd1(const char* t, const char* key, const std::string& value) {
    json o = json::object();
    o["t"]    = t;
    o[key]    = value;
    return send_obj(o);
}

std::string hello_json() {
    json o = json::object();
    o["t"]    = "hello";
    o["name"] = g.name;
    // What this peer would listen on if it turns out to be the host. The server
    // hands this back to whoever joins the room, which is the whole reason it
    // is sent up front rather than at create time.
    o["port"] = (int)config().net_port;
    return o.dump(-1, ' ', false, json::error_handler_t::replace);
}

// --- the worker -------------------------------------------------------------

// Non-blocking dial with a deadline, so a wrong address fails in five seconds
// instead of twenty and the worker can still be told to stop while it waits.
SOCKET dial(const char* addr, uint16_t port) {
    if (!wsa_init()) { set_error("WSAStartup failed"); return INVALID_SOCKET; }

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    if (InetPtonA(AF_INET, addr, &a.sin_addr) != 1) {
        // Not a dotted quad, so let the resolver try: a hostname is a legitimate
        // thing to type into the panel, and a bad one has to fail as "does not
        // resolve" rather than as a silent five-second timeout.
        addrinfo hints{};
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        char portstr[8];
        _snprintf_s(portstr, sizeof(portstr), _TRUNCATE, "%u", (unsigned)port);
        addrinfo* res = nullptr;
        const int rc = getaddrinfo(addr, portstr, &hints, &res);
        if (rc != 0 || !res) {
            set_error("'%s' is neither an address nor a name that resolves", addr);
            return INVALID_SOCKET;
        }
        memcpy(&a, res->ai_addr, sizeof(sockaddr_in));
        freeaddrinfo(res);
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        set_error("socket failed (WSA %d)", WSAGetLastError());
        return INVALID_SOCKET;
    }

    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    if (connect(s, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR &&
        WSAGetLastError() != WSAEWOULDBLOCK) {
        set_error("connect failed (WSA %d)", WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }

    fd_set wr;
    FD_ZERO(&wr);
    FD_SET(s, &wr);
    timeval tv{};
    tv.tv_sec  = (long)(kDialMs / 1000);
    tv.tv_usec = (long)((kDialMs % 1000) * 1000);
    const int ready = select(0, nullptr, &wr, nullptr, &tv);
    if (ready == 0) {
        set_error("no answer from %s:%u after %lu seconds",
                  addr, (unsigned)port, (unsigned long)(kDialMs / 1000));
        closesocket(s);
        return INVALID_SOCKET;
    }
    if (ready < 0) {
        set_error("select failed (WSA %d)", WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }

    int soerr = 0, slen = sizeof(soerr);
    getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soerr, &slen);
    if (soerr) {
        set_error("connection refused (WSA %d)", soerr);
        closesocket(s);
        return INVALID_SOCKET;
    }

    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);   // back to blocking for the receive loop
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&kRecvPollMs, sizeof(kRecvPollMs));
    return s;
}

DWORD WINAPI worker(LPVOID) {
    const std::string addr = g.addr;
    const uint16_t    port = g.port;

    const SOCKET s = dial(addr.c_str(), port);
    if (s == INVALID_SOCKET) {
        log_line_lvl(LogLevel::Warn, "SIGNAL", "!! %s", g.error);
        set_status("failed: %s", g.error);
        set_state(SignalState::Failed);
        return 0;
    }

    EnterCriticalSection(&g.send_cs);
    g.sock = s;
    LeaveCriticalSection(&g.send_cs);

    set_state(SignalState::Connected);
    log_line("SIGNAL", "connected to %s:%u", addr.c_str(), (unsigned)port);

    // hello goes out from the worker rather than from a queued request, so the
    // server never holds a connection that has not identified itself: the dial
    // and the introduction are one act.
    send_line(hello_json());

    std::string buf;
    buf.reserve(4096);
    char tmp[2048];
    bool closed_by_peer = false;

    while (!InterlockedCompareExchange(&g.stop, 0, 0)) {
        const int n = recv(s, tmp, (int)sizeof(tmp), 0);
        if (n == 0) { closed_by_peer = true; break; }
        if (n < 0) {
            const int e = WSAGetLastError();
            if (e == WSAEINTR || e == WSAETIMEDOUT) continue;   // the poll above
            log_line_lvl(LogLevel::Warn, "SIGNAL", "!! recv failed (WSA %d)", e);
            closed_by_peer = true;
            break;
        }
        buf.append(tmp, (size_t)n);

        size_t start = 0;
        for (;;) {
            const size_t nl = buf.find('\n', start);
            if (nl == std::string::npos) break;
            ring_push(buf.c_str() + start, (uint32_t)(nl - start));
            start = nl + 1;
        }
        if (start) buf.erase(0, start);
        // A line longer than the ring's slot can never be delivered, and letting
        // it grow is a memory leak driven by the peer on the other end.
        if (buf.size() > kLineMax * 2) {
            log_line_lvl(LogLevel::Warn, "SIGNAL",
                         "!! the server sent a line longer than %u bytes -- dropping the link",
                         (unsigned)kLineMax);
            closed_by_peer = true;
            break;
        }
    }

    EnterCriticalSection(&g.send_cs);
    if (g.sock == s) g.sock = INVALID_SOCKET;
    LeaveCriticalSection(&g.send_cs);
    shutdown(s, SD_BOTH);
    closesocket(s);

    if (InterlockedCompareExchange(&g.stop, 0, 0)) {
        // Asked to stop. Not a failure, and saying "closed" here would put an
        // error on the panel every time the player disconnected.
        return 0;
    }

    set_state(SignalState::Closed);
    if (closed_by_peer) {
        set_status("the server closed the connection");
        log_line_lvl(LogLevel::Warn, "SIGNAL", "!! the server closed the connection");
    }
    return 0;
}

void start_worker() {
    InterlockedExchange(&g.stop, 0);
    set_state(SignalState::Connecting);
    g.thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    if (!g.thread) {
        set_error("CreateThread failed (%lu)", GetLastError());
        set_state(SignalState::Failed);
    }
}

void stop_worker() {
    if (!g.thread && g.sock == INVALID_SOCKET) { set_state(SignalState::Off); return; }
    InterlockedExchange(&g.stop, 1);

    // shutdown() is what releases a recv parked in the kernel; the timeout set
    // in dial() is only the backstop.
    EnterCriticalSection(&g.send_cs);
    if (g.sock != INVALID_SOCKET) shutdown(g.sock, SD_BOTH);
    LeaveCriticalSection(&g.send_cs);

    if (g.thread) {
        if (WaitForSingleObject(g.thread, 3000) != WAIT_OBJECT_0)
            log_line_lvl(LogLevel::Warn, "SIGNAL",
                         "!! the signaling thread did not exit within 3s");
        CloseHandle(g.thread);
        g.thread = nullptr;
    }
    InterlockedExchange(&g.stop, 0);
    set_state(SignalState::Off);
}

void forget_room() {
    g.room[0]      = 0;
    g.role[0]      = 0;
    g.host_addr[0] = 0;
    g.host_port    = 0;
    g.peer_count   = 0;
    g.locked       = false;
    g.room_pw      = false;
    g.want_lock    = -1;
}

// --- messages from the server ------------------------------------------------

std::string jstr(const json& o, const char* key) {
    auto it = o.find(key);
    return (it != o.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

int jint(const json& o, const char* key, int fallback = 0) {
    auto it = o.find(key);
    return (it != o.end() && it->is_number_integer()) ? it->get<int>() : fallback;
}

void store_rooms(const json& arr) {
    g.room_count = 0;
    if (arr.is_array()) {
        for (const json& r : arr) {
            if (g.room_count >= kMaxRooms) break;
            if (!r.is_object()) continue;
            SignalRoom& out = g.rooms[g.room_count++];
            strncpy_s(out.id,   sizeof(out.id),   jstr(r, "id").c_str(),   _TRUNCATE);
            strncpy_s(out.name, sizeof(out.name), jstr(r, "name").c_str(), _TRUNCATE);
            strncpy_s(out.host, sizeof(out.host), jstr(r, "host").c_str(), _TRUNCATE);
            out.players = (uint16_t)jint(r, "players");
            out.cap     = (uint16_t)jint(r, "cap");
            { auto pw = r.find("pw"); out.pw = pw != r.end() && pw->is_boolean() && pw->get<bool>(); }
        }
    }
    g.rooms_valid = true;
}

void store_peers(const json& arr) {
    g.peer_count = 0;
    if (!arr.is_array()) return;
    for (const json& p : arr) {
        if (g.peer_count >= kMaxMembers) break;
        if (!p.is_object()) continue;
        SignalPeer& out = g.peers[g.peer_count++];
        strncpy_s(out.name, sizeof(out.name), jstr(p, "name").c_str(), _TRUNCATE);
        strncpy_s(out.role, sizeof(out.role), jstr(p, "role").c_str(), _TRUNCATE);
        auto a = p.find("away");
        out.away = a != p.end() && a->is_boolean() && a->get<bool>();
    }
}

// The room is real; tell the session layer what that means. This is the single
// place a lobby decision becomes a game session, and it goes through the same
// two requests the panel's manual buttons use -- so the runtime-role path is
// the normal path here rather than a diagnostic.
void adopt_room(const char* role) {
    strncpy_s(g.role, sizeof(g.role), role, _TRUNCATE);

    if (_stricmp(role, "host") == 0) {
        session_request_host(config().net_port);
        set_status("room %s -- you are the HOST (listening on port %u)",
                   g.room, (unsigned)config().net_port);
        log_line("SIGNAL", "room %s: we are the host -- hosting on port %u",
                 g.room, (unsigned)config().net_port);
    } else {
        session_request_join(g.host_addr, g.host_port);
        set_status("room %s -- joining the host at %s:%u", g.room, g.host_addr,
                   (unsigned)g.host_port);
        log_line("SIGNAL", "room %s: we are a client -- dialling %s:%u", g.room,
                 g.host_addr, (unsigned)g.host_port);
    }
}

void handle_line(const char* line) {
    json j = json::parse(line, nullptr, /*allow_exceptions=*/false,
                         /*ignore_comments=*/false);
    if (j.is_discarded() || !j.is_object()) {
        log_line_lvl(LogLevel::Warn, "SIGNAL",
                     "!! the server sent a line that is not a JSON object: %.80s", line);
        return;
    }

    const std::string t = jstr(j, "t");

    // ANY answer at all means whatever the auto-room rule sent has been dealt
    // with, refused or not. Cleared here rather than per branch so a message
    // type added later cannot leave the rule waiting forever.
    g.auto_waiting = false;

    if (t == "welcome") {
        { auto pw = j.find("pw"); g.server_pw = pw != j.end() && pw->is_boolean() && pw->get<bool>(); }
        auto it = j.find("rooms");
        if (it != j.end()) store_rooms(*it);
        set_status("connected to %s -- %u room(s)", g.server, (unsigned)g.room_count);
        log_line("SIGNAL", "welcomed by the server as '%s'", g.name);
        return;
    }

    if (t == "rooms") {
        auto it = j.find("rooms");
        if (it != j.end()) store_rooms(*it);
        return;
    }

    if (t == "joined") {
        const std::string role = jstr(j, "role");
        strncpy_s(g.room, sizeof(g.room), jstr(j, "id").c_str(), _TRUNCATE);
        strncpy_s(g.host_addr, sizeof(g.host_addr), jstr(j, "addr").c_str(), _TRUNCATE);
        g.host_port  = (uint16_t)jint(j, "port", 0);
        g.auto_done  = true;
        g.locked     = false;
        { auto pw = j.find("pw"); g.room_pw = pw != j.end() && pw->is_boolean() && pw->get<bool>(); }
        if (g.away) g.away_dirty = true;
        log_line("SIGNAL", "joined room %s as %s (host at %s:%u)", g.room, role.c_str(),
                 g.host_addr, (unsigned)g.host_port);
        adopt_room(role.c_str());
        return;
    }

    if (t == "room") {
        auto it = j.find("players");
        if (it != j.end()) store_peers(*it);
        {
            auto lk = j.find("locked");
            const bool locked = lk != j.end() && lk->is_boolean() && lk->get<bool>();
            if (locked != g.locked)
                log_line("SIGNAL", "room %s is now %s", g.room, locked ? "LOCKED" : "unlocked");
            g.locked = locked;
            auto pw = j.find("pw");
            if (pw != j.end() && pw->is_boolean()) g.room_pw = pw->get<bool>();
        }
        const std::string ev  = jstr(j, "event");
        const std::string who = jstr(j, "who");
        if (!ev.empty()) {
            _snprintf_s(g.event, sizeof(g.event), _TRUNCATE, "%s %s",
                        who.empty() ? "someone" : who.c_str(),
                        ev == "join"  ? "joined the room"
                        : ev == "leave" ? "left the room"
                        : ev == "lock"  ? "locked the room"
                        : ev == "unlock" ? "unlocked the room"
                        : ev == "away"  ? "is playing alone for a while"
                        : ev == "back"  ? "is back"
                                        : "created the room");
        }
        // A client that joined before it had an address still gets it here: the
        // membership message carries the same addr/port the "joined" one does.
        if (!g.host_addr[0]) {
            const std::string addr = jstr(j, "addr");
            if (!addr.empty())
                strncpy_s(g.host_addr, sizeof(g.host_addr), addr.c_str(), _TRUNCATE);
        }
        return;
    }

    if (t == "closed") {
        const std::string msg = jstr(j, "msg");
        log_line_lvl(LogLevel::Warn, "SIGNAL", "!! the room closed: %s", msg.c_str());
        set_status("the room closed -- %s", msg.c_str());
        forget_room();
        // The room was the session's reason to exist. A client whose host has
        // gone has nothing left to wait for.
        session_request_disconnect();
        return;
    }

    if (t == "error") {
        const std::string msg = jstr(j, "msg");
        set_error("%s", msg.c_str());
        strncpy_s(g.error_code, sizeof(g.error_code), jstr(j, "code").c_str(), _TRUNCATE);
        ++g.error_seq;
        log_line_lvl(LogLevel::Warn, "SIGNAL", "!! the server refused: %s", msg.c_str());
        set_status("the server said: %s", msg.c_str());
        return;
    }

    if (t == "pong") return;

    log_line_lvl(LogLevel::Warn, "SIGNAL", "!! unknown message type '%s' from the server",
                 t.c_str());
}

// --- auto room ---------------------------------------------------------------
//
// A named room in mgmp.json turns a two-instance test into "run both" with no
// clicking at all: the first peer to ask creates it, the second finds it in the
// list and joins. It is a convenience, and it does nothing unless signal.room
// names one.

void auto_room_step() {
    if (!g.auto_room[0] || g.auto_done) return;
    if (state() != SignalState::Connected) return;
    if (g.room[0]) { g.auto_done = true; return; }
    if (g.auto_waiting) return;
    if (g.auto_tries >= kAutoRoomTries) {
        log_line_lvl(LogLevel::Warn, "SIGNAL",
                     "!! signal.room = '%s' could not be resolved after %d tries -- "
                     "use the lobby panel", g.auto_room, g.auto_tries);
        g.auto_done = true;
        return;
    }

    if (!g.rooms_valid) {
        g.auto_waiting = true;
        send_cmd("list");
        return;
    }

    for (uint32_t i = 0; i < g.room_count; ++i) {
        if (_stricmp(g.rooms[i].name, g.auto_room) != 0) continue;
        log_line("SIGNAL", "auto room: '%s' already exists (%s), joining it",
                 g.auto_room, g.rooms[i].id);
        ++g.auto_tries;
        g.auto_waiting = true;
        signal_request_join(g.rooms[i].id);
        return;
    }

    log_line("SIGNAL", "auto room: creating '%s'", g.auto_room);
    ++g.auto_tries;
    g.auto_waiting = true;
    signal_request_create(g.auto_room);
}

// --- requests ----------------------------------------------------------------

void apply_request() {
    if (g.request.kind == Req::None) return;
    const Request req = g.request;
    g.request.kind = Req::None;

    switch (req.kind) {
        case Req::Connect:
            stop_worker();
            forget_room();
            g.rooms_valid = false;
            g.room_count  = 0;
            g.error[0]    = 0;
            g.event[0]    = 0;
            strncpy_s(g.addr, sizeof(g.addr), req.addr, _TRUNCATE);
            g.port = req.port;
            if (req.name[0]) strncpy_s(g.name, sizeof(g.name), req.name, _TRUNCATE);
            _snprintf_s(g.server, sizeof(g.server), _TRUNCATE, "%s:%u",
                        g.addr, (unsigned)g.port);
            g.auto_tries = 0;
            g.auto_done  = false;
            set_status("connecting to %s...", g.server);
            log_line("SIGNAL", "connecting to %s as '%s'", g.server, g.name);
            start_worker();
            break;

        case Req::Disconnect:
            if (state() == SignalState::Off && !g.room[0]) {
                set_status("not connected to a server");
                break;
            }
            log_line("SIGNAL", "disconnecting from %s", g.server);
            stop_worker();
            forget_room();
            g.server[0]   = 0;
            g.room_count  = 0;
            g.rooms_valid = false;
            set_status("not connected -- connect to a server to use the lobby");
            break;

        case Req::List:
            send_cmd("list");
            break;

        case Req::Create: {
            if (req.pw[0] && !g.server_pw) {
                // An old server would drop the password and make a public room: refuse instead.
                set_error("this server cannot protect rooms with a password -- update the server");
                set_status("the server cannot protect rooms with a password");
                strncpy_s(g.error_code, sizeof(g.error_code), "server", _TRUNCATE); ++g.error_seq;
                log_line_lvl(LogLevel::Warn, "SIGNAL", "!! not creating a password room: the server does not announce password support");
                break;
            }
            log_line("SIGNAL", "asking the server to create room '%s'%s", req.arg, req.pw[0] ? " (password)" : "");
            json o = json::object();
            o["t"] = "create"; o["name"] = req.arg;
            if (req.pw[0]) o["pw"] = req.pw;
            send_obj(o);
            break;
        }

        case Req::Join: {
            log_line("SIGNAL", "asking to join room %s%s", req.arg, req.pw[0] ? " (with a password)" : "");
            json o = json::object();
            o["t"] = "join"; o["room"] = req.arg;
            if (req.pw[0]) o["pw"] = req.pw;
            send_obj(o);
            break;
        }

        case Req::Leave:
            if (g.room[0]) send_cmd("leave");
            forget_room();
            // A room is a game session's introduction; walking out of it walks
            // out of the session.
            session_request_disconnect();
            set_status("left the room -- still connected to %s", g.server);
            send_cmd("list");
            break;

        case Req::None:
        default:
            break;
    }
}

} // namespace

// ---------------------------------------------------------------------------

void signal_init() {
    const Config& cfg = config();

    InitializeCriticalSection(&g.ring_cs);
    InitializeCriticalSection(&g.send_cs);
    g.ring_cs_ready = true;
    g.send_cs_ready = true;

    strncpy_s(g.addr, sizeof(g.addr), cfg.signal_addr, _TRUNCATE);
    g.port = cfg.signal_port;

    if (cfg.signal_name[0]) {
        strncpy_s(g.name, sizeof(g.name), cfg.signal_name, _TRUNCATE);
    } else {
        // A default that is useful rather than blank: two instances on one
        // machine are told apart by it, and the lobby shows it.
        char  computer[32] = {};
        DWORD n = sizeof(computer);
        if (GetComputerNameA(computer, &n))
            strncpy_s(g.name, sizeof(g.name), computer, _TRUNCATE);
        else
            strncpy_s(g.name, sizeof(g.name), "player", _TRUNCATE);
    }
    strncpy_s(g.auto_room, sizeof(g.auto_room), cfg.signal_room, _TRUNCATE);

    forget_room();
    set_status("not connected -- connect to a server to use the lobby");

    if (cfg.signal_auto_connect) signal_request_connect(g.addr, g.port, g.name);
}

void signal_shutdown() {
    stop_worker();
    forget_room();
}

void signal_update() {
    apply_request();

    if (state() == SignalState::Connected) {
        if (g.want_lock >= 0 && g.room[0]) {
            json o = json::object();
            o["t"] = "lock"; o["on"] = g.want_lock == 1;
            if (send_obj(o)) log_line("SIGNAL", "asking the server to %s room %s", g.want_lock ? "lock" : "unlock", g.room);
        }
        g.want_lock = -1;
        if (g.away_dirty) {
            json o = json::object();
            o["t"] = "away"; o["on"] = g.away;
            if (send_obj(o)) g.away_dirty = false;
        }
    }

    char line[kLineMax];
    while (ring_pop(line, sizeof(line))) handle_line(line);

    // Only when nothing newer is queued: auto_room_step may record a request,
    // and applying it out of order would defeat "the last click wins".
    if (g.request.kind == Req::None) auto_room_step();
}

// --- requests ---------------------------------------------------------------

void signal_request_connect(const char* addr, uint16_t port, const char* name) {
    g.request.kind = Req::Connect;
    strncpy_s(g.request.addr, sizeof(g.request.addr), addr ? addr : "127.0.0.1", _TRUNCATE);
    g.request.port = port;
    strncpy_s(g.request.name, sizeof(g.request.name), name ? name : "", _TRUNCATE);
}

void signal_request_disconnect() { g.request.kind = Req::Disconnect; }
void signal_request_list()       { g.request.kind = Req::List; }

// SHA-256 of a fixed prefix + the password, as lower-case hex. "" for no password; "!" when the hash could not be
// computed (the request is then refused by the server as a wrong password rather than sent in the clear).
static std::string pw_digest(const char* pw) {
    if (!pw || !*pw) return std::string();
    const std::string in = std::string("mgmp-room-v1|") + pw;
    UCHAR hash[32] = {};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)in.data(), (ULONG)in.size(), hash, sizeof(hash)) != 0)
        return "!";
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (UCHAR b : hash) { out.push_back(hex[b >> 4]); out.push_back(hex[b & 15]); }
    return out;
}

void signal_request_create(const char* room_name, const char* password) {
    g.request.kind = Req::Create;
    strncpy_s(g.request.arg, sizeof(g.request.arg), room_name ? room_name : "", _TRUNCATE);
    strncpy_s(g.request.pw, sizeof(g.request.pw), pw_digest(password).c_str(), _TRUNCATE);
}

void signal_request_join(const char* room_id, const char* password) {
    g.request.kind = Req::Join;
    strncpy_s(g.request.arg, sizeof(g.request.arg), room_id ? room_id : "", _TRUNCATE);
    strncpy_s(g.request.pw, sizeof(g.request.pw), pw_digest(password).c_str(), _TRUNCATE);
}

bool signal_server_has_passwords() { return g.server_pw; }
bool signal_room_has_password() { return g.room[0] && g.room_pw; }
const char* signal_error_code() { return g.error_code; }
uint32_t signal_error_seq() { return g.error_seq; }

void signal_request_leave() { g.request.kind = Req::Leave; }

bool signal_request_pending() { return g.request.kind != Req::None; }

void signal_request_lock(bool on) { g.want_lock = on ? 1 : 0; }
bool signal_room_locked() { return g.room[0] && g.locked; }
void signal_set_away(bool on) {
    if (g.away == on && !g.away_dirty) return;
    g.away = on; g.away_dirty = true;
    log_line("SIGNAL", "telling the room this player is %s", on ? "away (playing alone)" : "back");
}
bool signal_self_away() { return g.away; }

// --- state ------------------------------------------------------------------

SignalState signal_state() { return state(); }

const char* signal_state_name(SignalState s) {
    switch (s) {
        case SignalState::Off:        return "off";
        case SignalState::Connecting: return "connecting";
        case SignalState::Connected:  return "connected";
        case SignalState::Failed:     return "failed";
        case SignalState::Closed:     return "closed";
    }
    return "?";
}

const char* signal_error()      { return g.error; }
const char* signal_status()     { return g.status; }
const char* signal_server()     { return g.server; }
const char* signal_name()       { return g.name; }
const char* signal_room()       { return g.room; }
const char* signal_role()       { return g.role; }
const char* signal_host_addr()  { return g.host_addr; }
uint16_t    signal_host_port()  { return g.host_port; }
const char* signal_last_event() { return g.event; }

uint32_t signal_rooms(SignalRoom* out, uint32_t cap) {
    const uint32_t n = g.room_count < cap ? g.room_count : cap;
    for (uint32_t i = 0; i < n; ++i) out[i] = g.rooms[i];
    return n;
}

uint32_t signal_peers(SignalPeer* out, uint32_t cap) {
    const uint32_t n = g.peer_count < cap ? g.peer_count : cap;
    for (uint32_t i = 0; i < n; ++i) out[i] = g.peers[i];
    return n;
}

} // namespace mgmp
