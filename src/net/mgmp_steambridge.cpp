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
    return ok;
}

constexpr int kStateConnecting = 1, kStateConnected = 3, kStateClosedByPeer = 4, kStateProblem = 5;
constexpr int kSendReliableNoNagle = 8 | 1;          // k_nSteamNetworkingSend_Reliable | k_nSteamNetworkingSend_NoNagle
constexpr int kCfgCallbackStatusChanged = 201;
constexpr int kVirtualPort = 27;                     // our own; mgmp_steamtest uses 0
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

// Steam calls this inside RunCallbacks (on the bridge thread) for every state change of a connection made with it set.
void __cdecl on_status(void* info) {
    const uint32_t h = *(uint32_t*)info;               // SteamNetConnectionStatusChangedCallback_t::m_hConn
    const int st = steam_state(h);
    int i = find_entry(h);
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
    if (i < 0) { if (st == kStateClosedByPeer || st == kStateProblem) A.close(A.sockets, h, 0, "gone", false); return; }
    if (st == kStateConnected && !g_e[i].connected) {
        g_e[i].connected = true;
        log_line("STEAMNET", "Steam connection %u is up%s", h, g_e[i].dial ? " (joiner side)" : " (host side)");
    } else if (st == kStateClosedByPeer || st == kStateProblem) {
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
        log_line("STEAMNET", "connecting to SteamID64 %llu (connection %u)", (unsigned long long)g_req.id, h);
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
            log_line("STEAMNET", "listening for Steam peers on virtual port %d (handle %u)", kVirtualPort, g_listen_h);
        } else if (!want && g_listen_h) {
            A.close_listen(A.sockets, g_listen_h);
            g_listen_h = 0;
            for (int i = 0; i < kMaxEntries; ++i) if (g_e[i].used && !g_e[i].dial) drop_entry(i, "host stopped");
            log_line("STEAMNET", "no longer listening for Steam peers");
        }

        A.run_callbacks(A.sockets);
        handle_dial_request();
        for (int i = 0; i < kMaxEntries; ++i) if (g_e[i].used && g_e[i].connected) pump_entry(i);
        pump_tcp();
        for (int i = 0; i < kMaxEntries; ++i) if (g_e[i].used && g_e[i].closing && !(g_req.entry == i)) drop_entry(i, "closed");
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
