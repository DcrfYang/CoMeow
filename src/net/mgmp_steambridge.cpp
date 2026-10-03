#include "mgmp_steambridge.h"

#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "mgmp_log.h"

#pragma comment(lib, "ws2_32.lib")

namespace mgmp {
namespace {

// --- the flat API, by name (see mgmp_steamtest.cpp for how it was proven) ----------------------------------------------------

typedef void*    (__cdecl* fn_accessor)();
typedef uint64_t (__cdecl* fn_get_id)(void*);
typedef void     (__cdecl* fn_void1)(void*);
typedef int      (__cdecl* fn_relay_status)(void*, void*);
typedef uint32_t (__cdecl* fn_listen)(void*, int, int, const void*);
typedef bool     (__cdecl* fn_close_listen)(void*, uint32_t);
typedef uint32_t (__cdecl* fn_connect)(void*, const void*, int, int, const void*);
typedef int      (__cdecl* fn_accept)(void*, uint32_t);
typedef bool     (__cdecl* fn_close)(void*, uint32_t, int, const char*, bool);
typedef int      (__cdecl* fn_send)(void*, uint32_t, const void*, uint32_t, int, int64_t*);
typedef int      (__cdecl* fn_recv)(void*, uint32_t, void**, int);
typedef bool     (__cdecl* fn_rt_status)(void*, uint32_t, void*, int, void*);
typedef void     (__cdecl* fn_set_id64)(void*, uint64_t);
typedef void     (__cdecl* fn_cfg_setptr)(void*, int, void*);
typedef void     (__cdecl* fn_msg_release)(void*);
typedef int      (__cdecl* fn_status_details)(void*, void*);     // GetAuthenticationStatus(sockets, SteamNetAuthenticationStatus_t*) / GetRelayNetworkStatus(utils, SteamRelayNetworkStatus_t*)

struct Api {
    HMODULE dll = nullptr;
    fn_accessor sockets_acc = nullptr, utils_acc = nullptr, user_acc = nullptr;
    fn_get_id get_steamid = nullptr;
    fn_void1 relay_init = nullptr, run_callbacks = nullptr;
    fn_listen listen = nullptr;
    fn_close_listen close_listen = nullptr;
    fn_connect connect = nullptr;
    fn_accept accept = nullptr;
    fn_close close = nullptr;
    fn_send send = nullptr;
    fn_recv recv = nullptr;
    fn_rt_status rt_status = nullptr;
    fn_set_id64 set_id64 = nullptr;
    fn_cfg_setptr cfg_setptr = nullptr;
    fn_msg_release msg_release = nullptr;
    fn_status_details get_auth = nullptr, get_relay = nullptr;     // optional: diagnostics only
    void* sockets = nullptr;
    void* utils = nullptr;
};
Api A;

template <class T> bool resolve(T& out, const char* name) {
    out = (T)GetProcAddress(A.dll, name);
    if (!out) log_line("STEAMNET", "!! steam_api64.dll does not export %s", name);
    return out != nullptr;
}

bool resolve_all() {
    A.dll = GetModuleHandleW(L"steam_api64.dll");
    if (!A.dll) return false;
    bool ok = true;
    ok &= resolve(A.sockets_acc, "SteamAPI_SteamNetworkingSockets_SteamAPI_v012");
    ok &= resolve(A.utils_acc, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004");
    ok &= resolve(A.user_acc, "SteamAPI_SteamUser_v023");
    ok &= resolve(A.get_steamid, "SteamAPI_ISteamUser_GetSteamID");
    ok &= resolve(A.relay_init, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess");
    ok &= resolve(A.run_callbacks, "SteamAPI_ISteamNetworkingSockets_RunCallbacks");
    ok &= resolve(A.listen, "SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P");
    ok &= resolve(A.close_listen, "SteamAPI_ISteamNetworkingSockets_CloseListenSocket");
    ok &= resolve(A.connect, "SteamAPI_ISteamNetworkingSockets_ConnectP2P");
    ok &= resolve(A.accept, "SteamAPI_ISteamNetworkingSockets_AcceptConnection");
    ok &= resolve(A.close, "SteamAPI_ISteamNetworkingSockets_CloseConnection");
    ok &= resolve(A.send, "SteamAPI_ISteamNetworkingSockets_SendMessageToConnection");
    ok &= resolve(A.recv, "SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection");
    ok &= resolve(A.rt_status, "SteamAPI_ISteamNetworkingSockets_GetConnectionRealTimeStatus");
    ok &= resolve(A.set_id64, "SteamAPI_SteamNetworkingIdentity_SetSteamID64");
    ok &= resolve(A.cfg_setptr, "SteamAPI_SteamNetworkingConfigValue_t_SetPtr");
    ok &= resolve(A.msg_release, "SteamAPI_SteamNetworkingMessage_t_Release");
    A.get_auth  = (fn_status_details)GetProcAddress(A.dll, "SteamAPI_ISteamNetworkingSockets_GetAuthenticationStatus");
    A.get_relay = (fn_status_details)GetProcAddress(A.dll, "SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus");
    return ok;
}

constexpr int kStateConnecting = 1, kStateConnected = 3, kStateClosedByPeer = 4, kStateProblem = 5;
constexpr int kSendReliableNoNagle = 8 | 1;          // k_nSteamNetworkingSend_Reliable | k_nSteamNetworkingSend_NoNagle
constexpr int kCfgCallbackStatusChanged = 201;
constexpr int kVirtualPort = 27;                     // our own; mgmp_steamtest uses 0
constexpr int kVirtualPortRev = 28;                  // the joiner's listen port for a host-initiated (reverse) call
constexpr ULONGLONG kOutDialMs = 20000;
constexpr int kChunk = 16 * 1024;                    // bytes read from TCP per message
constexpr int kMaxEntries = 8;

// --- the bridge ----------------------------------------------------------------------------------------------------------

struct Entry {
    bool     used = false;
    uint32_t h = 0;             // the Steam connection
    SOCKET   s = INVALID_SOCKET; // the loopback TCP end this bridge owns
    bool     connected = false; // Steam says the connection is up
    bool     dial = false;      // made by steam_bridge_dial (the joiner side)
    bool     closing = false;
    bool     rev_in = false;    // the joiner side of a host-initiated (reverse) call
    bool     out = false;       // the host side of a host-initiated call
};
Entry g_e[kMaxEntries];

struct DialReq {
    volatile LONG state = 0;     // 0 idle, 1 asked, 2 connecting, 3 done (ok), 4 done (failed), 5 cancelled by the caller
    uint64_t id = 0;
    SOCKET   caller_end = INVALID_SOCKET;
    int      entry = -1;
    int      why = 0;
    ULONGLONG deadline = 0;
};
DialReq g_req;

struct RevWait {                 // the joiner waiting for the host's call
    volatile LONG state = 0;     // 0 idle, 1 armed, 2 call accepted (connecting), 3 done (ok), 4 done (failed), 5 cancelled by the caller
    SOCKET   caller_end = INVALID_SOCKET;
    int      entry = -1;
};
RevWait g_rev;

struct OutReq {                  // the host's host-initiated dial
    volatile LONG state = 0;     // 0 idle, 1 asked, 2 connecting
    uint64_t id = 0;
    int      entry = -1;
    ULONGLONG since = 0;
};
OutReq g_out;
volatile LONG g_want_listen2 = 0;
uint32_t g_listen2_h = 0;

// Steam's own state (see steam_bridge_status_text) and the last connection failure.
volatile LONG g_auth_avail = 0, g_relay_avail = 0, g_cfg_avail = 0;
char g_status_line[260] = "not polled yet";
volatile LONG g_last_reason = 0;
char g_last_dbg[132] = {};

volatile LONG g_started = 0;
volatile LONG g_ready = 0;
volatile LONG g_dead = 0;       // the worker found no usable Steam and ended
volatile LONG g_want_listen = 0;
volatile LONG g_game_port = 0;
uint32_t g_listen_h = 0;
uint64_t g_me = 0;
unsigned char g_cfg[32];

SOCKET make_nodelay(SOCKET s) {
    BOOL on = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof(on));
    return s;
}

bool loopback_pair(SOCKET& a, SOCKET& b) {
    a = b = INVALID_SOCKET;
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (l == INVALID_SOCKET) return false;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof(sa);
    if (bind(l, (sockaddr*)&sa, sizeof(sa)) != 0 || listen(l, 1) != 0 || getsockname(l, (sockaddr*)&sa, &len) != 0) { closesocket(l); return false; }
    SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c == INVALID_SOCKET || connect(c, (sockaddr*)&sa, sizeof(sa)) != 0) { if (c != INVALID_SOCKET) closesocket(c); closesocket(l); return false; }
    SOCKET d = accept(l, nullptr, nullptr);
    closesocket(l);
    if (d == INVALID_SOCKET) { closesocket(c); return false; }
    a = make_nodelay(c);
    b = make_nodelay(d);
    return true;
}

SOCKET connect_local(uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(port);
    if (connect(s, (sockaddr*)&sa, sizeof(sa)) != 0) { closesocket(s); return INVALID_SOCKET; }
    return make_nodelay(s);
}

int steam_state(uint32_t h) {
    alignas(8) unsigned char st[512] = {};
    return A.rt_status(A.sockets, h, st, 0, nullptr) ? *(int*)st : 0;
}

int find_entry(uint32_t h) {
    for (int i = 0; i < kMaxEntries; ++i) if (g_e[i].used && g_e[i].h == h) return i;
    return -1;
}

int free_entry() {
    for (int i = 0; i < kMaxEntries; ++i) if (!g_e[i].used) return i;
    return -1;
}

void drop_entry(int i, const char* why) {
    Entry& e = g_e[i];
    if (!e.used) return;
    log_line("STEAMNET", "bridge %u closed (%s)", e.h, why);
    if (e.h) A.close(A.sockets, e.h, 0, why, false);
    if (e.s != INVALID_SOCKET) { shutdown(e.s, SD_BOTH); closesocket(e.s); }
    e = Entry{};
}

// Why Steam ended a connection, from SteamNetConnectionStatusChangedCallback_t (m_hConn @0, m_info @8: m_eState @+176, m_eEndReason @+180, m_szEndDebug @+184).
// Read defensively -- a connection that "failed to connect" used to say nothing about why.
void log_end_reason(const void* cb, uint32_t h, int st) {
    __try {
        const unsigned char* c = (const unsigned char*)cb;
        const int reason = *(const int*)(c + 8 + 180);
        char dbg[129] = {};
        memcpy(dbg, c + 8 + 184, 128);
        for (int k = 0; k < 128 && dbg[k]; ++k) if ((unsigned char)dbg[k] < 32 || (unsigned char)dbg[k] > 126) { dbg[k] = 0; break; }
        log_line("STEAMNET", "connection %u ended: state %d, end reason %d '%s'", h, st, reason, dbg);
        InterlockedExchange(&g_last_reason, reason);
        memcpy(g_last_dbg, dbg, sizeof(g_last_dbg) - 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Steam calls this inside RunCallbacks (on the bridge thread) for every state change of a connection made with it set.
// The listen socket a connection came in on (SteamNetConnectionInfo_t::m_hListenSocket @144 inside m_info @8). Read defensively.
uint32_t listen_of(const void* cb) {
    __try { return *(const uint32_t*)((const unsigned char*)cb + 8 + 144); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// A Steam call arrived on the joiner's reverse listen socket: accepted only while waiting for it (steam_bridge_rev_arm).
void accept_reverse(uint32_t h) {
    if (InterlockedCompareExchange(&g_rev.state, 0, 0) != 1) { A.close(A.sockets, h, 0, "not expected", false); log_line("STEAMNET", "a Steam call arrived on the reverse port but nobody is waiting for it -- refused"); return; }
    const int slot = free_entry();
    SOCKET a = INVALID_SOCKET, b = INVALID_SOCKET;
    if (slot < 0 || !loopback_pair(a, b)) { A.close(A.sockets, h, 0, "busy", false); return; }
    if (A.accept(A.sockets, h) != 1) { closesocket(a); closesocket(b); A.close(A.sockets, h, 0, "accept failed", false); return; }
    g_e[slot].used = true; g_e[slot].h = h; g_e[slot].s = b; g_e[slot].rev_in = true;
    g_rev.entry = slot; g_rev.caller_end = a;
    InterlockedExchange(&g_rev.state, 2);
    log_line("STEAMNET", "the host is calling us back over Steam (connection %u) -- accepted", h);
}

void __cdecl on_status(void* info) {
    const uint32_t h = *(uint32_t*)info;               // SteamNetConnectionStatusChangedCallback_t::m_hConn
    const int st = steam_state(h);
    int i = find_entry(h);
    if (st == kStateConnecting && i < 0 && g_listen2_h && listen_of(info) == g_listen2_h) { accept_reverse(h); return; }
    if (st == kStateConnecting && i < 0 && g_listen_h) {              // a Steam peer is calling the host
        const uint16_t port = (uint16_t)InterlockedCompareExchange(&g_game_port, 0, 0);
        const int slot = free_entry();
        if (slot < 0 || !port) { A.close(A.sockets, h, 0, "busy", false); return; }
        SOCKET local = connect_local(port);
        if (local == INVALID_SOCKET) {
            log_line("STEAMNET", "!! a Steam peer called but the game's own port %u did not answer -- refused", (unsigned)port);
            A.close(A.sockets, h, 0, "no game", false);
            return;
        }
        if (A.accept(A.sockets, h) != 1) { closesocket(local); A.close(A.sockets, h, 0, "accept failed", false); return; }
        g_e[slot].used = true; g_e[slot].h = h; g_e[slot].s = local;
        log_line("STEAMNET", "a Steam peer is calling (connection %u) -- bridged to 127.0.0.1:%u", h, (unsigned)port);
        return;
    }
    if (i < 0) { if (st == kStateClosedByPeer || st == kStateProblem) { log_end_reason(info, h, st); A.close(A.sockets, h, 0, "gone", false); } return; }
    if (st == kStateConnected && !g_e[i].connected) {
        g_e[i].connected = true;
        log_line("STEAMNET", "Steam connection %u is up%s", h, g_e[i].dial ? " (joiner side)" : g_e[i].rev_in ? " (joiner side, host-initiated)" : g_e[i].out ? " (host side, host-initiated)" : " (host side)");
        if (g_e[i].rev_in && g_rev.entry == i && InterlockedCompareExchange(&g_rev.state, 0, 0) == 2) InterlockedExchange(&g_rev.state, 3);
    } else if (st == kStateClosedByPeer || st == kStateProblem) {
        log_end_reason(info, h, st);
        g_e[i].closing = true;
    }
}

void pump_entry(int i) {
    Entry& e = g_e[i];
    // Steam -> TCP
    for (;;) {
        void* msgs[8] = {};
        const int n = A.recv(A.sockets, e.h, msgs, 8);
        if (n <= 0) break;
        for (int k = 0; k < n; ++k) {
            const char* data = *(const char**)msgs[k];
            const int len = *(int*)((char*)msgs[k] + 8);
            int off = 0;
            while (!e.closing && off < len) {
                const int w = send(e.s, data + off, len - off, 0);
                if (w <= 0) { e.closing = true; break; }
                off += w;
            }
            A.msg_release(msgs[k]);
        }
    }
}

// TCP -> Steam for every entry that has bytes waiting.
void pump_tcp() {
    fd_set rd;
    FD_ZERO(&rd);
    int n = 0;
    for (int i = 0; i < kMaxEntries; ++i)
        if (g_e[i].used && g_e[i].connected && !g_e[i].closing && g_e[i].s != INVALID_SOCKET) { FD_SET(g_e[i].s, &rd); ++n; }
    if (!n) { Sleep(4); return; }
    timeval tv{ 0, 4000 };
    if (select(0, &rd, nullptr, nullptr, &tv) <= 0) return;
    static char buf[kChunk];
    for (int i = 0; i < kMaxEntries; ++i) {
        Entry& e = g_e[i];
        if (!e.used || e.s == INVALID_SOCKET || !FD_ISSET(e.s, &rd)) continue;
        const int r = recv(e.s, buf, sizeof(buf), 0);
        if (r <= 0) { e.closing = true; continue; }
        // 25 = k_EResultLimitExceeded: Steam's send buffer is full for a moment (a big transfer). Wait for it rather than lose bytes.
        int res = A.send(A.sockets, e.h, buf, (uint32_t)r, kSendReliableNoNagle, nullptr);
        for (int tries = 0; res == 25 && tries < 500; ++tries) {
            Sleep(2);
            A.run_callbacks(A.sockets);
            res = A.send(A.sockets, e.h, buf, (uint32_t)r, kSendReliableNoNagle, nullptr);
        }
        if (res != 1) { log_line("STEAMNET", "!! send on connection %u failed (%d)", e.h, res); e.closing = true; }
    }
}

void handle_dial_request() {
    const LONG st = InterlockedCompareExchange(&g_req.state, 0, 0);
    if (st == 5) {                                        // the caller gave up
        if (g_req.entry >= 0) drop_entry(g_req.entry, "dial cancelled");
        if (g_req.caller_end != INVALID_SOCKET) { closesocket(g_req.caller_end); g_req.caller_end = INVALID_SOCKET; }
        g_req.entry = -1;
        InterlockedExchange(&g_req.state, 0);
        return;
    }
    if (st == 1) {
        const int slot = free_entry();
        SOCKET a = INVALID_SOCKET, b = INVALID_SOCKET;
        if (slot < 0 || !loopback_pair(a, b)) { g_req.why = 3; InterlockedExchange(&g_req.state, 4); return; }
        alignas(8) unsigned char ident[256] = {};
        A.set_id64(ident, g_req.id);
        const uint32_t h = A.connect(A.sockets, ident, kVirtualPort, 1, g_cfg);
        if (!h) { closesocket(a); closesocket(b); g_req.why = 3; InterlockedExchange(&g_req.state, 4); return; }
        g_e[slot].used = true; g_e[slot].h = h; g_e[slot].s = b; g_e[slot].dial = true;
        g_req.entry = slot; g_req.caller_end = a;
        log_line("STEAMNET", "connecting to SteamID64 %llu (connection %u) -- status: %s", (unsigned long long)g_req.id, h, g_status_line);
        InterlockedExchange(&g_req.state, 2);
        return;
    }
    if (st == 2) {
        Entry& e = g_e[g_req.entry];
        if (!e.used || e.closing) {                       // closed under us
            if (g_req.caller_end != INVALID_SOCKET) { closesocket(g_req.caller_end); g_req.caller_end = INVALID_SOCKET; }
            if (e.used) drop_entry(g_req.entry, "failed to connect");
            g_req.why = 3; g_req.entry = -1;
            InterlockedExchange(&g_req.state, 4);
        } else if (e.connected) {
            g_req.entry = -1;                             // the entry lives on, now owned by the pump
            InterlockedExchange(&g_req.state, 3);
        }
    }
}

const char* avail_name(int a) {
    switch (a) {
        case 100: return "ready";
        case 3: return "attempting";
        case 2: return "waiting";
        case 1: return "not tried";
        case 0: return "unknown";
        case -10: return "retrying";
        case -100: return "failed before";
        case -101: return "FAILED";
        case -102: return "cannot try";
        default: return "?";
    }
}

// Once a second: Steam's authentication status (the certificate) and relay network status; a line in the log whenever they change.
void poll_status() {
    static ULONGLONG next = 0;
    static char last_sig[300] = {};
    const ULONGLONG now = GetTickCount64();
    if (now < next || (!A.get_auth && !A.get_relay)) return;
    next = now + 1000;
    alignas(8) unsigned char au[320] = {}, rl[320] = {};
    int a = 0, r = 0, cfg = 0, any = 0;
    char adbg[132] = {}, rdbg[132] = {};
    __try {
        if (A.get_auth && A.sockets)  { a = A.get_auth(A.sockets, au); memcpy(adbg, au + 4, sizeof(adbg) - 1); }
        if (A.get_relay && A.utils)   { r = A.get_relay(A.utils, rl); cfg = *(int*)(rl + 8); any = *(int*)(rl + 12); memcpy(rdbg, rl + 16, sizeof(rdbg) - 1); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    char* const dbgs[2] = { adbg, rdbg };
    for (char* p : dbgs) for (int k = 0; k < 131 && p[k]; ++k) if ((unsigned char)p[k] < 32 || (unsigned char)p[k] > 126) { p[k] = 0; break; }
    InterlockedExchange(&g_auth_avail, a); InterlockedExchange(&g_relay_avail, r); InterlockedExchange(&g_cfg_avail, cfg);
    char line[260];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "authentication: %s%s%s%s | relay network: %s | network config: %s | any relay: %s%s%s%s",
                avail_name(a), adbg[0] ? " (" : "", adbg, adbg[0] ? ")" : "", avail_name(r), avail_name(cfg), avail_name(any), rdbg[0] ? " (" : "", rdbg, rdbg[0] ? ")" : "");
    strncpy_s(g_status_line, sizeof(g_status_line), line, _TRUNCATE);
    if (strcmp(line, last_sig) != 0) {
        strncpy_s(last_sig, sizeof(last_sig), line, _TRUNCATE);
        log_line("STEAMNET", "Steam networking status -- %s", line);
    }
}

void handle_rev() {
    const LONG st = InterlockedCompareExchange(&g_rev.state, 0, 0);
    if (st == 5) {                                          // the joiner stopped waiting
        if (g_rev.entry >= 0) drop_entry(g_rev.entry, "reverse call cancelled");
        if (g_rev.caller_end != INVALID_SOCKET) { closesocket(g_rev.caller_end); g_rev.caller_end = INVALID_SOCKET; }
        g_rev.entry = -1;
        InterlockedExchange(&g_rev.state, 0);
    } else if (st == 2) {
        Entry& e = g_e[g_rev.entry];
        if (!e.used || e.closing) {
            if (g_rev.caller_end != INVALID_SOCKET) { closesocket(g_rev.caller_end); g_rev.caller_end = INVALID_SOCKET; }
            if (e.used) drop_entry(g_rev.entry, "reverse call failed");
            g_rev.entry = -1;
            InterlockedExchange(&g_rev.state, 4);
        }
    }
}

void handle_out() {
    const LONG st = InterlockedCompareExchange(&g_out.state, 0, 0);
    if (st == 1) {
        const uint16_t port = (uint16_t)InterlockedCompareExchange(&g_game_port, 0, 0);
        const int slot = free_entry();
        if (slot < 0 || !port) { log_line("STEAMNET", "!! the host-initiated call was asked for but there is no free slot or no game port"); InterlockedExchange(&g_out.state, 0); return; }
        SOCKET local = connect_local(port);
        if (local == INVALID_SOCKET) { log_line("STEAMNET", "!! the host-initiated call: the game's own port %u did not answer", (unsigned)port); InterlockedExchange(&g_out.state, 0); return; }
        alignas(8) unsigned char ident[256] = {};
        A.set_id64(ident, g_out.id);
        const uint32_t h = A.connect(A.sockets, ident, kVirtualPortRev, 1, g_cfg);
        if (!h) { closesocket(local); InterlockedExchange(&g_out.state, 0); return; }
        g_e[slot].used = true; g_e[slot].h = h; g_e[slot].s = local; g_e[slot].out = true;
        g_out.entry = slot; g_out.since = GetTickCount64();
        log_line("STEAMNET", "calling SteamID64 %llu back (connection %u) -- the host-initiated direction (status: %s)", (unsigned long long)g_out.id, h, g_status_line);
        InterlockedExchange(&g_out.state, 2);
    } else if (st == 2) {
        Entry& e = g_e[g_out.entry];
        if (!e.used || e.closing) { if (e.used) drop_entry(g_out.entry, "host-initiated call failed"); g_out.entry = -1; InterlockedExchange(&g_out.state, 0); }
        else if (e.connected) { g_out.entry = -1; InterlockedExchange(&g_out.state, 0); }
        else if (GetTickCount64() - g_out.since > kOutDialMs) { drop_entry(g_out.entry, "host-initiated call timed out"); g_out.entry = -1; InterlockedExchange(&g_out.state, 0); }
    }
}

DWORD WINAPI worker(LPVOID) {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    if (!resolve_all()) { InterlockedExchange(&g_dead, 1); log_line("STEAMNET", "Steam networking is not available in this process -- the TCP path only"); return 0; }
    for (int i = 0; i < 1800 && !A.sockets; ++i) { A.sockets = A.sockets_acc(); if (!A.sockets) Sleep(100); }
    A.utils = A.utils_acc();
    void* user = A.user_acc();
    if (!A.sockets || !A.utils || !user) { InterlockedExchange(&g_dead, 1); log_line("STEAMNET", "Steam never became usable here -- the TCP path only"); return 0; }
    g_me = A.get_steamid(user);
    A.relay_init(A.utils);
    memset(g_cfg, 0, sizeof(g_cfg));
    A.cfg_setptr(g_cfg, kCfgCallbackStatusChanged, (void*)&on_status);
    InterlockedExchange(&g_ready, 1);
    log_line("STEAMNET", "Steam networking is ready: this account is %llu", (unsigned long long)g_me);

    for (;;) {
        // listen socket follows the host_start / host_stop wishes
        const bool want = InterlockedCompareExchange(&g_want_listen, 0, 0) != 0;
        if (want && !g_listen_h) {
            g_listen_h = A.listen(A.sockets, kVirtualPort, 1, g_cfg);
            log_line("STEAMNET", "listening for Steam peers on virtual port %d (handle %u) -- status: %s", kVirtualPort, g_listen_h, g_status_line);
        } else if (!want && g_listen_h) {
            A.close_listen(A.sockets, g_listen_h);
            g_listen_h = 0;
            for (int i = 0; i < kMaxEntries; ++i) if (g_e[i].used && !g_e[i].dial) drop_entry(i, "host stopped");
            log_line("STEAMNET", "no longer listening for Steam peers");
        }

        {   // the joiner's reverse listen socket follows client_listen_start / stop
            const bool want2 = InterlockedCompareExchange(&g_want_listen2, 0, 0) != 0;
            if (want2 && !g_listen2_h) { g_listen2_h = A.listen(A.sockets, kVirtualPortRev, 1, g_cfg); log_line("STEAMNET", "listening for the host's call on virtual port %d (handle %u)", kVirtualPortRev, g_listen2_h); }
            else if (!want2 && g_listen2_h) { A.close_listen(A.sockets, g_listen2_h); g_listen2_h = 0; log_line("STEAMNET", "no longer listening for the host's call"); }
        }
        A.run_callbacks(A.sockets);
        poll_status();
        handle_dial_request();
        handle_rev();
        handle_out();
        for (int i = 0; i < kMaxEntries; ++i) if (g_e[i].used && g_e[i].connected) pump_entry(i);
        pump_tcp();
        for (int i = 0; i < kMaxEntries; ++i)
            if (g_e[i].used && g_e[i].closing && !(g_req.entry == i) && !(g_rev.entry == i && InterlockedCompareExchange(&g_rev.state, 0, 0) == 2) &&
                !(g_out.entry == i && InterlockedCompareExchange(&g_out.state, 0, 0) == 2)) drop_entry(i, "closed");
        // a joiner entry that is up but whose caller never came for it, or any entry nobody is waiting on: nothing to do
    }
    return 0;
}

} // namespace

void steam_bridge_init() {
    if (InterlockedCompareExchange(&g_started, 1, 0)) return;
    HANDLE h = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

bool steam_bridge_ready() { return InterlockedCompareExchange(&g_ready, 0, 0) != 0; }
uint64_t steam_bridge_id() { return steam_bridge_ready() ? g_me : 0; }

void steam_bridge_host_start(uint16_t game_port) {
    steam_bridge_init();
    InterlockedExchange(&g_game_port, game_port);
    InterlockedExchange(&g_want_listen, 1);
}

void steam_bridge_host_stop() { InterlockedExchange(&g_want_listen, 0); }

bool steam_bridge_dead() { return InterlockedCompareExchange(&g_dead, 0, 0) != 0; }

void steam_bridge_status_text(char* out, unsigned cap) {
    if (!out || !cap) return;
    if (steam_bridge_dead()) { _snprintf_s(out, cap, _TRUNCATE, "Steam networking is not available"); return; }
    if (!steam_bridge_ready()) { _snprintf_s(out, cap, _TRUNCATE, "Steam networking is starting"); return; }
    _snprintf_s(out, cap, _TRUNCATE, "%s", g_status_line);
}

void steam_bridge_last_failure(int* reason, char* dbg, unsigned cap) {
    if (reason) *reason = (int)InterlockedCompareExchange(&g_last_reason, 0, 0);
    if (dbg && cap) _snprintf_s(dbg, cap, _TRUNCATE, "%s", g_last_dbg);
}

void steam_bridge_client_listen_start() { steam_bridge_init(); InterlockedExchange(&g_want_listen2, 1); }
void steam_bridge_client_listen_stop() { InterlockedExchange(&g_want_listen2, 0); }

bool steam_bridge_rev_arm() {
    if (!steam_bridge_ready()) return false;
    InterlockedExchange(&g_want_listen2, 1);
    while (InterlockedCompareExchange(&g_rev.state, 0, 0) != 0) Sleep(10);      // a previous wait is still being cleaned up
    g_rev.caller_end = INVALID_SOCKET; g_rev.entry = -1;
    InterlockedExchange(&g_rev.state, 1);
    return true;
}

SOCKET steam_bridge_rev_wait(unsigned long timeout_ms, volatile long* stop, int* why) {
    if (why) *why = 0;
    const ULONGLONG t0 = GetTickCount64();
    for (;;) {
        const LONG st = InterlockedCompareExchange(&g_rev.state, 0, 0);
        if (st == 3) {
            SOCKET s = g_rev.caller_end;
            g_rev.caller_end = INVALID_SOCKET; g_rev.entry = -1;
            InterlockedExchange(&g_rev.state, 0);
            return s;
        }
        if (st == 4) { if (why) *why = 3; InterlockedExchange(&g_rev.state, 0); return INVALID_SOCKET; }
        if (st == 0) { if (why) *why = 3; return INVALID_SOCKET; }
        if ((stop && *stop) || GetTickCount64() - t0 >= timeout_ms) {
            if (why) *why = 2;
            InterlockedExchange(&g_rev.state, 5);               // the worker cleans up
            while (InterlockedCompareExchange(&g_rev.state, 0, 0) != 0) Sleep(10);
            return INVALID_SOCKET;
        }
        Sleep(20);
    }
}

void steam_bridge_rev_cancel() {
    if (InterlockedCompareExchange(&g_rev.state, 0, 0) == 0) return;
    InterlockedExchange(&g_rev.state, 5);
    while (InterlockedCompareExchange(&g_rev.state, 0, 0) != 0) Sleep(10);
}

void steam_bridge_dial_out(uint64_t joiner_steamid) {
    if (!joiner_steamid || !steam_bridge_ready()) return;
    if (InterlockedCompareExchange(&g_out.state, 0, 0) != 0) { log_line("STEAMNET", "a host-initiated call is already in progress -- the new request is ignored"); return; }
    g_out.id = joiner_steamid;
    InterlockedExchange(&g_out.state, 1);
}

SOCKET steam_bridge_dial(uint64_t host_steamid, unsigned long timeout_ms, volatile long* stop, int* why) {
    if (why) *why = 0;
    steam_bridge_init();
    // wait for Steam to be ready, within the timeout
    const ULONGLONG t0 = GetTickCount64();
    while (!steam_bridge_ready() && !InterlockedCompareExchange(&g_dead, 0, 0) && GetTickCount64() - t0 < timeout_ms && !(stop && *stop)) Sleep(50);
    if (!steam_bridge_ready()) { if (why) *why = 1; return INVALID_SOCKET; }
    // one dial at a time
    while (InterlockedCompareExchange(&g_req.state, 0, 0) != 0 && GetTickCount64() - t0 < timeout_ms && !(stop && *stop)) Sleep(20);
    if (InterlockedCompareExchange(&g_req.state, 0, 0) != 0) { if (why) *why = 2; return INVALID_SOCKET; }
    g_req.id = host_steamid;
    g_req.why = 0;
    g_req.caller_end = INVALID_SOCKET;
    g_req.entry = -1;
    InterlockedExchange(&g_req.state, 1);
    for (;;) {
        const LONG st = InterlockedCompareExchange(&g_req.state, 0, 0);
        if (st == 3) {
            SOCKET s = g_req.caller_end;
            g_req.caller_end = INVALID_SOCKET;
            InterlockedExchange(&g_req.state, 0);
            return s;
        }
        if (st == 4) {
            if (why) *why = g_req.why ? g_req.why : 3;
            InterlockedExchange(&g_req.state, 0);
            return INVALID_SOCKET;
        }
        if ((stop && *stop) || GetTickCount64() - t0 >= timeout_ms) {
            if (why) *why = 2;
            InterlockedExchange(&g_req.state, 5);              // the worker cleans up
            while (InterlockedCompareExchange(&g_req.state, 0, 0) != 0) Sleep(10);
            return INVALID_SOCKET;
        }
        Sleep(20);
    }
}

} // namespace mgmp
