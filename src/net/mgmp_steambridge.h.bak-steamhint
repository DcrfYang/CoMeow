// mgmp_steambridge.h -- the game's TCP transport carried over Steam's peer-to-peer network (2026-10-02).
//
// WHY. The game traffic is a TCP connection from every player to the host's port, which needs the host's router to forward that
// port. Steam's networking (ISteamNetworkingSockets, reached through the steam_api64.dll the game already loads) connects two
// accounts through NAT -- and, when that is impossible, through Valve's relays -- with nothing to open. mgmp_steamtest proved it
// works inside the game process.
//
// HOW, WITHOUT TOUCHING THE TRANSPORT. mgmp_net.cpp keeps speaking plain TCP; this module is a byte pump around it:
//
//   host    Steam listen socket  --accepts-->  for each Steam peer, a TCP connection to 127.0.0.1:<the game's own port>,
//                                              bytes pumped both ways -- to the host's net layer it is one more client
//   joiner  steam_bridge_dial(host's SteamID) -> a loopback TCP pair; one end is handed back to mgmp_net as "the connection to the
//                                              host", the other end is pumped to the Steam connection
//
// So framing, handshakes, the lockstep and every message stay exactly as they are; only the carrier changes. The lobby server
// carries each player's SteamID64 (create / join), and the joiner tries "steam:<id>" as one more candidate address in net_join.
#pragma once

#include <winsock2.h>
#include <cstdint>

namespace mgmp {

void     steam_bridge_init();                   // starts the worker (once); it waits for Steam to be initialised by the game
bool     steam_bridge_ready();                  // Steam networking is usable (and the relay network is coming up)
uint64_t steam_bridge_id();                     // this account's SteamID64, 0 when not ready

// Hosting: accept Steam peers and bridge each to 127.0.0.1:game_port. Stop when the room ends.
void steam_bridge_host_start(uint16_t game_port);
void steam_bridge_host_stop();

// Joining: connect to `host_steamid` through Steam and return a connected loopback TCP socket that carries that connection, or
// INVALID_SOCKET (and *why = a short reason code: 1 not ready, 2 timed out, 3 failed). Blocks up to timeout_ms; *stop (may be null)
// aborts it. Called from the dial thread, never the game thread.
SOCKET   steam_bridge_dial(uint64_t host_steamid, unsigned long timeout_ms, volatile long* stop, int* why);

// --- diagnostics (2026-10-03) -----------------------------------------------------------------------------------------------
// Steam's own state, polled once a second by the worker and logged when it changes: the authentication status (the certificate every
// P2P connection needs), the relay network and its configuration. 2026-10-02: for half an hour every dial to one host ended with
// "Bad cert: CA key ... is not known to us" -- an unknown CA key on the ACCEPTING side's Steam client -- and nothing in the log said what Steam
// thought of itself at the time.
bool steam_bridge_dead();                                // Steam networking is not available in this process
void steam_bridge_status_text(char* out, unsigned cap);  // "authentication: ready, relay network: ready, ..." (best effort, may be stale by a second)
// Why the last Steam connection ended: Steam's end reason (5001 = an internal error on the accepting side) and its debug text.
void steam_bridge_last_failure(int* reason, char* dbg, unsigned cap);

// --- reverse dialling ---------------------------------------------------------------------------------------------------------
// Steam's certificate check is asymmetric: the side whose Steam client holds an OLDER network configuration cannot accept a connection whose
// certificate was signed by a newer CA key, but it can still present its own. When a joiner's dial is refused that way, the HOST dials the joiner
// instead (the lobby server forwards the request). The joiner listens on its own virtual port for that, only while it is waiting for it.
void steam_bridge_client_listen_start();                 // joiner: listen for the host's call (virtual port 28)
void steam_bridge_client_listen_stop();
bool steam_bridge_rev_arm();                             // joiner: from now on one incoming call is accepted and bridged; false = Steam not ready
SOCKET steam_bridge_rev_wait(unsigned long timeout_ms, volatile long* stop, int* why);   // joiner: the connected loopback socket, or INVALID_SOCKET (why 2 timeout, 3 failed)
void steam_bridge_rev_cancel();                          // joiner: stop waiting
void steam_bridge_dial_out(uint64_t joiner_steamid);     // host: dial this joiner over Steam and bridge it to the game's own port (asynchronous)

} // namespace mgmp
