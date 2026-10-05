// mgmp_proto.h -- the wire format. Phase 4, layer 2.
//
// WHAT GOES ON THE WIRE, AND WHY IT IS SO LITTLE.
//
// Only TurnAction types 2 (use ability) and 3 (end turn) are ever transmitted.
// Types 6 (reaction broadcast) and 7 (invoke a std::function) are generated
// locally on both peers and must NOT be sent -- settled exhaustively in phase 1:
//
//   - ApplyTurnAction has exactly two call sites and is not virtual, so the
//     decision queue is the sole route in;
//   - all ~65 type-7 creation sites are passive/status/reaction callbacks
//     (OnAppliedToCharacter, OnReceivedDamage, CheckCounter, late_update, ...)
//     and not one of them is a brain;
//   - type 6 bypasses the queue entirely -- NextTurn calls ApplyTurnAction
//     directly at 0x1408E111E once per turn.
//
// That is what makes this protocol small enough to be a few hundred bytes a
// turn: a std::function cannot go on a socket, and it never has to.
//
// IDENTITY IS AUTHORED DATA, NOT POINTERS. Measured across runs C and D of the
// same battle, 0 of 21 ability pointers and 0 of 21 actor pointers matched. An
// ACTION therefore carries the ability's *slot* (the game's own FindAbility
// scheme: move/attack/bonus/spellN) plus its authored GON name. The receiver
// resolves by slot and validates by name -- and a disagreement between the two
// is itself a desync signal, caught before the wrong ability is ever played.
// See mgmp_ability.h for why the slot index is stable by construction.
//
// FRAMING. Length-prefixed, little-endian, u32 payload length not counting the
// length field itself. TCP is deliberate: head-of-line blocking is irrelevant
// when the game is turn-based (you are blocked on that message anyway), and a
// turn is a few hundred bytes you want reliable and ordered.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "mgmp_catbrief.h"

namespace mgmp {

// Bumped on any incompatible change to the encodings below -- AND on any change
// to what a peer COMPUTES from them, which is a wider contract than it sounds
// and was widened the hard way.
//
// Version 8 carries no new message and no changed field. What changed is the
// state hash: facing came out of it. Two peers running builds 7 and 8 handshake
// happily, agree on rng and queue, and then halt on turn 0 with a state-only
// mismatch -- which reads exactly like a game desync and is not one. The peer's
// hash in that halt was byte-identical to the value BOTH peers had agreed on in
// the previous session, which is the tell, and it cost a session to spot.
//
// So: if the value a peer puts in a message changes meaning, that is a protocol
// change even when every byte stays where it was. Refusing at connect is free;
// diagnosing it from a turn-0 halt is not.
// 28: CONTROL carries a 32-bit ability fingerprint per claimed cat -- what the cat
// IS, rather than where it sat in a list two peers can order differently. The
// older fields are untouched and a peer that ignored the new one would read
// `cats` exactly as before, which is precisely why this is a version bump: what a
// peer COMPUTES from the message changed with it.
// 29: CHOICE appends cat identity and node-local level step for owner-driven upgrades; both peers must upgrade.
// 30: node-bound state batches and ENTERNODE cat familiar membership; node hashes include familiars.
// 32: node-scoped local rosters and companion-list replacement must agree on both peers.
// 33: bootstrap validated shared 4+4 ownership before first battle; idempotent familiar imports.
// 34: pending host CATDATA no longer starves established clients publishing their own upgrades.
// 35: local-save setup exchange carries the client's selected cat images before the chapter barrier.
// 38: per-player durable checkpoint certificates and coordinated recovery.
// 46: CATS -- the cats a player is bringing (name, level, health, class, colours) for the room
//     panel, beside 45's PAGE. Cosmetic like it; the bump is only so an older peer's
//     unknown message id is never the way this is found out.
// 47: the handshake queue is FOUR confirmed saves deep (CHECKPOINT offers carry four candidates instead of
//     two) and SAVEWAIT -- the host tells everyone who has picked a save and which stage the pick is in.
// 39: fresh sessions pre-sync the host's serialized chapter_map row into the client's
//     save before load (departure-ready saves carry two different generated maps,
//     which the first node boundary refused to certify).
constexpr uint32_t kProtoVersion = 82;  // 82 MSG_SETTING (the host's combat speed, which every other player's game then uses), 81 MSG_CHAT (a line of text typed by one player, shown on every screen), 80 MSG_RNGL (each peer's per-action ledger of the draws it made on the shared stream, by call site) and MSG_DEEP (each peer's per-turn digest of every unit's derived values: stats, speed, turn-order keys), 2 CONTROL, 3 ENTERNODE, 4 SAVEFILE, 5 epoch, 6 CATDATA, 7 INVENTORY, 8 state hash drops facing + roster cap 254, 9 peer envelope + PEERS (up to 4 players), 10 state hash gains ElementList (tiles + equipment), 11 CURSOR, 12 state hash gains live-list membership and stops hashing departed cats, 13/14/15 CURSOR churn while the pointer moved from the board to the screen, 16 CURSOR carries the cursor-art index -- the peer's pointer is now the game's own texture for the state their game is in, 17 CHOICE -- event and level-up option choices, which is what replaces RUNSTATE, 18 the per-battle epoch COUNTER becomes a u64 battle_id -- the node seed both peers already share -- so battle identity survives a peer restarting, 19 CHOICE carries the node seed it was made on -- a held choice used to have no idea which node it belonged to and could surface on a later one, 20 RUNHIST (the run-history object the event roller reads) + NODEHASH (the meta layer finally gets the per-node check the battle layer has had per turn since version 5), 21 AIM -- the range/AOE tiles the other player is aiming at, drawn on this peer by the game's own Brain::DrawAbilityAOE; cosmetic like CURSOR and hashed by nothing, 22 AIM is read from the PLAYERBRAIN'S SELECTION (PlayerBrain+0x3D8/+0x358/+0x360) instead of the cached decision, and the receiver draws the RANGE tiles as well as the AOE -- not one byte of AimMsg moved, which is exactly the kind of change version 12 established has to bump anyway, 23 the range-tile call that came with 22 is REMOVED -- sub_140138A10 applies statuses rather than drawing, so mirroring it mutated the non-owning peer's simulation and cost a run; the bump exists so a peer still running 22 cannot join and do it, 24 the highlight is back and the tiles with it, but only ever called with sub_140151CE0 -- its apply_status half -- swallowed by T_HighlightRefresh and with the whole roster's cat state fenced across the call, 25 a Move aim also shows the ATTACK RANGE from the hovered square -- the game gets those tiles by displacing the cat and moving it back, so the receiver now makes two TacticsObject::Move calls per frame inside the same state fence; not one byte of AimMsg moved, version 12's rule again, 26 STATEDUMP -- on a hash mismatch each peer sends the per-cat table its own hash was taken over, so the log that halts also names the cat and the field instead of requiring two log files side by side, 27 SAVEFILE carries `fresh` -- whether the host is (re)starting a run from the save screen or catching a peer up. A host that goes back to the menu and picks a slot again starts a NEW run, and the client used to decline that save with "already in the run": the g.applied latch is per-PROCESS and the two sends were byte-identical, so nothing on the receiving side could tell them apart, and HOSTLEFT -- the other half of that story: `fresh` handles the host STARTING a run, HOSTLEFT handles it ENDING one. Both shipped in the same unreleased version, which is the only reason they share a number, 58 UNLOCKS carries the locked_bosses list too, 59 UNLOCKS carries the order the host's battle will spawn its party in, 60 UNLOCKS carries the level the host's battle picked, 61 BOARD carries the host's battle board and simulation stream, 62 UNLOCKS carries the host's queue of returning enemies, 63 ... and the other two queues of the director the battle build consumes, 64 ... and its list of active global modifiers (weather), 65 the modifiers move to MSG_WEATHER (weather only), 66 MSG_WEATHER carries the director's queued battle-start spawns (name + number), 67 MSG_WEATHER is gone: UNLOCKS carries the director's weather names with the level, 68 ... and with the start of EVERY battle build (build_info), not only the level pick, 69 BOARD covers EVERY unit (players too) and carries its turn-order keys (Character+0x954 / +0x958 / speed), 70 ... and the initiative base +0x5DC the first key is recomputed from, 71 BOARD is published at EVERY turn boundary (carries the turn), 72 the end-turn action carries the cat's final facing (a manual turn after the move is no action) and MSG_ROLL (the host decides a chance roll whose odds differ between peers), 73 a player is known by two ids in the save handshake (Steam id + install fingerprint), 74 a checkpoint OFFER may carry mode 4 (the save is on the map and its journal holds nothing to recover it with) so the save-selection panel can say so; a build that only knows modes 0..3 drops such an offer, 75 BOARD units carry the DEFINITION name of their kind (Character+0x240 -> +0x88) and a champion flag, so a peer holding another kind at that index can have it replaced by the game's own transform; chunks shrink from 16 to 8 units to stay inside the send buffer, 79 MSG_UQD (each peer's per-action digest of the unlock queries a battle made, compared on arrival) and MSG_PROPS (the host's whole save-property table, chunked), 78 UNLOCKS carries the class names of the host's save (the list the game's ability pools are built from), 77 BOARD units of a player's cat (kBoardHuman) carry the seven stats and the stat bonus (the layer-2 experiment of repairing a player's stats), 76 a checkpoint OFFER carries every confirmed save of the run (length-prefixed, up to kCheckpointCandidates) and a flags byte per save (kCheckpointAfterBoss: taken right after the chapter boss), and MSG_PEERLOG: on a desync or a halt each peer sends the tail of its own log, so the one log a player uploads holds both sides

// A frame's payload may not exceed this. RUNSTATE (phase 5) is the only message
// that will ever approach it; everything in phase 4 is under 128 bytes.
constexpr uint32_t kMaxPayload = 8u << 20;   // 8 MiB

// --- more than two players --------------------------------------------------
//
// TOPOLOGY IS A STAR AND THE HOST IS THE HUB. Not a mesh, for three reasons,
// and the third is the one that decides it:
//
//   1. The host already owns the run. Every meta-layer push (SAVEFILE, CATDATA,
//      INVENTORY, ENTERNODE) is host-authored, so the hub exists either way.
//   2. Three links at four players instead of six.
//   3. ONLY THE HOST NEEDS A REACHABLE PORT. In a mesh every player forwards a
//      port to every other player. That is the difference between "my friend
//      joined" and an evening of router configuration.
//
// A client's ACTION reaches the other clients as host -> relay. The extra hop
// costs a round trip that a turn-based game does not notice, and the relay is
// done on the host's RECEIVE thread, so it never touches the game thread.
//
// Peer 0 is always the host. Client ids are handed out at accept time and are
// never reused within a session, so an id survives another peer disconnecting.
// The control split is derived from a peer's POSITION in the sorted id list
// rather than from the raw id, so a gap left by a departed peer does not shift
// anybody's cats.
constexpr uint8_t kMaxPeers = 4;
constexpr uint8_t kHostPeer = 0;
constexpr uint8_t kNoPeer   = 0xFF;

// THE FRAME ENVELOPE.
//
//     [u32 len][u8 from][payload ...]        len counts `from` + payload
//
// `from` is the id of the peer that ORIGINATED the message, not the id of the
// peer we received it from. The host overwrites it when relaying, which is what
// lets client B tell an action of client A's apart from one of the host's.
//
// It lives in the envelope rather than in each message for the same reason the
// epoch does not: every message needs it, including the ones added later, and a
// field that must be remembered in nine encoders is a field that will be
// forgotten in the tenth.
constexpr uint32_t kEnvelopeBytes = 1;

enum MsgType : uint8_t {
    MSG_HELLO    = 1,   // both, at connect
    MSG_WELCOME  = 2,   // host -> client: accepted, here is your control set
    MSG_REFUSE   = 3,   // either: incompatible, with a human-readable reason
    MSG_ACTION   = 4,   // acting peer -> other: one type-2 or type-3 decision
    MSG_HASH     = 5,   // both, at each turn boundary
    MSG_HALT     = 6,   // on desync: stop and dump
    MSG_PING     = 7,   // keepalive; carries nothing
    MSG_CONTROL  = 8,   // both, per battle: this peer's half of the split
    MSG_ENTERNODE= 9,   // host -> client: the map node the host entered
    MSG_SAVEFILE =10,   // host -> client: the whole save file, bytes and all
    MSG_CATDATA  =11,   // host -> client: one cat, serialized by the game itself
    MSG_INVENTORY=12,   // host -> client: the whole run inventory
    MSG_PEERS    =13,   // host -> each client: who is in the session, and who you are
    MSG_CURSOR   =14,   // both, while in a battle: the tile this peer is pointing at
    MSG_CHOICE   =15,   // event: host -> client; level-up: cat owner -> peers
                        // WorldEvent or LevelUpScreen option choice
    MSG_RUNHIST  =16,   // host -> client: the run-history object, whole
    MSG_NODEHASH =17,   // both, at each map node: the meta layer's own hash
    MSG_AIM      =18,   // acting peer -> other: what the human at the other
                        // keyboard is currently AIMING, before they commit it
    MSG_STATEDUMP=19,   // both, ONLY after a hash has already disagreed: the
                        // per-cat table that hash was taken over, so the peer
                        // can print the actual diff instead of two log files
                        // that have to be lined up by hand
    MSG_HOSTLEFT =20,   // host -> client: the host is no longer in the run
    MSG_DEBUGHIT =21,   // both, DEBUG ONLY: "subtract this much hp from the enemy
                        // cat on this tile". It exists so a test battle can be ended
                        // quickly ON BOTH PEERS: applied locally and broadcast, the
                        // two write the same number, so the hashes agree again on the
                        // next turn instead of one peer walking out of a fight the
                        // other is still in (measured 2026-09-22).
    MSG_PARTY    =22,   // both: "this is my party". kind 0 = the sender's own picks,
                        // kind 1 = the AGREED party, which only peer 0 ever sends.
                        //
                        // The point of the whole feature: each player prepares their
                        // own cats, and the two lists become one party before anybody
                        // enters a battle. It rides on the run's cat id vector
                        // (MewDirector+1468/+1472, stride 8), which is the layer that
                        // actually decides who fights -- see the the design notes section
                        // "Who fights".
};

// Setup exchange: a client sends its four locally prepared cats after the
// equipment screen, and the host replies with its four runtime cats. This is
// deliberately separate from CATDATA: setup may create cats from a different
// save and must not be mistaken for an in-run edit.
constexpr uint8_t MSG_SETUP = 23;
constexpr uint8_t kSetupClientExport = 0;
constexpr uint8_t kSetupHostReply    = 1;
constexpr uint8_t kSetupResumeExport = 2;
constexpr uint8_t kSetupResumeReply = 3;
constexpr uint8_t kPartyMaxCats = 4;
constexpr uint8_t kSetupMaxCats = kPartyMaxCats * kMaxPeers;
inline int session_cat_owner(uint64_t id) {
    for (unsigned p=0;p<kMaxPeers;++p) {
        const uint64_t base=0x70000000ull + (uint64_t(p)<<24);
        if(id>base && id<=base+kPartyMaxCats) return int(p);
    }
    return -1;
}
constexpr uint8_t MSG_CHAPTER = 24;
constexpr uint8_t MSG_CHECKPOINT = 25; // node-boundary save proposal/ack/commit and slot identity
// The chapter map's node seeds, host -> clients (proto 54). Two peers generate the same map STRUCTURE (node count, types) but, when their saves differ, not
// the same 32-byte xoshiro seed per node; the host sends its seeds in chunks and a client whose structure matches adopts them before its map is reported
// to the checkpoint (mgmp_follow: map_seed_sync).
constexpr uint8_t  MSG_MAPSEEDS = 34;
constexpr uint32_t kMapSeedsChunk = 8;     // nodes per message
constexpr uint32_t kMapSeedsMaxNodes = 128;
struct MapSeedNode {
    uint8_t  type = 0;
    uint64_t seed[4] = {};
    uint32_t flags = 0;     // the dword at MapNode+0x168: consumed + the lock/hidden bytes next to it (generation sets them for locked nodes)
};
constexpr uint8_t kMapSeedsFromHost = 0;     // the host's seeds and the MERGED flags, host -> clients
constexpr uint8_t kMapSeedsFromClient = 1;   // a client's own flags (seeds unused), client -> host: the host merges them (a node locked for anybody is locked)
struct MapSeedsMsg {
    uint8_t  kind = kMapSeedsFromHost;
    uint32_t epoch = 0;     // one per publication; the chunks of one map share it
    uint32_t total = 0;     // nodes in the map
    uint32_t first = 0;     // index of nodes[0]
    uint8_t  count = 0;     // entries used in nodes[]
    MapSeedNode nodes[kMapSeedsChunk];
};
// The HOST's unlock answers (proto 56). A battle is generated from each peer's own unlock lists (which level groups, which pickups, ...), so two
// players with different progress build different battles. The host sends one bit per name of the game's blacklists (mgmp_unlock_lists.h, in that
// order: set = unlocked for the host) and every client answers the generation of a battle with them (mgmp_unlocks).
constexpr uint8_t MSG_UNLOCKS = 35;
// One entry of the director's queue of enemies that come back (MewDirector+0x610, 0x38 bytes each): what the enemy stage of the battle build spawns.
constexpr uint32_t kPendingMax = 6;
constexpr uint32_t kWeatherNames = 4;    // weathers in the list
constexpr uint32_t kWeatherLen = 32;     // longest name + NUL
constexpr uint32_t kPendingQueues = 3;   // see mgmp_unlocks: 0 returning enemies, 1 neutral critters, 2 carried units
struct PendingEnemy {
    uint32_t ident = 0;      // hash of the enemy definition's name
    int32_t  remaining = 0;  // +0x24: battles it still comes back for
    int32_t  hp = 0;         // +0x20: the health it returns with
    int32_t  mode = 0;       // +0x28
};
constexpr uint32_t kClassMax = 16, kClassLen = 24;
struct UnlocksMsg {
    uint32_t epoch = 0;
    uint32_t abilities = 0;      // bit n = locked_abilities[n] is unlocked for the host
    uint32_t passives = 0;
    uint8_t  items[16] = {};
    uint8_t  levels = 0;         // locked_levelgroups
    uint8_t  bosses = 0;         // locked_bosses (bit n = the boss can be drawn for the host)
    uint8_t  n_bosses = 0;
    uint8_t  n_abilities = 0, n_passives = 0, n_items = 0, n_levels = 0;   // the host's list sizes: a different game data version is refused
    // The values of the save properties the random events read (mgmp_event_keys.h, in that order): tokens, counters, flags.
    uint8_t  n_events = 0;
    int32_t  events[64] = {};
    // The order the HOST's battle will place its party in (the game stable-sorts the party by each cat's computed SPEED, highest first),
    // with the key each cat sorts on. A client whose own sort comes out differently is put into this order (mgmp_unlocks).
    uint8_t  n_spawn = 0;
    uint64_t spawn_ids[8] = {};
    int32_t  spawn_keys[8] = {};
    // The level the HOST's battle picked (sub_140394450), tagged with the battle id it was picked for (proto 60). The clients' own pick can differ
    // (the candidate list is filtered by per-save state) and is overwritten with this one.
    uint64_t level_node = 0;
    uint8_t  n_level_name = 0;
    char     level_name[64] = {};   // 64 (was 48): the game has level paths of 54 characters ('levels/desert/miniboss/butchercat/butcherminiboss2.lvl'), which were refused
    // The host's queue of returning enemies at the moment it picked the level (proto 62), sent with the level only. The enemy stage of a client's
    // build aligns its own queue with it (mgmp_unlocks: unlocks_build_enemies_begin).
    uint8_t  n_pending[kPendingQueues] = {};
    PendingEnemy pending[kPendingQueues][kPendingMax];
    // The names in the HOST's weather list (MewDirector+0x668, in order; proto 67), sent with the level. A client whose list differs builds the battle with these.
    uint8_t  n_weather = 0;
    char     weather[kWeatherNames][kWeatherLen] = {};
    // 1 when pending[] / weather[] are valid and belong to the battle `level_node` (proto 68). Sent with the level pick of an ordinary battle AND at the start of every
    // battle build (a mini-boss or boss node never goes through the level pick).
    uint8_t  build_info = 0;
    // proto 78: the host's list of CLASS names -- what the game's class-list function (0x1402322B0) returns for its two flag values (without / with Colorless), read off the host's save.
    // Every random ability the game draws from "the abilities of any unlocked class" (the jester boss's scramble spell, ConjureBonusAbility, ...) builds its pool from that list, and a client
    // whose save lists other classes drew from another pool (2026-10-04, b521766e: a boss's spell differed, its speed and max hp with it, the turn order after it). 255 = not sent.
    uint8_t  n_classes[2] = { 255, 255 };
    char     classes[2][kClassMax][kClassLen] = {};
};
constexpr uint8_t kChapterReady = 0;
constexpr uint8_t kChapterSelect = 1;
struct ChapterMsg {
    uint8_t kind = kChapterReady;
    uint32_t generation = 0;
    uint8_t act = 0;
    int32_t difficulty = 0;
    // The map flags (mgmp_unlocks: mapflag_HardPathUnlocked, ...Sewers..., ...Desert...) as a bit set (proto 53). kChapterReady: THIS player's.
    // kChapterSelect: the ones EVERY player has -- the chapter map is generated from them on all peers, so the host cannot enter a node a
    // player without the flag could not, and the host cannot choose a chapter somebody has not unlocked (mgmp_setup).
    uint32_t mapflags = 0;
};

// Recovery traffic never carries another player's database. Each participant
// retains its own save, bound by an all-member commit certificate.
enum CheckpointKind : uint8_t {
    kCheckpointSlot, kCheckpointConfig, kCheckpointOffer, kCheckpointSelect,
    kCheckpointLoaded, kCheckpointRelease, kCheckpointArrive, kCheckpointPrepare,
    kCheckpointAck, kCheckpointCommit, kCheckpointCommitted, kCheckpointAdvance,
    kCheckpointReject, kCheckpointFinished
};
struct CheckpointRef {
    uint64_t run = 0, seq = 0, stamp = 0, certificate = 0;
    uint8_t  flags = 0;     // proto 76, not part of the identity (equal() ignores it): kCheckpointAfterBoss
};
constexpr uint8_t kCheckpointAfterBoss = 1;   // the save was confirmed on the map right after the chapter boss: the next step is the next floor or home
// Protocol 41: Slot.identity is the persistent player ID, Slot.token is a fresh
// selection nonce and Slot.seq is 1 for an in-session chapter (0 for a file load).
// Config.identity is a fresh handshake epoch; Config.hashes echo selection nonces;
// Config.mode carries the same load intent. Later messages carry that epoch in
// identity. Prepare/Commit.hashes are actual per-player database hashes. The
// durable certificate digest excludes the epoch, preserving journal compatibility.
// How many confirmed saves a pairing keeps (files <key>.0 .. .3, newest first) and offers.
constexpr unsigned kCheckpointCandidates = 96;   // proto 76 (was 4): the journal keeps every confirmed save of the run until it settles; the newest this many are offered
struct CheckpointMsg {
    uint8_t kind = kCheckpointSlot, slot = 0xFF, mode = 0, count = 0;
    uint64_t identity = 0, run = 0, seq = 0, stamp = 0, map = 0, token = 0;
    uint8_t peers[kMaxPeers] = {}, slots[kMaxPeers] = {};
    uint64_t identities[kMaxPeers] = {}, hashes[kMaxPeers] = {};
    CheckpointRef candidates[kCheckpointCandidates] = {};
    // A player is known by TWO ids (proto 73): `identity` is the primary one, `identity2` the other -- the Steam id and the install fingerprint beside the DLL. Either one matching
    // is the same player, so reinstalling the mod (a new fingerprint) no longer orphans the recovery records. Only on the wire; the journal keeps its shape.
    uint64_t identity2 = 0, identities2[kMaxPeers] = {};
};

// The save-selection stage (proto 47, 2026-09-30), host -> everyone. Every player who has clicked a
// save waits on a panel that names who else has; once all have, the host chooses which handshake save
// to continue from (or the preparation stage) while the others wait for that choice. This message is
// the only thing the others learn from the host during that time. Cosmetic: it drives a panel and
// nothing else -- the handshake itself is the CHECKPOINT messages.
constexpr uint8_t MSG_SAVEWAIT = 31;
constexpr uint8_t kSaveWaitCollecting = 0;   // waiting for everyone to pick a save
constexpr uint8_t kSaveWaitChoosing   = 1;   // all picked; the host is choosing the handshake save
constexpr uint8_t kSaveWaitInvalid    = 2;   // no handshake save fits and somebody is mid-run
// In an "invalid" SAVEWAIT the mask names the players whose save is mid-run (bit n = transport id n); bit 7 says that EVERY one of them is on the map with no co-op record to recover it with
// (the panel then tells them to go back to the warehouse instead of "load the original save"). Old builds read only the low bits, so they show the older text with the same names.
constexpr uint8_t kHoldersNoRecord    = 0x80;
constexpr uint8_t kSaveWaitLoading    = 3;   // chosen; every peer is loading
struct SaveWaitMsg {
    uint8_t phase    = kSaveWaitCollecting;
    uint8_t selected = 0;      // bit n = the peer with transport id n has picked a save
};

// A HOST-DECIDED ROLL (proto 72, 2026-10-03). Some chance rolls take their odds from data that is not the same on every peer (the coin a killing blow drops: both peers
// draw the same number from the shared stream, and the odds it is compared with differed, so one peer spawned a coin the other never had and the state hash halted the
// battle). The host rolls and says what came out; a client takes that answer instead of its own. (battle, seq) names the roll: the seq-th one at that site in that battle.
constexpr uint8_t MSG_ROLL = 37;
constexpr uint8_t kRollSiteCoinDrop = 1;
struct RollMsg {
    uint64_t battle = 0;
    uint32_t turn   = 0;
    uint32_t seq    = 0;
    uint8_t  site   = 0;
    uint8_t  result = 0;
    float    chance = 0, luck = 0;     // the host's inputs, only for the log (to see which side differs)
};

// Abandoning the adventure by vote (proto 48, 2026-09-30). Whoever presses "Abandon Adventure" in the
// pause menu does not abandon: they PROPOSE, everybody else is asked, and only when every player has
// agreed does every peer run the game's own abandon at once. Symmetric like PAGE -- each peer authors its
// own messages and the host relays them -- so every peer sees the same set and reaches the same verdict
// without an arbiter. A proposal is named by (proposer id, nonce); `proposer`/`nonce` on an answer say
// which proposal it answers. Two proposals at once: the lower proposer id wins.
constexpr uint8_t MSG_ABANDON = 32;
constexpr uint8_t kAbandonPropose = 1;   // from = proposer
constexpr uint8_t kAbandonAgree   = 2;   // from = a voter
constexpr uint8_t kAbandonDeny    = 3;   // from = a voter -- the proposal is over
constexpr uint8_t kAbandonCancel  = 4;   // from = the proposer, withdrawn or timed out
struct AbandonMsg {
    uint8_t  kind     = 0;
    uint8_t  proposer = 0;
    uint16_t nonce    = 0;
};

// Room coordination (proto 49, 2026-09-30). Peer-authored and relayed, like PAGE.
//   kRoomName  -- "this transport id is the lobby member called <name>". Re-sent every few seconds. It is
//                 what pairs a session id with a row of the room list: the list is the server's (room
//                 order), the ids are the transport's, and after anyone reconnects the two orders differ.
//   kRoomAbort -- "start the save-selection round over": every peer drops its pick (and the host every
//                 pick it holds), so a wrong or invalid combination is never a dead end.
constexpr uint8_t MSG_ROOMCTL = 33;
constexpr uint8_t kRoomName  = 1;
constexpr uint8_t kRoomAbort = 2;
struct RoomCtlMsg {
    uint8_t kind = 0;
    char    name[32] = {};
};

// The fresh-session map pre-sync. A departure-ready save carries the map the
// game generated when its owner chose the party, and two peers that prepared
// on their own generated two DIFFERENT maps -- which the first node boundary
// refuses to certify. Before either peer loads, the host (the run's authority)
// ships its serialized files.chapter_map row to every other participant, and
// the receiver overwrites its own row. Cat data is not touched: the setup
// exchange owns the parties. size==0 means "the host's save has no chapter_map
// row" (a clean save) and the receiver does nothing. `data` follows the
// SaveFileMsg ownership contract: borrowed on encode, owned (malloc) on decode.
// In-session chapter seed: the host's global xoshiro stream, snapshotted when
// its chapter click is released and stamped onto the client before its chapter
// trampoline, so generate_map runs from the same stream on both peers and the
// fresh chapter's maps agree at the first node boundary (2026-09-28).
constexpr uint8_t MSG_CHAPTERSEED = 27;
struct ChapterSeedMsg {
    uint32_t generation = 0;   // keys the seed to its chapter exchange
    uint64_t rng[4] = {};      // the 32-byte global stream state
};

// Cat digest (proto 43, 2026-09-29): each peer, on the map, lists the run cats it
// holds as {id, fnv1a of the serialized image, size}. A receiver that is the
// AUTHORITY for a listed cat (its owner; the host for owner-0 / unknown cats) and
// holds different bytes re-sends its own copy -- so a cat that drifted on one
// client is healed from its owner instead of halting the next battle.
// Player page (proto 45, 2026-09-30): which screen each player is on, so the room
// panel can say more than "已连接". Cosmetic and never branched on -- a peer that
// has said nothing reads as Unknown, not as "main menu".
//
// It is sent by every peer (so it is relayed, like CATDIGEST) and on a TIMER, so
// like CURSOR and AIM it is outside the lockstep contract: a message that is late
// or lost costs a stale label, not a turn.
constexpr uint8_t MSG_PAGE = 29;
struct PageMsg {
    uint8_t page = 0;   // mgmp_page.h PageState
};

// The cats this player brings (proto 46, 2026-09-30): what the room panel draws to the right of
// their name -- head, health bar, level and class -- while they are in the setup screens or a
// run. Read by mgmp_catview out of the serialized CatData, sent on the same timer as PAGE and
// relayed the same way; a peer that has sent none reads as "nothing to show", never as a cat.
constexpr uint8_t MSG_CATS = 30;
struct CatsMsg {
    uint8_t  count = 0;                 // 0 = this player has no cats to show right now
    CatBrief cats[kBriefCats];
};

constexpr uint8_t MSG_CATDIGEST = 28;
constexpr uint32_t kCatDigestMax = 64;
struct CatDigestMsg {
    uint8_t  count = 0;
    uint64_t ids[kCatDigestMax] = {};
    uint64_t hashes[kCatDigestMax] = {};
    uint32_t sizes[kCatDigestMax] = {};
};

constexpr uint8_t MSG_CHAPTERMAP = 26;
struct ChapterMapMsg {
    uint64_t selection = 0;    // save-selection epoch; stale transfers cannot write a later save
    uint32_t size = 0;         // bytes of `data`
    uint64_t hash = 0;         // FNV-1a over those bytes, verified after writing
    uint8_t* data = nullptr;   // encode: borrowed. decode: owned by the receiver.
};

// A save file is a plain sqlite3 database (the shipped ones start with the
// literal "SQLite format 3 ") and the campaign save measures about 45 KB. The
// cap is generous but bounded: a garbled length must not make the receiver
// allocate wildly, and there is no legitimate save anywhere near this size.
constexpr uint32_t kMaxSaveBytes = 4u << 20;   // 4 MiB

// ---------------------------------------------------------------------------
// Little-endian cursor writer/reader.
//
// Hand-rolled rather than memcpy-of-struct because the struct layout must not
// leak onto the wire: TurnAction+0x04 is uninitialised padding that differs
// between runs for no reason (it read 0x85 in one copy and the ASCII "pone" --
// stale stack -- in another), and anything that blits a struct will eventually
// carry a field like that across and desync two peers over garbage.
// ---------------------------------------------------------------------------
struct Writer {
    uint8_t* buf;
    uint32_t cap;
    uint32_t len = 0;
    bool     ok  = true;

