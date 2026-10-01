// mgmp_lan.h -- local network play, no server machine. Layer 2.5, beside mgmp_signal.
//
// The lobby server (server/mgmp_server.cpp) is compiled into the mod too (mgmp_lobby_server.cpp). A player who wants to host
// on the local network starts it here, connects to it on 127.0.0.1 with the ordinary signaling client and creates a room;
// the lobby tells every joiner the address THEY reached it by, which is this machine's LAN address. Friends find the room
// either by typing that address into the online tab, or with the search below (a UDP broadcast the embedded lobby answers).
//
// Nothing here touches the game's own transport (TCP on the room's game port); it only gets the lobby up and finds it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mgmp {

// --- hosting ---------------------------------------------------------------------------------------------------------
// Starts the lobby on a free port (27700 first). False with `err` (a short English reason, shown after the translated label).
bool lan_host_start(std::string& err);
void lan_host_stop();
bool lan_host_running();
uint16_t lan_host_port();                       // the lobby's TCP port; 0 when it is not running

// This machine's private IPv4 addresses (192.168.x.x, 10.x.x.x, 172.16-31.x.x, 169.254.x.x), the ones a friend can type.
std::vector<std::string> lan_local_addresses();

// --- finding a lobby -------------------------------------------------------------------------------------------------
struct LanRoom {
    std::string addr;       // the lobby's address, as it answered from
    uint16_t    port = 27700;
    std::string id, name, host;
    int         players = 0, cap = 4;
    bool        pw = false;
};

enum class LanSearch { Idle, Searching, Done };

void lan_search_start();                        // broadcasts for about 1.5 s on a worker thread; no-op while searching
LanSearch lan_search_state();
std::vector<LanRoom> lan_search_results();      // a copy; rooms of every lobby that answered, passworded ones included

} // namespace mgmp
