#pragma once
// mgmp_nodehash -- the meta layer's per-node hash. The check the battle layer
// has had per turn since protocol version 5 and this half of the game never
// had at all.
//
// WHY IT IS NEEDED, IN ONE SENTENCE: the meta layer is synced by DETERMINISM
// plus a replicated choice, and determinism sync fails silently.
//
// The longer version is that it fails worse than silently. The per-node CATDATA
// and INVENTORY pushes OVERWRITE a divergence rather than report it, so a meta
// split does not surface where it happened -- it surfaces later, somewhere
// else, as a battle desync with nothing in either log to connect the two. The
// unexplained turn-0 mismatch of 2026-08-25 has exactly that shape: it followed
// a node that resolved two level-ups and an event, and diverged in both rng and
// state before a single turn had been played.
//
// WHAT IS HASHED, AND WHY EACH FIELD EARNED ITS PLACE. These are the inputs the
// event roller and the node itself actually read -- see mgmp_runhist.h for the
// reverse engineering behind the first two:
//
//   rng[4]     TLS+0x178, the simulation stream.                    COMPARED
//   cats_hash  the run's cat ids IN ORDER, because sub_1400AACD0 indexes that
//              list with one xoshiro step to choose the event's subject cat. A
//              different order is a different cat from the same draw. COMPARED
//   cat_count  carried beside the hash so a mismatch says whether the rosters
//              are different lengths or merely different.           COMPARED
//   hist_hash  the run history -- the used-event list the pool draw skips over.
//   inv_hash   coins, food, boxes and the three bucket counts. Cheap, and it
//              covers the one thing an event most often changes.
//   event[]    at the event sample point only: the name of the event that was
//              actually chosen.
//
// THE LAST THREE ARE PER-PLAYER AND ARE NOT COMPARED (2026-09-22). Each player's
// stash, and therefore the used-event list and hence which "?" event gets rolled,
// is now their own -- the design the user asked for. A difference in them is the
// intended state, so judging it a fault would report a mismatch on every node.
// They still ride in the message, and kNodeHashPerPlayerReport prints them, which
// keeps the evidence available on the day something ELSE looks wrong. See
// DESIGN-independent.md.
//
// TWO SAMPLE POINTS, answering different questions -- see NodeHashMsg.
//
// IT REPORTS, IT DOES NOT HALT. Deliberately, and not for the same reason the
// battle layer halts. A battle desync means every subsequent turn is fiction; a
// meta divergence usually means one number differs and the run is still
// playable, and stopping a co-op session dead over a coin count would be worse
// than telling the players. `net_nodehash_halt` exists for a session that wants
// the strict behaviour, and defaults off.
//
// SYMMETRIC. Both peers compute and send their own; each compares arrivals
// against a ring of what it sent, in both directions, for the reason
// mgmp_hashring.h spells out -- a host-authoritative hash could only ever
// report that the CLIENT disagreed, and it is the host that is authoritative
// about the run, so the direction that matters is the one it cannot carry.

#include <cstdint>

namespace mgmp {

struct NodeHashMsg;

// Only the MewDirector pointer VARIABLE, which is data and has no prologue to
// check. Everything read through it is range-checked at the point of use, the
// way mgmp_catsync validates the same slot.
void nodehash_set_base(uintptr_t base);

void nodehash_init();
void nodehash_shutdown();

// Called by mgmp_follow from both peers' paths into a node, immediately before
// EnterNode runs. See kNodePointEnter.
void nodehash_on_node(uint64_t node_seed, uint32_t node_index);

// Called from the WorldEvent::update hook. Samples once per screen, on the
// first tick at which the event's name can be read.
void nodehash_on_event_screen(void* world_event);

void nodehash_on_message(uint8_t from, const NodeHashMsg& m);

// WHICH EVENT THE OTHER PEER IS LOOKING AT, for the node we are standing in.
//
// This is the shared signal the per-player split needs (DESIGN-independent.md sec. 5):
// once each peer draws its own event, the OPTIONS are no longer common ground, so
// "is this a story event" cannot be answered from them -- but the NAME can be, because
// this module has been carrying it all along (`NODEHASH event on node ... is 'X'`).
//
// Same name  -> the two are looking at one event  -> it is shared   -> host decides.
// Differs    -> it is genuinely per-player        -> each decides its own.
//
// Returns false when no sample for that node has arrived yet, which is a real answer:
// the caller is expected to wait briefly rather than to guess -- see mgmp_choice's
// bounded wait, the same shape as the level-up screen's.
bool nodehash_peer_event_name(uint64_t node_seed, char* out, uint32_t cap);

} // namespace mgmp