    Writer(uint8_t* b, uint32_t c) : buf(b), cap(c) {}

    void raw(const void* p, uint32_t n) {
        if (!ok || len + n > cap) { ok = false; return; }
        memcpy(buf + len, p, n);
        len += n;
    }
    void u8v(uint8_t v)   { raw(&v, 1); }
    void u32v(uint32_t v) { raw(&v, 4); }
    void u64v(uint64_t v) { raw(&v, 8); }
    void i32v(int32_t v)  { raw(&v, 4); }

    // Length-prefixed string, u8 length. Every string in this protocol is a GON
    // name or a diagnostic reason, both well under 256 bytes.
    void str(const char* s) {
        size_t n = s ? strlen(s) : 0;
        if (n > 255) n = 255;
        u8v((uint8_t)n);
        raw(s, (uint32_t)n);
    }
};

struct Reader {
    const uint8_t* buf;
    uint32_t       len;
    uint32_t       pos = 0;
    bool           ok  = true;

    Reader(const uint8_t* b, uint32_t n) : buf(b), len(n) {}

    void raw(void* p, uint32_t n) {
        if (!ok || pos + n > len) { ok = false; return; }
        memcpy(p, buf + pos, n);
        pos += n;
    }
    uint8_t  u8v()  { uint8_t v = 0;  raw(&v, 1); return v; }
    uint32_t u32v() { uint32_t v = 0; raw(&v, 4); return v; }
    uint64_t u64v() { uint64_t v = 0; raw(&v, 8); return v; }
    int32_t  i32v() { int32_t v = 0;  raw(&v, 4); return v; }

