#pragma once
// mgmp_roster -- WRITING the run's party. The primitive the "each player brings
// their own cats" design is made of, and the only module in this project that
// edits the run on purpose.
//
// --- what "who fights" actually is, in two layers ----------------------------
//
// Measured 2026-09-22, and the first version of this file got it wrong, so the
// correction is the interesting part.
//
// THE AUTHORITATIVE LAYER IS THE RUN'S CAT ID VECTOR. `MewDirector + 1468` is its
// count, `+1472` its data pointer, and the element stride is 8 bytes with the id
// in the low four. Read live with a run loaded:
//
//     +1460 u32 = 446    capacity (0x1BE) -- room for far more than a party
//     +1464 u32 = 4      a second count-like field; purpose NOT established
//     +1468 u32 = 3      the count that matters
//     +1472 u64 = 0x...  the data pointer
//     ids at stride 8:   2fc 24d 2f6   (2df had just been dropped)
//
// Writing ONLY +1468 (4 -> 3), and nothing else at all, made the NEXT battle come
// up with three cats instead of four -- seen by the player and confirmed by the
// read-back. So this layer decides, it is writable, and its capacity means a party
// can be GROWN as well as shrunk, which is exactly what a per-peer party needs.
//
// THE DERIVED LAYER IS A PER-BATTLE COPY. `sub+0x18` carries a table of source
// entries and `sub+0x20` a parallel table of live containers, and slot 505
// (0x1F90/16) is the one consumers read. It is rebuilt from the run at each node
// boundary and RELEASED when the fight ends -- which is why an edit aimed at the
// map tick, between nodes, found nothing there and refused ("slot 505 did not read
// as {cap, count, array} plus a live container"). Its writers are all in the turn
// layer: 0x8E3420 resyncs seven slots from one body, the virtual 0x8E2A30 that
// drives it calls TurnControl::QueueDecision, and the other chain is
// 0x96AC70 -> 0x96D5C0. Derived, not authoritative.
//
// --- what this module does ---------------------------------------------------
//
// One debug number (config `debug.roster_shrink`) that drops that many cats from
// the run's party, once per process, from the map tick. It exists to PROVE the
// primitive before a merge is built on it, and it is verifiable from outside:
// enter any battle afterwards and count the cats.
//
// The merge itself -- two peers' choices becoming one party -- is the next piece.
// It writes this same vector: both peers send their picks, both compute the same
// deterministic merge, both write ids and count, guarded by the capacity.
//
// MEASURED 2026-09-22, and it does: see "The party merge, verified" in the design notes.
// The ids written here are exactly the human cats the next battle builds, on both
// peers -- confirmed from outside the module, by the fight.
//
// The reconciliation of a real DIFFERENCE was verified LATER THE SAME DAY, the way
// this comment said it had to be: `debug.roster_shrink = 1` in the CLIENT's config
// only, so the client dropped a cat the host still had. The merge restored it --
// `-> PARTY (agreed): [2d3 315 312 316]` on the host, `[2d3 315 312] -> [2d3 315 312
// 316], count 3 -> 4, read back identical` on the client. The host's list was intact
// in that test, so the union reduced to "the client's pick comes back"; two peers
// each missing a DIFFERENT cat has still not been seen.
//
// --- it edits a live run, loudly ---------------------------------------------
//
// Same rule as the panel's node jump and the debug damage hit: an action that
// changes the run says so at Warn, with the value it replaced beside the value it
// wrote, and nothing here runs unless config asks for it.

#include <cstddef>
#include <cstdint>

