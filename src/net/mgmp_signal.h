// mgmp_signal.h -- the lobby's link to the signaling server. Layer 2.5.
//
// WHY THIS EXISTS, AND WHAT IT REPLACED.
//
// The mod used to know its role before the game started: mgmp.json said host or
// client, and session_start() opened the matching socket during init. That made
// the two players unequal from the first frame -- one process was a host and
// the other was a client, and neither could become the other without editing a
// file and restarting. Finding each other was manual too: one of them had to
// know the other's IP address, read it out, and type it in.
//
// Now every instance launches NEUTRAL. No socket, no role, identical config on
// both machines. The role is decided in the lobby, which is this module:
//
//     connect to the server  ->  create a room (you are the host)
//                            ->  join a room   (you are a client)
//
// and the lobby's only job is to answer "which address is the host listening
// on" for everyone who joined. The moment it knows, it hands that to
// session_request_host / session_request_join -- the same two calls the debug
// panel's buttons make, so the runtime-role path (config_set_role plus the late
// hook installation) is exercised by the normal flow rather than by a
// diagnostic button.
//
// WHAT IT IS NOT: a transport. It never carries game traffic, and it is not in
// the same file as the transport for the same reason mgmp_net is not in
// mgmp_session: a lobby and a session have different lifetimes, different
// failure modes, and one of them is still useful when the other is dead.
//
// THE THREADING RULE IS mgmp_net's, for the same reason (nothing may block the
// game's frame thread), with one addition: DIALING IS ALSO OFF-THREAD. A TCP
// connect to an address that is not answering takes seconds -- up to twenty on
// Windows -- and that is the single most likely thing a player will do with a
// wrong IP typed into the panel. So the worker thread owns the socket end to
// end: it dials, it sends hello, it receives, and it pushes complete lines into
// a ring the game thread drains.
#pragma once

#include <cstdint>

