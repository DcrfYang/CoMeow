#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace mgmp {
struct CheckpointMsg;
struct ChapterMapMsg;
struct SaveWaitMsg;
// dev_tools: two instances on one Steam account are told apart by their fingerprints (the fingerprint stays the primary id); otherwise the Steam id is.
void checkpoint_init(uint64_t build_hash=0, uint64_t gpak_hash=0, bool dev_tools=false);
void checkpoint_shutdown();
bool checkpoint_active();
// Swallow native selection until every participant has validated/restored.
bool checkpoint_select(uint8_t slot, const wchar_t* path);
int checkpoint_autoselect();
// node_type: the MapNodeType (8 = the chapter boss); the save confirmed after that node is marked kCheckpointAfterBoss.
void checkpoint_on_node(uint64_t node_seed, uint32_t node_index, uint32_t node_type = 0);
void checkpoint_on_map(uint64_t map_hash);
// How long (ms, default 4000) a capture that fails at the map is retried -- a try every half second -- before the checkpoint latches as failed and every player is held at the next node.
void checkpoint_set_capture_patience(unsigned ms);
// Fresh-session map pre-sync (MSG_CHAPTERMAP): the host's serialized
// files.chapter_map row, written into this peer's save before load. A fresh
// departure-ready save carries its own generated map, and two peers that
// prepared separately carry two different ones -- the first node boundary
// refuses to certify that pair, so the host's row becomes the run's map.
void checkpoint_on_chaptermap(uint8_t from, const ChapterMapMsg& m);
// This peer's run came back from a handshake save that was already under way (RESTORE), not a fresh one (NEW).
bool checkpoint_run_restored();
bool checkpoint_needs_map();
bool checkpoint_can_enter();
void checkpoint_on_message(uint8_t from, const CheckpointMsg& m);
bool checkpoint_on_alone();

// THE TWO SAVES BEFORE THE MAP (2026-10-05). A run used to have no handshake save until its first map. Now every player's save is also taken, and certified by everybody like a node's, at two moments:
//   kStagePrep  -- everyone has come into the room and is in the warehouse (the preparation stage: the run can be started over from here);
//   kStageReady -- everyone is on the chapter page ("ready to start"): cats and gear are chosen, only the start is left.
// They sit in the same journal as the node saves and are listed with them, but they are not nodes: their seq is kStageSeqBase + the stage (a node's seq is its count), the node counter does not move, and
// restoring one starts the run over from that page (a fresh start, not a resumed map). checkpoint_stage_tick is called every frame with the page this peer is on (0 = neither); a stage that cannot be
// completed (a player moved on, a copy failed, too slow) is given up with a log line and never latches the room -- the node saves do not depend on it.
constexpr uint8_t  kStagePrep = 1, kStageReady = 2;
constexpr uint64_t kStageSeqBase = 0x40000000ull;
inline uint8_t checkpoint_stage_of_seq(uint64_t seq) { return (seq > kStageSeqBase && seq <= kStageSeqBase + kStageReady) ? (uint8_t)(seq - kStageSeqBase) : 0; }
void checkpoint_stage_tick(uint8_t page);
// The host's chapter choice waits (a few seconds at most) while the ready save is being taken: the run starting would drop it.
bool checkpoint_stage_holding();
// One file of a player's handshake folder (<save dir>\mgmp_handshake\<id>\<pairing>.N), read with no room up: which of this player's save slots it was taken from, when (stamp = FILETIME of the
// confirmation), which node (seq), how many players, and (database != null) the save itself. False for a file that is not a confirmed save of this player. `folder_id` is the id the folder is named after.
struct HandshakeSaveInfo { uint8_t slot = 255, players = 0, stage = 0; bool after_boss = false; uint64_t run = 0, seq = 0, stamp = 0; };   // after_boss: confirmed right after the chapter boss; stage: kStagePrep / kStageReady, 0 = a node's save
bool checkpoint_peek_file(const std::wstring& path, uint64_t folder_id, HandshakeSaveInfo& info, std::vector<uint8_t>* database);
// The handshake saves of a run that went solo ("continue alone", or a settlement with no room) are kept -- the tombstone that vetoes a resume is written at once -- until the player is next in the warehouse.
// _due: something is waiting; _deferred: do it now (called by the warehouse watch; does nothing while a room is up, and deletes only entries of that run).
bool checkpoint_cleanup_due();
void checkpoint_cleanup_deferred(const char* why);
void checkpoint_clear(bool broadcast = true);
// In-session next run: after checkpoint_clear settled this run, re-run the
// save-validation handshake with the already-selected slot and mint a FRESH
// run identity -- the in-session equivalent of a save-screen slot click.
// False unless this run settled (or the re-arm failed); true once in flight.
bool checkpoint_restart_run();
void checkpoint_fault(const char* reason);
// Skip THIS node's checkpoint without failing the session (cat-roster disagreement, RNG agrees).
void checkpoint_soft_fault(uint64_t node_seed, const char* reason);
const char* checkpoint_status();

