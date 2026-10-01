// src/net/mgmp_lobby_server.h -- the lobby server (server/mgmp_server.cpp) running inside the game process, for LAN play.
//
// The server source is compiled into the mod by mgmp_lobby_server.cpp with MGMP_EMBEDDED, so the LAN host and the online
// server speak exactly the same protocol: the host connects to its own lobby on 127.0.0.1 and creates a room like any client.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mgmp {

// Starts the lobby on `port` (TCP, all adapters) with UDP discovery on 27701, on its own thread. `log` receives its lines.
// True when it is running (also if it already was); otherwise false and `err` holds a reason.
bool lobby_server_start(uint16_t port, void (*log)(const char*), char* err, size_t errsz);
void lobby_server_stop();               // blocks until the thread has ended; no-op when not running
bool lobby_server_running();
uint16_t lobby_server_port();           // 0 when not running

}