    // Always NUL-terminates when it succeeds. On overflow the field is
    // truncated and `ok` stays true -- a 300-byte GON name is malformed input,
    // not a transport error, and it will fail the name cross-check anyway.
    void str(char* out, uint32_t out_size) {
        if (!out_size) { ok = false; return; }
        out[0] = 0;
        uint8_t n = u8v();
        if (!ok) return;
        if (pos + n > len) { ok = false; return; }
        uint32_t copy = n < out_size - 1 ? n : out_size - 1;
        memcpy(out, buf + pos, copy);
        out[copy] = 0;
        pos += n;                          // skip the whole field, not just the
                                           // part that fit -- truncating must
                                           // not desynchronise the cursor.
    }
};

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

// Sent by both peers immediately on connect.
//
// gpak_hash is NOT optional and the connection is refused without a match.
// Both peers need byte-identical game data because RollChance @ 0x14094B550
// takes NO draw when p >= 1.0 -- so the stream *position* depends on which
// procs were possible, not on which fired. Two peers that disagree about
// whether a chance was 0.9 or 1.0 desync even when the visible outcome is the
// same. That has to be caught here, not on turn 30.
struct Hello {
    uint32_t proto      = kProtoVersion;
    uint64_t gpak_hash  = 0;   // resources.gpak content hash
    uint64_t build_hash = 0;   // Mewgenics.exe build identity
    char     name[64]   = {};  // display name, diagnostics only
    // The HOST's rules that a client has to follow, not to agree on (proto 51): bit 0 = ui.block_new_cats (the "a new cat joins"
    // event effect does nothing). A client takes them from the host's HELLO; its own copy of the setting is not used in a room.
    uint8_t  rules      = 0;
};

// Host -> client, after a HELLO it accepts. `cats` is the set of cat indices
// the CLIENT may decide for; everything else is the host's. Both peers simulate
// every cat identically -- this is a control split, not a state split.
struct Welcome {
    // The simulation stream, TLS+0x178, all four xoshiro256 words. Sending it
    // is what makes "copy the host's save" unnecessary in the long run: the
    // client does not need to reproduce the host's RNG *history*, only to
    // arrive at the same state. Entering a battle does not re-seed -- measured
    // across four separate launches, all of which entered at the identical
    // s0=967e2d6d328620b1 -- so the state travels with the save, and sending it
    // here overrides whatever the client's own save would have supplied.
    uint64_t rng_state[4]   = {};
    uint8_t  cat_count      = 0;
    uint8_t  cats[32]       = {};
};

// Host -> each client, individually rather than broadcast, because `you` is
// different in every copy. Re-sent to everyone whenever the membership changes,
// so a peer that joins third does not leave the first two believing they are
// splitting the cats two ways.
//
// `ids` is sorted ascending and always contains kHostPeer. A peer's INDEX in
// this array -- not its id -- is its position in the control split, which is
// what keeps the split stable across a disconnect that leaves a gap.
struct PeersMsg {
    uint8_t you            = kNoPeer;   // the receiving peer's own id
    uint8_t count          = 0;         // members, host included
    uint8_t ids[kMaxPeers] = {};
};

// THE BATTLE EPOCH, carried by every per-battle message (ACTION, HASH, CONTROL).
//
// Turn numbering restarts at 0 in every battle, so a turn index alone does not
// identify anything: a message still in flight when both peers cross a battle
// boundary arrives looking exactly like a message about the battle they just
// entered. A stale HASH would then be compared against the new battle's turn N
// and manufacture a desync out of nothing; a stale ACTION is worse, because it
// would be injected as a real decision for whichever cat index it names.
//
// The identity is `battle_id`: the 64-bit node seed the battle was entered
// with, read out of MapNode+0x118 by the MapScreen::EnterNode hook on BOTH
// peers. It is not negotiated, not counted and not derived from anything local
// -- it is the same number on both machines because they entered the same node,
// which is the same reason this protocol has never needed a SEED message.
//
// IT USED TO BE A PER-PROCESS COUNTER, AND THAT BROKE ON RECONNECT. g.epoch
// started at 0 in lockstep_init and only ever incremented, so a client that
// relaunched sat at 0 while the host was at 108. All three consume paths gate
// on exact equality -- pend_take, verify_control, the hash matcher -- so the
// client discarded the host's actions, the host discarded the client's as
// stale, and the join barrier never opened. A silent stall in the first battle
// after any reconnect.
//
// Adopting the host's counter on join was the other candidate and is worse,
// because the bump is OBSERVATIONAL: ++epoch fires when lockstep notices the
// character list changed at a turn boundary, so "which epoch is this battle"
// is not knowable at the moment of adoption -- a host mid-fight has already
// bumped, a host on an event node has not and will not. A shared value has no
// such moment. Same rule as ability slots and CatData ids: identity comes from
// something both processes can read, never from a number one of them counted.
//
// Three cases on receipt, and they are genuinely different:
//
//   a RETIRED battle   the sender is still talking about a battle we have left.
//                      Drop it. This is the race the field exists to close.
//   our current one    normal.
//   anything else      the sender reached a battle we have not. HOLD it:
//                      dropping here is what made a peer entering a battle late
//                      lose the actions taken without it (see the late-join gap
//                      in the design notes), and holding costs nothing because the
//                      queues are keyed on battle_id and will not hand one over
//                      early.
//
// Ordering came free with a counter and does not with an id, which is why the
// receiver keeps a small set of the battle_ids it has RETIRED. That set is the
// only thing separating "old" from "not yet" -- and unlike a counter it stays
// correct across a restart, because it is populated from battles this peer
// actually played rather than from how many it has counted.
//
// Deliberately NOT a halt condition. A peer legitimately running a battle ahead
// or behind produces both mismatches, so halting on them would fire constantly
// on exactly the lag the lockstep is built to tolerate.

// Host -> client, one cat of the run, as the GAME serializes it.
//
// WHY THIS EXISTS. A save file syncs the run at the moment it is written, and
// nothing after. The host then equips a trinket, and the peers diverge in a way
// nothing detects until the battle: `state_hash` covers HP, shield, max HP,
// tile, facing and dead -- not abilities, not equipment -- so the first sign is
// the ability cross-check halting mid-battle with "slot 4:4 (gon
// 'tk_GlowingCoin') is empty on this peer". Measured, 2026-08-24.
//
// WHY IT IS BYTES AND NOT AN ACTION. Replaying the equip was the obvious plan
// and it is a dead end. InventoryItemBox::click() is `void click(void)` on a UI
// box that exists only while the inventory screen is open -- the client is not
// in that screen -- and it merely STARTS a flow (confirmation popup, then an
// AbilityChooser, which is a second decision point) whose actual mutation fans
// out across ~8 CatData helpers. Replaying means driving a UI the peer is not
// in; reimplementing means reproducing 8 mutations exactly, where one miss
// diverges silently. Both are worse than the halt they replace.
//
// So we ship the RESULT instead, through the game's own code:
//
//   glaiel::SerializeCatData(CatData&, ByteStream&, bool)  @ 0x14022E9A0
//
// It is bidirectional -- every field branches on the stream mode at
// ByteStream+0x00 (0 = read, 1 = write) -- so the same function that saves a cat
// on the host loads it on the client. Nothing is reimplemented, and the format
// carries its own version tag (19) as the first field.
//
// `id` is CatData+0x00, serialized right after that version tag and resolvable
// through the run's registry. A real identity, so unlike abilities this needs no
// index scheme and no name cross-check.
struct CatDataMsg {
    uint64_t id    = 0;    // CatData+0x00
    uint32_t size  = 0;
    uint64_t hash  = 0;    // FNV-1a over the bytes; also the host's change check
    uint8_t* data  = nullptr;  // encode: borrowed. decode: owned by the receiver.
};

struct SetupCat {
    uint64_t id = 0;
    uint32_t size = 0;
    uint64_t hash = 0;
    uint8_t* data = nullptr; // encode: borrowed; decode: owned
};

struct SetupMsg {
    uint8_t kind = kSetupClientExport;
    uint8_t count = 0;
    uint32_t generation = 0;
    uint64_t checkpoint = 0; // resume: map seeds/types/consumed flags; zero for new setup
    uint8_t members = 0, peers[kMaxPeers] = {};
    uint32_t requests[kMaxPeers] = {}; // each client's current export generation
    SetupCat cats[kSetupMaxCats] = {};
};

// A serialized cat is well under a kilobyte in practice. The cap is generous but
// bounded, for the same reason kMaxSaveBytes is: a garbled length must not make
// the receiver allocate wildly.
constexpr uint32_t kMaxCatBytes = 256u * 1024u;

// Host -> client, the whole run inventory, sent alongside CATDATA at every map
// node.
//
// WHY IT EXISTS. Equipping an item MOVES it: out of the run inventory, onto the
// cat. CATDATA syncs the cat half and nothing synced the other, so the client
// kept a duplicate of every item the host equipped -- an item on the cat AND
// still in the backpack. Nothing detected it either: `state_hash` covers HP,
// shield, max HP, tile, facing and dead, and the ability cross-check only looks
// at what the cat can do, so a spare copy of a trinket sitting in a bag on one
// peer is invisible until it is used.
//
// WHY IT IS THREE BLOBS AND NOT AN ITEM LIST. Items have no portable identity.
// On load each Equipment takes `++qword_1413BD998`, a process-global object-id
// counter that fifteen other classes also mint from and that is never written
// to or read from the stream. Host and client mint different ids for the same
// item, so no message may key on one -- which rules out reconciling item by
// item and leaves pushing each bucket whole. Unlike CatData, whose +0x00 id IS
// serialized and IS resolvable, an item's only persistent identity is its GON
// name plus its stat fields.
//
// The three buckets are the three the game itself saves: backpack, storage,
// trash. `size` may legitimately be 0 -- an empty bag is a normal state and
// must survive the round trip, which is why this decoder accepts a zero length
// where dec_catdata treats it as malformed.
//
// The three scalars ride along because they are the rest of what the inventory
// write driver persists, and they are plain ints on the Inventory object -- no
// serializer, no blob, nothing to intercept.
constexpr uint32_t kInvBuckets  = 3;             // backpack, storage, trash
constexpr uint32_t kMaxInvBytes = 512u * 1024u;  // per bucket

struct InventoryMsg {
    int32_t  coins = 0, food = 0, boxes = 0;
    // FNV-1a over the scalars and all three blobs. Doubles as the host's
    // change check, so an unchanged inventory costs three serializes and a
    // compare and sends nothing.
    uint64_t hash = 0;
    uint32_t size[kInvBuckets] = {};
    uint8_t* data[kInvBuckets] = {};  // encode: borrowed. decode: owned.
};

// One decision. This is the entire battle protocol.
struct ActionMsg {
    uint64_t battle_id  = 0;   // which battle; see above
    uint32_t turn       = 0;
    uint8_t  actor      = 0;   // cat index; a cross-check, see mgmp_lockstep.h
    uint8_t  type       = 0;   // 2 = ability, 3 = end turn. Never 6 or 7.
    uint8_t  slot_kind  = 0;   // SLOT_* from mgmp_ability.h
    uint8_t  slot_index = 0;
    uint8_t  b30        = 0;   // TurnAction+0x30, forwarded to Ability::Prime
    uint8_t  b31        = 0;   // TurnAction+0x31
    int32_t  tx = 0, ty = 0;   // target tile
    int32_t  dx = 0, dy = 0;   // direction
    char     gon[64]    = {};  // authored ability name -- the second identity
};

// Exchanged at every turn boundary. Cheap because the game is turn-based.
//
// The queue depth belongs in here as much as the state hash does: types 6 and 7
// exist *because* a passive fired, so if a proc roll differs between peers the
// queue populations diverge immediately -- before any visible state does. It is
// the earliest and cheapest divergence signal available, and it costs no
// serialization of the ~1390 component classes.
struct HashMsg {
    uint64_t battle_id   = 0;   // which battle; see the note above ActionMsg
    uint32_t turn        = 0;
    uint64_t rng_hash    = 0;   // over the 32-byte TLS+0x178 state
    uint64_t state_hash  = 0;   // positions / HP / status counts
    uint32_t queue_depth = 0;   // TurnControl+0x60
    uint32_t queue_sig   = 0;   // rolling hash of pending action types
};

// Sent by BOTH peers once per battle, at their own roster snapshot.
//
// It is a VERIFICATION, not an assignment, and that distinction is what keeps
// it free of races. Both peers derive the same split locally from the same
// roster -- byte-identical by measurement: 29 cats, same brain class per index,
// two processes with completely different heap addresses -- so neither waits
// for this to arrive before playing.
//
// What it buys is the check that used to be impossible. `Welcome` could catch
// two peers claiming the SAME cat, but a cat claimed by NEITHER was invisible
// and surfaced only as a battle that stalled on that cat's turn with nothing in
// either log to explain it. Two lists plus a shared roster make both halves
// checkable, and sending it in both directions puts the result in both logs.
//
// `humans` is included for the same reason the roster is printed: if the peers
// disagree on how many cats a human drives, every index below is meaningless,
// and saying so beats diffing the lists.
struct ControlMsg {
    uint64_t battle_id = 0;   // which battle; see the note above ActionMsg
    uint32_t humans   = 0;    // human-brained cats the sender counted
    uint8_t  count    = 0;    // how many of them the sender claims
    uint8_t  cats[32] = {};   // their indices, ascending
    uint32_t fp[32]   = {};   // ...and WHAT each of those cats is, by ability
                              // fingerprint (mgmp_ability). The index is a
                              // POSITION and two peers can disagree about one
                              // without either being wrong -- `cats` then reads
                              // as a healthy split while the two sides are
                              // describing different cats, and that is the
                              // failure this field exists to end. A permutation
                              // and a genuinely diverged roster look identical
                              // in `cats` and are told apart here.
};

// Both directions, while a battle is on screen: the board tile this peer is
// pointing at. The first message in this protocol that exists to be SEEN.
//
// A TILE, NOT A PIXEL, and that is the whole reason this is cheap. Two peers
// run at different resolutions, with independently panned cameras and different
// UI scale, so a screen-space cursor would land somewhere else on every other
// machine. A tile index means the same square on every screen, and the game
// already computes it for us: StatusMenu caches the tile under the mouse at
// StatusMenu+124 every frame (bounds-checked against the grid at StatusMenu+72,
// width at +184, height at +188), which is what drives its own ground pip.
//
// PURELY COSMETIC, AND DELIBERATELY OUTSIDE THE LOCKSTEP CONTRACT. Nothing here
// is hashed, nothing here is replayed, and a dropped or stale CURSOR cannot
// desync anything -- the worst case is a cursor that lags or disappears. That
// is why it may be sent on a throttle rather than at a command boundary, and
// why it carries the epoch only so a message crossing a battle boundary can be
// dropped instead of drawn on the wrong board.
//
// `on_board` is separate from the tile rather than encoded as a sentinel
// because "pointing at nothing" is the common case -- reading a tooltip, moving
// to the end-turn button -- and a cursor that freezes at the last tile it
// touched reads as a stuck peer rather than an idle one.
//
// `owns_turn` is what the transparency rule keys on. The receiver could in
// principle derive it (both peers agree on the roster and the split), but the
// sender knows it without ambiguity and one bit is cheaper than making the
// display depend on the control check having completed.
// What the human at the other keyboard is pointing an ability at, BEFORE they
// commit it -- the range/AOE tiles solo players see while they aim.
//
// Deliberately in the same class as CURSOR and not in the same class as ACTION:
// nothing here is hashed, replayed or acknowledged, a dropped or late one can
// only make a preview flicker, and it is therefore allowed to be sent on a
// timer rather than at a command boundary. An AIM is NOT a decision -- the same
// aim can be sent a hundred times and then abandoned. The decision, when it
// comes, still arrives as an ACTION and nothing about that changes.
//
// The three fields that matter are exactly the arguments the game itself passes
// to glaiel::Brain::DrawAbilityAOE @ 0x14013A030 from Brain::UpdateDecision:
//
//     mov rdx, [rdi+228h]     ; the Ability*  -> slot_kind / slot_index here
//     mov r8,  [rdi+230h]     ; iVec2D target -> tx, ty
//     mov r9,  [rdi+238h]     ; iVec2D dir    -> dx, dy
//
// The ability crosses the wire as a SLOT, never a pointer, for the reason the
// whole project settled long ago: 0 of 21 ability pointers matched between two
// runs. The GON name rides along as the same independent cross-check ACTION
// uses -- resolve by slot, validate by name.
struct AimMsg {
    uint64_t battle_id  = 0;   // which battle; a mismatch is dropped, not halted
    uint8_t  cat        = 0;   // roster index of the cat being aimed
    uint8_t  active     = 0;   // 0 = stopped aiming, draw nothing
    uint8_t  slot_kind  = 0;   // SLOT_* from mgmp_ability.h
    uint8_t  slot_index = 0;
    int32_t  tx = 0, ty = 0;   // target tile
    int32_t  dx = 0, dy = 0;   // direction
    char     gon[64]    = {};  // authored ability name -- the second identity
};

struct CursorMsg {
    uint64_t battle_id = 0;   // which battle; a mismatch is dropped, never halted
    int32_t  x         = 0;   // tile coordinates into the tactics grid
    int32_t  y         = 0;
    uint8_t  on_board  = 0;   // 0 = pointing off the grid; draw nothing
    uint8_t  owns_turn = 0;   // 1 = the cat currently deciding is this peer's


    // Where the sender's MOUSE is, as a fraction of its own window: 0,0 is the
    // top-left corner and 1,1 the bottom-right. Resolution-independent by
    // construction, which a pixel position would not be.
    //
    // This is the one field in the message that is NOT a board fact, and the
    // difference matters. The tile above means the same square on every
    // machine whatever the camera is doing; the fraction means the same place
    // on the SCREEN, which is the same place on the board only while both
    // players have the camera in the same position. That is why both are sent:
    // mgmp_cursor draws the tile, mgmp_overlay draws the fraction, and the
    // reticle stays right even when the pointer is merely close.
    float    nx        = 0.0f;
    float    ny        = 0.0f;