// --- the save-selection stage: who has picked, and the host choosing the handshake save -----------
//
// Until now the host's choose() picked the newest common confirmed save (or a fresh start) on its own
// the moment every offer was in. With the UI enabled it instead LISTS what it found and waits:
//
//   every player who clicked a save   ->  "waiting for the others" (who has picked, who has not)
//   all picked, host                  ->  the handshake saves every player holds in common (newest
//                                         first, up to four), plus the PREPARATION stage -- a fresh
//                                         start from the warehouse -- when no player's save is
//                                         mid-run
//   all picked, everyone else         ->  "waiting for the host to choose"
//   nothing fits and somebody is
//   mid-run (a save on the map)       ->  "this combination is invalid, return to the warehouse"
//
// Off by default so the state machine keeps its own regression tests; the UI switches it on when it
// exists to be clicked. In-session chapter restarts never wait: nobody is on a save screen then.
void checkpoint_set_manual(bool on);

struct SaveSyncEntry { uint64_t run = 0, seq = 0, stamp = 0; uint8_t flags = 0; };   // stamp = FILETIME of the confirmation; flags: kCheckpointAfterBoss
enum SaveSyncPhase : uint8_t {
    kSyncNone = 0,      // nothing to show
    kSyncWaiting,       // this player picked; not everyone has
    kSyncHostChoosing,  // host: choose the handshake save
    kSyncGuestWaiting,  // everyone else: the host is choosing
    kSyncInvalid,       // the combination cannot be continued
};
struct SaveSyncView {
    SaveSyncPhase phase = kSyncNone;
    bool     host = false;
    uint8_t  selected = 0;          // bit n = the peer with transport id n has picked (kSyncInvalid: ... holds an unfinished co-op record)
    bool     prep = false;          // the preparation stage is allowed (host choosing)
    unsigned n = 0;                 // handshake saves listed (host choosing), newest first
    SaveSyncEntry entry[96];        // = kCheckpointCandidates (mgmp_proto.h), newest first
};
bool checkpoint_sync_view(SaveSyncView& out);
// Host: -1 = the preparation stage, 0..n-1 = that handshake save. False when nothing is being chosen.
bool checkpoint_host_pick(int index);
// Put the "invalid" panel away; the next slot click starts a fresh round.
void checkpoint_sync_dismiss();
// START THE SAVE-SELECTION ROUND OVER (2026-09-30): this peer forgets its own pick and, on the host,
// every pick it was holding -- including the ones queued for "the next handshake". Refused (false) once
// the host has chosen, because from there the saves are being restored and loaded. Used by the room
// module for "re-pick", for "OK" on the invalid panel and when the room is unlocked; the others are
// told with ROOMCTL kRoomAbort and do the same.
bool checkpoint_abort_round();
// True while this peer has picked a save and the round has not reached the load.
bool checkpoint_round_open();
// Host, every frame: tells the others who has picked and which stage the pick is in.
void checkpoint_tick();
void checkpoint_on_savewait(uint8_t from, const SaveWaitMsg& m);
}
