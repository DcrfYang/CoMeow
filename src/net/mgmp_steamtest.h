// mgmp_steamtest.h -- a DEVELOPER EXPERIMENT (debug.steam_test, only with ui.dev_tools): can the mod use Steam's own peer-to-peer
// networking from inside the game process?
//
// The game already loads steam_api64.dll and initialises Steam. ISteamNetworkingSockets (flat API, exported by that DLL) gives
// two players NAT traversal and, when that fails, Valve's relay -- no port forwarding, and no relay of ours. This module
// only ASKS: it resolves the exports, reports the local SteamID and the relay network status, and opens a P2P connection
// between two instances, exchanging pings and logging the route Steam picked. Nothing in the game's own networking changes.
//
// debug.steam_test:   "probe"          report what is available, then try to connect to ourselves
//                     "host"           listen on virtual port 0 and echo what arrives; the log prints this account's SteamID64
//                     "join:<id64>"    connect to that SteamID64, send pings, log the round trip time and the route
// All lines are tagged STEAM in the mod's log.
#pragma once

namespace mgmp {

void steamtest_start(const char* mode);

}
