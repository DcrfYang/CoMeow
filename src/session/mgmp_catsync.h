#pragma once
// mgmp_catsync -- the host's cats are pushed to the client, as bytes the GAME
// produced, through the game's own serializer.
//
// THE BUG THIS CLOSES, measured 2026-08-24. Both peers loaded byte-identical
// saves (hash 78887bfc13cd5b04 on both). The host then equipped a trinket during
// the adventure. Four turns into the next battle:
//
//   !! HALT at turn 4: cat 19 slot 4:4 (gon 'tk_GlowingCoin') is empty on this peer
//
// A save file syncs the run at the moment it is written and nothing after it, so
// every meta-layer change since then is invisible. Worse, it is invisible to the
// detector too: `state_hash` covers HP, shield, max HP, tile, facing and dead --
// not abilities and not equipment -- so the peers agreed on every hash right up
// to the halt. What caught it was the ability cross-check (resolve by slot,
// validate by GON name), which is the one thing in the battle layer that looks
// at what a cat can DO rather than at what has happened to it.
//
// WHY BYTES AND NOT A REPLAYED ACTION. Replicating the click was the obvious
// plan, and the map layer's success makes it look easy. It is a dead end:
//
//   - glaiel::InventoryItemBox::click() @ 0x14034DF60 IS a clean boundary by the
//     usual test -- exactly one code caller, only data xref is .pdata, so not
//     virtual. But it is `void click(void)`: everything about what was clicked
//     lives in `this`, a UI box that exists only while the inventory screen is
//     open, and the client is not in that screen.
//   - it only STARTS a flow. Confirmation popup, then an AbilityChooser -- a
//     second decision point -- then lambdas whose actual mutation fans out over
//     ~8 CatData helpers. The bottom of that stack, sub_1400B3920, has 40 call
//     sites: a shared container primitive, the same trap as
//     CustomVector<T*>::push_back.
//
// So replaying means driving a UI flow on a peer that is not in it, and
// reimplementing means reproducing eight mutations exactly, where a single miss
// diverges SILENTLY. Both are worse than the halt they would replace.
//
// WHAT MAKES THE THIRD OPTION WORK:
//
//   void glaiel::SerializeCatData(CatData&, ByteStream&, bool)  @ 0x14022E9A0
//
// is BIDIRECTIONAL. Every field branches on the stream mode at ByteStream+0x00
// (0 = read, 1 = write), so the same function that saves a cat on the host loads
// one on the client. We reimplement nothing; we move bytes the game wrote into
// the game's own reader. The format even carries its own version tag (19) as its
// first field, and both peers run the same pinned build.
//
// Identity is free here, unlike everywhere else in this project. CatData+0x00 is
// a u64 id, serialized right after that version tag, and the run holds a
// registry that maps it to a CatData*. No index scheme, no name cross-check, no
// pointer that dies between runs.
//
// This is RUNSTATE from the design notes' protocol table, arriving early and scoped to
// cats.
//
// WHAT IT DOES NOT COVER, stated plainly: equipping MOVES an item from the run
// inventory onto the cat. Syncing CatData alone leaves the client with the item
// on the cat AND still in its inventory -- a duplicate. The run inventory is
// separate state and needs the same treatment; nothing here or in `state_hash`
// will notice. See "the inventory half" in the .cpp.
//
// --- WHAT CHANGED ON 2026-09-22: IT RUNS BOTH WAYS --------------------------
//
// Until now the host was the only writer: it published at its node entry and the
// client applied. The design asks for more -- each player may change what its OWN
// cats carry during a run -- and under the one-way rule the client's change was
// simply overwritten by the host's next push, which is a bug a player sees: the
// trinket goes back in the bag.
//
// Both peers now publish and both apply, and what keeps that from being two writers
// fighting over one cat is mgmp_split's rule one layer up: a cat has ONE editor, the
// peer whose input drives it. It is not enforced by a lookup here -- the split is
// over battle roster SLOTS, and this module works in cat IDs -- but by the BASELINE
// the two peers keep per cat (see the State comment in the .cpp): a peer publishes a
// cat only when its bytes differ from the value the two last agreed on, and agreeing
// is both "I sent this" and "I applied this". A client with no baseline for a cat --
// one the host has never sent it -- stays silent, and that is what stops a client's
// copy of a run it does not own from reverting the host's edits.
//
// WHAT IS STILL ONE-WAY, and the honest consequence: the INVENTORY (mgmp_invsync).
// Equipping moves an item out of the shared bag and onto the cat, and only the cat
// half travels back, so after a client equips, the item is on the cat on both peers
// AND still in the bag on both. That is a duplicate rather than a divergence, which
// is the saving grace: the two peers hold the SAME bag, so no hash disagrees, the
// node hash stays quiet, and no battle halts -- but the run has a spare copy of the
// item. The set is right; the cost is the spare. Closing it means mgmp_invsync going
// both ways under a merge rule, and an item's only portable identity is its GON name
// plus its stat fields -- see the reasoning in mgmp_invsync.h.

