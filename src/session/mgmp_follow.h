#pragma once
// mgmp_follow -- the client follows the host through the MAP screen.
//
// This is the first piece of the meta layer, and it exists because the map has
// something the rest of the meta layer does not: a single non-virtual choke
// point where a decision lands exactly once.
//
//   glaiel::MapScreen::EnterNode(MapScreen*, MapNode*)  @ 0x140391050
//
// One code caller (a std::function _Do_call at 0x14039A5B0, itself reached only
// from a UI vftable), and its only data xref is .pdata unwind info -- so it is
// not virtual and every map decision passes through it. That is the same shape
// as TurnControl::ApplyTurnAction, which is what made the battle layer
// tractable; the design notes' claim that the meta screens have "no ApplyTurnAction
// equivalent" turns out to be true of the house/shop/level-up screens but NOT
// of the map.
//
// THE FIND THAT MATTERS MOST IS INSIDE IT. EnterNode's prologue does:
//
//     mov    eax, 178h
//     mov    rcx, gs:58h
//     mov    rdx, [rcx]
//     movups xmm0, [rdi+118h]        ; rdi = MapNode
//     movups [rax+rdx], xmm0         ; TLS+0x178  <- node seed, bytes 0..15
//     movups xmm1, [rdi+128h]
//     movups [rax+rdx+10h], xmm1     ; TLS+0x188  <- node seed, bytes 16..31
//
// i.e. every map node carries its OWN 32-byte xoshiro256 seed at MapNode+0x118,
// and entering the node copies it wholesale into the simulation stream.
// MapScreen::generate_map writes the same 32 bytes (0x14021C558 / 0x14021C563).
//
// That is the mechanism behind a result this project had only measured:
// "entering a battle does not re-seed the stream -- four separate launches all
// entered at s0=967e2d6d328620b1", and "two peers whose streams had already
// drifted apart at the menu agreed on rng_hash by the first turn boundary".
// Both are this instruction pair. The consequence for co-op is strong: the
// peers do not need to exchange a seed at all. They need to enter the same
// node, and the seed comes with it.
//
// Which is exactly what this module does: the host publishes the node it
// entered, the client enters the same one, and local map input on the client is
// suppressed because the host owns the run.

#include <cstdint>

