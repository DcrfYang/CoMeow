// mgmp_perf.h -- is the lag the NETWORK or a PEER'S COMPUTER? (0.1.68, proto 104)
//
// A room's players report "very laggy" and the logs could not say whose machine or which link: nothing recorded how long a frame took, or how long a message took to come back.
// Every 2 s each peer sends the other a PING carrying the frame statistics of its last 2 s (frames, average / longest frame, frames over 100 ms); the other peer answers at once
// FROM ITS RECEIVE THREAD (so the round trip is the link's, not the other game's frame rate) with its own. Every 10 s each peer logs one PERF line holding both: its own frames,
// the round trip to the other peer, and the other peer's frames -- one log is enough to tell "the host's game is stalling" from "the link is slow".
// The lines are written only when something is notable (a frame over 150 ms on either side, a round trip over 100 ms) and otherwise once a minute. Grep PERF.
#pragma once

#include <cstdint>

namespace mgmp {

struct PingMsg;

// h_FrameBegin: the start of a frame (`t_in` receives a high-resolution stamp) and, right before the game's own frame runs, the end of the mod's part of it.
void perf_frame_begin(long long* t_in);
void perf_frame_mod_done(long long t_in);
// Every frame, after the network is pumped: sends the 2 s ping, writes the 10 s line.
void perf_tick();
// The receive thread: a ping (answered by the caller with perf_make_reply) or its reply (the round trip is taken).
void perf_on_ping(uint8_t from, const PingMsg& p);
uint32_t perf_make_reply(uint8_t* buf, uint32_t cap, const PingMsg& request);
void perf_reset();
// 0.1.84: how slow the worst link of this room is right now, in ms -- the larger of its last round trip and how long its oldest unanswered ping has waited (0: fine, or
// not in a room). `who` gets that peer's id. For the "the link is slow, this is not a freeze" toast (mgmp_room).
uint32_t perf_link_slow_ms(uint8_t* who);
// 0.1.85: this peer's round trip to that peer for the room panel -- the last ping's, or, once a ping has waited 3 s or more, how long it has waited. Another client
// has no link of its own (pings are not relayed): its figure is this peer's round trip to the host plus the host's to it, from the list the host's ping carries
// (proto 105), and `via_host` says so. False: no figure yet (still connecting), itself, or not in a room.
bool perf_peer_latency_ms(uint8_t peer, uint32_t* ms, bool* via_host);

} // namespace mgmp
