#include "mgmp_lan.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "json.hpp"
#include "mgmp_lobby_server.h"
#include "mgmp_log.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace mgmp {
namespace {

using nlohmann::json;

constexpr uint16_t kDiscoveryPort = 27701;                       // mgmp_server.cpp answers on it
constexpr uint16_t kLobbyPorts[]  = { 27700, 27702, 27703 };     // tried in this order; discovery reports whichever was taken

void lobby_log(const char* line) { log_line("LAN", "%s", line); }

bool is_private_v4(uint32_t host_order) {
    const unsigned a = host_order >> 24, b = (host_order >> 16) & 255;
    return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 169 && b == 254);
}

struct Adapter { uint32_t ip, mask; };   // network byte order

std::vector<Adapter> adapters() {
    std::vector<Adapter> out;
    ULONG len = 16 * 1024;
    std::vector<unsigned char> buf;
    for (int attempt = 0; attempt < 3; ++attempt) {
        buf.assign(len, 0);
        const ULONG rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                              nullptr, (PIP_ADAPTER_ADDRESSES)buf.data(), &len);
        if (rc == ERROR_BUFFER_OVERFLOW) continue;
        if (rc != NO_ERROR) return out;
        for (PIP_ADAPTER_ADDRESSES a = (PIP_ADAPTER_ADDRESSES)buf.data(); a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (PIP_ADAPTER_UNICAST_ADDRESS u = a->FirstUnicastAddress; u; u = u->Next) {
                if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
                const uint32_t ip = ((sockaddr_in*)u->Address.lpSockaddr)->sin_addr.s_addr;
                const unsigned bits = u->OnLinkPrefixLength;
                const uint32_t mask = bits >= 32 ? 0xFFFFFFFFu : htonl(bits ? (0xFFFFFFFFu << (32 - bits)) : 0u);
                out.push_back({ ip, mask });
            }
        }
        break;
    }
    return out;
}

// --- the search ------------------------------------------------------------------------------------------------------

std::mutex            g_mu;
std::vector<LanRoom>  g_results;
std::atomic<int>      g_state{ (int)LanSearch::Idle };
HANDLE                g_thread = nullptr;

void search_thread() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { g_state = (int)LanSearch::Done; return; }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    std::vector<LanRoom> found;
    if (s != INVALID_SOCKET) {
        BOOL yes = TRUE;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));
        sockaddr_in any{};
        any.sin_family = AF_INET;
        bind(s, (sockaddr*)&any, sizeof(any));

        std::vector<uint32_t> targets;            // network byte order
        bool have_private = false;
        for (const Adapter& a : adapters()) {
            if (!is_private_v4(ntohl(a.ip))) continue;
            have_private = true;
            targets.push_back((a.ip & a.mask) | ~a.mask);
        }
        targets.push_back(htonl(INADDR_BROADCAST));
        if (!have_private) targets.push_back(htonl(INADDR_LOOPBACK));   // no network at all: still find a lobby on this machine

        const DWORD t0 = GetTickCount();
        DWORD next_send = 0;
        while (GetTickCount() - t0 < 1500) {
            const DWORD el = GetTickCount() - t0;
            if (el >= next_send && next_send < 900) {          // three rounds: a datagram can be lost
                for (uint32_t t : targets) {
                    sockaddr_in to{};
                    to.sin_family      = AF_INET;
                    to.sin_addr.s_addr = t;
                    to.sin_port        = htons(kDiscoveryPort);
                    sendto(s, "MGMP-DISCOVER 1", 15, 0, (sockaddr*)&to, sizeof(to));
                }
                next_send += 450;
            }
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(s, &rd);
            timeval tv{ 0, 50000 };
            if (select(0, &rd, nullptr, nullptr, &tv) <= 0) continue;
            char buf[4096];
            sockaddr_in from{};
            int fl = sizeof(from);
            const int n = recvfrom(s, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fl);
            if (n <= 0) continue;
            buf[n] = 0;
            char ip[64] = {};
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
            try {
                const json j = json::parse(buf, buf + n, nullptr, false);
                if (!j.is_object() || j.value("t", "") != "here") continue;
                const int port = j.value("port", 27700);
                if (port < 1 || port > 65535 || !j.contains("rooms") || !j["rooms"].is_array()) continue;
                for (const json& r : j["rooms"]) {
                    if (!r.is_object()) continue;
                    LanRoom lr;
                    lr.addr    = ip;
                    lr.port    = (uint16_t)port;
                    lr.id      = r.value("id", "");
                    lr.name    = r.value("name", "");
                    lr.host    = r.value("host", "");
                    lr.players = r.value("players", 0);
                    lr.cap     = r.value("cap", 4);
                    lr.pw      = r.value("pw", false);
                    if (lr.id.empty()) continue;
                    bool dup = false;
                    // the same room answered from two addresses of one machine (a virtual adapter besides the real one): keep the first
                    for (const LanRoom& o : found) if (o.port == lr.port && o.id == lr.id && o.name == lr.name && o.host == lr.host) { dup = true; break; }
                    if (!dup) found.push_back(lr);
                }
            } catch (...) {}
        }
        closesocket(s);
    }
    WSACleanup();
    log_line("LAN", "search done: %u room(s)", (unsigned)found.size());
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_results = found;
    }
    g_state = (int)LanSearch::Done;
}

DWORD WINAPI search_entry(LPVOID) { search_thread(); return 0; }

} // namespace

// --- hosting ---------------------------------------------------------------------------------------------------------

bool lan_host_start(std::string& err) {
    if (lobby_server_running()) return true;
    char why[160] = {};
    for (uint16_t port : kLobbyPorts) {
        if (lobby_server_start(port, lobby_log, why, sizeof(why))) return true;
    }
    err = why;
    return false;
}

void lan_host_stop() { lobby_server_stop(); }
bool lan_host_running() { return lobby_server_running(); }
uint16_t lan_host_port() { return lobby_server_port(); }

std::vector<std::string> lan_local_addresses() {
    std::vector<std::string> out;
    WSADATA wsa{};
    const bool started = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    for (const Adapter& a : adapters()) {
        if (!is_private_v4(ntohl(a.ip))) continue;
        char b[64] = {};
        in_addr ia{};
        ia.s_addr = a.ip;
        inet_ntop(AF_INET, &ia, b, sizeof(b));
        out.push_back(b);
    }
    if (started) WSACleanup();
    return out;
}

// --- finding a lobby -------------------------------------------------------------------------------------------------

void lan_search_start() {
    if (g_state == (int)LanSearch::Searching) return;
    if (g_thread) { WaitForSingleObject(g_thread, 3000); CloseHandle(g_thread); g_thread = nullptr; }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_results.clear();
    }
    g_state = (int)LanSearch::Searching;
    g_thread = CreateThread(nullptr, 0, search_entry, nullptr, 0, nullptr);
    if (!g_thread) g_state = (int)LanSearch::Done;
}

LanSearch lan_search_state() { return (LanSearch)g_state.load(); }

std::vector<LanRoom> lan_search_results() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_results;
}

} // namespace mgmp
