#include "mgmp_upnp.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "mgmp_lan.h"
#include "mgmp_log.h"

#pragma comment(lib, "ws2_32.lib")

namespace mgmp {
namespace {

constexpr ULONGLONG kRenewEveryMs = 30ull * 60 * 1000;     // the lease is an hour
constexpr DWORD     kLeaseSeconds = 3600;

struct Router {
    std::string host;          // the control point
    uint16_t    port = 0;
    std::string path;          // controlURL
    std::string service;       // the full serviceType, e.g. urn:schemas-upnp-org:service:WANIPConnection:1
    std::string local_ip;      // our address on the router's network
};

std::mutex      g_mu;          // one operation at a time
Router          g_router;
bool            g_have_router = false;
volatile LONG   g_state = (LONG)UpnpState::Idle;
volatile LONG   g_want  = 0;   // 1 = the port should be mapped
volatile LONG   g_busy  = 0;   // an operation thread is running
uint16_t        g_port  = 0;
char            g_ext[64]   = {};
char            g_err[128]  = {};
ULONGLONG       g_mapped_at = 0;

void set_state(UpnpState s) { InterlockedExchange(&g_state, (LONG)s); }
void set_err(const char* e) { _snprintf_s(g_err, sizeof(g_err), _TRUNCATE, "%s", e); }

std::string lower(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); return s; }

// --- a tiny HTTP client -----------------------------------------------------------------------------------------------

bool dial(const std::string& host, uint16_t port, int ms, SOCKET& out) {
    out = INVALID_SOCKET;
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); return false; }
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
    bool ok = false;
    if (connect(s, res->ai_addr, (int)res->ai_addrlen) == 0) ok = true;
    else if (WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set wr, ex; FD_ZERO(&wr); FD_ZERO(&ex); FD_SET(s, &wr); FD_SET(s, &ex);
        timeval tv{ ms / 1000, (ms % 1000) * 1000 };
        if (select(0, nullptr, &wr, &ex, &tv) > 0 && FD_ISSET(s, &wr)) ok = true;
    }
    freeaddrinfo(res);
    if (!ok) { closesocket(s); return false; }
    u_long bl = 0; ioctlsocket(s, FIONBIO, &bl);
    DWORD to = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to, sizeof(to));
    out = s;
    return true;
}

// Sends `request`, reads the whole answer (the peer closes: Connection: close). `local` gets our end's address.
bool http_exchange(const std::string& host, uint16_t port, const std::string& request, std::string& response, std::string* local = nullptr) {
    SOCKET s;
    if (!dial(host, port, 3000, s)) return false;
    if (local) {
        sockaddr_in la{}; int ll = sizeof(la);
        if (getsockname(s, (sockaddr*)&la, &ll) == 0) { char b[64] = {}; inet_ntop(AF_INET, &la.sin_addr, b, sizeof(b)); *local = b; }
    }
    size_t off = 0;
    while (off < request.size()) {
        const int n = send(s, request.c_str() + off, (int)(request.size() - off), 0);
        if (n <= 0) { closesocket(s); return false; }
        off += (size_t)n;
    }
    response.clear();
    char buf[4096];
    for (;;) {
        const int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        response.append(buf, (size_t)n);
        if (response.size() > (1u << 20)) break;
    }
    closesocket(s);
    return !response.empty();
}

struct Url { std::string host; uint16_t port = 80; std::string path = "/"; };

bool parse_url(const std::string& u, Url& out) {
    if (lower(u.substr(0, 7)) != "http://") return false;
    const size_t hs = 7;
    const size_t pe = u.find('/', hs);
    const std::string hp = u.substr(hs, pe == std::string::npos ? std::string::npos : pe - hs);
    out.path = pe == std::string::npos ? "/" : u.substr(pe);
    const size_t c = hp.find(':');
    out.host = c == std::string::npos ? hp : hp.substr(0, c);
    out.port = c == std::string::npos ? 80 : (uint16_t)atoi(hp.c_str() + c + 1);
    return !out.host.empty() && out.port != 0;
}

std::string xml_value(const std::string& s, const char* tag, size_t from = 0, size_t* after = nullptr) {
    const std::string open = std::string("<") + tag + ">", close = std::string("</") + tag + ">";
    const size_t a = s.find(open, from);
    if (a == std::string::npos) return std::string();
    const size_t b = s.find(close, a);
    if (b == std::string::npos) return std::string();
    if (after) *after = b + close.size();
    return s.substr(a + open.size(), b - a - open.size());
}

int http_status(const std::string& r) {
    if (r.size() < 12 || r.compare(0, 5, "HTTP/") != 0) return 0;
    return atoi(r.c_str() + 9);
}

// --- discovery --------------------------------------------------------------------------------------------------------

