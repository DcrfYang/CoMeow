// mgmp_lockstep.h -- deterministic lockstep over the battle. Phase 4, layer 3.
//
// THE SEAM IS THE ONE THE REPLAYER ALREADY PROVED. Phase 2B injected recorded
// decisions at Brain::GetChoice and got a byte-identical battle back -- 17 of
// 29 decisions injected, the other 12 left for the AI to re-derive, all 12
// matched. This module swaps the recorded FIFO for a socket. Same hook, same
// injection point, same validation; the decisions now arrive from a peer.
//
// WHY WAITING IS FREE. GetChoice is a *poll*, not a decision point: it returns
// type=1 ("nothing decided yet") on every frame while waiting on a human, and
// did so for 1695 of 1711 calls in one tutorial battle. So a remote decision
// that has not arrived needs no timeout, no blocking recv and no frame budget.
// Return type=1 and the game waits exactly as it already waits for a person.
//
// CAT IDENTITY, AND WHY IT IS A SNAPSHOT.
//
// Pointers are unusable across peers, and turn order is unusable as an index:
// NextTurn shuffles the turn list every round with an inlined Fisher-Yates
// (sub_140085F80) drawing from the simulation stream TLS+0x178. Deterministic,
// so lockstep-safe -- but a cat's position in it changes every round.
//
// The pre-shuffle source is the battle's character list:
//
//     TurnControl+0x18 -> +0x08 -> +0x20 -> +0x1F90
//         { u32 refcount@0; u32 pad@4; u32 cap@8; u32 count@12; Character** data@16 }
//
// (the same list Ability::TargetIsValid and Character::BeginTurn/EndTurn read).
// Its ORDER is spawn order -- authored, identical on both peers given the same
// save and RNG state.
//
// We snapshot it once at battle start and index into the snapshot. That is
// deliberately not a live lookup: a live index is not stable and the wire
// carries indices. A snapshot pins the numbering for the whole battle, and
// Character pointers are stable *within* one battle (they are only unstable
// across runs, which is what runs C/D measured -- 0 of 21 matched).
//
// REMOVALS AND ARRIVALS ARE NOT THE SAME PROBLEM, and treating them alike was a
// bug. A death removes an entry and shifts every later one, so a live index is
// worthless -- but membership is tracked by POINTER (snapshot_membership), so a
// departure costs nothing. An ARRIVAL only appends, so it can take the next free
// index without disturbing anybody -- and refusing it did real damage: a summon
// with a PLAYER brain had no index, so it fell outside the ownership model
// entirely. Each peer's own window drove it, the two sims diverged on it, and
// because the overlay asks the same index-based question, BOTH players saw its
// ability bar enabled. Reported from play 2026-09-21 with one peer eleven turns
// ahead of the other by the end of that battle.
//
// So arrivals are adopted at every turn boundary (adopt_new_cats), and their
// ownership is inherited from the actor of the applied action that produced them
// -- the same action on both peers, so the same owner, with no message and no
// renumbering. AI summons keep their old treatment exactly: nobody owns them and
// both peers re-derive them, which is why that case never needed anything.
//
// AND THE ORDER THE CATS ARE HANDED OUT IN IS NOT THE ROSTER'S. `split_for` gives
// each peer a contiguous range of the human cats; which cats those are is decided
// by `split_order_by_fp`, i.e. by each cat's ability fingerprint
// (mgmp_ability::character_fingerprint) rather than by where it sat in the list.
// Two peers who arranged the same cats differently therefore still split the same
// cats -- which is what a permutation needs.
//
// WHAT THAT DOES NOT FIX, and it is better said here than discovered later: the
// WIRE is still index-keyed. An ACTION says "cat 17", and cat 17 is a different
// animal on a permuted peer -- so decisions would land on the wrong cat and the
// turn hash would disagree. CONTROL now carries the fingerprints and says exactly
// which of the two situations a session is in (a permutation, which is fixable
// upstream by making the run's cat list identical, or a roster that has already
// diverged, which is not).
//
// CONTROL IS A SPLIT OF INPUT, NOT OF STATE. Both peers simulate every cat
// identically. `net_control` only says which cats local input may decide for.
// For every other cat this module overwrites GetChoice unconditionally, which
// is also what suppresses a stray click on a cat you do not own.
#pragma once