    // WHICH CURSOR the sender's game is showing -- an index into the art table
    // in mgmp_overlay.cpp, whose entries are the shipped textures/cursor/*.png
    // file names. The receiver draws that same texture, so a peer hovering a
    // button shows the pointing hand and a peer aiming a spell shows the spell
    // cursor.
    //
    // An INDEX, not the state string: the string is a texture name the receiver
    // would have to trust enough to concatenate into a path, and both peers are
    // the same build by construction (HELLO checks the build hash). The cost is
    // that the table's ORDER is wire format -- appending is safe, reordering is
    // a protocol change. An index we do not know draws the plain arrow.
    uint8_t  mode      = 0;
};

// Host -> client, every time the host enters a map node.
//
// The map is the one meta screen that HAS a command boundary --
// MapScreen::EnterNode(MapScreen*, MapNode*) @ 0x140391050, one code caller and
// not virtual -- so following the host through it costs one message rather than
// a state push.
//
// `seed0` is the important field and it is not decoration. EnterNode's first
// act is to copy MapNode+0x118..0x137 into TLS+0x178, i.e. the node carries the
// 32-byte xoshiro256 state the battle will run on. If the two peers' nodes hold
// different seeds, the battle starts from different RNG and desyncs on its
// first roll -- so comparing one word of it here names the cause at the moment
// it is still fixable, instead of surfacing as a turn-3 hash mismatch.
//
// It also means the protocol never has to SEND a seed: entering the same node
// is what makes the streams equal.
struct EnterNodeMsg {
    uint32_t index      = 0;   // index into MapScreen's node vector (+0x7C/+0x80)
    uint32_t node_count = 0;   // the sender's map size, so a bad index is caught
    uint32_t type       = 0;   // MapNodeType: 5 battle, 6 hard, 8 boss, 12 shop...
    uint64_t seed0      = 0;   // first word of MapNode+0x118 -- see below
    // THE OTHER THREE WORDS, carried since 2026-09-21 and not used to compare.
    //
    // The paragraph above justified sending only seed0 on the grounds that the
    // protocol "never has to SEND a seed: entering the same node is what makes
    // the streams equal". That is exactly true while both peers load the same
    // save, and it is the arrangement the whole mod is built on -- but it is a
    // PRECONDITION, not a law, and the plan is for the two peers to be able to
    // bring their own saves (each player's own cats). Two different saves mean
    // two different maps, and then "the same node" is no longer the same 32
    // bytes -- so the words have to travel, and the client has to be able to
    // WRITE them (see tune::kAdoptHostSeed and the seed check in mgmp_follow).
    //
    // Appending is safe: the field order above is wire format and adding to the
    // END is not a protocol change (see the note over the message table).
    uint64_t seed[4]    = {};  // all 32 bytes of MapNode+0x118; seed[0] == seed0
    uint32_t familiar_count = 0;
    uint64_t familiar_ids[64] = {};
    // THE HOST'S PARTY, IN THE HOST'S ORDER (proto 44, 2026-09-29). The client used to take the
    // host's party order from the node snapshot's CATDATA batch -- which is FIRST-ARRIVAL order,
    // not the party's. Measured 2026-09-29 19:13: the batch listed cat 2 first because it had
    // been pushed earlier, the client adopted [2 1 3] against the host's [1 2 3], the node hash
    // differed on ORDER alone, and the checkpoint barrier refused the node for good. Appended
    // last; party_count 0 means "not known" and the receiver falls back to the batch order.
    uint32_t party_count = 0;
    uint64_t party_ids[16] = {};
};

// Event: host -> client. Level-up: the cat's owner -> peers.
//
// THIS IS THE MESSAGE THAT REPLACES RUNSTATE, AND THE REASON IT CAN IS
// DETERMINISM RATHER THAN SERIALIZATION.
//
// Both peers enter the same map node, and EnterNode copies MapNode+0x118 into
// the simulation stream TLS+0x178 UNCONDITIONALLY, before any node-type
// dispatch (0x1403910C2..0x1403910E5; the only branch ahead of it is the shift
// key). So an event node hands both peers a byte-identical RNG state, by the
// same mechanism that already makes battles deterministic. Every RNG draw found
// on the event path lands on that stream: the item-pool draw at 0x1408DB3FE
// (get_item_from_pool, 340 shipped occurrences -- the most common effect in the
// game), random_chance's randfloat, reward's RollChance,
// party_skip_next_fight_chance's rand2.
//
// The level-up roller looks like the exception -- it runs on a HEAP stream at
// object+0xB8 rather than the global one -- and it is not. That state is seeded
// splitmix64(CatData+0x00, the cat's seed) and then advanced by
// CatData+0xC30 xoshiro jumps (0x140379CA5..0x140379CE7), and BOTH fields are
// round-tripped by SerializeCatData. It is a pure function of cat state the
// client already has, which is presumably why Tyler wrote it that way: it makes
// a reroll reproducible for a given cat.
//
// What is therefore NOT derivable is the one thing no seed contains: which
// button a person pressed. That is all this message carries. The 119 authored
// effect commands, the familiar list at MewDirector+1576, the next-fight spawn
// queue at +1552, the adventure tokens at +1664, the legacy counters in the
// save-cache at +56 -- none of them need a wire format, because both peers
// compute them from the same seed and the same choice.
//
// RESOLVE BY INDEX, VALIDATE BY NAME. Both screens hold their offered options
// as a 240-byte-stride array (WorldEvent+224..+232, LevelUpScreen+864..+872)
// built in authored file order from data both peers loaded out of the same
// resources.gpak, which HELLO already hashes. The index is stable by
// construction. The name rides along anyway and a mismatch is shouted about,
// because that is the cross-check that caught the equipment bug in the battle
// layer: a disagreement means the two option lists were BUILT differently, and
// that deserves a loud line rather than a silently wrong choice.
//
// NOT hashed, not acknowledged, not replayed -- but unlike CURSOR it is also
// not droppable. A lost CHOICE leaves the client sitting on a screen forever,
// which is a visible stall rather than a wrong game, and that is the trade this
// whole layer already makes everywhere else.
// A CHOICE IS ABOUT ONE NODE, AND UNTIL PROTO 19 IT DID NOT SAY WHICH.
//
// Measured 2026-08-25, and it is the whole reason `node_seed` exists. The host
// entered map node 6 (an event), picked option 0 ('int') and walked on to node
// 17 before the client's map tick had entered node 6 at all. The client held
// the choice -- correctly, its option list did not exist -- and then applied it
// TWO NODES LATER to node 3's event, whose option 0 was 'dex'. The name
// cross-check saw the disagreement, reported it, and obeyed it, because a
// differing label was assumed to mean two option lists built differently rather
// than two different events:
//
//   client  !! event option 0 is 'dex' here but 'int' on the host --
//              taking it anyway, the lists are the same length
//   host    (no event choice published at node 3 at all)
//
// From there the two runs genuinely differed -- different effects applied to
// different cats -- and three nodes later the peers were rolling different
// events outright (2 options against 3), at which point the client's screen
// refused every option and the run had to be restarted.
//
// The lesson is the one this protocol has now learned four times: A PER-SCREEN
// INDEX IS NOT AN IDENTITY. Ability slots, CatData ids and battle_id all
// replaced a local number with something both processes can read, and this is
// the same replacement for the fourth time. The node seed is free -- both peers
// already read MapNode+0x118 to agree on battle_id -- and it makes "is this
// choice about the screen in front of me" answerable instead of assumed.
struct ChoiceMsg {
    uint8_t  kind  = 0;     // kChoiceEvent or kChoiceLevelUp
    uint32_t index = 0;     // option index into that screen's array
    uint32_t count = 0;     // options the sender had, so a bad index is caught
    uint32_t aux   = 0;     // level-up: the LevelUpOption type at +0. event: 0.
    uint64_t node_seed = 0; // MapNode+0x118 of the sender's node. Level-up requires
                            // nonzero; event alone allows 0 (unknown / no check).
    char     name[48] = {}; // event: the stat key at entry+64 ("con", "coins",
                            // "none"...). level-up: the option name at +200.
    // Level-up identity is the pair (cat_id, cat_seed); all three are 0 for events.
    uint64_t cat_id = 0;    // full CatData+0xC48 save key, never a truncated id
    uint64_t cat_seed = 0;  // CatData+0x00; zero is a valid seed
    char event_name[64] = {}; // event page identity, empty for level-ups (protocol 40)
    uint32_t level_step = 0; // committed choices in this node, starting at 0;
                             // +1 after each successful local or remote upgrade
};

constexpr uint8_t kChoiceEvent   = 0;
constexpr uint8_t kChoiceLevelUp = 1;

// Host -> client, once the host has committed to a save file.
//
// This is the bluntest possible answer to "both peers must start from the same
// state", and it is blunt on purpose: the save is a single self-contained
// sqlite3 file, so shipping it whole needs none of the ~1390-class
// serialization that RUNSTATE will. the design notes' own first-milestone advice was
// "copy the host's save file, do not match checkpoints" -- this is that, done
// by the mod instead of by hand.
//
// WHY IT CARRIES A SLOT INDEX AS WELL AS A NAME. The name is what the host's
// SaveSelection called the file ("steamcampaign01.sav"); the slot is where it
// sat in that screen's name vector. They are not redundant, because
// SaveSelection::init builds one of TWO name arrays -- campaignNN.sav or
// steamcampaignNN.sav -- depending on the build. Two peers could therefore
// agree on the slot and disagree on the name. The client resolves by name and
// falls back to the slot, which makes the mismatch survivable instead of fatal.
//
// `data` is NOT copied by the flat NetMsg queue: the decoder heap-allocates it
// and the consumer must hand the frame back to net_msg_release. That is the one
// allocation on the receive path in this protocol, and it is affordable because
// it happens once per session rather than once per turn.
struct SaveFileMsg {
    uint32_t slot = 0;         // index into SaveSelection's name vector
    uint32_t size = 0;         // bytes of `data`
    uint64_t hash = 0;         // FNV-1a over those bytes, checked after writing
    char     name[64] = {};    // the host's filename for the slot
    uint8_t* data = nullptr;   // encode: borrowed. decode: owned by the receiver.