std::vector<std::string> ssdp_locations() {
    std::vector<std::string> locs;
    std::vector<std::string> binds = lan_local_addresses();
    binds.insert(binds.begin(), std::string());      // "" = the default interface
    const char* targets[] = { "urn:schemas-upnp-org:device:InternetGatewayDevice:1", "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
                              "upnp:rootdevice" };
    for (const std::string& bindip : binds) {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) continue;
        sockaddr_in me{};
        me.sin_family = AF_INET;
        if (!bindip.empty()) inet_pton(AF_INET, bindip.c_str(), &me.sin_addr);
        if (bind(s, (sockaddr*)&me, sizeof(me)) != 0) { closesocket(s); continue; }
        DWORD ttl = 2; setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
        sockaddr_in to{};
        to.sin_family = AF_INET; to.sin_port = htons(1900);
        inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
        for (const char* st : targets) {
            char msg[512];
            const int n = _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: %s\r\n\r\n", st);
            sendto(s, msg, n, 0, (sockaddr*)&to, sizeof(to));
        }
        const DWORD t0 = GetTickCount();
        while (GetTickCount() - t0 < 1800) {
            fd_set rd; FD_ZERO(&rd); FD_SET(s, &rd);
            timeval tv{ 0, 100000 };
            if (select(0, &rd, nullptr, nullptr, &tv) <= 0) continue;
            char buf[2048];
            const int n = recv(s, buf, sizeof(buf) - 1, 0);
            if (n <= 0) continue;
            buf[n] = 0;
            std::string r(buf);
            const std::string lr = lower(r);
            const size_t p = lr.find("location:");
            if (p == std::string::npos) continue;
            size_t e = r.find("\r\n", p);
            std::string loc = r.substr(p + 9, e == std::string::npos ? std::string::npos : e - p - 9);
            while (!loc.empty() && (loc.front() == ' ' || loc.front() == '\t')) loc.erase(loc.begin());
            bool dup = false;
            for (const std::string& l : locs) if (l == loc) dup = true;
            if (!dup) locs.push_back(loc);
        }
        closesocket(s);
        if (!locs.empty()) break;       // the first interface that finds a router is enough
    }
    return locs;
}

bool read_description(const std::string& location, Router& out) {
    Url u;
    if (!parse_url(location, u)) return false;
    std::string resp, local;
    const std::string req = "GET " + u.path + " HTTP/1.1\r\nHOST: " + u.host + ":" + std::to_string(u.port) + "\r\nConnection: close\r\n\r\n";
    if (!http_exchange(u.host, u.port, req, resp, &local) || http_status(resp) != 200) return false;
    const size_t body = resp.find("\r\n\r\n");
    const std::string xml = body == std::string::npos ? resp : resp.substr(body + 4);

    std::string base = xml_value(xml, "URLBase");
    Url b = u; b.path = "/";
    if (!base.empty()) parse_url(base, b);

    size_t pos = 0;
    for (;;) {
        size_t after = 0;
        const std::string svc = xml_value(xml, "service", pos, &after);
        if (svc.empty() && after == 0) break;
        pos = after;
        const std::string type = xml_value(svc, "serviceType");
        if (type.find("WANIPConnection") == std::string::npos && type.find("WANPPPConnection") == std::string::npos) continue;
        std::string ctl = xml_value(svc, "controlURL");
        if (ctl.empty()) continue;
        out.service = type;
        out.local_ip = local;
        if (lower(ctl.substr(0, 7)) == "http://") {
            Url c;
            if (!parse_url(ctl, c)) continue;
            out.host = c.host; out.port = c.port; out.path = c.path;
        } else {
            out.host = b.host; out.port = b.port;
            out.path = ctl[0] == '/' ? ctl : "/" + ctl;
        }
        return true;
    }
    return false;
}

// --- SOAP -------------------------------------------------------------------------------------------------------------

// Returns the HTTP status (0 = no answer) and the body.
int soap(const Router& r, const char* action, const std::string& args, std::string& body_out) {
    const std::string body =
        "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + std::string(action) + " xmlns:u=\"" + r.service + "\">" +
        args + "</u:" + action + "></s:Body></s:Envelope>\r\n";
    const std::string req = "POST " + r.path + " HTTP/1.1\r\nHOST: " + r.host + ":" + std::to_string(r.port) +
        "\r\nCONTENT-TYPE: text/xml; charset=\"utf-8\"\r\nCONTENT-LENGTH: " + std::to_string(body.size()) +
        "\r\nSOAPACTION: \"" + r.service + "#" + action + "\"\r\nConnection: close\r\n\r\n" + body;
    std::string resp;
    if (!http_exchange(r.host, r.port, req, resp)) return 0;
    const size_t p = resp.find("\r\n\r\n");
    body_out = p == std::string::npos ? std::string() : resp.substr(p + 4);
    return http_status(resp);
}

std::string add_args(const Router& r, uint16_t port, DWORD lease) {
    return "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) + "</NewExternalPort><NewProtocol>TCP</NewProtocol>"
           "<NewInternalPort>" + std::to_string(port) + "</NewInternalPort><NewInternalClient>" + r.local_ip + "</NewInternalClient>"
           "<NewEnabled>1</NewEnabled><NewPortMappingDescription>CoMeow</NewPortMappingDescription><NewLeaseDuration>" +
           std::to_string(lease) + "</NewLeaseDuration>";
}