#include <cstdint>

// THE PER-CAT SERIALIZER, IN ONE CALL (2026-09-23). Defined in mgmp_catsync.cpp at GLOBAL
// scope -- the linker names it ?serialize_cat@@YAIPEAXPEAPEAE@Z, with no namespace prefix
// -- so the declaration belongs HERE, outside `namespace mgmp`. A declaration written
// inside that namespace binds to mgmp::serialize_cat and never resolves, which is exactly
// how the first attempt at using it from mgmp_choice.cpp failed to link.
//
// Why anyone else wants it: this is the bytes SerializeCatData produces for one cat, i.e.
// the very image mgmp_catsync compares between the peers. It is the only way to see a
// change that lives BEHIND A POINTER (a replaced passive ability, for one) -- hashing the
// CatData object's own bytes cannot see those, measured 2026-09-23.
// (Declarations moved INSIDE namespace mgmp on 2026-09-23 -- see below. A declaration at global
//  scope mangles differently from a definition at mgmp scope, which is the LNK2019 that had been
//  sitting unexplored in this file's history.)

// WRITE A CAT IMAGE OVER AN ARBITRARY CatData (2026-09-23).
//
// The same bidirectional serializer, in read mode, against whatever address is handed in --
// which is not always a cat of the run. It exists for the level-up path: the game's pending
// level-up carries a CatData COPY (the id at CatData+0xC48 was found inside the object the
// opener is passed -- see the OPENER scan in mgmp_choice), and the panel, the stat numbers and
// the four options are all built from that copy. Writing THIS peer's cat into the copy, before
// the screen is built, is what makes the three of them the game's own work around this peer's
// cat instead of the shared draw's.
//
// `len` is bounds-checked against kMaxCatBytes and the read position must land exactly on it,
// because a short read would leave the destination half-overwritten: the same two checks the
// cat-sync apply path makes, for the same reason.
bool catsync_deserialize_into(void* cat, const uint8_t* image, uint32_t len);