    // WHY the host is sending, and the two answers need OPPOSITE handling on
    // the client. Without it the client cannot tell them apart, and the one it
    // guessed was the wrong one.
    //
    //   fresh = 1  the host just clicked a slot on the save-selection screen.
    //              It is (re)starting the run. A client that has already loaded
    //              one must DROP it and follow, because the host is no longer
    //              in the run that client is holding.
    //   fresh = 0  a catch-up copy for a peer that joined or reconnected. A
    //              client already in the run must KEEP it -- applying a save
    //              off the selection screen is not possible, and the per-node
    //              CATDATA/INVENTORY/ENTERNODE pushes resynchronise it.
    //
    // Measured 2026-08-28: the host went back to the menu and picked the same
    // slot again (host log seq 43/44, re-sent), and the client declined it at
    // seq 42 with "already in the run" -- the g.applied latch, which is set
    // once per PROCESS and had no way to know a new run had started. The two
    // sends are byte-identical without this field, hash included, so no amount
    // of comparison on the receiving side could have separated them.
    uint8_t  fresh = 0;
};

// Host -> client, once: the host has left the adventure.
//
// WHY THE CLIENT CANNOT WORK THIS OUT FOR ITSELF. Every other thing the host
// does inside a run announces itself -- ENTERNODE at each node, CHOICE at each
// decision, CATDATA and INVENTORY at each boundary. LEAVING the run announces
// nothing at all: the host simply stops sending, which is indistinguishable
// from a host who is thinking, reading a tooltip, or in a long battle. So a
// client whose host went back to the house or out to the title screen sat in a
// run nobody else was playing, with nothing in either log saying so.
//
// It is deliberately a STATEMENT OF FACT and not a command. What the receiving
// peer does about it is the receiver's business (mgmp_leave takes it out to the
// main menu when it can), and a peer that is already out of a run does nothing
// at all rather than treating it as an error.
//
// `scene` is which of House / MainMenu / SaveSelectionScreen the host is now
// showing. Diagnostic only -- the client never branches on it -- but it is the
// difference between "the host left" and "the host went back to the house to
// breed, and will be a while".
struct HostLeftMsg {
    char scene[32] = {};
};

// Host -> client: the run-history object at *(MewDirector+1424), whole.
//
// WHY IT HAS TO BE SYNCED, WHICH WAS NOT OBVIOUS AND IS NOT OPTIONAL.
//
// The event a node shows is NOT a pure function of the node seed. MapScreen::
// select_event @ 0x140395D10 hands off to sub_1408DA560, which:
//
//   1. picks a RANDOM CAT out of the run's roster -- sub_1400AACD0 is one
//      inline xoshiro step used to index the list, so the same stream state
//      picks a different cat if the list differs;
//   2. reads that cat's ExcludeFromEvents (a pool filter) and
//      ChanceToForceEvent (which can FORCE a specific event outright, on a
//      rand2 gated by the cat's own stat);
//   3. draws from the pool in a retry loop that SKIPS EVERY EVENT ALREADY
//      USED -- the used-event list living at tracker+96, tested by
//      sub_1408D9EB0 and appended to by sub_1408D9E50.
//
// So two peers with identical streams and identical cats still roll different
// events the moment their used-event lists differ by one entry. The object is
// in the save (sub_1408DD2F0 is called only from save_adventure and
// ContinueAdventure), which is why a rejoin always "fixed" it and why nothing
// caught it in a session that never rejoined -- CATDATA and INVENTORY were the
// only run state the mod pushed, and this is neither.
//
// It also holds the per-node-type counters bumped on every return to the map
// (two 19-int arrays, one slot per MapNodeType).
//
// WHOLE OBJECT, VIA THE GAME'S OWN BIDIRECTIONAL SERIALIZER, for exactly the
// reasons CatData is done that way: sub_1408DD2F0 branches on ByteStream+0x00
// and carries its own version tag, so the host writes and the client reads with
// no format reimplemented here and nothing to keep in step when Tyler adds a
// field.
struct RunHistMsg {
    uint32_t size = 0;         // bytes of `data`
    uint64_t hash = 0;         // FNV-1a over those bytes
    uint8_t* data = nullptr;   // encode: borrowed. decode: owned by the receiver.
};

// A serialized run history is a few strings and two 19-int arrays. The bound is
// loose enough for a long run's used-event list and tight enough that a garbled
// length cannot make the receiver allocate wildly.
constexpr uint32_t kMaxRunHistBytes = 1u << 20;   // 1 MiB

// Both peers, at every map node: the meta layer's own hash.
//
// THE BATTLE LAYER HAS HAD A PER-TURN HASH SINCE VERSION 5 AND THE META LAYER
// HAD NOTHING, which is the asymmetry this closes. The battle layer earned its
// hash because lockstep fails silently; the meta layer is now synced by
// DETERMINISM plus a replicated choice, which fails silently in exactly the
// same way and had no check at all.
//
// Worse than nothing, in fact: the per-node CATDATA and INVENTORY pushes
// OVERWRITE a divergence rather than report it, so a meta-layer split surfaced
// later, somewhere else, as a battle desync. That is the shape of the
// unexplained turn-0 mismatch of 2026-08-25 (cluster A), which followed a node
// that resolved two level-ups and an event.
//
// TWO SAMPLE POINTS, because they answer different questions:
//
//   kNodePointEnter -- taken by both peers immediately before EnterNode. The
//       rng words are therefore the state the PREVIOUS node left behind, which
//       is the drift signal: EnterNode is about to overwrite it from
//       MapNode+0x118, so a disagreement here is the last moment it is
//       attributable to what just happened rather than to what is about to.
//   kNodePointEvent -- taken on the first WorldEvent::update of a screen. By
//       then the event has been chosen, so this one carries its NAME. Two peers
//       reporting different names is the reported bug, named outright, instead
//       of being inferred three screens later from an option-count refusal.
//
// SYMMETRIC, like CONTROL and unlike every other meta message: both peers send
// their own and check the other's. A host-authoritative hash could only ever
// report that the client disagreed, and it is the host that is authoritative
// about the run -- so the interesting direction is the one a host-only message
// cannot carry.
struct NodeHashMsg {
    uint64_t node_seed  = 0;    // which node this is about -- the same 64 bits
                                // battle_id and ChoiceMsg::node_seed use
    uint32_t node_index = 0;    // for the log line only; seed is the identity
    uint8_t  point      = 0;    // kNodePointEnter / kNodePointEvent
    uint64_t rng[4]     = {};   // TLS+0x178, the simulation stream
    uint64_t hist_hash  = 0;    // over the serialized run history (RunHistMsg)
    uint64_t cats_hash  = 0;    // over the run's cat ids, IN ORDER -- the list
                                // sub_1400AACD0 indexes to pick the event's cat
    uint32_t cat_count  = 0;
    uint64_t inv_hash   = 0;    // coins/food/boxes + the three bucket counts
    char     event[48]  = {};   // kNodePointEvent: WorldEvent+0x1A10. Else "".
};

constexpr uint8_t kNodePointEnter = 0;
constexpr uint8_t kNodePointEvent = 1;

struct HaltMsg {
    uint32_t turn = 0;
    char     reason[192] = {};
};

// THE DESYNC DUMP. Sent by both peers, once, at the moment a hash disagrees --
// and at no other time, which is what keeps it free.
//
// A hash says THAT two peers diverged and can never say what by. The cat table
// was already printed locally, but only into this peer's own log, so naming the
// field that moved meant getting both log files onto one screen and lining up
// forty-odd rows by eye. The state each peer hashed is a few kilobytes; sending
// it once, after the run is already lost, costs nothing and turns that into one
// line per differing field.
//
// It carries the state AS OF THE HASHED TURN, not as of now. That distinction
// is the whole reliability of the thing: a mismatch can be noticed when the
// peer's hash arrives, which is not necessarily the boundary we took ours at,
// and comparing a stale row against a fresh one manufactures differences that
// were never there.
//
// `stride` is sizeof(the sender's record). It is checked rather than assumed:
// two peers built from different revisions would otherwise reinterpret each
// other's bytes and print a confident diff of nonsense -- exactly the failure
// this message exists to prevent one level up.
constexpr uint32_t kMaxDumpCats  = 254;
constexpr uint32_t kMaxDumpBytes = kMaxDumpCats * 256u;

struct StateDumpMsg {
    uint64_t battle_id = 0;
    uint32_t turn      = 0;
    uint32_t count     = 0;          // records in `data`
    uint32_t stride    = 0;          // bytes per record, as the SENDER packs it
    uint32_t size      = 0;          // count * stride
    uint8_t* data      = nullptr;    // encode: borrowed. decode: owned.
};

inline uint32_t statedump_frame_size(const StateDumpMsg& m) { return m.size + 64; }

// MSG_UQD (proto 79): the digest of the unlock queries and save-property reads one ACTION made, sent by every peer at every action (mgmp_unlocks: unlockq_flush). The receiver compares it with its own for
// the same flush number: a difference is a battle that asked its save different things on the two peers, found inside the action rather than a turn later at a hash.
constexpr uint8_t MSG_UQD = 40;
struct UqdMsg {
    uint64_t battle = 0;
    uint32_t seq = 0, turn = 0, action = 0, n = 0;      // the flush number in this battle, where it was taken, the number of queries
    uint32_t kind[6] = {};                              // per kind: ability, passive, item, level, boss, property
    uint32_t digest = 0, first_rng = 0;
};
inline uint32_t enc_uqd(uint8_t* p, uint32_t cap, const UqdMsg& m) {
    if (!m.battle) return 0;
    Writer w(p, cap);
    w.u8v(MSG_UQD); w.u64v(m.battle); w.u32v(m.seq); w.u32v(m.turn); w.u32v(m.action); w.u32v(m.n);
    for (int k = 0; k < 6; ++k) w.u32v(m.kind[k]);
    w.u32v(m.digest); w.u32v(m.first_rng);
    return w.ok ? w.len : 0;
}
inline bool dec_uqd(Reader& r, UqdMsg& m) {
    m.battle = r.u64v(); m.seq = r.u32v(); m.turn = r.u32v(); m.action = r.u32v(); m.n = r.u32v();
    for (int k = 0; k < 6; ++k) m.kind[k] = r.u32v();
    m.digest = r.u32v(); m.first_rng = r.u32v();
    return r.ok && r.pos == r.len && m.battle != 0;
}

// MSG_RNGL (proto 80): what ONE ACTION drew from the shared stream on this peer: how many draws, a digest of their ORDER (function and call site, not the values) and a table of the call sites with
// the number of draws each made (bit 27 of a site key: the draw was made between actions, not inside an apply). Every peer sends its own at every action (mgmp_diag: rngl_flush); the receiver compares it with its own for the same flush number. A difference names the call sites whose
// draw counts differ -- the answer to "the stream drifted inside this action", which the per-action reseed line alone cannot give. A site is the return address of the draw as an RVA with the draw's
// function in the top four bits.
constexpr uint8_t  MSG_RNGL = 42;
constexpr uint32_t kRnglSites = 40;
struct RnglMsg {
    uint64_t battle = 0;
    uint32_t seq = 0, turn = 0, action = 0;
    uint32_t n = 0, digest = 0;                         // draws made INSIDE the game's apply-action calls in the window, digest of their order
    uint32_t n_loose = 0, digest_loose = 0;             // draws made between actions (the AI's decisions; on the owner's peer also the aim previews)
    uint8_t  sites = 0;                                 // entries used below
    uint32_t more = 0;                                  // distinct sites that did not fit
    uint32_t key[kRnglSites] = {};
    uint32_t count[kRnglSites] = {};
};
inline uint32_t enc_rngl(uint8_t* p, uint32_t cap, const RnglMsg& m) {
    if (!m.battle || m.sites > kRnglSites) return 0;
    Writer w(p, cap);
    w.u8v(MSG_RNGL); w.u64v(m.battle); w.u32v(m.seq); w.u32v(m.turn); w.u32v(m.action); w.u32v(m.n); w.u32v(m.digest); w.u32v(m.n_loose); w.u32v(m.digest_loose); w.u8v(m.sites); w.u32v(m.more);
    for (uint32_t i = 0; i < m.sites; ++i) { w.u32v(m.key[i]); w.u32v(m.count[i]); }
    return w.ok ? w.len : 0;
}
inline bool dec_rngl(Reader& r, RnglMsg& m) {
    m.battle = r.u64v(); m.seq = r.u32v(); m.turn = r.u32v(); m.action = r.u32v(); m.n = r.u32v(); m.digest = r.u32v(); m.n_loose = r.u32v(); m.digest_loose = r.u32v(); m.sites = r.u8v(); m.more = r.u32v();
    if (!r.ok || m.sites > kRnglSites || !m.battle) return false;
    for (uint32_t i = 0; i < m.sites; ++i) { m.key[i] = r.u32v(); m.count[i] = r.u32v(); }
    return r.ok && r.pos == r.len;
}

// MSG_DEEP (proto 80): one digest per unit of the values the state hash does NOT cover and the turn order and the damage depend on (the seven stats and their bonus, speed, the turn-order keys and base,
// maximum hp), taken at the turn boundary, before the host's board can repair anything. Compared on arrival and only ever LOGGED: it names the unit and the first turn a derived value differed.
constexpr uint8_t  MSG_DEEP = 43;
constexpr uint32_t kDeepUnits = 64;
struct DeepMsg {
    uint64_t battle = 0;
    uint32_t turn = 0;
    uint8_t  n = 0;
    uint32_t digest[kDeepUnits] = {};                   // 0: the unit could not be read
};
inline uint32_t enc_deep(uint8_t* p, uint32_t cap, const DeepMsg& m) {
    if (!m.battle || m.n > kDeepUnits) return 0;
    Writer w(p, cap);
    w.u8v(MSG_DEEP); w.u64v(m.battle); w.u32v(m.turn); w.u8v(m.n);
    for (uint32_t i = 0; i < m.n; ++i) w.u32v(m.digest[i]);
    return w.ok ? w.len : 0;
}
inline bool dec_deep(Reader& r, DeepMsg& m) {
    m.battle = r.u64v(); m.turn = r.u32v(); m.n = r.u8v();
    if (!r.ok || m.n > kDeepUnits || !m.battle) return false;
    for (uint32_t i = 0; i < m.n; ++i) m.digest[i] = r.u32v();
    return r.ok && r.pos == r.len;
}

// MSG_SETTING (proto 82): a game setting that has to be the HOST's on every peer because it changes what the animations and the turn layer do over time -- today only the combat speed. Authored by the host
// when its value differs from what it last sent (and again when a player joins); a client uses the value in place of its own, for the reads the game makes, until the session ends.
constexpr uint8_t MSG_SETTING = 45;
constexpr uint8_t kSettingCombatSpeed = 1;
struct SettingMsg {
    uint8_t id = 0;
    float   value = 0.0f;
};
inline uint32_t enc_setting(uint8_t* p, uint32_t cap, const SettingMsg& m) {
    if (m.id != kSettingCombatSpeed || !(m.value > 0.0f) || m.value > 1000.0f) return 0;
    Writer w(p, cap);
    w.u8v(MSG_SETTING); w.u8v(m.id); w.u32v(*(const uint32_t*)&m.value);
    return w.ok ? w.len : 0;
}
inline bool dec_setting(Reader& r, SettingMsg& m) {
    m.id = r.u8v();
    const uint32_t bits = r.u32v();
    memcpy(&m.value, &bits, sizeof(bits));
    return r.ok && r.pos == r.len && m.id == kSettingCombatSpeed && m.value > 0.0f && m.value <= 1000.0f;
}

// MSG_CHAT (proto 81): one line of text a player typed (Enter opens the box, Enter again sends). Authored by every peer and relayed to the others; the receiver keeps it in its history and shows it for ten
// seconds. The name is the sender's lobby name, which is only a label: nothing here is trusted for anything but being shown (drawn as plain text, never interpreted).
constexpr uint8_t  MSG_CHAT = 44;
constexpr uint32_t kChatNameMax = 32, kChatTextMax = 200;
struct ChatMsg {
    char name[kChatNameMax] = {};
    char text[kChatTextMax] = {};
};
inline uint32_t enc_chat(uint8_t* p, uint32_t cap, const ChatMsg& m) {
    if (!m.text[0]) return 0;
    Writer w(p, cap);
    w.u8v(MSG_CHAT); w.str(m.name); w.str(m.text);
    return w.ok ? w.len : 0;
}
inline bool dec_chat(Reader& r, ChatMsg& m) {
    r.str(m.name, sizeof(m.name)); r.str(m.text, sizeof(m.text));
    return r.ok && r.pos == r.len && m.text[0];
}

// MSG_PROPS (proto 79): the host's whole table of integer save properties, as (hash of the key, value) pairs, in chunks (a chunk of kPropsChunk pairs). The client answers any property a battle
// or an event reads with the host's value (mgmp_unlocks: event_property_answer), not only the 55 the events were known to read.
constexpr uint8_t  MSG_PROPS = 41;
constexpr uint32_t kPropsChunk = 100, kPropsMax = 4096;
struct PropsMsg {
    uint32_t epoch = 0, total = 0, first = 0;
    uint8_t  count = 0;
    uint32_t hash[kPropsChunk] = {};
    int32_t  value[kPropsChunk] = {};
};
inline uint32_t enc_props(uint8_t* p, uint32_t cap, const PropsMsg& m) {
    if (!m.epoch || m.count > kPropsChunk || m.total > kPropsMax || m.first + m.count > m.total) return 0;
    Writer w(p, cap);
    w.u8v(MSG_PROPS); w.u32v(m.epoch); w.u32v(m.total); w.u32v(m.first); w.u8v(m.count);
    for (uint32_t i = 0; i < m.count; ++i) { w.u32v(m.hash[i]); w.i32v(m.value[i]); }
    return w.ok ? w.len : 0;
}
inline bool dec_props(Reader& r, PropsMsg& m) {
    m.epoch = r.u32v(); m.total = r.u32v(); m.first = r.u32v(); m.count = r.u8v();
    if (!r.ok || m.count > kPropsChunk || m.total > kPropsMax || m.first + m.count > m.total || !m.epoch) return false;
    for (uint32_t i = 0; i < m.count; ++i) { m.hash[i] = r.u32v(); m.value[i] = r.i32v(); }
    return r.ok && r.pos == r.len;
}

// MSG_AUDIT (proto 76): the PRE-BATTLE AUDIT. At the first turn boundary of a battle, before anything is hashed or overwritten, each peer says what every player
// cat looks like to it (a fingerprint and the text it is a fingerprint of), so that either log alone shows whether the two peers started from the same cats.
constexpr uint8_t  MSG_AUDIT = 39;
constexpr uint32_t kAuditMaxCats = 8;
constexpr uint32_t kAuditText = 232;
struct AuditCat { uint64_t id = 0, fp = 0; char text[kAuditText] = {}; };
struct AuditMsg {
    uint64_t battle_id = 0;
    uint8_t  n = 0;
    AuditCat cat[kAuditMaxCats];
};
inline uint32_t enc_audit(uint8_t* p, uint32_t cap, const AuditMsg& m) {
    if (m.n > kAuditMaxCats) return 0;
    Writer w(p, cap);
    w.u8v(MSG_AUDIT); w.u64v(m.battle_id); w.u8v(m.n);
    for (unsigned i = 0; i < m.n; ++i) { w.u64v(m.cat[i].id); w.u64v(m.cat[i].fp); w.raw((const uint8_t*)m.cat[i].text, kAuditText); }
    return w.ok ? w.len : 0;
}
inline bool dec_audit(Reader& r, AuditMsg& m) {
    m.battle_id = r.u64v(); m.n = r.u8v();
    if (!r.ok || m.n > kAuditMaxCats) return false;
    for (unsigned i = 0; i < m.n; ++i) {
        m.cat[i].id = r.u64v(); m.cat[i].fp = r.u64v();
        if (!r.ok || r.pos + kAuditText > r.len) return false;
        memcpy(m.cat[i].text, r.buf + r.pos, kAuditText); r.pos += kAuditText;
        m.cat[i].text[kAuditText - 1] = 0;
    }
    return r.ok && r.pos == r.len;
}

// MSG_PEERLOG (proto 76): the tail of the sender's own log, sent when a battle desyncs or halts (and when a checkpoint fails), so the receiver
// writes it into ITS log. In an open test one player of a room may be the only one who uploads anything; this makes that one log enough.
constexpr uint8_t  MSG_PEERLOG = 38;
constexpr uint32_t kMaxPeerLogBytes = 256u * 1024u;
struct PeerLogMsg {
    uint64_t battle_id = 0;
    uint32_t turn      = 0;
    char     why[96]   = {};         // what made the sender share it
    uint32_t size      = 0;          // bytes of text in `data` (lines separated by '\n')
    uint8_t* data      = nullptr;    // encode: borrowed. decode: owned.
};
inline uint32_t peerlog_frame_size(const PeerLogMsg& m) { return m.size + 128; }
inline uint32_t enc_peerlog(uint8_t* p, uint32_t cap, const PeerLogMsg& m) {
    if (!m.data || m.size == 0 || m.size > kMaxPeerLogBytes) return 0;
    Writer w(p, cap);
    w.u8v(MSG_PEERLOG);
    w.u64v(m.battle_id); w.u32v(m.turn);
    w.raw((const uint8_t*)m.why, sizeof(m.why));
    w.u32v(m.size); w.raw(m.data, m.size);
    return w.ok ? w.len : 0;
}
inline bool dec_peerlog(Reader& r, PeerLogMsg& m) {
    m.data = nullptr;
    m.battle_id = r.u64v(); m.turn = r.u32v();
    if (!r.ok || r.pos + sizeof(m.why) > r.len) return false;
    memcpy(m.why, r.buf + r.pos, sizeof(m.why)); r.pos += sizeof(m.why);
    m.why[sizeof(m.why) - 1] = 0;
    m.size = r.u32v();
    if (!r.ok || m.size == 0 || m.size > kMaxPeerLogBytes || r.pos + m.size > r.len) return false;
    uint8_t* buf = (uint8_t*)malloc(m.size + 1);
    if (!buf) return false;
    memcpy(buf, r.buf + r.pos, m.size); buf[m.size] = 0;
    r.pos += m.size;
    m.data = buf;
    return true;
}

inline uint32_t enc_statedump(uint8_t* p, uint32_t cap, const StateDumpMsg& m) {
    if (!m.data || m.size == 0 || m.size > kMaxDumpBytes) return 0;
    Writer w(p, cap);
    w.u8v(MSG_STATEDUMP);
    w.u64v(m.battle_id); w.u32v(m.turn);
    w.u32v(m.count); w.u32v(m.stride); w.u32v(m.size);
    w.raw(m.data, m.size);
    return w.ok ? w.len : 0;
}

// Allocates m.data on success and nothing on any failure, so a partial frame
// cannot leak. Same contract as dec_catdata.
inline bool dec_statedump(Reader& r, StateDumpMsg& m) {
    m.data = nullptr;
    m.battle_id = r.u64v();
    m.turn      = r.u32v();
    m.count     = r.u32v();
    m.stride    = r.u32v();
    m.size      = r.u32v();
    if (!r.ok) return false;
    if (m.count == 0 || m.count > kMaxDumpCats) return false;
    if (m.stride == 0 || m.size == 0 || m.size > kMaxDumpBytes) return false;
    if (m.size != m.count * m.stride) return false;
    if (r.pos + m.size > r.len) return false;
    uint8_t* buf = (uint8_t*)malloc(m.size);
    if (!buf) return false;
    memcpy(buf, r.buf + r.pos, m.size);
    r.pos += m.size;
    m.data = buf;
    return true;
}

// --- encode -----------------------------------------------------------------

inline uint32_t enc_hello(uint8_t* p, uint32_t cap, const Hello& h) {
    Writer w(p, cap);
    w.u8v(MSG_HELLO);
    w.u32v(h.proto); w.u64v(h.gpak_hash); w.u64v(h.build_hash); w.str(h.name); w.u8v(h.rules);
    return w.ok ? w.len : 0;
}

inline uint32_t enc_welcome(uint8_t* p, uint32_t cap, const Welcome& v) {
    Writer w(p, cap);
    w.u8v(MSG_WELCOME);
    for (int i = 0; i < 4; ++i) w.u64v(v.rng_state[i]);
    w.u8v(v.cat_count);
    for (uint8_t i = 0; i < v.cat_count && i < 32; ++i) w.u8v(v.cats[i]);
    return w.ok ? w.len : 0;
}

inline uint32_t enc_action(uint8_t* p, uint32_t cap, const ActionMsg& a) {
    Writer w(p, cap);
    w.u8v(MSG_ACTION);
    w.u64v(a.battle_id);
    w.u32v(a.turn);
    w.u8v(a.actor); w.u8v(a.type); w.u8v(a.slot_kind); w.u8v(a.slot_index);
    w.u8v(a.b30);   w.u8v(a.b31);
    w.i32v(a.tx); w.i32v(a.ty); w.i32v(a.dx); w.i32v(a.dy);
    w.str(a.gon);
    return w.ok ? w.len : 0;
}

inline uint32_t enc_hash(uint8_t* p, uint32_t cap, const HashMsg& h) {
    Writer w(p, cap);
    w.u8v(MSG_HASH);
    w.u64v(h.battle_id);
    w.u32v(h.turn); w.u64v(h.rng_hash); w.u64v(h.state_hash);
    w.u32v(h.queue_depth); w.u32v(h.queue_sig);
    return w.ok ? w.len : 0;
}

inline bool valid_chapter(const ChapterMsg& m) {
    return m.generation != 0 &&
        ((m.kind == kChapterReady && m.act == 0 && m.difficulty == 0) ||
         (m.kind == kChapterSelect && m.act >= 1 && m.act <= 3 &&
          m.difficulty >= 0 && m.difficulty <= 1000));
}
inline uint32_t enc_chapter(uint8_t* p, uint32_t cap, const ChapterMsg& m) {
    if (!valid_chapter(m)) return 0;
    Writer w(p, cap);
    w.u8v(MSG_CHAPTER); w.u8v(m.kind); w.u32v(m.generation);
    w.u8v(m.act); w.i32v(m.difficulty); w.u32v(m.mapflags);
    return w.ok ? w.len : 0;
}
inline bool dec_chapter(Reader& r, ChapterMsg& m) {
    m.kind = r.u8v(); m.generation = r.u32v();
    m.act = r.u8v(); m.difficulty = r.i32v(); m.mapflags = r.u32v();
    return r.ok && r.pos == r.len && valid_chapter(m);
}

inline bool valid_mapseeds(const MapSeedsMsg& m) {
    return m.kind <= kMapSeedsFromClient && m.epoch != 0 && m.total >= 1 && m.total <= kMapSeedsMaxNodes && m.count >= 1 && m.count <= kMapSeedsChunk &&
           m.first <= m.total && m.count <= m.total - m.first;
}
inline uint32_t enc_mapseeds(uint8_t* p, uint32_t cap, const MapSeedsMsg& m) {
    if (!valid_mapseeds(m)) return 0;
    Writer w(p, cap);
    w.u8v(MSG_MAPSEEDS); w.u8v(m.kind); w.u32v(m.epoch); w.u32v(m.total); w.u32v(m.first); w.u8v(m.count);
    for (uint32_t i = 0; i < m.count; ++i) {
        w.u8v(m.nodes[i].type);
        for (int k = 0; k < 4; ++k) w.u64v(m.nodes[i].seed[k]);
        w.u32v(m.nodes[i].flags);
    }
    return w.ok ? w.len : 0;
}
inline bool dec_mapseeds(Reader& r, MapSeedsMsg& m) {
    m.kind = r.u8v(); m.epoch = r.u32v(); m.total = r.u32v(); m.first = r.u32v(); m.count = r.u8v();
    if (!r.ok || m.count > kMapSeedsChunk) return false;
    for (uint32_t i = 0; i < m.count; ++i) {
        m.nodes[i].type = r.u8v();
        for (int k = 0; k < 4; ++k) m.nodes[i].seed[k] = r.u64v();
        m.nodes[i].flags = r.u32v();
    }
    return r.ok && r.pos == r.len && valid_mapseeds(m);
}

// THE HOST'S BOARD (proto 61). A battle is built by each peer from its OWN save, and the unit kinds, tiles and numbers that depend on per-save data
// (a pickup's roll, a corpse, a placement draw ...) came out differently on the two machines -- measured 2026-10-02: 34 of 36 units equal and two
// coins on other tiles, which was a turn-0 halt. The host now sends, once per battle at the first turn boundary, every NON-PLAYER unit of its roster
// (index, kind, hp / max hp / shield, tile, facing) and the state of the shared simulation stream; a client overwrites its own board with them
// before the first turn hash and carries on from the host's stream, so the enemies' decisions are the host's as well (mgmp_lockstep: board_*).
constexpr uint8_t  MSG_BOARD = 36;
constexpr uint32_t kBoardChunk = 8;       // units per message (16 until proto 75, when each unit gained a name of up to kBoardDefLen bytes)
constexpr uint32_t kBoardDefLen = 40;     // the longest definition name a unit row carries (with its NUL); a longer name travels as empty and the unit is never replaced
constexpr uint32_t kBoardMax = 254;       // the roster cap (kMaxCats in mgmp_lockstep)
constexpr uint8_t  kBoardDead = 1, kBoardLinked = 2, kBoardHuman = 4, kBoardGone = 8, kBoardChampion = 16, kBoardRemoved = 32;   // kBoardHuman: a player's cat on the host -- only its turn-order keys are meant to be taken over; kBoardGone: the unit has left the host's live list (its object is freed: NOTHING in the unit's row is state, and a peer must not read or write it)
struct BoardUnit {
    uint8_t  index = 0;      // the unit's index in the battle roster (the roster order is the same on every peer)
    uint32_t ident = 0;      // hash of the unit's kind (Character+0x248, the authored type name)
    int32_t  hp = 0, shield = 0, maxhp = 0;
    uint8_t  flags = 0;      // kBoardDead / kBoardLinked
    int32_t  tx = 0, ty = 0; // the tile
    int32_t  fx = 0, fy = 0; // the facing
    // The turn order is a shuffle of the character list (the shared stream) and a sort on these two keys (sub_1408E3C20): Character+0x954 (2*speed + bonus) and +0x958 (a random
    // number drawn when the character was created). A peer whose keys differ plays another turn order (2026-10-03: the host's mini-boss first, the client's own cat first).
    int32_t  key_a = 0, key_b = 0, speed = 0;   // Character+0x954, +0x958, and the speed stat +0x5CC (the input of the first, for the log)
    // +0x954 is RECOMPUTED (recompute_stats: 2*speed + [+0x5DC]) whenever the unit's stats are touched, so writing it alone is undone at the next recompute; +0x5DC is the base the game
    // sets once at creation (base_initiative + the random initiative_variation), and it is what has to be the host's (proto 70).
    int32_t  init_base = 0;                      // Character+0x5DC
    // proto 75: the unit's DEFINITION name -- *(Character+0x240)+0x88, the string the game's own spawn / transform take ('Leaper', not the display key 'ENEMY_LEAPER_NAME' in +0x248, which made the
    // game stop with "No Character Named ..."). Empty for a player's cat and for a unit that has left. kBoardChampion: the unit is a champion (Character+0xCDE; the definition name is the base one).
    char     def[kBoardDefLen] = {};
    // proto 77: ONLY on the wire for a player's cat (flags & kBoardHuman): Character+0x5BC (str dex con int spd cha lck) and the bonus at +0x5E8. Zero for everything else.
    int32_t  stat[7] = {};
    int32_t  stat_bonus = 0;
};
struct BoardMsg {
    uint64_t battle = 0;     // the battle id (the node seed both peers share)
    uint32_t turn = 0;       // the turn boundary the snapshot was taken at (0 = battle start; one per boundary since proto 71)
    uint32_t cats = 0;       // the roster size on the host
    uint32_t total = 0;      // non-player units over all chunks
    uint32_t first = 0;      // index into the unit list of units[0]
    uint8_t  count = 0;      // entries used in units[]
    uint64_t rng[4] = {};    // the host's simulation stream at that boundary
    BoardUnit units[kBoardChunk];
};
// What the receive thread has put together from the chunks (net_host_board).
struct BoardAssembled {
    uint64_t battle = 0;
    uint32_t turn = 0;
    uint32_t cats = 0, total = 0, got = 0;
    bool     complete = false;
    uint64_t rng[4] = {};
    bool     have[kBoardMax] = {};
    BoardUnit unit[kBoardMax];
};

inline bool valid_board(const BoardMsg& m) {
    if (m.battle == 0 || m.cats > kBoardMax || m.total > m.cats || m.count > kBoardChunk || m.first > m.total ||
        m.count > m.total - m.first || (m.count == 0 && m.total != 0)) return false;
    for (uint32_t i = 0; i < m.count; ++i) if (m.units[i].index >= m.cats) return false;
    return true;
}
inline uint32_t enc_board(uint8_t* p, uint32_t cap, const BoardMsg& m) {
    if (!valid_board(m)) return 0;
    Writer w(p, cap);
    w.u8v(MSG_BOARD); w.u64v(m.battle); w.u32v(m.turn); w.u32v(m.cats); w.u32v(m.total); w.u32v(m.first); w.u8v(m.count);
    for (int k = 0; k < 4; ++k) w.u64v(m.rng[k]);
    for (uint32_t i = 0; i < m.count; ++i) {
        const BoardUnit& u = m.units[i];
        w.u8v(u.index); w.u32v(u.ident); w.i32v(u.hp); w.i32v(u.shield); w.i32v(u.maxhp); w.u8v(u.flags);
        w.i32v(u.tx); w.i32v(u.ty); w.i32v(u.fx); w.i32v(u.fy); w.i32v(u.key_a); w.i32v(u.key_b); w.i32v(u.speed); w.i32v(u.init_base);
        w.str(u.def);
        if (u.flags & kBoardHuman) { for (int k = 0; k < 7; ++k) w.i32v(u.stat[k]); w.i32v(u.stat_bonus); }
    }
    return w.ok ? w.len : 0;
}
inline bool dec_board(Reader& r, BoardMsg& m) {
    m.battle = r.u64v(); m.turn = r.u32v(); m.cats = r.u32v(); m.total = r.u32v(); m.first = r.u32v(); m.count = r.u8v();
    for (int k = 0; k < 4; ++k) m.rng[k] = r.u64v();
    if (!r.ok || m.count > kBoardChunk) return false;
    for (uint32_t i = 0; i < m.count; ++i) {
        BoardUnit& u = m.units[i];
        u.index = r.u8v(); u.ident = r.u32v(); u.hp = r.i32v(); u.shield = r.i32v(); u.maxhp = r.i32v(); u.flags = r.u8v();
        u.tx = r.i32v(); u.ty = r.i32v(); u.fx = r.i32v(); u.fy = r.i32v(); u.key_a = r.i32v(); u.key_b = r.i32v(); u.speed = r.i32v(); u.init_base = r.i32v();
        r.str(u.def, kBoardDefLen);
        for (int k = 0; k < 7; ++k) u.stat[k] = 0;
        u.stat_bonus = 0;
        if (u.flags & kBoardHuman) { for (int k = 0; k < 7; ++k) u.stat[k] = r.i32v(); u.stat_bonus = r.i32v(); }
    }
    return r.ok && r.pos == r.len && valid_board(m);
}

inline bool valid_unlocks(const UnlocksMsg& m) {
    return m.epoch != 0 && m.n_abilities <= 32 && m.n_passives <= 32 && m.n_items <= 128 && m.n_levels <= 8 && m.n_bosses <= 8 && m.n_events <= 64 && m.n_spawn <= 8 && m.n_level_name < sizeof(m.level_name) && m.n_pending[0] <= kPendingMax && m.n_pending[1] <= kPendingMax && m.n_pending[2] <= kPendingMax && m.n_weather <= kWeatherNames;
}
inline uint32_t enc_unlocks(uint8_t* p, uint32_t cap, const UnlocksMsg& m) {
    if (!valid_unlocks(m)) return 0;
    Writer w(p, cap);
    w.u8v(MSG_UNLOCKS); w.u32v(m.epoch); w.u32v(m.abilities); w.u32v(m.passives);
    w.raw(m.items, sizeof(m.items)); w.u8v(m.levels); w.u8v(m.bosses);
    w.u8v(m.n_abilities); w.u8v(m.n_passives); w.u8v(m.n_items); w.u8v(m.n_levels); w.u8v(m.n_bosses);
    w.u8v(m.n_events);
    for (uint32_t i = 0; i < m.n_events; ++i) w.i32v(m.events[i]);
    w.u8v(m.n_spawn);
    for (uint32_t i = 0; i < m.n_spawn; ++i) { w.u64v(m.spawn_ids[i]); w.i32v(m.spawn_keys[i]); }
    w.u64v(m.level_node); w.u8v(m.n_level_name);
    for (uint32_t i = 0; i < m.n_level_name; ++i) w.u8v((uint8_t)m.level_name[i]);
    for (uint32_t q = 0; q < kPendingQueues; ++q) {
        w.u8v(m.n_pending[q]);
        for (uint32_t i = 0; i < m.n_pending[q]; ++i) { const PendingEnemy& e = m.pending[q][i]; w.u32v(e.ident); w.i32v(e.remaining); w.i32v(e.hp); w.i32v(e.mode); }
    }
    w.u8v(m.n_weather);
    for (uint32_t i = 0; i < m.n_weather; ++i) { const uint8_t len = (uint8_t)strnlen(m.weather[i], kWeatherLen - 1); w.u8v(len); for (uint8_t k = 0; k < len; ++k) w.u8v((uint8_t)m.weather[i][k]); }
    w.u8v(m.build_info);
        for (int f = 0; f < 2; ++f) {      // proto 78
        w.u8v(m.n_classes[f]);
        for (uint32_t i = 0; m.n_classes[f] != 255 && i < m.n_classes[f] && i < kClassMax; ++i) {
            const uint8_t len = (uint8_t)strnlen(m.classes[f][i], kClassLen - 1);
            w.u8v(len);
            for (uint8_t k = 0; k < len; ++k) w.u8v((uint8_t)m.classes[f][i][k]);
        }
    }
    return w.ok ? w.len : 0;
}
inline bool dec_unlocks(Reader& r, UnlocksMsg& m) {
    m.epoch = r.u32v(); m.abilities = r.u32v(); m.passives = r.u32v();
    for (size_t i = 0; i < sizeof(m.items); ++i) m.items[i] = r.u8v();
    m.levels = r.u8v(); m.bosses = r.u8v();
    m.n_abilities = r.u8v(); m.n_passives = r.u8v(); m.n_items = r.u8v(); m.n_levels = r.u8v(); m.n_bosses = r.u8v();
    m.n_events = r.u8v();
    if (m.n_events > 64) return false;
    for (uint32_t i = 0; i < m.n_events; ++i) m.events[i] = r.i32v();
    m.n_spawn = r.u8v();
    if (m.n_spawn > 8) return false;
    for (uint32_t i = 0; i < m.n_spawn; ++i) { m.spawn_ids[i] = r.u64v(); m.spawn_keys[i] = r.i32v(); }
    m.level_node = r.u64v(); m.n_level_name = r.u8v();
    if (m.n_level_name >= sizeof(m.level_name)) return false;
    for (uint32_t i = 0; i < m.n_level_name; ++i) m.level_name[i] = (char)r.u8v();
    m.level_name[m.n_level_name] = 0;
    for (uint32_t q = 0; q < kPendingQueues; ++q) {
        m.n_pending[q] = r.u8v();
        if (m.n_pending[q] > kPendingMax) return false;
        for (uint32_t i = 0; i < m.n_pending[q]; ++i) { PendingEnemy& e = m.pending[q][i]; e.ident = r.u32v(); e.remaining = r.i32v(); e.hp = r.i32v(); e.mode = r.i32v(); }
    }
    m.n_weather = r.u8v();
    if (m.n_weather > kWeatherNames) return false;
    for (uint32_t i = 0; i < m.n_weather; ++i) { const uint8_t len = r.u8v(); if (len >= kWeatherLen) return false; for (uint8_t k = 0; k < len; ++k) m.weather[i][k] = (char)r.u8v(); m.weather[i][len] = 0; }
    m.build_info = r.u8v();
    for (int f = 0; f < 2; ++f) {
        m.n_classes[f] = r.u8v();
        if (m.n_classes[f] != 255 && m.n_classes[f] > kClassMax) return false;
        for (uint32_t i = 0; m.n_classes[f] != 255 && i < m.n_classes[f]; ++i) {
            const uint8_t len = r.u8v();
            if (len >= kClassLen) return false;
            for (uint8_t k = 0; k < len; ++k) m.classes[f][i][k] = (char)r.u8v();
            m.classes[f][i][len] = 0;
        }
    }
    return r.ok && r.pos == r.len && valid_unlocks(m);
}

inline uint32_t enc_chapterseed(uint8_t* p, uint32_t cap, const ChapterSeedMsg& m) {
    if (m.generation == 0) return 0;
    Writer w(p, cap);
    w.u8v(MSG_CHAPTERSEED); w.u32v(m.generation);
    for (int i = 0; i < 4; ++i) w.u64v(m.rng[i]);
    return w.ok ? w.len : 0;
}
inline bool dec_chapterseed(Reader& r, ChapterSeedMsg& m) {
    m.generation = r.u32v();
    for (int i = 0; i < 4; ++i) m.rng[i] = r.u64v();
    return r.ok && r.pos == r.len && m.generation != 0;
}

inline uint32_t enc_catdigest(uint8_t* p, uint32_t cap, const CatDigestMsg& m) {
    if (m.count > kCatDigestMax) return 0;
    Writer w(p, cap);
    w.u8v(MSG_CATDIGEST); w.u8v(m.count);
    for (uint32_t i = 0; i < m.count; ++i) {
        if (!m.ids[i]) return 0;
        w.u64v(m.ids[i]); w.u64v(m.hashes[i]); w.u32v(m.sizes[i]);
    }
    return w.ok ? w.len : 0;
}
inline bool dec_catdigest(Reader& r, CatDigestMsg& m) {
    m.count = r.u8v();
    if (!r.ok || m.count > kCatDigestMax) return false;
    for (uint32_t i = 0; i < m.count; ++i) {
        m.ids[i] = r.u64v(); m.hashes[i] = r.u64v(); m.sizes[i] = r.u32v();
        if (!m.ids[i]) return false;
    }
    return r.ok && r.pos == r.len;
}

// `ncand` is how many candidate slots go on the wire. The network speaks all of them; the journal on
// disk (mgmp_checkpoint's encode_entry) keeps its version-39 shape -- two, always empty there -- so the
// files players already hold stay readable.
inline uint32_t enc_checkpoint(uint8_t* p, uint32_t cap, const CheckpointMsg& m, unsigned ncand = kCheckpointCandidates, bool alt = true) {
    if (m.kind > kCheckpointFinished || m.count > kMaxPeers || m.mode > 4) return 0;
    Writer w(p, cap);
    w.u8v(MSG_CHECKPOINT); w.u8v(m.kind); w.u8v(m.slot); w.u8v(m.mode); w.u8v(m.count);
    w.u64v(m.identity); w.u64v(m.run); w.u64v(m.seq); w.u64v(m.stamp); w.u64v(m.map); w.u64v(m.token);
    for (unsigned i=0;i<kMaxPeers;++i) {
        w.u8v(m.peers[i]); w.u8v(m.slots[i]); w.u64v(m.identities[i]); w.u64v(m.hashes[i]);
    }
    if (ncand == kCheckpointCandidates) {
        // THE WIRE (proto 76): a count, then that many saves with their flags. The candidates are packed from index 0; the first empty one ends the list.
        unsigned n = 0;
        while (n < kCheckpointCandidates && m.candidates[n].run) ++n;
        w.u8v((uint8_t)n);
        for (unsigned i = 0; i < n; ++i) {
            const auto& c = m.candidates[i];
            w.u64v(c.run); w.u64v(c.seq); w.u64v(c.stamp); w.u64v(c.certificate); w.u8v(c.flags);
        }
    } else {
        // THE JOURNAL ON DISK: its fixed version-39 shape, no flags (an entry keeps its own flags in a section of its own)
        for (unsigned i = 0; i < ncand && i < kCheckpointCandidates; ++i) {
            const auto& c = m.candidates[i];
            w.u64v(c.run); w.u64v(c.seq); w.u64v(c.stamp); w.u64v(c.certificate);
        }
    }
    if (alt) { w.u64v(m.identity2); for (unsigned i = 0; i < kMaxPeers; ++i) w.u64v(m.identities2[i]); }
    return w.ok ? w.len : 0;
}
inline bool dec_checkpoint(Reader& r, CheckpointMsg& m, unsigned ncand = kCheckpointCandidates, bool alt = true) {
    m.kind=r.u8v(); m.slot=r.u8v(); m.mode=r.u8v(); m.count=r.u8v();
    m.identity=r.u64v(); m.run=r.u64v(); m.seq=r.u64v(); m.stamp=r.u64v(); m.map=r.u64v(); m.token=r.u64v();
    for (unsigned i=0;i<kMaxPeers;++i) {
        m.peers[i]=r.u8v(); m.slots[i]=r.u8v(); m.identities[i]=r.u64v(); m.hashes[i]=r.u64v();
    }
    if (ncand == kCheckpointCandidates) {
        const unsigned n = r.u8v();
        if (n > kCheckpointCandidates) return false;
        for (unsigned i = 0; i < n; ++i) {
            auto& c = m.candidates[i];
            c.run=r.u64v(); c.seq=r.u64v(); c.stamp=r.u64v(); c.certificate=r.u64v(); c.flags=r.u8v();
        }
    } else {
        for (unsigned i = 0; i < ncand && i < kCheckpointCandidates; ++i) {
            auto& c = m.candidates[i];
            c.run=r.u64v(); c.seq=r.u64v(); c.stamp=r.u64v(); c.certificate=r.u64v();
        }
    }
    if (alt) { m.identity2=r.u64v(); for (unsigned i = 0; i < kMaxPeers; ++i) m.identities2[i]=r.u64v(); }
    return r.ok && r.pos==r.len && m.kind<=kCheckpointFinished && m.count<=kMaxPeers && m.mode<=4;
}

inline uint32_t setup_frame_size(const SetupMsg& m) {
    uint32_t n = 15 + 1 + 5*kMaxPeers; // header + frozen membership + request generations
    if (m.count > kSetupMaxCats) return 0;
    for (uint8_t i = 0; i < m.count; ++i) {
        if (m.cats[i].size > kMaxCatBytes) return 0;
        n += 20 + m.cats[i].size; // id + size + hash + payload
    }
    return n;
}

inline uint32_t enc_setup(uint8_t* p, uint32_t cap, const SetupMsg& m) {
    if (m.count > kSetupMaxCats) return 0;
    Writer w(p, cap);
    w.u8v(MSG_SETUP); w.u8v(m.kind); w.u8v(m.count); w.u32v(m.generation);
    w.u64v(m.checkpoint);
    w.u8v(m.members);
    for(unsigned i=0;i<kMaxPeers;++i) { w.u8v(m.peers[i]); w.u32v(m.requests[i]); }
    for (uint8_t i = 0; i < m.count; ++i) {
        const SetupCat& c = m.cats[i];
        if (!c.id || !c.data || !c.size || c.size > kMaxCatBytes) return 0;
        w.u64v(c.id); w.u32v(c.size); w.u64v(c.hash); w.raw(c.data, c.size);
    }
    return w.ok ? w.len : 0;
}

inline uint32_t catdata_frame_size(const CatDataMsg& m) { return m.size + 64; }

inline uint32_t enc_catdata(uint8_t* p, uint32_t cap, const CatDataMsg& m) {
    if (!m.data || m.size == 0 || m.size > kMaxCatBytes) return 0;
    Writer w(p, cap);
    w.u8v(MSG_CATDATA);
    w.u64v(m.id);
    w.u32v(m.size);
    w.u64v(m.hash);
    w.raw(m.data, m.size);
    return w.ok ? w.len : 0;
}

inline uint32_t runhist_frame_size(const RunHistMsg& m) { return m.size + 64; }

inline uint32_t enc_runhist(uint8_t* p, uint32_t cap, const RunHistMsg& m) {
    if (!m.data || m.size == 0 || m.size > kMaxRunHistBytes) return 0;
    Writer w(p, cap);
    w.u8v(MSG_RUNHIST);
    w.u32v(m.size);
    w.u64v(m.hash);
    w.raw(m.data, m.size);
    return w.ok ? w.len : 0;
}

inline uint32_t enc_nodehash(uint8_t* p, uint32_t cap, const NodeHashMsg& m) {
    Writer w(p, cap);
    w.u8v(MSG_NODEHASH);
    w.u64v(m.node_seed); w.u32v(m.node_index); w.u8v(m.point);
    for (int i = 0; i < 4; ++i) w.u64v(m.rng[i]);
    w.u64v(m.hist_hash); w.u64v(m.cats_hash); w.u32v(m.cat_count);
    w.u64v(m.inv_hash);
    w.str(m.event);
    return w.ok ? w.len : 0;
}

inline uint32_t inventory_frame_size(const InventoryMsg& m) {
    uint32_t n = 64;
    for (uint32_t i = 0; i < kInvBuckets; ++i) n += m.size[i];
    return n;
}

inline uint32_t enc_inventory(uint8_t* p, uint32_t cap, const InventoryMsg& m) {
    for (uint32_t i = 0; i < kInvBuckets; ++i) {
        if (m.size[i] > kMaxInvBytes) return 0;
        if (m.size[i] && !m.data[i])  return 0;   // a length with no bytes
    }
    Writer w(p, cap);
    w.u8v(MSG_INVENTORY);
    w.i32v(m.coins); w.i32v(m.food); w.i32v(m.boxes);
    w.u64v(m.hash);
    // Every length first, then every payload. Keeping the sizes contiguous
    // lets the decoder validate the whole frame's arithmetic before it
    // allocates anything, so a garbled length cannot make it allocate one
    // buffer and then discover the next is impossible.
    for (uint32_t i = 0; i < kInvBuckets; ++i) w.u32v(m.size[i]);
    for (uint32_t i = 0; i < kInvBuckets; ++i)
        if (m.size[i]) w.raw(m.data[i], m.size[i]);
    return w.ok ? w.len : 0;
}

// Allocates m.data[*] on success and the caller owns them from that moment. On
// any failure nothing stays allocated -- including buffers taken for earlier
// buckets before a later one proved impossible -- so a partial frame cannot
// leak. Same contract as dec_savefile and dec_catdata.
inline bool dec_inventory(Reader& r, InventoryMsg& m) {
    for (uint32_t i = 0; i < kInvBuckets; ++i) { m.data[i] = nullptr; m.size[i] = 0; }
    m.coins = r.i32v(); m.food = r.i32v(); m.boxes = r.i32v();
    m.hash  = r.u64v();
    if (!r.ok) return false;

    uint32_t size[kInvBuckets] = {};
    uint64_t total = 0;
    for (uint32_t i = 0; i < kInvBuckets; ++i) {
        size[i] = r.u32v();
        if (!r.ok || size[i] > kMaxInvBytes) return false;
        total += size[i];
    }
    if (r.pos + total > r.len) return false;

    for (uint32_t i = 0; i < kInvBuckets; ++i) {
        m.size[i] = size[i];
        if (!size[i]) continue;             // an empty bucket is not an error
        uint8_t* buf = (uint8_t*)malloc(size[i]);
        if (!buf) {
            for (uint32_t j = 0; j < i; ++j) { free(m.data[j]); m.data[j] = nullptr; }
            return false;
        }
        memcpy(buf, r.buf + r.pos, size[i]);
        r.pos += size[i];
        m.data[i] = buf;
    }
    return true;
}

inline uint32_t enc_control(uint8_t* p, uint32_t cap, const ControlMsg& c) {
    Writer w(p, cap);
    w.u8v(MSG_CONTROL);
    w.u64v(c.battle_id);
    w.u32v(c.humans);
    w.u8v(c.count);
    for (uint8_t i = 0; i < c.count && i < 32; ++i) w.u8v(c.cats[i]);
    // ...and WHAT each of those cats is, after the indices so the frame stays a
    // superset of version 27's. Adding the field to the struct and forgetting
    // this loop is a specific, measured failure: the sender filled `fp`, the wire
    // carried zeros, and the receiver reported a perfectly healthy split as "the
    // two runs have already diverged". Caught by reading both logs of a real
    // session rather than by the compiler, which had nothing to complain about.
    for (uint8_t i = 0; i < c.count && i < 32; ++i) w.u32v(c.fp[i]);
    return w.ok ? w.len : 0;
}

inline uint32_t enc_peers(uint8_t* p, uint32_t cap, const PeersMsg& v) {
    Writer w(p, cap);
    w.u8v(MSG_PEERS);
    w.u8v(v.you);
    w.u8v(v.count);
    for (uint8_t i = 0; i < v.count && i < kMaxPeers; ++i) w.u8v(v.ids[i]);
    return w.ok ? w.len : 0;
}

inline uint32_t enc_cursor(uint8_t* p, uint32_t cap, const CursorMsg& c) {
    Writer w(p, cap);
    w.u8v(MSG_CURSOR);
    w.u64v(c.battle_id);
    w.u32v((uint32_t)c.x);
    w.u32v((uint32_t)c.y);
    w.u8v(c.on_board);
    w.u8v(c.owns_turn);
    // Bit-cast rather than scaled to an integer: both peers are x86-64 running
    // the same build, so the representation is identical, and a fixed-point
    // encoding would quantise the one value whose whole point is that it moves
    // smoothly.
    uint32_t bits;
    memcpy(&bits, &c.nx, 4); w.u32v(bits);
    memcpy(&bits, &c.ny, 4); w.u32v(bits);
    w.u8v(c.mode);
    return w.ok ? w.len : 0;
}

inline uint32_t enc_aim(uint8_t* p, uint32_t cap, const AimMsg& m) {
    Writer w(p, cap);
    w.u8v(MSG_AIM);
    w.u64v(m.battle_id);
    w.u8v(m.cat); w.u8v(m.active); w.u8v(m.slot_kind); w.u8v(m.slot_index);
    w.u32v((uint32_t)m.tx); w.u32v((uint32_t)m.ty);
    w.u32v((uint32_t)m.dx); w.u32v((uint32_t)m.dy);
    w.str(m.gon);
    return w.ok ? w.len : 0;
}

inline uint32_t enc_enter_node(uint8_t* p, uint32_t cap, const EnterNodeMsg& m) {
    Writer w(p, cap);
    w.u8v(MSG_ENTERNODE);
    w.u32v(m.index); w.u32v(m.node_count); w.u32v(m.type); w.u64v(m.seed0);
    w.u64v(m.seed[1]); w.u64v(m.seed[2]); w.u64v(m.seed[3]);
    if (m.familiar_count > 64) return 0;
    w.u32v(m.familiar_count);
    for (uint32_t i = 0; i < m.familiar_count; ++i) {
        if (!m.familiar_ids[i] || m.familiar_ids[i] == UINT64_MAX) return 0;
        for (uint32_t j = 0; j < i; ++j)
            if (m.familiar_ids[i] == m.familiar_ids[j]) return 0;
        w.u64v(m.familiar_ids[i]);
    }
    if (m.party_count > 16) return 0;
    w.u8v((uint8_t)m.party_count);
    for (uint32_t i = 0; i < m.party_count; ++i) {
        if (!m.party_ids[i] || m.party_ids[i] == UINT64_MAX) return 0;
        for (uint32_t j = 0; j < i; ++j)
            if (m.party_ids[i] == m.party_ids[j]) return 0;
        w.u64v(m.party_ids[i]);
    }
    return w.ok ? w.len : 0;
}

inline uint32_t enc_choice(uint8_t* p, uint32_t cap, const ChoiceMsg& m) {
    Writer w(p, cap);
    w.u8v(MSG_CHOICE);
    w.u8v(m.kind); w.u32v(m.index); w.u32v(m.count); w.u32v(m.aux);
    w.u64v(m.node_seed);
    w.str(m.name);
    w.u64v(m.cat_id); w.u64v(m.cat_seed); w.u32v(m.level_step); w.str(m.event_name);
    return w.ok ? w.len : 0;
}

// Needs a buffer of at least savefile_frame_size(m) bytes -- far larger than
// the 512-byte stack buffer every other message uses, which is why net.cpp
// gives this one its own send path.
inline uint32_t savefile_frame_size(const SaveFileMsg& m) { return m.size + 128; }

// A chapter-map row is a few KB, well under the save budget, but it still needs
// the same oversized-frame send path as MSG_SAVEFILE rather than the 512-byte
// stack buffer the small messages use.
inline uint32_t chaptermap_frame_size(const ChapterMapMsg& m) { return m.size + 64; }

inline uint32_t enc_chaptermap(uint8_t* p, uint32_t cap, const ChapterMapMsg& m) {
    if (m.size > kMaxSaveBytes) return 0;
    if (m.size && !m.data) return 0;
    Writer w(p, cap);
    w.u8v(MSG_CHAPTERMAP);
    w.u64v(m.selection);
    w.u32v(m.size); w.u64v(m.hash);
    if (m.size) w.raw(m.data, m.size);
    return w.ok ? w.len : 0;
}

inline uint32_t enc_savefile(uint8_t* p, uint32_t cap, const SaveFileMsg& m) {
    if (!m.data || m.size == 0 || m.size > kMaxSaveBytes) return 0;
    Writer w(p, cap);
    w.u8v(MSG_SAVEFILE);
    w.u32v(m.slot); w.u32v(m.size); w.u64v(m.hash); w.str(m.name);
    w.u8v(m.fresh);
    w.raw(m.data, m.size);
    return w.ok ? w.len : 0;
}

inline uint64_t savefile_hash(const void* p, uint32_t n) {
    const uint8_t* b = (const uint8_t*)p;
    uint64_t h = 1469598103934665603ULL;
    for (uint32_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

inline uint32_t enc_page(uint8_t* p, uint32_t cap, const PageMsg& m) {
    Writer w(p, cap);
    w.u8v(MSG_PAGE);
    w.u8v(m.page);
    return w.ok ? w.len : 0;
}

inline bool dec_page(Reader& r, PageMsg& m) {
    m.page = r.u8v();
    return r.ok && r.pos == r.len;
}

inline uint32_t enc_cats(uint8_t* p, uint32_t cap, const CatsMsg& m) {
    if (m.count > kBriefCats) return 0;
    Writer w(p, cap);
    w.u8v(MSG_CATS);
    w.u8v(m.count);
    for (uint32_t i = 0; i < m.count; ++i) {
        const CatBrief& c = m.cats[i];
        w.str(c.name);
        w.i32v(c.level); w.i32v(c.hp); w.i32v(c.maxhp);
        w.u8v(c.klass);
        w.i32v(c.fur); w.i32v(c.coat); w.i32v(c.tex);
    }
    return w.ok ? w.len : 0;
}

inline bool dec_cats(Reader& r, CatsMsg& m) {
    m.count = r.u8v();
    if (!r.ok || m.count > kBriefCats) return false;
    for (uint32_t i = 0; i < m.count; ++i) {
        CatBrief& c = m.cats[i];
        r.str(c.name, sizeof(c.name));
        c.level = r.i32v(); c.hp = r.i32v(); c.maxhp = r.i32v();
        c.klass = r.u8v();
        c.fur = r.i32v(); c.coat = r.i32v(); c.tex = r.i32v();
    }
    return r.ok && r.pos == r.len;
}

inline uint32_t enc_savewait(uint8_t* p, uint32_t cap, const SaveWaitMsg& m) {
    if (m.phase > kSaveWaitLoading) return 0;
    Writer w(p, cap);
    w.u8v(MSG_SAVEWAIT); w.u8v(m.phase); w.u8v(m.selected);
    return w.ok ? w.len : 0;
}
inline bool dec_savewait(Reader& r, SaveWaitMsg& m) {
    m.phase = r.u8v(); m.selected = r.u8v();
    return r.ok && r.pos == r.len && m.phase <= kSaveWaitLoading;
}

inline uint32_t enc_roll(uint8_t* p, uint32_t cap, const RollMsg& m) {
    if (!m.site || m.result > 1) return 0;
    Writer w(p, cap);
    w.u8v(MSG_ROLL); w.u64v(m.battle); w.u32v(m.turn); w.u32v(m.seq); w.u8v(m.site); w.u8v(m.result);
    uint32_t c, l; memcpy(&c, &m.chance, 4); memcpy(&l, &m.luck, 4);
    w.u32v(c); w.u32v(l);
    return w.ok ? w.len : 0;
}
inline bool dec_roll(Reader& r, RollMsg& m) {
    m.battle = r.u64v(); m.turn = r.u32v(); m.seq = r.u32v(); m.site = r.u8v(); m.result = r.u8v();
    uint32_t c = r.u32v(), l = r.u32v(); memcpy(&m.chance, &c, 4); memcpy(&m.luck, &l, 4);
    return r.ok && r.pos == r.len && m.site != 0 && m.result <= 1;
}

inline uint32_t enc_abandon(uint8_t* p, uint32_t cap, const AbandonMsg& m) {
    if (m.kind < kAbandonPropose || m.kind > kAbandonCancel) return 0;
    Writer w(p, cap);
    w.u8v(MSG_ABANDON); w.u8v(m.kind); w.u8v(m.proposer); w.u8v((uint8_t)(m.nonce & 0xFF)); w.u8v((uint8_t)(m.nonce >> 8));
    return w.ok ? w.len : 0;
}
inline bool dec_abandon(Reader& r, AbandonMsg& m) {
    m.kind = r.u8v(); m.proposer = r.u8v(); m.nonce = r.u8v(); m.nonce = (uint16_t)(m.nonce | (r.u8v() << 8));
    return r.ok && r.pos == r.len && m.kind >= kAbandonPropose && m.kind <= kAbandonCancel;
}

inline uint32_t enc_roomctl(uint8_t* p, uint32_t cap, const RoomCtlMsg& m) {
    if (m.kind < kRoomName || m.kind > kRoomAbort) return 0;
    Writer w(p, cap);
    w.u8v(MSG_ROOMCTL); w.u8v(m.kind); w.str(m.name);
    return w.ok ? w.len : 0;
}
inline bool dec_roomctl(Reader& r, RoomCtlMsg& m) {
    m.kind = r.u8v(); r.str(m.name, sizeof(m.name));
    return r.ok && r.pos == r.len && m.kind >= kRoomName && m.kind <= kRoomAbort;
}

inline uint32_t enc_hostleft(uint8_t* p, uint32_t cap, const HostLeftMsg& m) {
    Writer w(p, cap);
    w.u8v(MSG_HOSTLEFT);
    w.str(m.scene);
    return w.ok ? w.len : 0;
}

// Debug only, and it carries the battle id for the same reason AIM does: a tile
// means nothing without knowing which board it is on, and a hit that arrived one
// battle late would land on a stranger.
struct DebugHitMsg {
    uint64_t battle_id = 0;
    int32_t  tx        = 0;
    int32_t  ty        = 0;
    int32_t  amount    = 0;
};

inline uint32_t enc_debughit(uint8_t* p, uint32_t cap, const DebugHitMsg& m) {
    Writer w(p, cap);
    w.u8v(MSG_DEBUGHIT);
    w.u64v(m.battle_id);
    w.u32v((uint32_t)m.tx); w.u32v((uint32_t)m.ty); w.u32v((uint32_t)m.amount);
    return w.ok ? w.len : 0;
}

// A party, in cat ids. Eight is the design's ceiling (up to four per player) with
// room to spare; anything longer is refused rather than truncated, because a
// silently shortened party is a party the two peers disagree about.
constexpr uint8_t kMaxPartyIds = 8;

struct PartyMsg {
    uint8_t  kind  = 0;                  // 0 = my picks, 1 = the agreed party
    uint8_t  count = 0;
    uint32_t ids[kMaxPartyIds] = {};
};

inline uint32_t enc_party(uint8_t* p, uint32_t cap, const PartyMsg& m) {
    Writer w(p, cap);
    w.u8v(MSG_PARTY);
    w.u8v(m.kind);
    w.u8v(m.count);
    for (uint8_t i = 0; i < m.count && i < kMaxPartyIds; ++i) w.u32v(m.ids[i]);
    return w.ok ? w.len : 0;
}

inline uint32_t enc_halt(uint8_t* p, uint32_t cap, const HaltMsg& h) {
    Writer w(p, cap);
    w.u8v(MSG_HALT);
    w.u32v(h.turn); w.str(h.reason);
    return w.ok ? w.len : 0;
}

inline uint32_t enc_refuse(uint8_t* p, uint32_t cap, const char* reason) {
    Writer w(p, cap);
    w.u8v(MSG_REFUSE); w.str(reason);
    return w.ok ? w.len : 0;
}

// --- decode -----------------------------------------------------------------
// Each takes a Reader already positioned past the u8 message type.

inline bool dec_hello(Reader& r, Hello& h) {
    h.proto = r.u32v(); h.gpak_hash = r.u64v(); h.build_hash = r.u64v();
    r.str(h.name, sizeof(h.name));
    h.rules = r.u8v();
    return r.ok;
}

inline bool dec_welcome(Reader& r, Welcome& v) {
    for (int i = 0; i < 4; ++i) v.rng_state[i] = r.u64v();
    v.cat_count = r.u8v();
    if (v.cat_count > 32) return false;
    for (uint8_t i = 0; i < v.cat_count; ++i) v.cats[i] = r.u8v();
    return r.ok;
}

inline bool dec_action(Reader& r, ActionMsg& a) {
    a.battle_id = r.u64v();
    a.turn = r.u32v();
    a.actor = r.u8v(); a.type = r.u8v(); a.slot_kind = r.u8v(); a.slot_index = r.u8v();
    a.b30 = r.u8v();   a.b31 = r.u8v();
    a.tx = r.i32v(); a.ty = r.i32v(); a.dx = r.i32v(); a.dy = r.i32v();
    r.str(a.gon, sizeof(a.gon));
    // Reject anything that is not a real decision at the edge. Types 6 and 7
    // are locally generated; if one ever arrives, the sender is broken and
    // applying it would double-fire a reaction on this peer.
    return r.ok && (a.type == 2 || a.type == 3);
}

inline bool dec_hash(Reader& r, HashMsg& h) {
    h.battle_id = r.u64v();
    h.turn = r.u32v(); h.rng_hash = r.u64v(); h.state_hash = r.u64v();
    h.queue_depth = r.u32v(); h.queue_sig = r.u32v();
    return r.ok;
}

// Allocates m.data on success; the caller owns it from that moment. On any
// failure nothing is allocated and m.data stays null, so a partial frame cannot
// leak. Same contract as dec_savefile.
inline bool dec_setup(Reader& r, SetupMsg& m) {
    for (uint8_t i = 0; i < kSetupMaxCats; ++i) m.cats[i].data = nullptr;
    m.kind = r.u8v(); m.count = r.u8v(); m.generation = r.u32v();
    m.checkpoint = r.u64v();
    m.members=r.u8v();
    for(unsigned i=0;i<kMaxPeers;++i) { m.peers[i]=r.u8v(); m.requests[i]=r.u32v(); }
    if (!r.ok || m.kind > kSetupResumeReply || m.count > kSetupMaxCats) return false;
    for (uint8_t i = 0; i < m.count; ++i) {
        SetupCat& c = m.cats[i];
        c.id = r.u64v(); c.size = r.u32v(); c.hash = r.u64v();
        if (!r.ok || !c.id || !c.size || c.size > kMaxCatBytes || r.pos + c.size > r.len) {
            for (uint8_t j = 0; j < i; ++j) { free(m.cats[j].data); m.cats[j].data = nullptr; }
            return false;
        }
        c.data = (uint8_t*)malloc(c.size);
        if (!c.data) {
            for (uint8_t j = 0; j < i; ++j) { free(m.cats[j].data); m.cats[j].data = nullptr; }
            return false;
        }
        memcpy(c.data, r.buf + r.pos, c.size); r.pos += c.size;
    }
    return true;
}

inline bool dec_catdata(Reader& r, CatDataMsg& m) {
    m.data = nullptr;
    m.id   = r.u64v();
    m.size = r.u32v();
    m.hash = r.u64v();
    if (!r.ok) return false;
    if (m.size == 0 || m.size > kMaxCatBytes) return false;
    if (r.pos + m.size > r.len) return false;
    uint8_t* buf = (uint8_t*)malloc(m.size);
    if (!buf) return false;
    memcpy(buf, r.buf + r.pos, m.size);
    r.pos += m.size;
    m.data = buf;
    return true;
}

// Allocates m.data on success; nothing on any failure, so a partial frame
// cannot leak. Same contract as dec_catdata.
inline bool dec_runhist(Reader& r, RunHistMsg& m) {
    m.data = nullptr;
    m.size = r.u32v();
    m.hash = r.u64v();
    if (!r.ok) return false;
    if (m.size == 0 || m.size > kMaxRunHistBytes) return false;
    if (r.pos + m.size > r.len) return false;
    uint8_t* buf = (uint8_t*)malloc(m.size);
    if (!buf) return false;
    memcpy(buf, r.buf + r.pos, m.size);
    r.pos += m.size;
    m.data = buf;
    return true;
}

inline bool dec_nodehash(Reader& r, NodeHashMsg& m) {
    m.node_seed  = r.u64v();
    m.node_index = r.u32v();
    m.point      = r.u8v();
    for (int i = 0; i < 4; ++i) m.rng[i] = r.u64v();
    m.hist_hash  = r.u64v();
    m.cats_hash  = r.u64v();
    m.cat_count  = r.u32v();
    m.inv_hash   = r.u64v();
    r.str(m.event, sizeof(m.event));
    return r.ok;
}

inline bool dec_control(Reader& r, ControlMsg& c) {
    c.battle_id = r.u64v();
    c.humans = r.u32v();
    c.count  = r.u8v();
    if (c.count > 32) return false;
    for (uint8_t i = 0; i < c.count; ++i) c.cats[i] = r.u8v();
    for (uint8_t i = 0; i < c.count; ++i) c.fp[i] = r.u32v();
    return r.ok;
}

inline bool dec_peers(Reader& r, PeersMsg& v) {
    v.you   = r.u8v();
    v.count = r.u8v();
    if (v.count == 0 || v.count > kMaxPeers) return false;
    for (uint8_t i = 0; i < v.count; ++i) v.ids[i] = r.u8v();
    if (!r.ok) return false;
    // The list must be sorted and must contain the receiver, because the split
    // is computed from a position in it. A malformed one would silently give two
    // peers the same position, which is double control of the same cat.
    bool saw_you = (v.you == v.ids[0]);
    for (uint8_t i = 1; i < v.count; ++i) {
        if (v.ids[i] <= v.ids[i - 1]) return false;
        if (v.ids[i] == v.you) saw_you = true;
    }
    return saw_you;
}

inline bool dec_aim(Reader& r, AimMsg& m) {
    m.battle_id  = r.u64v();
    m.cat        = r.u8v();
    m.active     = r.u8v();
    m.slot_kind  = r.u8v();
    m.slot_index = r.u8v();
    m.tx = (int32_t)r.u32v(); m.ty = (int32_t)r.u32v();
    m.dx = (int32_t)r.u32v(); m.dy = (int32_t)r.u32v();
    r.str(m.gon, sizeof(m.gon));
    // Both coordinates reach a game function that indexes the tactics grid, so
    // they are bounded here rather than there. Loose on purpose, the same way
    // CURSOR's are: the real grid size is only known with a live StatusMenu,
    // and a wild-but-bounded tile draws nothing instead of faulting.
    if (m.tx < -1000 || m.tx > 1000 || m.ty < -1000 || m.ty > 1000) return false;
    if (m.dx <   -8  || m.dx >    8 || m.dy <   -8  || m.dy >    8) return false;
    return r.ok;
}

inline bool dec_cursor(Reader& r, CursorMsg& c) {
    c.battle_id = r.u64v();
    c.x         = (int32_t)r.u32v();
    c.y         = (int32_t)r.u32v();
    c.on_board  = r.u8v();
    c.owns_turn = r.u8v();
    uint32_t bits = r.u32v(); memcpy(&c.nx, &bits, 4);
    bits          = r.u32v(); memcpy(&c.ny, &bits, 4);
    // A NaN or a wild fraction reaches a vertex shader, so it is rejected here
    // rather than drawn somewhere impossible. The bound is loose on purpose --
    // the sender already clamps, and a pointer half a screen off the edge is
    // better evidence of a bug than a pointer silently pinned to a corner.
    if (!(c.nx > -1.0f && c.nx < 2.0f) || !(c.ny > -1.0f && c.ny < 2.0f)) return false;
    c.mode = r.u8v();
    // A tile index reaches an array subscript on the drawing side, so it is
    // range-checked here rather than there. The bound is deliberately loose --
    // the real grid bounds are only known with a live StatusMenu -- but it
    // rejects the garbage that would matter.
    if (c.on_board && (c.x < 0 || c.y < 0 || c.x > 4095 || c.y > 4095)) return false;
    return r.ok;
}

inline bool dec_enter_node(Reader& r, EnterNodeMsg& m) {
    m.index = r.u32v(); m.node_count = r.u32v(); m.type = r.u32v(); m.seed0 = r.u64v();
    m.seed[0] = m.seed0;
    m.seed[1] = r.u64v(); m.seed[2] = r.u64v(); m.seed[3] = r.u64v();
    m.familiar_count = r.u32v();
    if (!r.ok || m.familiar_count > 64) return false;
    for (uint32_t i = 0; i < m.familiar_count; ++i) {
        m.familiar_ids[i] = r.u64v();
        if (!r.ok || !m.familiar_ids[i] || m.familiar_ids[i] == UINT64_MAX) return false;
        for (uint32_t j = 0; j < i; ++j)
            if (m.familiar_ids[i] == m.familiar_ids[j]) return false;
    }
    // MANDATORY, not optional: making it optional would turn a frame truncated right after the
    // familiars into a valid one with no party, which the truncation tests rightly forbid. The
    // protocol bump is what keeps an old sender away from a new receiver.
    m.party_count = r.u8v();
    if (!r.ok || m.party_count > 16) return false;
    for (uint32_t i = 0; i < m.party_count; ++i) {
        m.party_ids[i] = r.u64v();
        if (!r.ok || !m.party_ids[i] || m.party_ids[i] == UINT64_MAX) return false;
        for (uint32_t j = 0; j < i; ++j)
            if (m.party_ids[i] == m.party_ids[j]) return false;
    }
    return r.ok;
}

inline bool dec_party(Reader& r, PartyMsg& m) {
    m.kind  = r.u8v();
    m.count = r.u8v();
    if (m.count > kMaxPartyIds) return false;   // refuse, never truncate
    for (uint8_t i = 0; i < m.count; ++i) m.ids[i] = r.u32v();
    return r.ok;
}

inline bool dec_debughit(Reader& r, DebugHitMsg& m) {
    m.battle_id = r.u64v();
    m.tx        = (int32_t)r.u32v();
    m.ty        = (int32_t)r.u32v();
    m.amount    = (int32_t)r.u32v();
    return r.ok;
}

inline bool dec_choice(Reader& r, ChoiceMsg& m) {
    m.kind  = r.u8v();
    m.index = r.u32v();
    m.count = r.u32v();
    m.aux   = r.u32v();
    m.node_seed = r.u64v();
    r.str(m.name, sizeof(m.name));
    m.cat_id = r.u64v(); m.cat_seed = r.u64v(); m.level_step = r.u32v();
    r.str(m.event_name, sizeof(m.event_name));
    if (!r.ok) return false;
    if (m.kind != kChoiceEvent && m.kind != kChoiceLevelUp) return false;
    if (m.count == 0 || m.count > 64 || m.index >= m.count) return false;
    if (m.kind == kChoiceLevelUp)
        return m.node_seed != 0 && m.cat_id != 0 && m.cat_id != UINT64_MAX && !m.event_name[0];
    return m.cat_id == 0 && m.cat_seed == 0 && m.node_seed != 0 &&
           m.level_step < 64 && m.event_name[0];
}

// Allocates m.data on success. The caller owns it from that moment; on any
// failure nothing is allocated and m.data stays null, so a partial frame cannot
// leak.
inline bool dec_savefile(Reader& r, SaveFileMsg& m) {
    m.data = nullptr;
    m.slot = r.u32v();
    m.size = r.u32v();
    m.hash = r.u64v();
    r.str(m.name, sizeof(m.name));
    m.fresh = r.u8v();
    if (!r.ok) return false;
    if (m.size == 0 || m.size > kMaxSaveBytes) return false;
    if (r.pos + m.size > r.len) return false;
    uint8_t* buf = (uint8_t*)malloc(m.size);
    if (!buf) return false;
    memcpy(buf, r.buf + r.pos, m.size);
    r.pos += m.size;
    m.data = buf;
    return true;
}

// Allocates m.data on success, same contract as dec_savefile. size==0 is a
// valid frame ("nothing to sync") and leaves m.data null.
inline bool dec_chaptermap(Reader& r, ChapterMapMsg& m) {
    m.data = nullptr;
    m.selection = r.u64v();
    m.size = r.u32v();
    m.hash = r.u64v();
    if (!r.ok) return false;
    if (m.size == 0) return r.pos == r.len;
    if (m.size > kMaxSaveBytes) return false;
    if (r.pos + m.size != r.len) return false;
    uint8_t* buf = (uint8_t*)malloc(m.size);
    if (!buf) return false;
    memcpy(buf, r.buf + r.pos, m.size);
    r.pos += m.size;
    m.data = buf;
    return true;
}

inline bool dec_hostleft(Reader& r, HostLeftMsg& m) {
    r.str(m.scene, sizeof(m.scene));
    return r.ok;
}

inline bool dec_halt(Reader& r, HaltMsg& h) {
    h.turn = r.u32v();
    r.str(h.reason, sizeof(h.reason));
    return r.ok;
}

inline const char* msg_name(uint8_t t) {
    switch (t) {
        case MSG_HELLO:   return "HELLO";
        case MSG_WELCOME: return "WELCOME";
        case MSG_REFUSE:  return "REFUSE";
        case MSG_ACTION:  return "ACTION";
        case MSG_HASH:    return "HASH";
        case MSG_HALT:    return "HALT";
        case MSG_PING:    return "PING";
        case MSG_CONTROL: return "CONTROL";
        case MSG_ENTERNODE: return "ENTERNODE";
        case MSG_SAVEFILE: return "SAVEFILE";
        case MSG_CATDATA:  return "CATDATA";
        case MSG_INVENTORY: return "INVENTORY";
        case MSG_PEERS:   return "PEERS";
        case MSG_CURSOR:  return "CURSOR";
        case MSG_CHOICE:  return "CHOICE";
        case MSG_RUNHIST: return "RUNHIST";
        case MSG_NODEHASH: return "NODEHASH";
        case MSG_AIM:     return "AIM";
        case MSG_STATEDUMP: return "STATEDUMP";
        case MSG_PEERLOG:   return "PEERLOG";
        case MSG_AUDIT:     return "AUDIT";
        case MSG_UQD:       return "UQD";
        case MSG_PROPS:     return "PROPS";
        case MSG_RNGL:      return "RNGL";
        case MSG_CHAT:      return "CHAT";
        case MSG_SETTING:   return "SETTING";
        case MSG_DEEP:      return "DEEP";
        case MSG_HOSTLEFT: return "HOSTLEFT";
        case MSG_PAGE:     return "PAGE";
        case MSG_CATS:     return "CATS";
        case MSG_SAVEWAIT: return "SAVEWAIT";
        case MSG_ABANDON:  return "ABANDON";
        case MSG_ROOMCTL:  return "ROOMCTL";
        case MSG_DEBUGHIT: return "DEBUGHIT";
        case MSG_PARTY:    return "PARTY";
        case MSG_SETUP:    return "SETUP";
        case MSG_CHAPTER:  return "CHAPTER";
        case MSG_CHAPTERSEED: return "CHAPTERSEED";
        case MSG_CATDIGEST: return "CATDIGEST";
        case MSG_CHECKPOINT:return "CHECKPOINT";
        default:          return "?";
    }
}

} // namespace mgmp
