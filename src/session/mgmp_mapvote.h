// mgmp_mapvote.h -- THE MAP: EVERY PLAYER PICKS THE NEXT NODE BEFORE THE PARTY MOVES (0.1.95, proto 106).
//
// Until now the host's click was the run's only decision: it walked the party to the node, entered it and published it, and a client's click was swallowed at
// EnterNode (after the party had already started to walk). Now, in a room, a click on a node is a VOTE:
//
//   * T_NodeClick (the node's click callback, the only writer of the marker's "selected" slot) is swallowed on EVERY peer. Nobody's party icon moves on a click.
//   * the pick is sent to everybody (MSG_NODEVOTE) and shown on every screen as a small paw in that player's colour at one corner of the node
//     (P1 top left, P2 top right, P3 bottom left, P4 bottom right). Clicking the picked node again takes the pick back; clicking another node moves it.
//   * when EVERY player has picked, the HOST's pick is clicked for real (the game's own callback, on the host): the party walks there, the host's EnterNode
//     publishes the node and every other peer follows, as before. Where the players picked different nodes (after a mini-boss: the normal or the hard way; after a
//     boss: the next floor or home) the host's pick is the one that counts -- but it still waits for the others to pick.
//
// A host that needs to go on without somebody holds Ctrl while it clicks: its pick is entered at once, whatever the others have done.
//
// Outside a room (alone, or no follow) nothing here is active: the click is the game's own.
#pragma once

#include <cstdint>
#include "mgmp_proto.h"

namespace mgmp {

// --- the book of picks: pure (tests/test_mapvote.cpp) ----------------------------------------------------------------------------------------
struct MapVote { uint64_t seed0 = 0; uint32_t index = 0; bool set = false; };
struct MapVoteBook {
    MapVote v[kMaxPeers];
    void clear() { for (MapVote& x : v) x = MapVote{}; }
    bool any() const { for (const MapVote& x : v) if (x.set) return true; return false; }
    bool has(uint8_t p) const { return p < kMaxPeers && v[p].set; }
    // True when the pick changed (a new one, or another node than before).
    bool put(uint8_t p, uint64_t seed0, uint32_t index) {
        if (p >= kMaxPeers) return false;
        MapVote& x = v[p];
        const bool changed = !x.set || x.seed0 != seed0 || x.index != index;
        x.set = true; x.seed0 = seed0; x.index = index;
        return changed;
    }
    bool drop(uint8_t p) { if (p >= kMaxPeers || !v[p].set) return false; v[p] = MapVote{}; return true; }
    bool is(uint8_t p, uint64_t seed0, uint32_t index) const { return has(p) && v[p].seed0 == seed0 && v[p].index == index; }
    // How many of the `n` members in `ids` have picked; and whether they all have (at least one member).
    unsigned count(const uint8_t* ids, uint8_t n) const { unsigned c = 0; for (uint8_t i = 0; i < n; ++i) if (has(ids[i])) ++c; return c; }
    bool all(const uint8_t* ids, uint8_t n) const { return n > 0 && count(ids, n) == n; }
    // A click by `p` on (seed0, index): its new state -- true = it now holds that pick, false = it took the pick back (it had the same node). `changed` is always true.
    bool toggle(uint8_t p, uint64_t seed0, uint32_t index) {
        if (is(p, seed0, index)) { drop(p); return false; }
        put(p, seed0, index);
        return true;
    }
};

// --- the game side ---------------------------------------------------------------------------------------------------------------------------
// T_NodeClick: `node` is the clicked MapNode. True = swallowed (a pick); false = the game's own click runs (not in a room, or not something to vote on).
bool mapvote_on_click(void* node);
// MSG_NODEVOTE from another player.
void mapvote_on_message(uint8_t from, const NodeVoteMsg& m);
// Every map update. `click` runs the game's own click callback for a node (the host's pick, once every player has picked).
typedef void (*MapVoteClickFn)(void* node);
void mapvote_tick(void* map, MapVoteClickFn click);
// The overlay's mouse, on the 1920x1080 stage, every frame: the place of a click, to calibrate the map layer against the screen.
void mapvote_note_mouse(float stage_x, float stage_y);
// LevelUpScreen::update ticked (from its hook); and whether it did in the last half second. Only for the overlay's log line: that screen keeps ticking after it is gone.
void mapvote_note_screen_over();
bool mapvote_level_ticking();
// MapScreen::update ticked in the last 300 ms (every tick, whatever the follow module is doing with it).
bool mapvote_map_ticking();
// All picks forgotten (a node was entered, the map changed, the room ended).
void mapvote_reset(const char* why);

// For the overlay (mgmp_menu): where each player's paw goes. Positions are on the 1920x1080 stage; `half` is half a node's size there.
struct MapVoteMark { uint8_t peer = 0; float cx = 0, cy = 0, half = 0, angle = 0; };   // angle: radians, -45..+45 degrees, a fixed pseudo-random one per player and pick
int  mapvote_marks(MapVoteMark* out, int cap);
// For the status line: how many of the room's players have picked, of how many; whether this player has; whether this one is the host's turn to be waited for.
bool mapvote_counts(unsigned& have, unsigned& total, bool& i_picked);

} // namespace mgmp