namespace mgmp {

// Declared in mgmp_proto.h; forward-declared here the same way mgmp_leave does it,
// so this header does not drag the whole wire format into every translation unit
// that only wants to ask about the party.
struct PartyMsg;

// The damage-hit prologue check lives on (see mgmp_roster.cpp): this module no
// longer calls rva 0x96B470, but the verification is kept because a build that
// cannot be identified is a build nothing should be written to.
void roster_init();

// Kept for the same reason: a TurnControl is no longer needed to reach the party
// (the MewDirector is), but the hand-off is still recorded so a log says whether
// one was ever seen.
void roster_set_turn_control(void* turn_control);

// Called from the map tick. Defers to there not because the party needs a map --
// it does not -- but because a RUN EDIT belongs on a known-safe tick, and the map
// tick is the one the game's own node click lands in.
void roster_tick();

// One line of live state, for the panel and for a log a human is reading.
void roster_status(char* out, size_t out_size);

// --- the party exchange (MSG_PARTY) ------------------------------------------
//
// Each peer's own picks are already in its own run -- that IS the cat box the
// player fills in the House -- so the exchange is one small message per change and
// no new UI. Peer 0 (the host) merges deterministically (its order first, then
// whatever the others added) and broadcasts the AGREED list; everyone writes the
// same ids into their own run, which is the layer that decides who fights.
//
// --- AND NOW EACH PEER SAYS ONLY ITS OWN HALF (2026-09-22, 4+4 steps 1-2) -------
//
// The 4+4 shape is: the two players field two cats each, and the BATTLE still holds
// four -- the game's native size -- so the party list both peers agree on is
// MINE + YOURS, two of each, in peer 0's order. That is the same merge as before; what
// changed is what a peer PUBLISHES. It used to publish its whole run list, which only
// worked because both peers load the same save and therefore said the same thing. Now
// it publishes the cats that are ITS OWN, and the other half arrives over the wire.
//
// WHO IS MINE is answered by lockstep_cat_is_mine -- the same answer the control split
// uses, and the one reader that works on the map rather than only inside a battle. Its
// contract sets the rule, and the rule is not symmetric:
//
//   provably mine       publish it
//   provably the peer's do NOT publish it: those ids come from the peer, verified,
//                       never invented by this module
//   not known yet       publish it anyway
//
// The third case is the conservative one, and before the first BATTLE it is the ONLY
// case: the ownership table is filled at a battle snapshot, so early in a run every
// peer publishes everything and the result is byte-identical to the old behaviour. No
// cat can be lost by an answer this module does not have.
//
// Called from the map tick, i.e. between nodes and never during a battle. It is a
// no-op when the two lists already agree, which is the common case while both
// peers load the same save -- so being on by default cannot hurt a session that
// never asked for it.
void roster_party_tick();

// --- ADDING IDS: the one write the 4+4 import needs (2026-09-23) --------------
//
// mgmp_catsync's import CREATES the other player's cats (CatData objects, the game's
// own deserializer); this puts their ids INTO the run's list, which is the layer the
// rest of the run actually reads -- who fights, who an event picks, what the save gets.
//
// It goes through the same write as everything else (capacity check, write, read
// back, Warn with before/after) rather than being a second writer of that field. The
// ORDERING is left to the normal exchange: once an id is in the list the publish
// offers it (ownership unknown, so it is offered), and peer 0's merge is what makes
// both peers agree on one ordered list -- which is what the control split and the
// index-keyed ACTION wire both need.
bool roster_add_ids(const uint64_t* ids, uint32_t count, const char* why);

// --- ROUTE A: LET THE GEAR SCREEN SEE THE WHOLE RUN (2026-09-23) -------------------
//
// The client cannot equip its familiars because the game's gear screen reads the RUN'S
// PARTY vector and nothing else. Measured, not assumed: CatSelector_Right (rva 0xE91E0)
// does `mov r8d,[rcx+0x5BC]` / `mov rdx,[r9+0x5C0]`, and a live capture of that handler
// (tune::kCatSelectorProbe, now off) showed rcx = the button's own callable holding, at
// +0x10, the MewDirector whose +0x5BC/+0x5C0 were the party's count (4) and array.
//
// So the screen is aimed at ONE pointer, and this function re-aims it -- at a stand-in
// object that answers with the run's WHOLE cat list (party followed by familiars)
// instead of the party alone. Nothing about the run changes: the party vector is not
// written, no message is published, no rotation is needed, and both peers can look at
// their own screens at the same time. Ownership is still what decides whose edit wins.
//
// Called for every button the game updates (the same detour the button probe uses),
// which is how the screen is recognised and how it is noticed to be gone.
void roster_catselect_redirect(void* button);

// Node entry must stop if the shared roster cannot be restored and read back.
bool roster_party_swap_release();
void roster_party_swap_after_node(uint32_t type);
void roster_party_swap_on_map();
void roster_party_swap_watch();
void roster_party_swap_reset();

// The game's post-battle promotion fills the party to four from the familiar list.
// With variable parties (Host3/Client2 ...) that moves another peer's cat into the
// host's party. This puts every cat back on its owner's side -- owner 0 in the
// party, everyone else in the familiars -- keeping order. Only writes when the
// shape is wrong; false when the lists are unreadable or an owner is unknown.
// Map-phase only (never during a battle).
bool roster_normalize_shared(const char* why);

// Client, at node entry after the shared roster is restored: take the run's
// membership from the host's node snapshot (its cats = the host party, plus the
// familiar ids), because an event can change it on the host alone -- CatHole's
// leave_party_temporarily was the measured case. Rewrites the three lists and
// re-bases the local map view. False only when the write itself failed; lists
// that do not validate are logged and left alone (the node hash reports them).
// LEAVING THE ROOM (2026-10-05): the run keeps only this peer's own session cats. Every id of another player's session range (0x70..0x7F, owner != keep_pos) is taken out of the party, the mirror and the familiar
// list; ordinary ids (a cat the game itself brought in) stay, because their owner cannot be proven. If the party would be left empty the own cats of the familiar list move up. Map phase only. Returns the number
// of ids removed, 0 when there was nothing to remove, -1 when the lists could not be read or written (the caller may try again).
int roster_leave_prune(uint8_t keep_pos, const char* why);
// The same for a player who dropped out of a room that goes on: only the cats of the positions in `mask` (bit p = position p) are taken out. Same return convention.
int roster_drop_positions(uint32_t mask, const char* why);

bool roster_adopt_host_shared(const uint64_t* cats, uint32_t cat_count,
                              const uint64_t* familiars, uint32_t familiar_count,
                              const char* why);

// --- T1 (2026-09-23): the same ids, into the FAMILIARS list instead -----------------
//
// Same append, different vector: MewDirector+0x640, the list the engine itself uses
// when a cat cannot join the party (0x929E4C). It exists to answer one question --
// whether the map layer tolerates a familiar list filled from the wire -- without
// touching the party at all, because the party is the thing that crashes (see
// tune::kImportToFamiliars). Ids already present are skipped; a helper duplicated is a
// state the engine never builds.
// True when all requested ids read back present, including an already-imported save.
bool roster_add_familiars(const uint64_t* ids, uint32_t count, const char* why);

// Setup barrier helpers.  They replace the local party and establish the same
// party=host/familiars=client layout on both peers before the map is entered.
bool roster_setup_replace_party(const uint64_t* ids, uint32_t count, const char* why);
bool roster_setup_install_shared(const uint64_t* ids, uint32_t count,
                                 const char* why);
void roster_setup_set_ready(bool ready);

// A PARTY message. kind 0 = a peer's picks (stored, and merged if we are peer 0);
// kind 1 = the agreed list (written, unless we are the one who sent it).
void roster_on_party(uint8_t from, const PartyMsg& m);

} // namespace mgmp