bool find_router() {
    if (g_have_router) return true;
    for (const std::string& loc : ssdp_locations()) {
        Router r;
        if (read_description(loc, r)) {
            g_router = r; g_have_router = true;
            log_line("UPNP", "router found: %s:%u%s (we are %s)", r.host.c_str(), (unsigned)r.port, r.path.c_str(), r.local_ip.c_str());
            return true;
        }
    }
    return false;
}

// Adds (or renews) the mapping. True when the router accepted it.
bool add_mapping(uint16_t port) {
    std::string body;
    int st = soap(g_router, "AddPortMapping", add_args(g_router, port, kLeaseSeconds), body);
    if (st != 200 && body.find("725") != std::string::npos)         // OnlyPermanentLeasesSupported
        st = soap(g_router, "AddPortMapping", add_args(g_router, port, 0), body);
    if (st == 200) return true;
    const std::string code = xml_value(body, "errorCode"), desc = xml_value(body, "errorDescription");
    char e[128];
    _snprintf_s(e, sizeof(e), _TRUNCATE, "the router refused (HTTP %d %s %s)", st, code.c_str(), desc.c_str());
    set_err(e);
    log_line_lvl(LogLevel::Warn, "UPNP", "!! %s", e);
    return false;
}

void delete_mapping(uint16_t port) {
    std::string body;
    const std::string args = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) + "</NewExternalPort><NewProtocol>TCP</NewProtocol>";
    const int st = soap(g_router, "DeletePortMapping", args, body);
    log_line("UPNP", "mapping of TCP %u removed (HTTP %d)", (unsigned)port, st);
}

void read_external_ip() {
    std::string body;
    if (soap(g_router, "GetExternalIPAddress", std::string(), body) == 200) {
        const std::string ip = xml_value(body, "NewExternalIPAddress");
        _snprintf_s(g_ext, sizeof(g_ext), _TRUNCATE, "%s", ip.c_str());
    }
}

// --- operations (each on its own thread, one at a time) ---------------------------------------------------------------

enum class Op { Add, Delete };
struct OpArg { Op op; uint16_t port; };

DWORD WINAPI op_thread(LPVOID p) {
    OpArg a = *(OpArg*)p;
    delete (OpArg*)p;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        WSADATA wsa{};
        const bool wsa_ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
        if (a.op == Op::Add) {
            if (!g_want) {                                    // asked to stop before we even began
                if (g_state == (LONG)UpnpState::Working) set_state(UpnpState::Idle);
            } else if (!wsa_ok || !find_router()) {
                set_err("no UPnP router answered");
                log_line("UPNP", "no UPnP router answered");
                set_state(UpnpState::Failed);
            } else if (!g_want) {
                set_state(UpnpState::Idle);
            } else if (add_mapping(a.port)) {
                read_external_ip();
                g_mapped_at = GetTickCount64();
                log_line("UPNP", "TCP %u is forwarded (the router's address: %s)", (unsigned)a.port, g_ext[0] ? g_ext : "unknown");
                if (!g_want) { delete_mapping(a.port); set_state(UpnpState::Idle); }   // stopped meanwhile
                else set_state(UpnpState::Mapped);
            } else {
                set_state(UpnpState::Failed);
            }
        } else {
            if (g_have_router && !g_want) delete_mapping(a.port);
            if (!g_want) { g_ext[0] = 0; set_state(UpnpState::Idle); }
        }
        if (wsa_ok) WSACleanup();
    }
    InterlockedExchange(&g_busy, 0);
    return 0;
}

void spawn(Op op, uint16_t port) {
    InterlockedExchange(&g_busy, 1);
    HANDLE h = CreateThread(nullptr, 0, op_thread, new OpArg{ op, port }, 0, nullptr);
    if (h) CloseHandle(h);
    else InterlockedExchange(&g_busy, 0);
}

} // namespace

void upnp_start(uint16_t port) {
    if (g_want) return;
    g_port = port;
    g_err[0] = 0; g_ext[0] = 0;
    InterlockedExchange(&g_want, 1);
    set_state(UpnpState::Working);
    log_line("UPNP", "asking the router to forward TCP %u", (unsigned)port);
    spawn(Op::Add, port);
}

void upnp_stop() {
    if (!g_want) return;
    InterlockedExchange(&g_want, 0);
    const UpnpState s = upnp_state();
    if (s == UpnpState::Failed || s == UpnpState::Idle) { set_state(UpnpState::Idle); return; }
    spawn(Op::Delete, g_port);          // waits for a running Add (the mutex), which also sees g_want == 0
}

void upnp_update() {
    if (!g_want || g_busy || upnp_state() != UpnpState::Mapped) return;
    if (GetTickCount64() - g_mapped_at < kRenewEveryMs) return;
    g_mapped_at = GetTickCount64();
    log_line("UPNP", "renewing the lease");
    spawn(Op::Add, g_port);
}

UpnpState   upnp_state()       { return (UpnpState)InterlockedCompareExchange(&g_state, 0, 0); }
const char* upnp_external_ip() { return g_ext; }
const char* upnp_error()       { return g_err; }

} // namespace mgmp
