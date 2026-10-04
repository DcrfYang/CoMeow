// mgmp_tuning.h -- the settings that are NOT in mgmp.json.
//
// Every constant here used to be a config key. They were removed from the file
// because a knob nobody turns is not a feature: it costs a parser branch, a
// line of documentation, a field in Config, and -- worst of the four -- it
// makes the config file long enough that the keys that DO matter stop being
// visible in it.
//
// The test for staying in mgmp.json was "has this ever actually been changed",
// and the evidence accepted was a flag in tools/net_test.ps1: role, address,
// port, cat split, the four diagnostic switches that script can flip, the
// recorder, and the panel. Nothing else passed, so nothing else is in the file.
//
// Changing one of these means editing this header and rebuilding, which for a
// mod that is rebuilt every session is the same gesture as editing a config
// file -- and it puts the switch next to the code it governs instead of in a
// file the code has to be trusted to read correctly.
#pragma once

#include <cstdint>

namespace mgmp {
namespace tune {

// --- the phase-1 text trace -------------------------------------------------

// Open a console window alongside the log file. OFF: everything it ever showed
// is now readable somewhere better -- the stamped file log keeps the whole run,
// the ImGui panel shows it live and filtered, and the failures that happen
// before the logger exists go to mgmp_boot.log, which the console never saw
// either. All it added was a second window to alt-tab past.
constexpr bool     kConsole      = false;
// Print raw pointers in trace lines. They differ between processes, so two
// peers' logs only diff cleanly with this off -- which is why it existed. In
// practice the diffing is done on the tagged lines, not the whole log.
constexpr bool     kPointers     = true;
// Bytes of TurnAction to hexdump when kTaRaw asks for one.
constexpr uint32_t kTaDump       = 64;
constexpr bool     kTaRaw        = false;
// Log every Brain::GetChoice poll, not just the ones that decided something.
// 1695 of 1711 calls in one battle return "nothing decided".
constexpr bool     kChoiceAll    = false;
// Frames between FRAME lines; 0 silences them. Effectively always 0 already:
// the frame hook is forced on by net_role and by the panel, and both silenced
// the logging when they did it, because they wanted the socket pumped and not
// a line every 60 frames.
constexpr uint32_t kFrameLogEvery = 0;

// --- phase 2: the recorder --------------------------------------------------

// Record draws from every RNG stream, not just the simulation one. On is the
// wrong setting and always was: the non-global draws turned out to be the
// interesting ones (they are how TLS+0x198 was found at all), and the volume
// that justified filtering never materialised.
constexpr bool     kRngGlobalOnly = false;
// Put EV_FRAME records in the stream. Only run D needed them -- a starved run
// has fewer frames by construction, so frames are excluded from the diff.
constexpr bool     kRecordFrames  = false;

// Which brains a decision may be injected into, and -- the load-bearing use --
// which cats count as HUMAN when the control split is derived. Substring match
// against the live RTTI class name.
//
// This is the one culled setting with a real consequence: it decides which cats
// the two peers divide between them. It is a constant because there has never
// been a second answer. If a summonable cat ever turns up with a PlayerBrain,
// this is where the exception goes -- see the summon note in the design notes.
constexpr const char* kReplayBrains = "PlayerBrain";

// --- the two hooks that CHANGE the game rather than watch it ----------------
//
// Both still announce themselves loudly in the startup banner while on, which
// is the property that mattered -- not that they were reachable from a file.

// No-op MewDirector::ApplySaveScumPenalty, so that reloading the same save to
// repeat a battle does not mutate cat state as a function of the reload count.
// This is a capture-methodology switch, not a cheat: with it live, run N and
// run N+1 start from different states by construction.
// ON: a multiplayer test session relaunches the game constantly for reasons
// that have nothing to do with the run -- a rebuilt DLL, a peer that dropped,
// a deliberate reconnect -- and Steven counts every one of them. The penalty
// was measuring our tooling, not the player.
// SINCE 2026-10-01 THE HOOK IS A PASS-THROUGH OUTSIDE MULTIPLAYER: single player keeps the penalty and
// Steven's warning exactly as shipped (h_SaveScum / h_ScumWarn test for a live session with another player).
constexpr bool     kHookSaveScum  = true;
// Convert TimeDelayStatusApplication's wall-clock countdown to a turn count.
// Never yet run against real content -- the one reachable user is AZ_LoseHead.
constexpr bool     kHookTimeDelay = false;
// Turn boundaries such a status waits before firing. 1 = the next boundary.
constexpr uint32_t kTimeDelayTurns = 1;

// --- the meta layer's sync modules ------------------------------------------
//
// Each of these was a switch whose documented purpose was "turn it off to tell
// a broken push apart from a broken assumption". That is a real debugging move
// and it is still available -- by editing the line, which is a rebuild rather
// than a relaunch. What it is NOT any more is something a stale config file in
// one peer's directory can silently disagree with the other peer about, and
// every one of them is a setting where the two peers disagreeing is worse than
// either value.

// --- TEMPORARY MEASUREMENT: who owns a level-up (2026-09-22) ----------------
//
// The design asks for the OWNER of a cat to choose that cat's upgrade, rather than
// the host choosing everything. That needs two things this project has never
// measured: which cat a level-up screen belongs to, and whether a battle Character
// can be turned into a cat id (the roster knows who controls what, but it is a list
// of Character pointers, while every other layer speaks ids).
//
// Set to false once the level-up screen names its cat and the roster's link to it is
// confirmed. See the probes in mgmp_choice.cpp and mgmp_lockstep.cpp.
constexpr bool     kLevelUpProbe = false;

// --- TEMPORARY MEASUREMENT: the run history at the node-hash sample point ----
//
// Measured 2026-09-22, proto 30, both peers: after a mid-run reconnect, at the FIRST
// node the client followed, the client's node-entry sample disagreed on `history`
// ONLY (NODEHASH !! MISMATCH ... the peers will roll DIFFERENT EVENTS), and the nodes
// after it agreed again.
//
// The obvious explanation is wrong, and the log says so: the client had already
// applied the whole batch (5 cats + the host's history, 768 bytes) at seq 213-220 and
// only sampled at 222. So either the two history OBJECTS really differ right after an
// apply that was supposed to overwrite, or something wrote to one of them in between.
// Those need different fixes, so both sides print their history's SIZE and HASH at the
// same points -- publish, apply, and the sample -- and the answer is in the pair.
//
// MEASURED 2026-09-22 16:0x, and the answer was "not again", in the useful sense:
//
//   host   PROBE after publish        929 bytes, hash a96d1303197d28de
//   host   PROBE node entry sample    929 bytes, hash a96d1303197d28de
//   client PROBE after apply          929 bytes, hash a96d1303197d28de   <- faithful overwrite
//   client PROBE node entry sample    929 bytes, hash a96d1303197d28de   <- nothing wrote between
//   both   NODEHASH ... AGREES
//
// So an apply DOES produce the same object, and nothing writes to it between the apply
// and the sample -- both candidate explanations for the one-node mismatch are out. That
// session differed in one way that matters: the reconnect RE-SENT THE CURRENT NODE
// (`FOLLOW -> re-sent the CURRENT node 5/21 (event) ... it joined after we entered it`),
// so the client's state was refreshed mid-run instead of being left as it was. The
// mismatch has therefore not been reproduced, and is now most likely a residue of the
// old "catch-up refused -> node NOT announced" path rather than a standing bug.
//
// KEPT, but back OFF: this is the DETECTOR for that case, at one extra serialize per node.
// Flip it on first if a node's `history` bucket ever disagrees again -- it prints both
// peers' byte counts and hashes at the sample points, which is what tells "different
// objects" apart from "the same object, sampled at different moments".
constexpr bool     kRunHistProbe = false;

// --- WHAT IS PER-PLAYER NOW (2026-09-22) ------------------------------------
//
// The design: each player's cats, stash (bags plus coins/food/boxes), upgrade
// choices and "?" event picks are their own; only the STORY events -- chapter,
// going home, quest items -- stay shared and host-decided. See
// DESIGN-independent.md for the boundary and the evidence behind it.
//
// INVSYNC IS THEREFORE OFF, and that one line is most of "per-player stash": the
// two peers are separate processes with separate Inventory objects, and the only
// reason their bags ever matched is that this module PUSHED the host's over the
// client's. Nothing replaces it, and nothing needs to. A peer that JOINS still
// starts from the host's save (mgmp_savefile writes it and redirects the load) --
// that is the one moment the bags are meant to be equal, and it is not this
// module's doing.
//
// A consequence to know before it surprises someone: a shop's stock is derived
// from the node seed, so both peers see the same shelf and EITHER can buy the
// same item, each into its own bag. That is per-player, not a bug -- and it is
// also why the node hash stopped comparing the inventory (see kNodeHashPerPlayer
// below and mgmp_nodehash's report()).
constexpr bool     kInvSync   = false;  // (was true) push the run inventory per map node

// THE SECOND QUESTION FOR mgmp_listprobe (2026-09-23), and it needs its own switch
// because it needs its own AIM.
//
// The probe's first mode watches the battle's character list, and it arms from a
// TurnControl -- which exists only inside a battle. The question now is WHO WRITES THE
// RUN'S PARTY VECTOR when the player leaves the House, i.e. exactly where there is no
// TurnControl and nothing to arm from. Same instrument, same debug registers, a
// different address: MewDirector+1468/+1472 (count and data pointer), which is the
// layer the design notes and the probe's own notes both end up pointing at as the one that
// decides.
//
// ON for this investigation. It observes only -- the handler logs, clears the trap and
// continues -- and its hits are identified by (rip, slot, watched address), so our own
// write to that vector (mgmp_roster's write_party) shows up as an mgmp.dll frame and is
// therefore easy to tell from the game's.
// OFF after the answer was had (2026-09-23): MODE 2's question -- who writes the run's party
// vector -- is answered and recorded, and while it runs it re-aims all four hardware slots every
// time that vector moves, which silently dismantled another instrument's breakpoints once (the
// log that caught it is quoted beside kCatSelectorProbe). Switch it back on only for the same
// question.
constexpr bool     kPartyProbe = false;

constexpr bool     kCatSync   = true;   // each peer publishes its OWN cats (both ways)

// Initial setup experiment: both peers keep the save they selected locally.
// The client then exports its four selected cats at the chapter barrier and
// the host replies with the runtime cats. The old host-save redirect remains
// available by turning this off.
constexpr bool     kLocalSetup = true;

// THE 4+4 IMPORT (2026-09-23), AND WHY IT IS A SWITCH.
//
// On, a peer reads `mgmp_import.bin` from next to its own mgmp.dll once per run and
// takes those cats into the run: the file is written by tools/save_dump.py --export
// and holds CatData blobs, exactly the bytes mgmp_catsync already deserializes.
//
// The point is the 4+4 shape: each player's OWN cats (from their OWN save) inside the
// one shared run. Until this exists, the client plays a copy of the HOST's save and
// therefore owns cats that are the host's, which is fine for 2+2 but cannot grow.
//
// It is a switch for one specific reason, stated rather than implied: it also relaxes
// the rule that only a CLIENT may create a cat it has never seen (see the gate in
// catsync_apply_snapshot). The host creating an unknown cat is deliberate here and
// wrong everywhere else -- a stray message must not populate the host's registry -- so
// the relaxation is tied to the feature that needs it and defaults to off.
constexpr bool     kPartyImport = true;  // read mgmp_import.bin into the run, once

// --- T1: THE PEER'S CATS GO TO THE FAMILIARS LIST INSTEAD OF THE PARTY (2026-09-23)
//
// The run's party is an exactly-four thing in the engine, not merely a four-slot
// allocation: six cats in it crash the map layer with an access violation at
// exe+0x99A792, identical on both peers, seconds after the write (measured twice
// tonight, and it is NOT the heap corruption of FEASIBILITY §10.3 -- that one is gone
// since write_party started using the engine's own append). So the shape being built
// is: each peer's own cats stay in the party, and the OTHER peer's cats ride in the
// familiars list -- which is the engine's own answer to "this cat cannot join the
// party" (0x929E4C: `lea rcx,[rdi+0x640]; call 0x48010`).
//
// ON: the import adds to the familiars list and never touches the party; the party
// reassert is skipped, because the imported ids are supposed to be absent from the
// party in this mode. OFF: exactly the old behaviour.
constexpr bool     kImportToFamiliars = true;

// --- STEP B: THE SPLIT ORDERED BY OWNERSHIP (2026-09-23) ---------------------------
//
// The split hands out a contiguous RANGE of an ordered list (host takes the front).
// Today the order is the ability-fingerprint order -- identical on both peers whatever
// the roster arrangement, which is why it works at all -- but it knows only WHAT a cat
// is, never WHOSE it is, so `peer 0:4 peer 1:4` did not mean "each player's own four".
//
// With lockstep_note_owner, the order can be ownership first, and then the range hands
// each player exactly its own cats -- provided the note table is COMPLETE for the battle
// and the notes are absolute positions (see lockstep_note_owner). That completeness is
// required, not optional: a table filled on one peer and not the other would order the
// two differently, and an order that differs is a split that differs.
//
// OFF returns the fingerprint order exactly, with one log line saying so.
constexpr bool     kOwnershipSplit = true;
constexpr bool     kChoice    = true;   // replicate event / level-up decisions
// OFF, AND THIS IS THE SWITCH THAT MAKES "?" EVENTS PER-PLAYER.
//
// The event roll reads the used-event list (the design notes, "the event roll is not
// pure"), so two peers with drifting lists roll DIFFERENT events from the same
// node seed -- which is exactly what independent "?" events MEANS. Pushing the
// host's list over the client's is what made them identical until now.
//
// IT GOES OFF ONLY TOGETHER WITH mgmp_choice's STORY/PER-PLAYER SPLIT, never
// before it: with the pools per-player the peers are looking at different
// screens, so replicating a random event's choice would apply a decision about
// an event the other peer is not looking at. The two are one change and are
// meant to be read as one.
//
// ONE ASSUMPTION, STATED BECAUSE THE RUN DEPENDS ON IT: the event the map shows
// after its FINAL BOSS is a story event (the user's rule) and is therefore shown
// to both peers whatever their pools say. If that ever fails, the symptom is a
// chapter-level divergence right after a boss -- and the fix is this one flag
// back to true, which restores the shared pool for the whole run.
// BACK ON, 2026-10-03: the "?" events are SHARED again (kPerPlayerNodes is off for good), but nothing pushed the used-event list any more, so the two saves' lists
// drifted and the same node rolled DIFFERENT events (host: Little John pushed onto rocks, client: Little John hungry) -- and an event's effects (add_weather
// RainingFrogs ...) reach the shared battle: a toad weather on one peer only was a halt. The host's list goes over the client's at every node entry, as before.
constexpr bool     kRunHist   = true;   // push *(MewDirector+1424), the used-event list
constexpr bool     kNodeHash  = true;   // the meta layer's per-node hash

// PRINT WHAT A NODE AND THE MAP LOOK LIKE AT EVERY FOLLOWED NODE ENTRY (2026-09-22).
//
// For the one question the "continue alone" work is still missing: a following client is
// TAKEN INTO the host's node -- the same event, the same shop -- but its OWN map does not
// advance, so a solo peer can walk back into nodes the host already consumed. The client
// really does call the game's own EnterNode for those entries, so what is missing is
// whatever marks "this node is done, the party is here".
//
// The client's BATTLE entries DO advance its map, so the same peer on the same map yields
// both cases with no extra setup: dump every followed entry, then compare a battle entry
// against an event entry. The field that differs is the one to write. Read-only; off by
// default because it prints a line per node.
//
// OFF AGAIN (2026-09-22, end of the map work). It was on for the diagnosis that found the
// two fields the map needed -- MapNode+0x168 (this node is done) and the marker's selection
// slot at *(MapScreen+0xA0)+0x60 (where the party icon is drawn) -- and the fixes for both
// are in. The code stays; switch it on when another field has to be found the same way
// (dump the window on BOTH peers at the same node and compare).
constexpr bool     kNodeProbe = false;

// TELL THIS PEER'S MAP THAT A FOLLOWED NODE IS DONE (2026-09-22).
//
// MapNode+0x168 is 0 while a node is un-consumed and 1 once the game has finished with it;
// measured on both peers at three nodes (hard, treasure, event): the host's copy flips
// 0 -> 1 every time, and the client's copy -- which follows into the same nodes -- stays 0
// forever. That is what "a following client's map never advances" means in memory, and it
// is why a solo peer walks back into nodes the host had already cleared.
//
// So the client sets the same dword the host's own code sets. One dword, only 0 -> 1, read
// back, and nothing else on the node is touched.
constexpr bool     kNodeConsumeMark = true;

// PER-PLAYER NODE CONTENT (2026-09-22): each player sees its OWN random "?" event,
// its own chest contents and its own shop stock, drawn from the same node seed.
//
// HOW, and it is the user's own suggestion: the CLIENT's simulation stream takes ONE
// EXTRA DRAW immediately after EnterNode has loaded the node's seed into it, so the
// two peers draw different values from the same starting point. Nothing is faked and
// no event is forged -- the game rolls what it rolls; the client simply rolls second.
// See follow_perturb_after_enter.
//
// WHAT IT MUST NEVER TOUCH: a battle. The fight is the shared, hashed simulation, so
// follow_perturb_after_enter refuses the battle node types (5 battle, 6 hard,
// 7 miniboss, 8 boss) and the stream is re-seeded by EnterNode at every node entry,
// which contains the perturbation to the node that made it.
//
// TURNED OFF 2026-09-22 17:0x, then BACK ON the same evening, and both halves of
// that story matter.
//
// OFF, because it worked and that broke the run: the peers really did draw different
// events, chests and shop stock (`perturbed ... node 9 (event)`, `node 18
// (treasure)`), but a node's effects are applied by the SHARED simulation to whatever
// cat it picked out of the SHARED roster -- so each peer's event modified the OTHER
// peer's cats, both peers published the same ids with different bytes, and the next
// battle halted at turn 0 with `8 of 14 cat(s) differ`. See DESIGN-independent.md R-B.
//
// BACK ON, because the rule that closes that hole is now enforced (see the section on
// "one cat, one editor" in the design notes): a peer publishes only the cats it edits, and
// never adopts the peer's copy of a cat it owns. With that in place, a node effect
// that lands on the other player's cat is simply not published by the wrong side --
// and the owner's copy wins at the next exchange.
//
// IT GOES ON AND OFF TOGETHER WITH mgmp_choice's per-player CHOICE BRANCH, which
// reads this same flag. Turning the content off while leaving the choice split live
// is what diverged a run at a chapter boundary on 2026-09-22: with both peers shown
// the SAME event, "each peer picks its own" is one event resolved two ways.
//
// OFF AGAIN, 2026-09-22 evening, for the mirror-image reason -- and the CONTENT half
// worked perfectly this time (`perturbed ... (event)` on the client, and the player
// saw the two events differ). What failed is the STORY/RANDOM TEST, because it reads
// the OPTIONS, and the options are exactly what stopped being shared:
//
//   * the classifier runs once per peer, on that peer's OWN event;
//   * so the two peers can classify the same node differently;
//   * the host's event looked story-ish here, so it PUBLISHED its choice, and the
//     client -- whose event was different -- injected that choice BY INDEX into its
//     own screen: it never saw its options and jumped straight to the resolution.
//
// THE LESSON, and it is the design note's own edge: a per-player decision may only be
// made from a signal the two peers still SHARE. Options no longer qualify. The NODE
// does -- the user's rule ("the event after the map's final boss is always a story
// event, everything else is random") is computable identically on both peers from the
// node type alone, which is what the classifier must be rebuilt on before this flag
// goes back on. See DESIGN-independent.md sec. 5.
// OFF FOR GOOD, 2026-09-22 late evening -- and this time the reason is not a bug in
// any of it, but a limit of the approach. THE THIRD AND DECISIVE MEASUREMENT:
//
//   turn 0 hash mismatch (state only): rng IDENTICAL, queue identical,
//   14 of 22 cat(s) differ, and every single difference is elem0/elem1 -- the
//   ElementList. Nothing was summoned or removed.
//
// The client's event ('Happening') put an elemental status on the whole board and the
// host's ('CatHole') did not. That is not a cat's stat that an ownership rule can
// arbitrate -- it is SHARED SIMULATION STATE attached to the fight, and 14 of the 22
// entries are not run cats at all. mgmp_choice.h predicted exactly this, in its own
// header, a day earlier:
//
//   weather_roll, add_weather, next_fight_from_set, next_event_from_set, ... were NOT
//   traced to a primitive ... If a desync ever shows up on an event node and the
//   choice indices matched, these are the first suspects.
//
// So: AN EVENT'S EFFECTS ARE NOT CONFINED TO THE PEER THAT DREW IT. Drawing different
// events per peer therefore cannot be made safe by ownership, replication or
// classification -- the effects include weather, elemental auras and the next fight's
// spawn queue, which both peers' battles read. The content half is off, and the
// per-player IDEA for "?" events does not survive in this form.
//
// WHAT IS KEPT, because it is useful and costs nothing: mgmp_choice now decides
// "shared event or not" from the event NAME (nodehash_peer_event_name) rather than
// from the options, and its log says which it found. With this flag off both peers
// see one event, so the two always agree -- and if that ever stops being true, the log
// says so on the same line.
constexpr bool     kPerPlayerNodes = false;
// Print the PER-PLAYER components of the node hash when they differ. Off, because
// they are now EXPECTED to differ and a line per node is noise nobody reads. The
// values still ride in every message -- this is how to see them on the day
// something else in the meta layer looks wrong and the stash is a suspect.
constexpr bool     kNodeHashPerPlayerReport = false;

// Whether a NODEHASH mismatch is fatal. Off, unlike the battle layer's halt:
// a battle desync makes every later turn fiction, a meta divergence is usually
// one number and a run that still plays.
constexpr bool     kNodeHashHalt = false;

// --- CARRIER PROBE (2026-09-22): what does an event actually WRITE? -----------
//
// It exists for one question, and the question is the whole reason per-player "?"
// events failed: an event's effects reach the SHARED simulation (weather, elemental
// auras, the next fight's spawn queue), so a peer that drew its own event diverges
// from one that did not -- measured, 14 of 22 cats differing in their ElementList
// with the rng identical, and the next battle halted at turn 0.
//
// mgmp_choice.h names the suspects it never traced: the familiar list at
// MewDirector+1576, the next-fight spawn queue at +1552, the adventure-token vector
// at +1664, and legacy counters in the save-cache at +56. This dumps all of them plus
// the familiar list's OTHER offset (+0x640, which mgmp_addresses.h uses and the choice
// header does not) at both sample points, on both peers -- so the diff says which
// carriers an event touched, instead of a guess deciding it.
//
// Raw words AND the first bytes behind a word that points somewhere readable: a list
// carrier's identity is in its TARGET, and its own word is a heap address that differs
// between the two processes by construction. A pointer that cannot be read is reported
// as such, never dereferenced blindly.
constexpr bool     kCarrierProbe = false;   // RELEASE: diagnostic dump at every node

// --- TEST AID: MAKE EVERY BATTLE SURVIVABLE (2026-09-22) --------------------
//
// ON at the user's request, and it is the answer to a real problem: this mod is
// being tested by PLAYING it, and a run that cannot get through its battles cannot
// reach the shop, the event or the next node -- which is where every current
// question lives.
//
// The debug-hit panel already does this by hand, and the difference matters: the
// panel edits ONE peer, so the two diverge by design and every log after it is
// worthless as evidence (the log says so itself). THIS rule runs on BOTH peers from
// the same inputs at the same turn, so the two stay in lockstep and the session
// remains readable.
//
// SET TO false TO PLAY PROPERLY AGAIN. (Or set it false once the battles in question
// have been dealt with -- it is a tune constant, so it needs a rebuild.)
constexpr bool     kTestWeakenEnemies = false;   // superseded by the runtime switch debug.test_weaken (needs ui.dev_tools)

// --- the battle layer's detection -------------------------------------------

// Include character HP/shield/tile in the per-turn hash. Off leaves the RNG
// stream and the queue. The hash gates itself on evidence anyway.
constexpr bool     kStateHash  = true;
// One line per cat whose state changed, at every turn boundary. This is the
// only thing that answers "when did the field last move, and by how much",
// which is the question a two-HP difference actually turns on.
constexpr bool     kStateTrace = true;

// --- peer cursors -----------------------------------------------------------
//
// Presentation. Nothing here is hashed, sent at a command boundary, or able to
// reach a decision, so these are the safest constants in the file.

constexpr bool     kCursors        = true;  // the board reticle
constexpr bool     kCursorGl       = true;  // the screen-space pointer
// One magenta arrow at screen centre with no peer and no session, to tell "our
// GL is broken" apart from "no peer position arrived". Both look like no arrow.
constexpr bool     kCursorGlTest   = false;
// Bounded trace of the pointer fraction at both ends, re-armed on every resize.
constexpr bool     kCursorTrace    = false;
constexpr uint32_t kCursorAlpha    = 100;   // the peer whose cat is deciding
constexpr uint32_t kCursorAlphaDim = 30;    // everybody else
// Ink height in pixels at kCursorRefH, scaled by the content rectangle so a
// bigger window draws a bigger cursor the way the game's own does.
constexpr uint32_t kCursorPx       = 34;
constexpr uint32_t kCursorRefH     = 720;

// DRAW THE PEER'S POINTER AS THE CURSOR THEIR GAME IS SHOWING, rather than
// always as the plain arrow. OFF while the reported drift is being isolated.
//
// The report (2026-08-28) was that the peer pointer is exact until the player
// starts AIMING, at which point it moves. `mode` is the only thing in
// draw_cursor that changes at that moment, so it is the only candidate, and the
// shipped art says why it could move anything at all:
//
//   state       ink box              ink h    vs default
//   default     ( 29, 2)-(108,108)   106      --
//   move        ( 29, 2)-(107,120)   118      x1.113
//   spell       ( 29, 2)-(111,123)   121      x1.142
//   attack      ( 29, 2)-(106,128)   126      x1.189
//
// Every aiming state shares default's ink ORIGIN and is 11-19% TALLER, because
// the badge hangs below the arrow. The sizing divided by that height, so the
// glyph changed size the moment the state changed -- measured from the shipped
// PNGs, not inferred.
//
// That defect is fixed independently (kCursorInkRefH below): every state is
// scaled by `default`'s ink height, so the glyph is the same size and sits in
// the same place whichever cursor the peer is showing. The one candidate the
// report had is therefore gone, and this is back ON -- while it was off, `mode`
// was read, published, transmitted and stored, and then thrown away one line
// into draw_cursor. A field that survives the whole pipeline and dies at the
// draw call looks exactly like a replication failure from every log line.
constexpr bool     kPeerCursorArt  = true;

// Sizing reference: `default`'s ink height, measured above. EVERY state is now
// scaled by this rather than by its own ink box, so which cursor a peer is
// showing can never change how big their pointer is or where it sits. The badge
// on an aiming cursor simply hangs below the arrow, which is what it does in
// the game.
constexpr float    kCursorInkRefH  = 106.0f;
// THE GAME'S FIXED CONTENT ASPECT, and the letterbox rectangle is derived from
// it rather than from the GL viewport.
//
// The viewport was the original source and it is not trustworthy: whether the
// value observed at swap time belongs to the game's fixed-aspect offscreen pass
// or to its full-window composite depends on which pass happened to be bound
// last. When it is the composite, the "content rectangle" collapses to the whole
// window and the letterbox correction silently disappears -- which is invisible
// while both peers run the SAME window size, because both then measure the
// pointer against the same wrong rectangle and the error cancels exactly. It
// only shows when the two aspects differ, and it shows as the peer pointer
// gaining speed on the short axis and straying into the black bars. Reported
// from the wild 2026-08-28, with "same resolution everything is good" as the
// tell.
//
// 16:9, from TWO independent readings. The shipped `swfs/ui.swf` declares a
// stage of 25600x14400 twips = 1280x720 exactly, which is what the whole UI is
// authored against; and the one recorded runtime measurement agrees -- a
// 958x1120 window rendered content 958x539 (958 * 9 / 16 = 538.9) with
// 290-pixel bars top and bottom ((1120 - 539) / 2 = 290.5).
//
// Derived arithmetic is identical on both peers and does not depend on GL state,
// which is exactly the property the viewport lacked. The binary confirms why it
// lacked it: the game's own cursor pass (sub_140A16B00 @ 0x140A16E05 and
// 0x140A17069) calls SDL_GetWindowSizeInPixels and then
// glViewport(0, 0, whole drawable) before drawing, so on any frame that pass
// runs last the viewport we observe at swap time is the full window.
//
// It is cross-checked rather than trusted: the overlay logs the game's own
// viewport beside the derived rectangle, so a wrong aspect is one line away
// from being visible instead of being a slow drift nobody can name.
constexpr int      kContentAspectW = 16;
constexpr int      kContentAspectH = 9;

// Exponential smoothing time constant. CURSOR is throttled to ~20 messages a
// second, so drawn raw the arrow reads as a peer with an unsteady hand. 60 ms
// is roughly one throttle interval.
constexpr uint32_t kCursorSmoothMs = 60;

// --- the combat menu lock ---------------------------------------------------

// Grey the ability bar out while it belongs to a cat a PEER controls, using the
// game's own disabled button state. Presentation only: it writes one int on a
// UI object after the game has computed it, and the clicks it stops were already
// being discarded at Brain::GetChoice. See mgmp_combatlock.h.
//
// Here rather than in mgmp.json by the same test as everything else in this
// file: there is no experiment that wants it off. The reason it is a constant at
// all is that it costs two hooks, one of them on Button::update -- so turning it
// off has to remove the hooks, not just the effect, which is why the config
// layer reads this rather than the module doing it.
constexpr bool     kCombatLock = true;

// --- the peer's aim preview -------------------------------------------------

// Draw the range / AOE tiles the OTHER player is currently aiming at, using the
// game's own Brain::DrawAbilityAOE. Presentation, and in the strictest sense:
// the call is made with the simulation stream saved and restored around it, so
// it cannot move the sim even if the fence pass missed a site. See mgmp_aim.h.
//
// A constant rather than a config key by the usual test: there is no experiment
// that wants it off. It is read once at init, so turning it off removes the
// publish and the draw, not just the effect.
constexpr bool     kAimPreview = true;

// --- RE instruments ---------------------------------------------------------

// HARDWARE WRITE WATCHPOINTS ON THE BATTLE'S CAT LIST.
//
// OFF, and it stays off: its question has been answered and written down. What
// the 2026-09-21 runs settled, in one place:
//
//   * the roster is TWO parallel tables on the scene object -- sub+0x20 holds
//     live containers, sub+0x18 holds the SOURCE entries they are rebuilt from,
//     both indexed by the same 505 (= 0x1F90/16);
//   * sub+0x18's entry 505 is written DURING a battle, by two different paths:
//     an out-lined append at rva 0x48093 reached from the ability chain
//     (0x7A2BA0+0x169F is where effects resolve), and a mutator at rva 0x96B9C1
//     inside fn 0x96B750 reached from combat maintenance via fn 0x9D5120;
//   * so the SOURCE decides who fights, the live list is a per-turn copy of it
//     (0x96B470 copies it through three per-candidate flag bytes), and a
//     per-battle choice belongs at that copy -- not in the encounter builder.
//
// Turn it on again when a new question about the list needs the same witness; the
// header of mgmp_listprobe.h lists the five ways it can lie to you while it runs
// (the worst of them, an in-trap re-arm the kernel silently discards, is there
// with the measurement that proved it).
//
// It only observes -- the handler logs, clears the trap and continues, and it
// writes to no game memory and no register the game owns. The cost is a handful
// of single-step traps per battle, on the one address range being watched.
// ON AGAIN, FOR A QUESTION ONLY IT CAN ANSWER (2026-09-23). Two peers built battles with 22 and
// 25 entities and the same node-entry hash, and the extra three were 3-hp AI creatures that
// neither peer's node state, cats, nor spawn queue accounted for. Its slots are free now (MODE 2
// and MODE 3 are both off), and the question is exactly the one it exists for: WHO APPENDS to the
// battle's character list -- a hit names the RIP and its .pdata caller chain, so the code that
// spawned the extra three gets named instead of guessed at.
constexpr bool     kRosterProbe = false;   // RELEASE: hardware write watchpoints are a diagnostic, off for players

// NAME EVERY BUTTON THE GAME UPDATES, ONCE EACH.
//
// Temporary, and it earns its keep. The setup flow a run begins with -- party,
// equipment, chapter -- is the one part of starting a game the mod does not
// touch, and the question about it is not "how would I hook these screens" but
// "which button COMMITS each one". Button+504 is the button's own name, the
// update detour already fires for every button in the game (it is how the
// client's Play and Quit To Menu presses are made), and names are how this
// project identifies screens -- MainMenu_Button_Play, Button_PauseMenu_QuitToMenu.
// So one line per unique name walks the whole flow out of the log, which is
// cheaper and far more reliable than guessing at screen classes.
//
// Its only cost is a string read per button per frame, returning on the first
// byte until the name is in a 256-entry table. Set it back to true when another
// screen needs naming.
//
// Back ON 2026-09-22 for one run, because the level-up probe found a screen whose
// tick never arrived and the fastest way to name an unidentified screen is the
// buttons it draws. Turn it off again once that screen has a name.
//
// WHAT THE 2026-09-21 WALK-THROUGH BOUGHT, kept here because the names are the
// finding and the callback column is what makes them usable:
//
//   setup flow, host side, in the order the player meets them
//     SaveFile_TerminateButton, EndDay_Sign, HouseButton, Depart_Sign
//     CatSelector_Left / CatSelector_Right                  <- the party
//     InventoryButton_{Collar,trinket,weapon,head,face,neck,modifier}
//     EquippedButton_{Collar,trinket,face,head}             <- the gear
//     Button_LowerDifficulty / Button_RaiseDifficulty, CloseButton
//     Map_Node, Map_Backpack                                <- already on the map
//
//   callback (Button+240 -> vtable slot 2), the function a press runs
//     CatSelector_Left/Right        rva 0xE9250 / 0xE91E0   distinct
//     Button_Lower/RaiseDifficulty  rva 0x6A260 / 0x6A0A0   distinct
//     ALL 11 gear buttons           rva 0x352810            ONE handler
//     EndDay_Sign AND Depart_Sign   rva 0x1F8EA0            ONE handler
//
// THE ONE-HANDLER COLUMNS ARE THE POINT. Gear and "leave the house" are generic
// handlers with the choice living in the std::function's capture, so there is no
// per-screen commit to hook and a per-click replication of the setup cannot be
// built that way. The run's identity is therefore synced the way everything else
// in this project is -- by the save, after the choices are IN it. See the gate in
// savefile_pump.
// ON for 2026-09-23's route A (the client equipping its familiars from the game's own
// screen). The probe already named this flow once -- the table above is its output --
// and the entry that matters here is "ALL 11 gear buttons -> rva 0x352810, ONE
// handler": the gear screen does not have a per-cat commit to hook, it has ONE generic
// handler whose choice lives in the std::function's capture. So the question route A
// asks is narrower than "rebuild the list": WHERE DOES THAT ONE HANDLER GET THE CAT IT
// IS EDITING FROM -- and can that be a familiar? The button probe is what names the
// buttons and callbacks on the screen this happens on.
constexpr bool     kLogButtons = false;   // RELEASE: names every button every frame -- a diagnostic

// --- MODE 3: THE CAT SELECTOR'S OBJECT (2026-09-23, route A) ------------------------
//
// The client cannot equip its familiars because the gear screen reads the run's party
// (measured -- see the disassembly note in mgmp_listprobe.cpp). What the screen does
// NOT do is read the MewDirector directly: it reaches the party vector through two
// pointers the button object holds at +8 and +0x10 (`mov r8d,[rcx+0x5BC]` for the count,
// `mov rdx,[r9+0x5C0]` for the data). If those two pointers can be aimed at the
// familiars, the client edits its own cats in the GAME'S OWN screen while the run stays
// untouched -- no rotation, nothing published, both peers free to edit at once.
//
// So this mode does one thing: it puts an EXECUTE breakpoint on CatSelector_Right
// (rva 0xE91E0) and prints the object that handler is handed, plus what its two
// pointers point at. It is armed from the tick like everything else here, and it is
// NOT a hot address -- it is a button callback, which is the difference between this
// and the append reader that hung the game (27,000 traps a second on a shared chunk).
// Hits are capped at 8 and say so.
// OFF, AND IT MUST STAY OFF UNTIL THE HANDLER SETS RF -- IT ALREADY TOOK THE GAME DOWN
// ONCE (2026-09-23).
//
// The capture it was written for SUCCEEDED, on the first hit, before the flood: the
// object is the button's own capture (rcx == the `cb=` address the button probe logs),
// `[rcx+8]` is a state object holding the selection index at +0x78, and `[rcx+0x10]` is
// the MewDirector -- its +0x5BC read 4 and its +0x5C0 pointed at the run's party array.
// That is everything route A needed, and it cost one click.
//
// What went wrong is the reason mgmp_listprobe's own header says an execute-breakpoint
// reader was REMOVED from that file: a #DB raised by an EXECUTE breakpoint re-fires
// immediately unless the handler sets the resume flag, so one click became 6,766,400
// traps, a 12 MB log, and a hung/crashed game. A write watchpoint does not need RF; an
// execute one cannot live without it. The flag is set false here rather than deleted so
// the mechanism is not re-enabled by accident -- and the handler now sets RF, so it can
// be turned back on deliberately.
// ON AGAIN, AIMED AT THE PANEL BUILDER INSTEAD (2026-09-23).
//
// The cat-selector address it used to carry is its own investigation and is not needed here.
// What is needed is the MECHANISM: a mode whose tick arms exactly the address it is told to,
// manages its own slots and heals them. The reason it matters tonight is a measured one --
// MODE 2 (tune::kPartyProbe) re-aims all four hardware slots every time the run's party
// vector moves, and the log shows it doing exactly that:
//
//   user exec breakpoint: slot[2] the option-template copier ... armed
//   slot[0] party.count ... slot[1] party.data ... slot[2] 2nd.count ... slot[3] 2nd.data
//
// i.e. the party investigation silently dismantled the panel-build breakpoints before the
// battle they were armed for. Only one mode runs per tick (this one returns early), so
// turning this on gives the panel observation the slots to itself.
//
// The address is the option-template copier (base+0x3D5F00), the function that fills the
// screen's option storage by copying 0x20-byte templates out of globals -- the panel's data.
// ONE breakpoint on purpose: the old append reader's 27,000-trap storm is what turned this
// off in the first place, and the difference here is that this function builds a screen
// rather than being a shared hot chunk. If the log ever shows it storming, the mode goes
// back off and the next attempt watches the screen's memory instead.
// OFF AGAIN, ITS JOB DONE (2026-09-23, later the same night). Aimed at the panel builder it found
// nothing (see the note at kRva_PanelBuilder in mgmp_listprobe.cpp for why: the screen object is
// reused, so nothing about the panel is built again). What DID find the layer was a write watch on
// the screen's own fields, and the answer -- the pending level-up's CatData copy, and the opener
// that runs before the panel is built -- is written up in the design notes. The mechanism stays here,
// switched off, because arming an address from a tick is exactly the instrument a question like
// that needs, and the previous paragraph explains when to switch it back on.
constexpr bool     kCatSelectorProbe = false;

// --- ROUTE A, TURNED ON: THE GEAR SCREEN ANSWERS WITH THE WHOLE RUN (2026-09-23) ----
//
// The capture above found the one pointer the gear screen reads its list through, so the
// client can reach its own cats in the game's own screen by aiming that pointer at a
// stand-in that answers with the run's whole cat list (party, then familiars). Nothing
// about the run changes -- no party write, nothing published, no rotation -- and both
// peers keep their own screen at the same time. See roster_catselect_redirect.
//
// ON, because it is the feature the client asked for and it is fully reversible: the
// pointer is put back the moment the screen closes. If the screen shows anything odd,
// this and mgmp_listprobe's kCatSelectorProbe are the two switches to turn off.
constexpr bool     kCatSelectRedirect = true;

// --- ROUTE A, RE-DONE WITH REAL STATE (2026-09-23) ---------------------------------
//
// The stand-in ABOVE worked -- the client really could reach its familiars in the game's
// own gear screen -- and it was wrong, because that screen WRITES through the pointer it
// reads its list from: the writes landed in our copy, which is where "infinite equipping"
// and gear that never appears came from. A stand-in has to be perfect to be safe.
//
// So: same screen, same recognition, REAL state. While this peer has that screen open,
// its party contents and its familiar contents trade places -- counts unchanged (checked
// to be four and four first), the run's list never grown, and nothing published for the
// duration, so the other peer cannot talk this one out of what the player is looking at.
//
// It only lives while the screen is open, on the map, and it is put back the moment the
// screen closes. That matters: a battle's roster is party-then-familiars IN ORDER, so two
// peers holding different lists would fight in different turn orders -- a real desync.
constexpr bool     kPartySwapWhileEditing = true;

// WRITE THE HOST'S NODE SEED ONTO THIS PEER'S NODE.
//
// OFF, and it stays off until the two peers can bring their own saves (the
// "each player's own cats" design). While both load the same save the maps are
// identical, the seeds are identical, and this switch would do nothing at all --
// which is exactly why it is not the default: a mechanism that cannot be observed
// doing anything is a mechanism nobody has seen work.
//
// What it is for: with two different saves the maps differ, the node at index N
// carries different 32 bytes of xoshiro256 state on each peer, and mgmp_follow
// today REFUSES that node with "the maps differ" -- a correct, loud failure.
// Turning this on makes it adopt the host's 32 bytes instead, so the battle rolls
// the host's stream. The map's STRUCTURE must still agree (node count, node
// types); that half cannot be patched one node at a time and belongs to the
// save-level merge.
//
// Every adoption is logged, with all four words, on the peer that adopted.
constexpr bool     kAdoptHostSeed = true;    // 2026-10-02: ON -- with the map seed sync it only fires for a node the whole-map sync did not cover

// HOW LONG A PEER WAITS FOR THE HOST'S DATA BEFORE GIVING UP (2026-10-02). Raised from 5 s / 3 s / 8 s after tests over the internet: a slow or
// lossy link (or a host that is still building) needs more than a LAN does, and giving up too early is the worse failure -- the peer then plays
// the board / level / map IT built, which is a desync. When a wait does run out the player is told (room_sync_trouble: "the client's connection
// to the host may have a problem, the games cannot be kept in sync"). The first two park the GAME THREAD (nothing is drawn meanwhile), so they are
// long enough for a bad link and short enough not to look like a crash.
//
// NOT RAISED, ON PURPOSE: the level-up owner waits and the event-name wait in mgmp_choice. They are also the NORMAL way out of screens that exist on
// one peer only (a shop purchase, a per-player event), so a longer bound is a longer dead click on every one of those, and a notice would be
// a false alarm.
// IN A ROOM THE SHARED SIMULATION STREAM IS RESET AT EVERY TURN AND EVERY ACTOR'S START (mgmp_lockstep: lockstep_reseed). Draws made in between differ between the peers for a thousand
// reasons (a per-save input, a frame-timed effect, a unit created with another roll), and one extra draw used to shift every roll after it for the rest of the battle. With the stream
// derived from (battle, turn, actor) a difference can only live until the next actor begins -- and the turn hash still says it happened. false = the game's own stream continues.
constexpr bool kReseedPerTurn = true;
// DEBOUNCED DESYNC (2026-10-04): a STATE-ONLY turn-hash mismatch (rng and queue agree) does not halt at once. The host's board of the next boundary repairs what
// the board carries (units, kinds, hit points, tiles, turn-order keys), so a unit that was a beat late on one side agrees again one boundary later. Only a state
// mismatch at two CONSECUTIVE boundaries, or any rng / queue mismatch, halts. Both peers decide from the same pair of hashes, so they decide the same. Every
// mismatch -- debounced or not -- is reported (DESYNC REPORT lines) and offers the log upload.
constexpr bool kDesyncDebounce = true;
// THE SIMULATION REVISION, mixed into the handshake's build hash (mgmp_session: hash_build). Raise it whenever a change alters what a battle computes (the board
// sync, the reseeds, what is hashed): two peers on different revisions then refuse to pair instead of halting in the first battle. No wire change.
// THE SAVES' OWN IDENTITY (2026-10-04): the handshake saves (mgmp_checkpoint) are stamped with the build identity too, and a file stamped with another one is "foreign" and unreadable. They must NOT
// follow kSimRevision: raising it for a battle-maths change made every handshake save of a run in progress unreadable ("the save combination is invalid", the run lost). This number is mixed into
// THEIR identity instead and changes only when what a SAVE holds changes meaning. It is 2 because the files written while kSimRevision was 2 are the oldest ones still in use.
constexpr uint32_t kCheckpointSimIdentity = 2;
constexpr uint32_t kSimRevision = 11;      // 11: the unlock-query digest leaves the save-property reads out (the game's own save reads them at times of its own); 10: the unlock answers also end when the fight's level-up screen is built (a normal win leaves objects standing); 9: the unlock-query record counts only the fight's own queries (no node entry, publishing, map or level-up reads); 8: the unlock answers end when every enemy is down (the level-up pool is built in that frame); 7: the property table is read at +8/+0x10 and the unlock record has the same gate on both peers; 6: the unlock answers end with the fight; 5: every save property in the host's whole table is answered with the host's in a window (proto 79); 4: a client in a battle draws ability pools from the HOST's class list (proto 78); 3: the board also repairs a player's cat
// LAYER 1 of repairing a player's cat (2026-10-04): the host's board overwrites a client's player cats' hit points, shield, tile and facing, and writes a cat the host has dead down to 0 hit
// points (the game's own death handling finishes it). NOT the max hit points, the seven stats, skills or gear: those are inputs the game recomputes (layer 2 is a live test, see
// mgmp_spawntest: the dev button that corrupts one of this peer's own cats' stats).
constexpr bool kBoardRepairPlayerCats = true;
// AFTER A HALT THE FIGHT IS FINISHED FOR THE PLAYERS (2026-10-04): once this peer has halted, every kHaltFinishEveryMs every enemy still standing is struck down (hp written to 0; the game's own death handling
// does the rest), on each peer by itself, until the battle is won. Without it a halted battle can only be left by quitting. Works in a release too: it does not depend on the developer tools.
constexpr bool     kHaltAutoFinish    = true;
constexpr uint32_t kHaltFinishEveryMs = 2000;
constexpr uint32_t kDebugHitWaitMs = 5000;   // a debug hit whose tile holds no enemy here yet (the host's enemy move is still being played on this peer) is held this long (mgmp_lockstep: debug_hits_pump)
constexpr bool     kBoardEveryTurn = true;    // the host publishes its board at EVERY turn boundary and a client takes it over (mgmp_lockstep: board_sync), not only at the battle start
constexpr uint32_t kBoardTurnWaitMs = 3000;   // how long a client waits for the host's board of a later turn before playing the turn without it
constexpr uint32_t kBoardWaitMs = 20000;    // the host's battle board and stream (mgmp_lockstep: board_sync)
constexpr uint32_t kLevelWaitMs = 12000;    // the host's level for this battle (mgmp_unlocks: unlocks_level_pick)
constexpr uint32_t kSeedWaitMs  = 20000;    // the host's map seeds / the clients' map flags (mgmp_follow: map_seed_sync)

// AIMED AT THE APPEND CHUNK BY EXECUTE BREAKPOINT -- ABANDONED, DO NOT REVIVE.
//
// It was tried once (2026-09-21) and it HUNG THE GAME. Two reasons, both worth
// keeping:
//
//   1. THE CHUNK IS SHARED. It is an outlined body that a hot unrelated path also
//      runs -- measured ~27,000 times a second while the game sat on the main
//      menu, ~90,000/s during loading. An instrument that fires per call is the
//      wrong instrument for it, whatever it reads.
//   2. AN EXECUTE BREAKPOINT IS A FAULT, not a trap. The instruction has not run
//      when the handler is entered, so resuming correctly depends on the resume
//      flag suppressing the immediate re-trigger -- and nothing here verified
//      that Windows sets it for a VEH continue on a hot address. The write
//      watchpoints never had this problem in thousands of hits: they trap AFTER
//      the store, so the condition has already passed.
//
// What survived it: `log_callers`, which unwinds the stack with the image's own
// .pdata instead of scanning for code-looking words, so the caller of a roster
// write is now exact. That answers the same question from the write side, where
// the trap is safe.
constexpr bool     kProbeAppend = false;

// --- the debug panel --------------------------------------------------------

// Lines the log pane keeps for scrollback. The ring behind it holds 4096.
constexpr uint32_t kUiLogLines = 2000;

} // namespace tune
} // namespace mgmp
