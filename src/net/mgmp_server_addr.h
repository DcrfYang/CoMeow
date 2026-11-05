// mgmp_server_addr.h -- the project's own server, by name and by address (2026-10-31).
//
// Releases point mgmp.json's signal.addr at kOfficialHost (the lobby, the log upload, the server relay and the launcher's updates all take it from there).
// A name can fail where the address would not -- the resolver is down, the domain lapsed, a crashed process cannot reach DNS -- so the places that
// resolve it try kOfficialIp when the name does not resolve. Any other name gets no fallback.
#pragma once
#include <cstring>

namespace mgmp {

constexpr const char* kOfficialHost = "comeow.top";
constexpr const char* kOfficialIp   = "49.233.209.67";

// The address to try when `host` does not resolve: kOfficialIp for the project's own server name (case-insensitive, an optional "www."), else nullptr.
inline const char* official_fallback(const char* host) {
    if (!host) return nullptr;
    if (!_strnicmp(host, "www.", 4)) host += 4;
    return _stricmp(host, kOfficialHost) == 0 ? kOfficialIp : nullptr;
}

// "host" or "host:port" is the project's own server (by name or by address).
inline bool is_official_server(const char* hostport) {
    if (!hostport) return false;
    char h[128] = {};
    size_t n = 0;
    while (hostport[n] && hostport[n] != ':' && n < sizeof(h) - 1) { h[n] = hostport[n]; ++n; }
    return official_fallback(h) != nullptr || strcmp(h, kOfficialIp) == 0;
}

} // namespace mgmp
