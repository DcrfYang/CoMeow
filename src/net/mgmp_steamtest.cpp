#include "mgmp_steamtest.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "mgmp_log.h"
#include "mgmp_steambridge.h"

#include <winsock2.h>
#include <ws2tcpip.h>

namespace mgmp {
namespace {

// --- the flat API, resolved by name from the DLL the game itself loaded -------------------------------------------------

typedef void*    (__cdecl* fn_accessor)();
typedef uint64_t (__cdecl* fn_get_id)(void*);
typedef const char* (__cdecl* fn_get_name)(void*);
typedef void     (__cdecl* fn_void1)(void*);
typedef int      (__cdecl* fn_relay_status)(void*, void*);
typedef uint32_t (__cdecl* fn_listen)(void*, int, int, const void*);
typedef uint32_t (__cdecl* fn_connect)(void*, const void*, int, int, const void*);
typedef int      (__cdecl* fn_accept)(void*, uint32_t);
typedef bool     (__cdecl* fn_close)(void*, uint32_t, int, const char*, bool);
typedef int      (__cdecl* fn_send)(void*, uint32_t, const void*, uint32_t, int, int64_t*);
typedef int      (__cdecl* fn_recv)(void*, uint32_t, void**, int);
typedef bool     (__cdecl* fn_rt_status)(void*, uint32_t, void*, int, void*);
typedef int      (__cdecl* fn_detail)(void*, uint32_t, char*, int);
typedef void     (__cdecl* fn_set_id64)(void*, uint64_t);
typedef void     (__cdecl* fn_cfg_setptr)(void*, int, void*);
typedef void     (__cdecl* fn_msg_release)(void*);

struct Api {
    HMODULE dll = nullptr;
    fn_accessor sockets_acc = nullptr, utils_acc = nullptr, user_acc = nullptr, friends_acc = nullptr;
    fn_get_id get_steamid = nullptr;
    fn_get_name persona = nullptr;
    fn_void1 relay_init = nullptr, run_callbacks = nullptr;
    fn_relay_status relay_status = nullptr;
    fn_listen listen = nullptr;
    fn_connect connect = nullptr;
    fn_accept accept = nullptr;
    fn_close close = nullptr;
    fn_send send = nullptr;
    fn_recv recv = nullptr;
    fn_rt_status rt_status = nullptr;
    fn_detail detail = nullptr;
    fn_set_id64 set_id64 = nullptr;
    fn_cfg_setptr cfg_setptr = nullptr;
    fn_msg_release msg_release = nullptr;
    void* sockets = nullptr;
    void* utils = nullptr;
};
Api A;

template <class T> bool resolve(T& out, const char* name) {
    out = (T)GetProcAddress(A.dll, name);
    if (!out) log_line("STEAM", "!! steam_api64.dll does not export %s", name);
    return out != nullptr;
}

bool resolve_all() {
    A.dll = GetModuleHandleW(L"steam_api64.dll");
    if (!A.dll) { log_line("STEAM", "!! steam_api64.dll is not loaded in this process"); return false; }
    bool ok = true;
    ok &= resolve(A.sockets_acc, "SteamAPI_SteamNetworkingSockets_SteamAPI_v012");
    ok &= resolve(A.utils_acc, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004");
    ok &= resolve(A.user_acc, "SteamAPI_SteamUser_v023");
    ok &= resolve(A.friends_acc, "SteamAPI_SteamFriends_v018");
    ok &= resolve(A.get_steamid, "SteamAPI_ISteamUser_GetSteamID");
    ok &= resolve(A.persona, "SteamAPI_ISteamFriends_GetPersonaName");
    ok &= resolve(A.relay_init, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess");
    ok &= resolve(A.relay_status, "SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus");
    ok &= resolve(A.run_callbacks, "SteamAPI_ISteamNetworkingSockets_RunCallbacks");
    ok &= resolve(A.listen, "SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P");
    ok &= resolve(A.connect, "SteamAPI_ISteamNetworkingSockets_ConnectP2P");
    ok &= resolve(A.accept, "SteamAPI_ISteamNetworkingSockets_AcceptConnection");
    ok &= resolve(A.close, "SteamAPI_ISteamNetworkingSockets_CloseConnection");
    ok &= resolve(A.send, "SteamAPI_ISteamNetworkingSockets_SendMessageToConnection");
    ok &= resolve(A.recv, "SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection");
    ok &= resolve(A.rt_status, "SteamAPI_ISteamNetworkingSockets_GetConnectionRealTimeStatus");
    ok &= resolve(A.detail, "SteamAPI_ISteamNetworkingSockets_GetDetailedConnectionStatus");
    ok &= resolve(A.set_id64, "SteamAPI_SteamNetworkingIdentity_SetSteamID64");
    ok &= resolve(A.cfg_setptr, "SteamAPI_SteamNetworkingConfigValue_t_SetPtr");
    ok &= resolve(A.msg_release, "SteamAPI_SteamNetworkingMessage_t_Release");
    return ok;
}

// --- the experiment -----------------------------------------------------------------------------------------------------

constexpr int kStateConnecting = 1, kStateFindingRoute = 2, kStateConnected = 3, kStateClosedByPeer = 4, kStateProblem = 5;
constexpr int kSendReliable = 8;
constexpr int kCfgCallbackStatusChanged = 201;       // k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged

const char* state_name(int s) {
    switch (s) {
        case 0: return "none"; case kStateConnecting: return "connecting"; case kStateFindingRoute: return "finding route";
        case kStateConnected: return "CONNECTED"; case kStateClosedByPeer: return "closed by peer";
        case kStateProblem: return "problem detected locally"; default: return "?";
    }
}

struct Conn { uint32_t h = 0; bool accepted = false; bool announced = false; };
Conn g_in[8];                 // connections accepted on our listen socket
uint32_t g_out = 0;           // our outgoing connection
bool g_out_connected = false;
bool g_out_announced = false;
bool g_listening = false;
volatile LONG g_done = 0;

int conn_state(uint32_t h, int* ping = nullptr) {
    alignas(8) unsigned char st[512] = {};
    if (!A.rt_status(A.sockets, h, st, 0, nullptr)) return 0;
    if (ping) *ping = *(int*)(st + 4);
    return *(int*)st;
}

void log_route(uint32_t h, const char* who) {
    char buf[2048] = {};
    A.detail(A.sockets, h, buf, sizeof(buf));
    // one log line per row: the text says whether the route is direct or relayed and which relay
    log_line("STEAM", "%s route detail follows", who);
    for (char* p = buf; *p;) {
        char* e = strchr(p, '\n');
        if (e) *e = 0;
        if (*p) log_line("STEAM", "   %s", p);
        if (!e) break;
        p = e + 1;
    }
}

// Steam calls this from inside RunCallbacks, on our thread, for every state change of a connection that was made with it set.
void __cdecl on_status(void* info) {
    const uint32_t h = *(uint32_t*)info;               // SteamNetConnectionStatusChangedCallback_t::m_hConn
    const int st = conn_state(h);
    log_line("STEAM", "connection %u -> %s", h, state_name(st));
    if (st == kStateConnecting && h != g_out) {         // somebody is calling us: take it
        const int r = A.accept(A.sockets, h);
        for (Conn& c : g_in) if (!c.h) { c.h = h; c.accepted = (r == 1); break; }
        log_line("STEAM", "incoming connection %u: AcceptConnection -> %d (1 = OK)", h, r);
    } else if (st == kStateClosedByPeer || st == kStateProblem) {
        A.close(A.sockets, h, 0, "done", false);
        if (h == g_out) { g_out = 0; g_out_connected = false; }
        for (Conn& c : g_in) if (c.h == h) c = Conn{};
    }
}

void config_with_callback(unsigned char* cfg /* 32 bytes */) {
    memset(cfg, 0, 32);
    A.cfg_setptr(cfg, kCfgCallbackStatusChanged, (void*)&on_status);
}

void pump_messages(uint32_t h, bool echo) {
    for (;;) {
        void* msgs[8] = {};
        const int n = A.recv(A.sockets, h, msgs, 8);
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) {
            const char* data = *(const char**)msgs[i];
            const int len = *(int*)((char*)msgs[i] + 8);
            std::string s(data, (size_t)(len > 0 && len < 256 ? len : 0));
            if (echo) {
                log_line("STEAM", "host got '%s' on %u -- echoing", s.c_str(), h);
                A.send(A.sockets, h, s.c_str(), (uint32_t)s.size(), kSendReliable, nullptr);
            } else {
                // "ping <n> <sent-at-ms>" came back
                unsigned n2 = 0; unsigned long long t0 = 0;
                if (sscanf_s(s.c_str(), "ping %u %llu", &n2, &t0) == 2)
                    log_line("STEAM", "echo #%u: round trip %llu ms", n2, (unsigned long long)(GetTickCount64() - t0));
                else log_line("STEAM", "got '%s'", s.c_str());
            }
            A.msg_release(msgs[i]);
        }
    }
}

// "bridge": the REAL bridge (mgmp_steambridge) end to end in one process -- a local TCP echo server stands in for the game's port, the
// host side of the bridge listens for Steam peers, and we dial our own SteamID through Steam, send 300 KB and compare what comes back.
SOCKET g_echo_listen = INVALID_SOCKET;

DWORD WINAPI echo_server(LPVOID) {
    SOCKET c = accept(g_echo_listen, nullptr, nullptr);
    if (c == INVALID_SOCKET) return 0;
    char buf[8192];
    for (;;) {
        const int n = recv(c, buf, sizeof(buf), 0);
        if (n <= 0) break;
        int off = 0;
        while (off < n) { const int w = send(c, buf + off, n - off, 0); if (w <= 0) { off = n; break; } off += w; }
    }
    closesocket(c);
    return 0;
}

void bridge_selftest() {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    steam_bridge_init();
    for (int i = 0; i < 600 && !steam_bridge_ready(); ++i) Sleep(100);
    if (!steam_bridge_ready()) { log_line("STEAM", "RESULT: bridge test: Steam networking did not become ready"); return; }
    g_echo_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in sa{};
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof(sa);
    bind(g_echo_listen, (sockaddr*)&sa, sizeof(sa)); listen(g_echo_listen, 2); getsockname(g_echo_listen, (sockaddr*)&sa, &len);
    const uint16_t port = ntohs(sa.sin_port);
    HANDLE th = CreateThread(nullptr, 0, echo_server, nullptr, 0, nullptr);
    log_line("STEAM", "bridge test: echo server on 127.0.0.1:%u stands in for the game's port; this account is %llu", (unsigned)port,
             (unsigned long long)steam_bridge_id());
    steam_bridge_host_start(port);
    Sleep(1500);                                          // the listen socket comes up on the bridge thread
    int why = 0;
    volatile long stop = 0;
    const ULONGLONG t0 = GetTickCount64();
    SOCKET s = steam_bridge_dial(steam_bridge_id(), 20000, &stop, &why);
    if (s == INVALID_SOCKET) { log_line("STEAM", "RESULT: bridge test FAILED: steam_bridge_dial gave nothing (reason %d)", why); steam_bridge_host_stop(); return; }
    log_line("STEAM", "bridge test: the bridged socket is up after %llu ms", (unsigned long long)(GetTickCount64() - t0));
    const int total = 300 * 1024;
    std::string out(total, 0), in;
    for (int i = 0; i < total; ++i) out[i] = (char)((i * 131 + 7) & 0xFF);
    const ULONGLONG t1 = GetTickCount64();
    int sent = 0;
    in.reserve(total);
    DWORD tmo = 20000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    while ((int)in.size() < total) {
        if (sent < total) {                               // interleave: write a slice, read what is there
            const int w = send(s, out.data() + sent, total - sent > 8192 ? 8192 : total - sent, 0);
            if (w <= 0) break;
            sent += w;
        }
        fd_set rd; FD_ZERO(&rd); FD_SET(s, &rd);
        timeval tv{ 0, sent < total ? 1000 : 3000000 };
        if (select(0, &rd, nullptr, nullptr, &tv) > 0) {
            char buf[16384];
            const int r = recv(s, buf, sizeof(buf), 0);
            if (r <= 0) break;
            in.append(buf, (size_t)r);
        } else if (sent >= total) break;
    }
    const ULONGLONG ms = GetTickCount64() - t1;
    const bool same = (int)in.size() == total && in == out;
    log_line("STEAM", "RESULT: bridge test %s -- sent %d, received %d bytes back in %llu ms, content %s",
             same ? "PASSED" : "FAILED", sent, (int)in.size(), (unsigned long long)ms, same ? "identical" : "DIFFERENT");
    closesocket(s);
    Sleep(500);
    steam_bridge_host_stop();
    if (th) CloseHandle(th);
}

DWORD WINAPI experiment(LPVOID arg) {
    std::string mode((const char*)arg);
    delete[] (char*)arg;
    if (mode == "bridge") { bridge_selftest(); return 0; }
    log_line("STEAM", "steam_test = '%s' -- waiting for Steam to be ready in this process", mode.c_str());
    if (!resolve_all()) { log_line("STEAM", "RESULT: the Steam networking exports are not all there -- see above"); return 0; }

    // Steam is initialised by the game, a little after we load.
    for (int i = 0; i < 900 && !A.sockets; ++i) {
        A.sockets = A.sockets_acc();
        if (!A.sockets) { if (i % 100 == 0) log_line("STEAM", "ISteamNetworkingSockets is not available yet (Steam not initialised?)"); Sleep(100); }
    }
    A.utils = A.utils_acc();
    void* user = A.user_acc();
    void* fr = A.friends_acc();
    if (!A.sockets || !A.utils || !user) { log_line("STEAM", "RESULT: Steam never became usable from this process"); return 0; }
    const uint64_t me = A.get_steamid(user);
    log_line("STEAM", "Steam is up: this account's SteamID64 = %llu, persona '%s'", (unsigned long long)me, fr ? A.persona(fr) : "?");

    A.relay_init(A.utils);
    for (int i = 0; i < 200; ++i) {
        alignas(8) unsigned char rs[512] = {};
        const int avail = A.relay_status(A.utils, rs);
        static int last = -999;
        if (avail != last) { last = avail; log_line("STEAM", "relay network availability = %d (100 = ready; negative = failed) -- %s", avail, (const char*)(rs + 16)); }
        if (avail == 100) break;
        Sleep(100);
    }

    unsigned char cfg[32];
    config_with_callback(cfg);

    const bool want_listen = (mode == "host" || mode == "probe");
    uint64_t target = 0;
    if (mode == "probe") target = me;
    else if (mode.compare(0, 5, "join:") == 0) target = strtoull(mode.c_str() + 5, nullptr, 10);

    uint32_t listen_h = 0;
    if (want_listen) {
        listen_h = A.listen(A.sockets, 0, 1, cfg);
        g_listening = listen_h != 0;
        log_line("STEAM", "listening on virtual port 0 (handle %u)%s", listen_h,
                 mode == "host" ? " -- start the OTHER instance with steam_test = \"join:<this SteamID64>\"" : "");
    }
    if (target) {
        alignas(8) unsigned char ident[256] = {};
        A.set_id64(ident, target);
        g_out = A.connect(A.sockets, ident, 0, 1, cfg);
        log_line("STEAM", "connecting to SteamID64 %llu on virtual port 0 (handle %u)", (unsigned long long)target, g_out);
    }

    const ULONGLONG t0 = GetTickCount64();
    const ULONGLONG limit = mode == "probe" ? 30000ull : 15ull * 60 * 1000;
    unsigned sent = 0;
    ULONGLONG next_ping = 0, next_status = 0;
    bool connected_once = false, echoed = false;
    while (GetTickCount64() - t0 < limit && !g_done) {
        A.run_callbacks(A.sockets);
        for (Conn& c : g_in) if (c.h) pump_messages(c.h, true);
        if (g_out) {
            const int st = conn_state(g_out);
            if (st == kStateConnected && !g_out_announced) {
                g_out_announced = true; g_out_connected = true; connected_once = true;
                int ping = 0; conn_state(g_out, &ping);
                log_line("STEAM", "OUTGOING connection is up after %llu ms, ping %d ms", (unsigned long long)(GetTickCount64() - t0), ping);
                log_route(g_out, "outgoing");
            }
            if (g_out_connected) {
                pump_messages(g_out, false);
                if (GetTickCount64() >= next_ping && sent < 20) {
                    char m[64];
                    const int n = _snprintf_s(m, sizeof(m), _TRUNCATE, "ping %u %llu", ++sent, (unsigned long long)GetTickCount64());
                    A.send(A.sockets, g_out, m, (uint32_t)n, kSendReliable, nullptr);
                    next_ping = GetTickCount64() + 500;
                }
                if (sent >= 20 && GetTickCount64() > next_ping + 2000 && mode != "host") break;
            }
        }
        for (Conn& c : g_in) {
            if (c.h && !c.announced && conn_state(c.h) == kStateConnected) {
                c.announced = true; echoed = true;
                log_line("STEAM", "INCOMING connection %u is up", c.h);
                log_route(c.h, "incoming");
            }
        }
        if (GetTickCount64() >= next_status) {
            next_status = GetTickCount64() + 5000;
            int ping = 0, st = g_out ? conn_state(g_out, &ping) : 0;
            if (g_out) log_line("STEAM", "outgoing: %s, ping %d ms, %u ping(s) sent", state_name(st), ping, sent);
        }
        Sleep(20);
    }
    log_line("STEAM", "RESULT: %s", connected_once ? "a P2P connection through Steam worked (see the route lines above)"
                                     : (echoed ? "the listening side got a connection (see above)"
                                               : "no connection came up -- see the state lines above"));
    if (g_out) A.close(A.sockets, g_out, 0, "bye", false);
    return 0;
}

} // namespace

void steamtest_start(const char* mode) {
    if (!mode || !mode[0]) return;
    static LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0)) return;
    char* arg = new char[strlen(mode) + 1];
    strcpy_s(arg, strlen(mode) + 1, mode);
    HANDLE h = CreateThread(nullptr, 0, experiment, arg, 0, nullptr);
    if (h) CloseHandle(h); else delete[] arg;
}

} // namespace mgmp