namespace mgmp {

struct CatDataMsg;
struct CatDigestMsg;
struct CatBrief;
struct SetupMsg;

// Take this cat's image into a caller-owned buffer / lay an image over an arbitrary CatData.
// (See the two notes above for what they are for and why the declarations live here rather than
// at global scope.)
uint32_t serialize_cat(void* cat, uint8_t** out);
bool     catsync_deserialize_into(void* cat, const uint8_t* image, uint32_t len);
// The cats THIS player brings, as room-panel briefs (mgmp_catbrief.h): every cat of the run
// (party AND familiars -- the mod swaps and replaces those lists, so which list a cat sits in says
// nothing about whose it is), each serialized by the game, read by mgmp_catblob.h, its maximum health
// asked of the game itself (CatData::max_health), then kept only when it is this peer's by IDENTITY:
//   - a session id (0x70000000 + owner<<24 + n) names its owner;
//   - otherwise the ownership notes lockstep holds;
//   - otherwise, once the run is a shared one (it holds session ids), the cat is the HOST's -- the
//     authority for a cat nobody claimed, e.g. one an event just added;
//   - before there is any shared run, the cat is simply this player's own pick.
// Returns false when the run's cats cannot be read right now (the caller keeps what it had);
// otherwise true with `n` filled, at most `max`, in a stable order. Read-only, needs no session.
bool catsync_local_briefs(CatBrief* out, uint32_t max, uint32_t& n);

// Has the game marked this cat dead for good (corpse destroyed, or the `kill` event)? Read off its CatData; false when it cannot be
// looked up. See kCatData_Killed in mgmp_addresses.h.
bool catsync_cat_perished(uint64_t id);

// Is there a cat with this id in the game's registry? (A cat gained during the run -- the gain_cat_familiar event -- is one.)
bool catsync_cat_exists(uint64_t id);

// FNV-1a of a cat's serialized image -- the hash CATDATA carries; 0 when it cannot be serialized. For logs.
uint64_t catsync_image_hash(void* cat);
// Capture the four cats currently selected in this peer's local setup. The
// returned images are owned by the caller and must be freed with free().
bool catsync_export_party_setup(SetupMsg& out);
// Read existing session IDs from BOTH saved lists; never clone or renumber on resume.
bool catsync_export_resume(SetupMsg& out, uint8_t owner);
void catsync_setup_sent(const SetupMsg& snapshot);
// Transactionally isolate owned cats immediately before native home settlement.
bool catsync_prepare_settlement(void* director);
// RETURN A RUN'S CLONES TO THE CATS THEY WERE MADE FROM (2026-10-01, review finding F1).
// A player's selected cats are cloned into session cats for the run (0x70000000 + owner<<24 + n); the game flags
// the ORIGINALS "on adventure" at departure and settles only the clones. This finds this peer's SETTLED clones
// (flags non-zero, not on adventure), pairs each with the one ordinary cat that has the same seed (the original --
// the clone is a byte copy), writes the clone's image over it (id kept; flags, level, equipment, injuries as the
// run left them) and retires the clone (flags 0, the state every leftover clone is already in). An original that is
// not on adventure, a seed that is not unique, or a write that fails leaves everything as it was. Called right
// after the native settlement and, as a safety net for saves the old behaviour left behind, before a new run's
// clones are made. Returns how many clones were merged.
unsigned catsync_merge_session_cats(const char* why);

// The House is about to write its state (T_HouseSave; `house` = the writer's first argument). Every House cat entity whose id is a clone's that the merge swapped into its original
// gets the ORIGINAL's id, so `house_state` names the cats that came home -- see swap_identity.
void catsync_house_save(void* house);

// THE SAVE THE NATIVE SETTLEMENT WRITES IS NOT THE ONE THAT COUNTS (2026-10-03). MewDirector::SaveGame is the settlement's own last step, and it runs BEFORE the clone merge: the file on disk then
// holds the settled CLONES under their session ids, the originals still "on adventure", and a house_state naming the clones. A player who reloads before anything saves again got the
// un-retired originals back. After a merge that changed anything, the game's own SaveGame is called once more (the House state writer hook puts the House's ids right inside it).
bool catsync_save_game(const char* why);

// WHICH ORIGINAL A CLONE CAME FROM, REMEMBERED (2026-10-02). The merge used to find an original by its seed among cats flagged "out on
// adventure" -- fine from the warehouse, but a run resumed from a handshake save may no longer offer that evidence. The setup now
// records clone -> original, the journal keeps the record (disk version 3), a restore reads it back, and the merge trusts it first
// (after checking the seed still matches); the seed search stays as the fallback.
struct CloneOriginNote { uint64_t clone = 0; uint64_t original = 0; };
void     catsync_note_origin(uint64_t clone, uint64_t original);
uint64_t catsync_origin_of(uint64_t clone);                    // 0 when not recorded
uint32_t catsync_origin_export(CloneOriginNote* out, uint32_t max);
void     catsync_origin_import(const CloneOriginNote* in, uint32_t count);
void     catsync_origin_clear();                                   // tests: the table as it is on a fresh process
// ORIGINALS THE OLD BEHAVIOUR LEFT "ON ADVENTURE" (2026-10-01). Before the merge existed, a joint run's originals kept
// the on-adventure bit for good and their progress went to clones that were later overwritten -- the user's saves hold
// seven or eight such cats, invisible in the House. When no run of this player is in progress (the chapter page of a
// fresh setup, and right after a run ended) every ordinary cat still flagged on adventure that is not in the run's own
// party is such a leftover: the bit is cleared and the cat is home again. Returns how many.
unsigned catsync_release_stuck_cats(const char* why);
// Make the selected four cats independent of the other save.  The returned
// setup message uses deterministic session IDs and the local party is changed
// to those IDs before the message is sent.
bool catsync_prepare_party_setup(SetupMsg& out, uint8_t owner_pos);

// Resolves and prologue-checks the game functions this module calls. Called
// from hooks_verify_module alongside rng_set_base, because a bad address here
// is a refusal at startup rather than a crash three screens later.
void catsync_set_base(uintptr_t base);

void catsync_init();
void catsync_shutdown();

// EITHER PEER: serialize every cat in the run and send the ones whose bytes differ
// from the value the two peers last agreed on. The host calls it where it enters a
// map node -- the point both peers already synchronize on, and the last moment
// before a battle can be built out of a stale cat. The client calls it from the map
// tick (see catsync_map_tick), which is what lets an equip made on this peer's own
// cat reach the host in time to be in the next battle. Cheap and idempotent:
// unchanged cats cost a serialize and a hash compare, and send nothing.
//
// `quiet_if_nothing` is for the caller that runs often: the map tick would otherwise
// repeat "0 sent, 0 unchanged" once a second forever. A node entry passes false, and
// that matters more than it looks -- "the publish ran and had NOTHING to send" is the
// evidence that a cat this peer applied earlier was not sent back, which is the whole
// of the no-revert property. Suppressing it made that proof unobservable.
bool catsync_publish(const char* why, bool quiet_if_nothing = false, bool force = false);

// HOST: the same push, to ONE peer -- the join catch-up, and nothing else. The
// rest of the session expects a broadcast. A refusal (cats held, no run, a cat
// that would not serialize) is reported to the caller rather than only logged,
// because the catch-up makes the node message that follows it conditional on
// this having arrived.
bool catsync_publish_to(uint8_t peer, const char* why, bool force = false);
bool catsync_apply_snapshot(const CatDataMsg& m, const char* why, bool allow_create = false);
struct EnterNodeMsg;
bool catsync_read_familiars(EnterNodeMsg& node);
bool catsync_apply_familiars(const EnterNodeMsg& node);

// Diagnose side: print the run's party list and the familiar list BY ID, at the moment the host
// is about to publish a node. Written 2026-09-23 because the client kept receiving four cats
// that were all its own -- which means the host's publish has nothing else to carry, and the
// only way to say which list holds whose cats on a live run is to look. Log only.
void catsync_dump_lists(const char* why);
// The party, mirror and familiar vectors as raw {cap, count, pointer, ids}. Log only.
void catsync_dump_vectors(const char* why);

// EITHER PEER: forget the baseline, so the next catsync_publish is a FULL push.
// For a reconnecting peer -- see the comment on the definition. Both peers need it,
// because after a reconnect the OTHER one has none of what this one already sent.
void catsync_forget();

// EITHER PEER: resolve m.id in this peer's run and deserialize over it.
//
// A cat that arrives while this peer is in a battle is HELD, not applied and
// not dropped -- writing it would edit state the fight derives from outside the
// hashed stream, and dropping it loses it for good, because the sender dedupes
// against the last hash it sent. Both peers hold now; the drains are the client's
// map-follow tick and the host's node entry.
void catsync_on_message(const CatDataMsg& m);

// EITHER PEER: apply every held cat, if any. The client calls it from the
// map-follow tick, immediately before this peer enters the node the host entered --
// the same point invsync_apply_pending uses, and for the same two reasons: it is off
// the battle screen, and it is still in time for whatever the node opens. The HOST
// calls it at its own node entry, immediately before the publish below it, which is
// the same two properties from the other side: no battle has been built yet, and
// what it just applied is in the state that follows.
void catsync_apply_pending(const char* why);

// CLIENT: the throttled publish on the map tick. The host's edits reach this peer at
// the node boundary; this peer's own edits have to travel the other way, and waiting
// for the next node the host enters is too late for THAT node's battle -- so it
// publishes while the map is up. No-op on the host, whose node-entry publish already
// covers the same ground.
void catsync_map_tick();

// --- the per-cat digest and owner-authoritative healing (2026-09-29, proto 43) ---
//
// Every peer's map tick broadcasts CATDIGEST: (id, hash, size) for every cat in its
// run, about once a second and immediately when a hash changes. The host relays it,
// so every peer sees every other peer's digest.
//
// Stored here, COMPARED on this peer's own map tick (never inside a battle). For a
// cat whose bytes differ, the cat's OWNER is the authority -- lockstep_owner_pos,
// else the control split, else the host. The authority re-sends its copy (to that
// peer when it has a direct link, which the host does; broadcast otherwise, which
// the host relays). A difference has to be seen in two consecutive digests with the
// same pair of hashes first, so a push that is merely in flight is not answered.
// Four quick attempts, then one every ~16 digests with an Error line: a copy that
// will not converge is reported and retried, never a stopped run. A peer that is not
// the authority only logs the difference; the first time a cat differs, each peer
// writes its own copy's bytes next to the log (mgmp_catdump_*.bin) for diffing.
void catsync_on_digest(uint8_t from, const CatDigestMsg& m);

} // namespace mgmp