namespace mgmp {

enum class SignalState {
    Off,         // no session with the server, and none wanted
    Connecting,  // the worker is dialling
    Connected,   // hello/welcome done; the lobby is usable
    Failed,      // the dial or the connection failed; see signal_error()
    Closed,      // the server hung up on us
};

const char* signal_state_name(SignalState s);

// One row of the lobby's room table. Fixed-size and plain, so the panel can
// hold a snapshot without allocating every frame.
struct SignalRoom {
    char     id[16]   = {};
    char     name[48] = {};
    char     host[32] = {};
    uint16_t players  = 0;
    uint16_t cap      = 0;
    bool     pw       = false;   // the room has a password (the list shows a padlock)
};

// One member of the room this peer is in.
struct SignalPeer {
    char name[32] = {};
    char role[8]  = {};   // "host" | "client"
    bool away     = false; // playing on their own for a while (a house boss fight)
};

// --- lifecycle ---------------------------------------------------------------
//
// Reads the signal block of mgmp.json, seeds the panel's fields from it, and --
// if signal.auto_connect -- records a connect request. Nothing is dialled here:
// the request is applied by the first signal_update, so there is exactly one
// path into a connection whether it was configured or clicked.
//
// Called from session_start().
void signal_init();

// Closes the socket and stops the worker. Nothing calls this at runtime -- the
// mod is injected and never unloaded -- but a process that does want to tear
// the lobby down has to be able to.
void signal_shutdown();

// Once per frame, from session_update() and BEFORE its apply_request: a room
// created or joined this frame then has its session request applied in this
// same frame rather than the next.
void signal_update();

// --- requests (recorded by the panel, applied at the top of a frame) ---------
//
// Same contract as session_request_*: the last thing clicked is what was meant,
// and nothing here touches a socket from inside the draw path.

// addr is the server; name is this player's lobby name ("" keeps the current
// one, which defaults to the computer name).
void signal_request_connect(const char* addr, uint16_t port, const char* name);
void signal_request_disconnect();
void signal_request_list();
// `password` is optional: empty/null = an open room (create) / no password offered (join). It is hashed here --
// the server only ever sees a SHA-256 digest -- and a server that predates room passwords is refused rather than
// silently making the room public (see signal_server_has_passwords).
// How the host wants to be reached (2026-10-03): its own game port, or Steam's peer-to-peer relay. A joiner then uses only that carrier.
// Relay = the lobby server carries the game's bytes (it can only afford a few such rooms: see signal_relay_info).
enum class RoomTransport : int { Any = 0, Direct = 1, Steam = 2, Relay = 3 };
void signal_request_create(const char* room_name, const char* password = nullptr, RoomTransport transport = RoomTransport::Any);
// The carrier of the room this peer is in: "direct", "steam", or "" (an older host / a LAN room: everything is tried).
const char* signal_room_transport();

// The pre-check of the game port, before a room is created: the mod listens on it for a moment (and asks the router for a mapping), and the server
// dials it from outside. Runs by itself after connecting to a public server; signal_request_direct_check() runs it again (e.g. after the player
// opened the port).
enum class DirectCheck : int {
    Unknown,   // not run yet
    Checking,  // under way
    Open,      // the server reached the port (or the server is on this network: nothing to check)
    Closed,    // the server could not reach it: the port has to be opened (forwarded) before "direct" can be chosen
    Busy,      // another program already listens on the port here
    NoAnswer,  // the server did not answer (an older server): direct is allowed, but unproven
};
DirectCheck signal_direct_check();
void signal_request_direct_check();
// Steam as a carrier: usable = Steam networking is up in this process. `text` = its state in one line (for the panel / the log).
bool signal_steam_usable();
// The lobby server is on this machine / this network (a LAN lobby): no carrier needs choosing there.
bool signal_server_private();
// The server's relay allowance as it last said ("welcome"/"rooms"): true when this server can relay at all; `used` of `max` rooms are taken.
bool signal_relay_info(int* used, int* max);
void signal_steam_status(char* out, unsigned cap);
void signal_request_join(const char* room_id, const char* password = nullptr);
// Leave the room AND drop the game session: the room exists to introduce the
// two peers, so staying connected to the server while walking out of the room
// is a state with no meaning.
void signal_request_leave();
bool signal_request_pending();

// The room lock (host only, 2026-09-30): a locked room is hidden from the server's list and refuses
// joins. Applied on the next signal_update, independently of the one-slot request above so a lock
// click can never be swallowed by a list refresh. signal_room_locked() is the SERVER's answer, as
// broadcast to every member -- not what was asked.
void signal_request_lock(bool on);
bool signal_room_locked();
// Tell the room this player is (not) away playing alone. Re-sent after a reconnect to the server.
void signal_set_away(bool on);
bool signal_self_away();

// --- room passwords (2026-10-01) ------------------------------------------------------------------------
bool        signal_server_has_passwords();   // the server announced it understands them
bool        signal_room_has_password();      // the room this peer is in has one
// The last refusal's machine-readable reason ("password" when a join lacked or got the password wrong, "" otherwise)
// and a counter that goes up with every refusal, so the panel can react to a NEW one exactly once.
const char* signal_error_code();
uint32_t    signal_error_seq();

// Hosts: the server dials our game port from outside a moment after the room is created (and again once the router mapping
// is up). 0 = not known yet, 1 = reachable, 2 = NOT reachable (friends outside this network will time out).
int signal_reach();

// --- state for the panel -----------------------------------------------------

SignalState signal_state();
const char* signal_error();      // "" unless state is Failed
const char* signal_status();     // one line: what the lobby shows at the top
const char* signal_server();     // "addr:port" we are connected to, "" if not
const char* signal_name();       // this player's lobby name
const char* signal_room();       // room id, "" when not in one
const char* signal_role();       // "host" | "client" | ""
// where the host is; "" until a room is joined. May be a comma-separated list (the address, then fallbacks): pass it
// to session_request_join as it is.
const char* signal_host_addr();
uint16_t    signal_host_port();
const char* signal_last_event(); // last membership line, for the lobby's log

// Snapshots. Both return how many were written.
uint32_t signal_rooms(SignalRoom* out, uint32_t cap);
uint32_t signal_peers(SignalPeer* out, uint32_t cap);

} // namespace mgmp