#include <cstdint>
#include "mgmp_proto.h"

namespace mgmp {

// Called once the transport reports Ready. Reads net_control from the config.
// Verifies and resolves the one game function this module CALLS
// (Character::get_affecting_elements). Called from hooks_install alongside
// catsync_set_base. Failure is non-fatal and only turns off element hashing.
void lockstep_set_base(uintptr_t base);

void lockstep_init();
void lockstep_shutdown();
bool lockstep_active();

// Drains the network queue and dispatches. Safe to call every frame; it is
// called from the frame hook so messages arrive even while no brain is polled.
void lockstep_pump();

// --- the seam ---------------------------------------------------------------

// Mirror of replay_fill_choice. Returns true if `out` was overwritten.
//
//   locally-controlled cat -> false, but the brain's own decision is captured
//                             and sent if it is a real one (type 2 or 3);
//   remote cat             -> true, always: either the peer's decision, or
//                             type=1 to keep waiting.
bool lockstep_fill_choice(void* brain, void* out);

// Called from the ApplyTurnAction hook, before the original runs. Clears the
// outstanding-send guard and validates that what landed is what we expected.
void lockstep_on_applied(const void* action, const void* actor);
// After Character::EndTurn: the owner's final facing of that cat (see apply_remote) is written again if the end of the turn changed it.
void lockstep_after_endturn(void* self);

// Called from the NextTurn hook. Snapshots the cat list on the first call of a
// battle, then exchanges and compares the turn hash.
void lockstep_turn_boundary(void* turn_control);

// DEBUG ACTION, in the same class as the panel's node jump: subtract `amount` from
// the hp of every ENEMY cat standing on board tile (tx, ty), and return how many
// were hit.
//
// It exists to make a battle end quickly while testing -- a TurnControl only
// exists inside a fight, so every session that needs one has to fight first.
//
// It writes ONE field, hp, and nothing else: no damage call, no death call, no
// animation. The game's own checks then do whatever it does with hp <= 0. That
// keeps the blast radius small, and it means the peers' hashes will disagree from
// the next turn (the write is not in either peer's action stream) -- which the log
// says at Warn, because a deliberate divergence that is not labelled reads exactly
// like a desync.
int lockstep_graze_enemy_on_tile(int32_t tx, int32_t ty, int32_t amount);

// ARM IT AND THEN CLICK A TARGET -- the two-step shape the game itself uses for
// every ability, and the reason is not stylistic.
//
// The one-step version asked the panel button to read "the tile under the mouse",
// which cannot work: to press a button in the panel the mouse has to BE on the
// panel, so at the moment of the click the game's hover -- StatusMenu+124, the only
// reading of "what is the player pointing at" that is not a reimplementation of the
// isometric projection -- describes the panel, not the enemy. Measured
// 2026-09-22: "nothing was standing there" every time.
//
// So arming remembers the amount and the next left click on the board is the
// target. The click still reaches the game (it is a real click), which is exactly
// what happens when a player clicks a target in the game's own flow.
void lockstep_arm_enemy_hit(int32_t amount);

// HANDED THE TURN CONTROL FROM THE NEXTTURN HOOK, ABOVE THE SESSION GATE -- same contract
// as mgmp_listprobe's and mgmp_roster's hand-offs, and for the same reason: a TurnControl
// is in hand in exactly one place, and a test aid that only worked inside a session would
// be useless in the one case it is wanted (a solo fight). See the definition for what it
// is used for -- the live character list, for lockstep_debug_hit_all when no battle has
// been snapshotted.
void lockstep_set_turn_control(void* turn_control);

// In a room: set the shared simulation stream from (battle, turn, actor) -- called by the NextTurn hook after the turn boundary (`actor` = null) and by the BeginTurn hook
// (`actor` = the character, `arg` its argument). The same call on every peer gives the same stream whatever was drawn before it. A no-op outside a room or a snapshotted battle.
void lockstep_reseed(const void* actor, int arg);

// The armed click's landing point: apply AND broadcast, so both peers write the
// same hp change. Local-only was the first version and it left the other peer
// fighting a battle this one had left -- see MSG_DEBUGHIT.
int  lockstep_debug_hit(int32_t tx, int32_t ty, int32_t amount);

// DEBUG: `amount` damage to EVERY enemy on the field, no targeting. Requested from
// play 2026-09-22 because a deathrattle can spawn enemies mid-battle and the
// arm-then-click tool cannot reach one: the click resolves a TILE against the
// roster, and a cat that appeared during the turn has no roster entry yet.
//
// It adopts mid-battle arrivals FIRST (adopt_new_cats -- the same call a turn
// boundary makes), then sweeps the roster and reuses lockstep_debug_hit per enemy,
// so every hit still broadcasts and the two peers write the same numbers. The
// session therefore stays readable, which the one-sided version cannot claim.
//
// Returns how many cats it hit. Players' cats and familiars are left alone.
int  lockstep_debug_hit_all(int32_t amount);

// A unit of the roster was replaced by another object in place (the game's transform, sub_1408D3C20, made by mgmp_spawntest's developer test): the roster entry that held `old_chr` now holds `new_chr`.
// No-op outside a snapshotted battle or when `old_chr` is not in the roster. board_apply does the same by hand for a unit it replaces.
void lockstep_roster_replace(const void* old_chr, void* new_chr);
// A turn-hash mismatch happened in this battle (debounced or halted): true once per battle, with a one-line summary for the upload. The menu offers the log upload.
bool lockstep_take_desync_notice(char* text, size_t cap);
// Why the battle halted (this peer's own halt, or the other player's), empty while it has not.
const char* lockstep_halt_reason();
// LAYER 2 EXPERIMENT (dev_tools only): arm the board's writing of a player's stats, and name one of this peer's own player cats to corrupt.
void lockstep_dev_arm_stat_repair();
// DEVELOPER BUTTON: halt the battle on purpose, exactly as a real desync would (the log record, the other peer told, the halt notice and the upload offer in the menu, the auto-finish of the fight). False
// when there is nothing to halt (no battle snapshotted in a session, or already halted).
bool lockstep_dev_force_halt();
const void* lockstep_dev_player_cat();

// Send the tail of this log to the other player(s), who write it into theirs (PEERLOG lines): so that the one log a player uploads holds both sides.
// Called on a desync, a halt and a failed checkpoint; rate-limited inside.
void lockstep_share_log(const char* why);
// One log line on how the battle that is over ended (who was standing, how many enemies were left). Once per battle; silent when there is none.
void lockstep_log_battle_summary(const char* why);

// A DEBUGHIT from a peer. Ignores one that names a different battle.
void lockstep_on_debug_hit(uint8_t from, const DebugHitMsg& m);

// 0 when nothing is armed. For the panel to show, and for the click handler.
int32_t lockstep_armed_enemy_hit();

// Called by the click handler once the hit has been attempted.
void lockstep_disarm_enemy_hit();

// --- what the cursor overlay needs ------------------------------------------

// This peer's battle counter, so a cosmetic message crossing a battle boundary
// can be dropped rather than drawn on the wrong board. 0 outside a battle.
// Which battle this peer is in: the node seed both peers read out of
// MapNode+0x118. kNoBattle when not in one. See mgmp_battleid.h.
uint64_t lockstep_battle_id();
// A chance roll whose odds are not the same on every peer (the coin a kill drops): `local` is what this peer's own roll gave. The host's answer is sent to the clients; a
// client waits briefly for it and returns it instead (its own on a timeout). Outside a room / a battle it returns `local`. site: kRollSite* (mgmp_proto.h).
bool lockstep_roll_resolve(uint8_t site, double chance, double luck, bool local);

// From the map layer, on BOTH peers, as a node is entered. Establishes battle
// identity without any negotiation -- both peers pass the same seed because
// they entered the same node.
void lockstep_enter_battle(uint64_t seed0);

// A peer joined or reconnected: replay this battle's decisions to it so it can
// fast-forward into a fight already under way. To that peer only.
void lockstep_catchup(uint8_t peer);

// True when the cat the game is currently asking for a decision is one this
// peer controls -- the "it is your turn" bit, and the thing peer cursors fade
// on. Refreshed at every GetChoice poll, which happens on essentially every
// frame of a battle, so it is never more than a frame stale.
//
// Deliberately NOT derived from the turn order: a cat's position in it is
// reshuffled every round by NextTurn's inlined Fisher-Yates, so the only stable
// answer comes from the roster index of whoever is actually being polled.
bool lockstep_local_actor();

// Does a peer -- specifically NOT this one -- own the input for this exact
// Character? Asked by the combat-menu lock, which has a Character* in hand
// (CombatMenu+280) and must not depend on who happens to be being polled.
//
// lockstep_local_actor answers "is the cat currently being asked for a decision
// mine", which is refreshed at a GetChoice poll and therefore holds its last
// value for as long as a decision stays cached -- fine for fading a cursor,
// wrong for a menu that is on screen across that whole window.
//
// Answers false for everything it cannot PROVE: no session, no snapshot, a cat
// that is not in the roster (a summon), or an AI cat. The caller is cosmetic,
// and a menu greyed out when it should not be reads as a bug in the game.
bool lockstep_peer_owns_character(const void* character);

// Diagnostic only: matches the upgrade subject against identities saved before Character teardown.
void lockstep_probe_roster_ids(const void* subject);
bool lockstep_cat_owner(uint64_t save_id, uint64_t seed, uint8_t& owner);
void lockstep_reset_cat_owners();

// IS THIS CAT THIS PEER'S TO DECIDE? -- and unlike lockstep_cat_owner above, THIS
// ONE WORKS ON THE MAP (2026-09-22).
//
// That difference is the whole reason it exists. lockstep_cat_owner requires
// `g.active && g.snapped && ...` -- a battle in progress -- while the layer that
// needs an ownership answer (mgmp_catsync) does almost all of its work BETWEEN
// nodes, on the map. The answer is not recomputed here: it is COPIED, once per
// battle, from the control split lockstep has just derived, and kept until a new
// run. See the cache in State.
//
// `mine` is only meaningful when this returns true. FALSE MEANS "NOBODY HAS TOLD ME
// YET" -- before this run's first battle, or for a cat that is not in the human
// roster at all (an AI cat, a familiar). Callers must treat that as "unknown" and
// keep whatever rule they had, never as "not mine".
bool lockstep_cat_is_mine(uint64_t save_id, bool& mine);

// --- OWNERSHIP, TOLD INSTEAD OF DERIVED (2026-09-23, 4+4 step B) --------------
//
// The automatic split is arithmetic over an ORDER (today: the ability-fingerprint
// order, so two peers who arranged the same cats differently still agree). What that
// order cannot express is that a cat is PLAYER 0's or PLAYER 1's -- it only knows WHAT
// each cat is, and any two players' cats have fingerprints like anyone else's. The 4+4
// shape needs the stronger fact, because it wants each player to drive its OWN cats
// rather than "half of a shared list" (measured 2026-09-23: with party 4 + familiars 4
// the split came out `peer 0:4 peer 1:4`, and the four handed to each peer were not
// chosen by ownership at all).
//
// So a module that KNOWS can say it: mgmp_catsync, which reads this peer's own cats out
// of its own save, notes an ABSOLUTE session position per cat id (0 = host, 1 = the
// second player). Both peers therefore end up with the same table, which is the
// property the split order depends on -- the positions are absolute, not "mine/theirs",
// so the two processes agree on them without a message per cat.
//
// This half only REMEMBERS and LOGS. The split still uses the fingerprint order until
// the ordering step lands, and that step will refuse to use this table unless it is
// COMPLETE for the battle (every human cat has an entry) -- a partially-populated table
// would order differently on the two peers, which is exactly the failure the whole
// mechanism exists to avoid.
void lockstep_note_owner(uint64_t save_id, uint8_t owner_pos);
bool lockstep_owner_pos(uint64_t save_id, uint8_t& owner_pos);

// --- journal persistence of the owner notes (2026-09-28) -----------------------
//
// The return-home settlement's ownership filter needs to know whose cat is
// whose WITHOUT a battle: a session resumed onto the map can walk an event
// node straight into the home node, and the battle split that filled the old
// check's table never runs (measured live 2026-09-28: both peers held the
// finalize with the game frozen at the door). The note table is the
// session-absolute truth, and the checkpoint journal now carries a copy of it
// with every staged entry; the restore path imports it back before the game
// loads the save. Version 1 journals predate the section and import nothing --
// the roster exchange notes the cats anyway, so old runs resume exactly as
// they did before.
struct LockstepOwnerNote { uint64_t save_id = 0; uint8_t owner_pos = 0; };
uint32_t lockstep_owner_note_export(LockstepOwnerNote* out, uint32_t max);
void     lockstep_owner_note_import(const LockstepOwnerNote* in, uint32_t count);

// --- diagnostics ------------------------------------------------------------

struct LockstepStats {
    uint32_t sent = 0, applied = 0, pending = 0;
    uint32_t desyncs = 0;
    uint32_t cats = 0, local_cats = 0;
    bool     halted = false;
};
LockstepStats lockstep_stats();

// True once a HASH mismatch or a HALT from the peer has stopped the battle.
bool lockstep_halted();

// Whether this peer is standing in a battle it has already snapshotted -- i.e.
// a fight is on screen and lockstep is driving it.
//
// The distinction that matters is against a peer which is merely CONNECTED
// while the other one fights: that peer has no snapshot, so it returns false
// and is safe to push state at. A peer for which this returns true has live
// Character objects whose state belongs to the deterministic stream, and
// writing to them out of band changes the battle underneath the hashes.
//
// mgmp_catsync and mgmp_invsync gate their apply paths on it. See the note on
// catsync_on_message for the shield that got through before they did.
bool lockstep_in_battle();
// The CURRENT battle (the one lockstep_enter_battle last named) has been built and its roster snapshotted: unlike lockstep_in_battle() this is false
// again the moment the next battle is entered, until that one is built.
bool lockstep_battle_ready();

// --- AND THE ONE SIGNAL THAT SURVIVES A HALT: THE BATTLE IS OVER (2026-09-23) ---------
//
// Between the victory screen and the level-up screen NOTHING presses a button, so the button
// detour cannot see that window; and lockstep_in_battle() stops being usable the moment a
// session has halted, because it is `g.active && g.snapped && !g.halted` and `halted` never
// clears. Both of those blocked the level-up work.
//
// The battle id registry does not have either problem. It already tracks `current` and marks
// ids retired, and ids are what the barrier and the stale-message checks run on. So "the
// battle we are in has been retired" is the fight-is-over moment, readable from anywhere, with
// no screen and no button involved -- which is exactly when the client's party has to be its
// own again, before the game draws which cat levels.
bool lockstep_battle_retired();

// AND THE ONE THAT ACTUALLY WORKS: WHEN THE TURNS STOP (2026-09-23, third try).
//
// Two signals were tried and both were wrong: lockstep_in_battle() (false for ever once a
// session has halted) and battle retirement (is_retired is about battles already BEHIND us, so
// asking it about the current one is false by construction -- the log proved it, the swap kept
// not being applied). Turns, on the other hand, are certain: every turn stamps this, and the
// level-up draw happens a few seconds after the last one, so a gap with no turn after turns
// have been seen IS the victory screen, with no screen, button or live session required.
uint64_t lockstep_last_turn_ms();

// AND WHAT SEPARATES "THINKING" FROM "FINISHED" (2026-09-23, fourth signal -- each one taught
// by a measurement). The heartbeat alone fires during a battle as well, because a pause while
// the player thinks is longer than the gap; and gating it on lockstep_in_battle() (the first
// attempt) blocked it entirely, because that reads true after a fight as well as during one.
//
// The character list is the thing that actually goes away: the probe file records that the
// roster watch "disarms when the holder is released", so at the victory screen the list no
// longer resolves. Heartbeat AND list-gone is the fight being over, with no screen, button or
// live session involved -- which is exactly the window the party swap has to land in.
bool lockstep_battle_list_gone();

// A fight is up RIGHT NOW: the roster was snapshotted and the character list it came from is still the live one. Unlike
// lockstep_in_battle() this survives a halt (a dropped peer halts the session), so it answers "was the player in a fight when the
// peer went away". False at the victory screen, in the warehouse and with no session ever armed.
bool lockstep_fight_up();
// The fight is won: live battle, it had enemies, none is left standing (plain reads). Earlier than the list going away -- the level-up's queries come in the frame of the last kill.
bool lockstep_enemies_all_down();

// A cat's health AS THE BATTLE HAS IT, found by the cat's own identity (CatData+0xC48, the id the
// run lists carry) rather than by roster position. Only while the fight is really up -- the same
// test debug_snapshot_ready makes: the snapshot's list is still the list the turn control resolves --
// because lockstep_in_battle() stays true after the last fight until the NEXT one re-snapshots, and
// the characters it remembers are freed by then. During a fight the cat's CatData still holds the
// health it had going in; the Character is where damage lands, and it lands there for BOTH peers'
// cats, so any peer can show any player's cat. False when no fight is up or the id is not in it.
bool lockstep_live_health(uint64_t save_id, int32_t& hp, int32_t& maxhp);

// The aim preview writes Character+0x388, and the backstab test reads it, so a
// wall-clock-gated presentation write decides damage. Call `begin` on entry to
// Brain::UpdateDecision and `end` on the way out: whatever the preview turned
// the cat to is put back, leaving facing written only by committed actions.
// Both are inert outside a net session. See the long note on the definitions.
// Everything mgmp_aim needs about one character, answered once: is it a HUMAN
// cat in this battle's roster at all, which index, and does the PEER own it.
// False for a summon, for an AI cat and outside a battle -- an aim preview is
// meaningless in all three cases.
bool lockstep_aim_subject(const void* character, uint8_t& cat, bool& peer_owns);

void     lockstep_preview_facing_begin(void* brain);
void     lockstep_preview_facing_end();
uint32_t lockstep_preview_facing_count();

// --- the state fence -------------------------------------------------------
//
// Snapshot every snapped cat's simulation state, run something, then check that
// none of it moved. Built for mgmp_aim, which calls a function INTO the game on
// the one peer that does not own the cat -- the shape where any state write is
// a divergence by construction.
//
// `end` puts FACING back, because that is the field the aim path is known to
// write and the one whose correct value is unambiguous (the last action's).
// Everything else it can only report: a moved HP or tile is already a
// divergence and inventing a value for it would hide that. Returns the number
// of cats that changed, and names the first few in the log.
//
// Cheap enough for a per-frame caller: one pass over the roster, no allocation,
// and it returns immediately when lockstep is not in a battle.
void     lockstep_state_fence_begin();
uint32_t lockstep_state_fence_end(const char* what);
uint32_t lockstep_state_fence_hits();

} // namespace mgmp