namespace mgmp {

struct EnterNodeMsg;

void follow_init();
void follow_shutdown();

// A NEW RUN IS STARTING -- not a new session. Clears where this peer is standing,
// the queued nodes and the fault latch.
//
// The distinction is not cosmetic and it is called out because it has already
// been paid for once: a reconnect runs go_ready again (session_shutdown -> begin
// -> WELCOME), and re-initialising the MODULE must not throw away what is known
// about the RUN. `have_here` is exactly that kind of fact -- clearing it on a
// reconnect makes the host's catch-up look like a new node, and the peer walks
// back into a node it has finished. That is why follow_init does not call this;
// the save load does (via choice_reset_run).
void follow_reset_run();
struct NetMsg;
bool follow_hold_state(NetMsg& m);
void follow_after_enter_node();

// CLIENT ONLY, READ ONLY, OFF BY DEFAULT (tune::kNodeProbe): dump a followed node's own
// word window and the map's, so the field that marks "this node is consumed / the party is
// here" can be found by comparing a battle entry (which advances the client's map) with an
// event or shop entry (which does not). See the definition -- it writes nothing.
void follow_probe_node(void* node);

// CLIENT ONLY, ON BY DEFAULT (tune::kNodeConsumeMark): mark the node this peer just
// followed into as CONSUMED on ITS OWN copy of the map. MapNode+0x168 is the flag the
// game's own code flips when a node is finished -- measured 0 -> 1 on the host for three
// different nodes and 0 forever on the client -- so without this the client's map never
// advances and a solo peer walks nodes the host already cleared. One byte, only 0 -> 1,
// read back. See the definition.
void follow_mark_node_consumed(void* node);

// CLIENT ONLY: tell this peer's map MARKER that the party is on the node we just followed
// into, by writing the same slot MapNode::Click writes (*(MapScreen+0xA0)+0x60).
//
// The marker is where the party is DRAWN: +0x50 is where it stands and +0x60 is what was
// selected, and the game animates one to the other itself. Writing the selection rather
// than the position means no position is faked -- the game does the move, exactly as if
// the player had clicked. Visual only: the functional half ("this node is done") is
// follow_mark_node_consumed, and play already works without it.
void follow_move_marker_to(void* node);

// The chapter map's node seeds. The host sends its own (once per map); a client with the same node count and types writes them over its own
// before its map is reported to the checkpoint, which otherwise refuses two maps whose seeds differ. Stored on arrival, applied in follow_map_update.
struct MapSeedsMsg;
void follow_on_mapseeds(uint8_t from, const MapSeedsMsg& m);

// CLIENT ONLY, and only for a node whose content this peer is allowed to draw
// differently -- i.e. anything that is NOT a battle. Advances this peer's
// simulation stream by one draw immediately after EnterNode returns, which is what
// makes the two peers roll DIFFERENT "?" events, chest contents and shop stock
// from the same node seed. Called from h_MapUpdate's follow path, the client's only
// way into a node. See the definition and DESIGN-independent.md sec. 3.
void follow_perturb_after_enter(void* node);

// True when this peer should not act on its own map input -- i.e. we are the
// client and following is on.
bool follow_suppresses_local_input();

// Called from h_EnterNode. `sent` reports whether the decision was published.
// Returns false if the caller should NOT run the original -- the client's own
// clicks are swallowed here.
bool follow_on_enter_node(void* map_screen, void* node, bool* sent);

// Called at the tail of MapScreen::update, every frame the map is up. Returns
// the MapNode the caller should now pass to the original EnterNode, or nullptr.
// Returning the node rather than calling it keeps the o_EnterNode pointer in
// mgmp_hooks.cpp, where every other original lives.
uint64_t follow_resume_checkpoint(void* map_screen);
void* follow_map_update(void* map_screen);

void follow_on_message(const EnterNodeMsg& m);

// HOST: tell a peer that just joined which node the run is standing in. ENTERNODE
// is otherwise published only at the instant of entry, so a peer connecting
// afterwards would sit on the map forever.
void follow_catchup(uint8_t peer);

// --- "are we standing ON the map, or inside a node?" -------------------------
//
// True while the map has ticked recently and is neither initializing nor entering a node.
//
// This exists because MewDirector::save_adventure is a node-boundary function
// and we were calling it at an arbitrary moment. The game itself only ever
// calls it from ReturnToMap. Calling it mid-battle persists "inside node N,
// unresolved", and a run reloaded from that sits on the map unable to enter the
// node or move past it -- which is exactly what happened on 2026-08-26 when a
// client's reconnect triggered savefile_catchup at turn 2 of a fight.
//
// A staleness window rather than a bare flag: MapScreen::update does not run on
// the frame a node is being entered, and one frame of "not on the map" is not a
// reason to refuse a flush.
bool follow_on_map();

// --- enumerating the map, for the debug panel --------------------------------

// Number of nodes on the live map, or 0 if there is no map screen yet.
uint32_t follow_node_count();

// Type, seed and name of one node. False if the index is out of range or the
// map is not up.
bool follow_node_info(uint32_t index, uint32_t& type, uint64_t& seed0,
                      const char** type_name);

// Which node the run is standing in, as last entered or followed. False if this
// peer has not entered one yet -- which includes every freshly loaded run, so
// prefer follow_marker_node below for "where am I".
bool follow_current_node(uint32_t& index);

// The seed of the node this peer is standing in, or 0 if it has not entered one
// this session. Recorded on BOTH peers by remember_node, so the host stamps its
// CHOICE messages with it and the client checks a held choice against it.
//
// This is the same 64 bits battle_id uses, and for the same reason: it is the
// only name for a node that two processes can independently arrive at.
uint64_t follow_here_seed();

// Where the map MARKER is, and what is SELECTED, read live out of the game
// (MapScreen+0xA0 -> +0x50 / +0x60, the slot MapNode::Click writes).
//
// Unlike follow_current_node these survive a reload, because they are the
// game's own state rather than this module's memory of the session. Both are
// validated against the node vector before being reported, so a wrong offset
// returns false rather than a confident wrong index.
bool follow_marker_node(uint32_t& index);
bool follow_selected_node(uint32_t& index);

// PANEL: ask to enter node `index` on the next MapScreen::update tick.
//
// DEFERRED, and to that tick specifically -- not merely off the render thread.
// Entering a node tears down and builds scenes, and the one moment in the frame
// that is known to be safe for it is the tail of MapScreen::update, because
// that is where the game's own UI callback fires it from and where the client's
// follow path has always called it. Anywhere else is our own novel call site.
//
// Host only: entering a node is a run decision, and it publishes ENTERNODE
// through the ordinary hook, so the client follows exactly as if the node had
// been clicked. This EDITS THE RUN and says so in the log every time.
void follow_request_jump(uint32_t index);

} // namespace mgmp
