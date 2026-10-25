// mgmp_net.h -- TCP transport. Phase 4, layer 2.
//
// One connection, two roles, behind an interface. SteamNetworkingMessages002 is
// already linked into the game if this ever needs NAT punch or relay; the cost
// of TCP is losing those, so a first milestone is direct-IP or port-forward.
//
// THE ONE HARD RULE: nothing here may block the game's frame thread.
//
// A dedicated receive thread owns the socket and drains it into a queue the
// game thread pops under a lock. That is affordable because of a property the
// game already has -- Brain::GetChoice is a *poll*, not a decision point. It
// returns type=1 ("nothing decided yet") on every frame while waiting on a
// human, and did so for 1695 of 1711 calls in one tutorial battle. So a remote
// decision that has not arrived yet needs no special handling at all: return
// type=1 and the game waits exactly the way it already waits for a person.
//
// That is why this transport has no timeouts, no frame budget and no blocking
// receive on the game thread. Waiting is free.
#pragma once

#include <cstdint>
#include "mgmp_proto.h"

namespace mgmp {

enum class NetRole { None, Host, Client };

enum class NetState {
    Idle,          // not started
    Listening,     // host, waiting for a peer
    Connecting,    // client, dialling
    Connected,     // socket up, HELLO not yet exchanged
    Ready,         // HELLO/WELCOME done, battle may proceed
    Failed,        // gave up; see net_error()
    Closed,        // peer hung up or we did
};

// One decoded inbound message. The union is flat rather than tagged-pointer so
// the receive thread can copy it into the queue without allocating -- an
// allocation on the receive path is a stall waiting to happen.
struct NetMsg {
    uint8_t   type = 0;
    // Which peer AUTHORED this, from the frame envelope -- not which socket it
    // arrived on. The two differ for anything the host relays between clients,
    // and the difference is the whole point: with four players, "an action
    // arrived" is not enough, you need to know whose.
    uint8_t   from = kNoPeer;
    Hello     hello;
    PeersMsg  peers;
    Welcome   welcome;
    ActionMsg action;
    HashMsg   hash;
    ControlMsg control;
    CursorMsg cursor;
    AimMsg    aim;
    EnterNodeMsg enter_node;
    ChoiceMsg    choice;
    DebugHitMsg  debughit;   // debug only: see MSG_DEBUGHIT
    PartyMsg     party;      // see MSG_PARTY
    SetupMsg     setup;      // setup cat export/reply, see MSG_SETUP
    ChapterMsg   chapter;
    ChapterSeedMsg chapterseed; // in-session chapter seed, see MSG_CHAPTERSEED
    CheckpointMsg checkpoint;
    // The two messages that do not fit the "flat, no allocation" rule above.
    // Their `data` is heap-allocated by the decoder and owned by whoever pops
    // the frame, so EVERY net_poll caller must pass the frame to
    // net_msg_release when it is done with it -- including the ones that
    // ignored it.
    SaveFileMsg savefile;
    ChapterMapMsg chaptermap;   // fresh-session map pre-sync, see MSG_CHAPTERMAP
    CatDataMsg  catdata;
    InventoryMsg inventory;   // owns up to kInvBuckets buffers
    RunHistMsg   runhist;     // owns its buffer, same contract as catdata
    StateDumpMsg statedump;   // owns its buffer, same contract as catdata
    PeerLogMsg   peerlog;     // owns its buffer, same contract as catdata (proto 76)
    AuditMsg     audit;       // the pre-battle audit of the player cats (proto 76): flat
    UqdMsg       uqd;         // one action's unlock-query digest (proto 79): flat
    PropsMsg     props;       // a chunk of the host's save-property table (proto 79): flat
    RnglMsg      rngl;        // one action's ledger of draws on the shared stream (proto 80): flat
    ChatMsg      chat;        // a line of text one player typed (proto 81): flat
    DrawMsg      draw;        // a piece of a player's drawing over the map or the battle (proto 87): flat
    SettingMsg   setting;     // the host's combat speed (proto 82): flat
    PingMsg      ping;        // a request / reply with the sender's frame statistics (proto 104, mgmp_perf): flat, answered on the receive thread
    EquipMsg     equip;       // the consumable a backpack auto-equipped on its owner's peer (proto 85): flat
    TreasureMsg  treasure;    // a claim on / the ledger of a shared treasure chest (proto 90, experimental): flat
    HouseCoopMsg housecoop;   // the house boss co-op: statuses, the vote, the go (proto 94, experimental): flat
    DeepMsg      deep;        // one turn's per-unit digest of derived values (proto 80): flat
    NodeHashMsg  nodehash;
    CatDigestMsg catdigest;  // per-cat hash digest, see MSG_CATDIGEST
    HostLeftMsg  hostleft;
    PageMsg      page;      // which screen that peer is on, see MSG_PAGE
    CatsMsg      cats;      // the cats that peer brings, see MSG_CATS
    SaveWaitMsg  savewait;  // the save-selection stage, host -> everyone, see MSG_SAVEWAIT
    AbandonMsg   abandon;   // the abandon-adventure vote, every peer, see MSG_ABANDON
    RoomCtlMsg   roomctl;   // room coordination, every peer, see MSG_ROOMCTL
    MapSeedsMsg  mapseeds;  // the chapter map's node seeds, host -> clients, see MSG_MAPSEEDS
    UnlocksMsg   unlocks;   // the host's unlock answers, host -> clients, see MSG_UNLOCKS
    BoardMsg     board;     // the host's battle board and stream, host -> clients, see MSG_BOARD
    BoardSumMsg  boardsum;  // the host's board as one digest per unit (proto 98), host -> clients, see MSG_BOARDSUM
    BoardReqMsg  boardreq;  // a client asking for the full board of a boundary (proto 98), client -> host, see MSG_BOARDREQ
    RollMsg      roll;      // a host-decided chance roll, host -> clients, see MSG_ROLL
    NodeVoteMsg  nodevote;  // a player's pick of the next map node (proto 106): flat
    SoulCatMsg   soulcat;   // the cat a Soul Jar holds, from the jar's owner (proto 100); owns its buffer, same contract as catdata
    HaltMsg   halt;
    char      refuse[192] = {};
};

// Frees anything the frame owns and nulls it, so calling it twice is safe and
// calling it on a frame that owns nothing costs a branch.
void net_msg_release(NetMsg& m);

// --- lifecycle (game thread) ------------------------------------------------

// port is the TCP port; host binds it, client dials <addr>:<port>.
bool net_host(uint16_t port);
// `addr` may be a comma-separated list (up to four) of addresses to try in order. Returns at once (the dial runs on a thread):
// the state is Connecting, then Connected or Failed (see net_error, net_dial_seq).
bool net_join(const char* addr, uint16_t port);

// The lobby's way of asking the HOST to dial this peer over Steam when the peer's own dial was refused (mgmp_steambridge: reverse dialling). Set once by
// the signaling layer; called from the dial thread with the host's SteamID64, true when the request was sent.
void net_set_steam_reverse_hook(bool (*hook)(uint64_t host_steamid));
// THE SERVER AS THE CARRIER (2026-10-03). Where the lobby server's relay is (it is the lobby server itself); set by the signaling layer.
void net_relay_set_server(const char* host, uint16_t port);
// Host: a joiner asked for pipe `id` -- open a connection to the server, answer with the host's token, and bridge it to the game's own port on this machine.
// Runs on its own thread; every bridge ends by itself, or all at once with net_relay_stop().
bool net_relay_serve(const char* host_token, uint32_t id, uint16_t game_port);
void net_relay_stop();
// A joiner dials the relay with the candidate "relay:<its token>" in the address list given to net_join.
uint32_t net_dial_seq();              // goes up by one each time a dial gives up on every address
int      net_dial_error();            // the last WSA error of that failure (10060 timed out, 10061 refused, 10051/10065 no route)
void net_shutdown();

NetState    net_state();
NetRole     net_role();
const char* net_error();          // "" unless state is Failed
bool        net_active();         // role != None && state not Failed/Closed

// --- traffic ----------------------------------------------------------------

// Send is thread-safe and never blocks the caller on the peer: the socket is
// left in blocking mode for writes, but a turn is a few hundred bytes and the
// kernel send buffer swallows it whole. If that ever stops being true the fix
// is a send queue, not a timeout.
// Send to every connected peer. On a client that is the host and nothing else;
// on the host it is every live client, which is what turns each existing
// host-authored push into a broadcast without touching its call site.
// True if it reached at least one peer.
bool net_send(const uint8_t* payload, uint32_t len);

// Send to one peer. Used where a message is genuinely point to point: WELCOME
// and REFUSE, and the per-peer PEERS copies.
bool net_send_peer(uint8_t peer, const uint8_t* payload, uint32_t len);

// This peer's own id, kHostPeer on the host and kNoPeer on a client until the
// first PEERS arrives. Peer 0 is always the host.
uint8_t net_self();

// Members of the session, host included. 0 before the first PEERS.
uint8_t net_peer_count();

// This peer's INDEX in the membership list. Differs from its id once
// anyone has disconnected, and it is the index -- never the id -- that the
// control split is computed from.
uint8_t net_peer_pos();

// Copy the member ids out, in that same order. False before the first PEERS.
bool net_peer_ids(uint8_t* out, uint8_t cap);

// THE SEAT ORDER (proto 86, 2026-10-10). The membership list is ordered by the players, not by when they connected: the host first, then the
// others in the order the save selection put them (mgmp_checkpoint's configure_host: by each player's persistent id, the Steam id where there is
// one). A position is what a session cat's id, a cat's owner note and the recovery journal's name are made of, so with the connection order a
// room whose two clients came back the other way round held the run and could not read it. Members the order does not name (someone who
// connected after it was set) follow it in connection order. `ids` must start with the host. Cleared with the session.
void net_set_seat_order(const uint8_t* ids, uint8_t n);

bool net_send_hello(const Hello& h);
bool net_send_welcome(const Welcome& w);
bool net_send_action(const ActionMsg& a);
bool net_send_hash(const HashMsg& h);
bool net_send_control(const ControlMsg& c);
bool net_send_enter_node(const EnterNodeMsg& m);
bool net_send_choice(const ChoiceMsg& m);
// Debug only, and it is a broadcast: the whole point is that both peers write the
// same hp change.
bool net_send_debughit(const DebugHitMsg& m);
// The party exchange: my picks, or the agreed list. See MSG_PARTY.
bool net_send_party(const PartyMsg& m);
// Setup exchange used before the chapter screen: client export to host, then
// host reply with the runtime cats. The payload is borrowed for the call.
bool net_send_setup(const SetupMsg& m);
bool net_send_chapter(const ChapterMsg& m);
bool net_send_chapterseed(const ChapterSeedMsg& m);
bool net_send_checkpoint(const CheckpointMsg& m);
// Every run cat's (id, serialized hash, size). Broadcast; the host relays it.
bool net_send_catdigest(const CatDigestMsg& m);
// Cosmetic and high-frequency relative to everything else here, so it is the
// one message a caller is expected to throttle. See mgmp_cursor.h.
bool net_send_cursor(const CursorMsg& c);
// Same contract as net_send_cursor: outside the lockstep contract entirely, so
// a drop can only make a preview flicker.
bool net_send_aim(const AimMsg& m);
// Copies m.data into a temporary frame buffer; the caller keeps ownership of
// it. This is the only send that can block the game thread for a measurable
// time -- ~45 KB is more than a default socket send buffer, so it may wait for
// the peer to drain. It happens once per session, off the battle path.
bool net_send_savefile(const SaveFileMsg& m);
// Same, to one peer -- for a player who joined after the host published.
bool net_send_savefile_to(uint8_t peer, const SaveFileMsg& m);
// Same contract as net_send_savefile, but small: a serialized cat is well under
// a kilobyte, so this one is cheap enough to send several of in a row.
bool net_send_catdata(const CatDataMsg& m);
// The run inventory, whole. Same contract again; sent alongside the cats at
// each map node, and skipped entirely when nothing in it changed.
bool net_send_inventory(const InventoryMsg& m);
// Same contract as net_send_catdata: `data` is borrowed for the duration.
bool net_send_runhist(const RunHistMsg& m);
// The same three, to ONE peer, for the join catch-up. Not an optimisation: on a
// client these three arrive as the node-snapshot batch, and a batch is closed by
// an ENTERNODE that only the joining peer receives -- so the copy broadcast to
// the peers already in the run is a batch nothing will ever consume, and while
// it sits there the client refuses to publish its own edits (see
// follow_map_update's `incoming.empty()` gate) and folds a stale state into the
// next node it enters.
bool net_send_catdata_to(uint8_t peer, const CatDataMsg& m);
bool net_send_inventory_to(uint8_t peer, const InventoryMsg& m);
bool net_send_runhist_to(uint8_t peer, const RunHistMsg& m);
bool net_send_nodehash(const NodeHashMsg& m);

// Fresh-session map pre-sync: host to one peer, before either side loads its
// save. Point-to-point because the host addresses every participant itself.
bool net_send_chaptermap_to(uint8_t peer, const ChapterMapMsg& m);

// Host-authored and sent once per departure, so it needs no throttle and no
// dedupe of its own -- mgmp_leave will not send a second one until the host has
// been back inside a run.
bool net_send_hostleft(const HostLeftMsg& m);
bool net_send_page(const PageMsg& m);
bool net_send_cats(const CatsMsg& m);
bool net_send_savewait(const SaveWaitMsg& m);
bool net_send_abandon(const AbandonMsg& m);
bool net_send_roomctl(const RoomCtlMsg& m);
// Host-authored: net_send reaches every client.
bool net_send_mapseeds(const MapSeedsMsg& m);
// Host-authored: net_send reaches every client.
bool net_send_unlocks(const UnlocksMsg& m);
// The newest level name the host sent (filled by the receive thread, not the frame queue): true when it is for `node_id`.
bool net_host_level(uint64_t node_id, char* out, size_t cap);
// The host's queue of returning enemies that came with that level (same mailbox): true when the level message for `node_id` has arrived.
bool net_host_pending(uint64_t node_id, PendingEnemy (*out)[kPendingMax], uint8_t* n, uint32_t* total = nullptr);   // out[kPendingQueues][kPendingMax], n[kPendingQueues]
// The host's battle board (MSG_BOARD): host-authored, net_send reaches every client; _to is the replay for a peer that joins late.
bool net_send_board(const BoardMsg& m);
// A roll the host decided (MSG_ROLL): sent to every client; a client looks it up by (battle, seq, site).
bool net_send_roll(const RollMsg& m);
bool net_host_roll(uint64_t battle, uint32_t seq, uint8_t site, RollMsg& out);
bool net_send_board_to(uint8_t peer, const BoardMsg& m);
// The board of battle `battle` as the receive thread assembled it from the host's chunks (the game thread may be parked waiting for it, so it cannot
// come through the frame queue): true once every chunk is in.
// The host's board for one TURN boundary of a battle (the last few are kept): true once every chunk of it is in.
bool net_host_board(uint64_t battle, uint32_t turn, BoardAssembled& out);
// HOST, proto 98: publish one boundary's board. The chunks are kept (the last kBoardKeep boundaries) for a client that asks for them (MSG_BOARDREQ, answered on the receive
// thread); `sum` null = turn 0: the chunks go to every client; otherwise every client gets the digest and only the ones that asked to be streamed get the chunks too.
// Returns the number of clients reached.
uint32_t net_board_publish(const BoardMsg* chunks, uint32_t n, const BoardSumMsg* sum);
// CLIENT, proto 98: the host's digest of one boundary (the last many are kept); whether anything of a LATER boundary of that battle has arrived (the host is past it); the request.
bool net_host_boardsum(uint64_t battle, uint32_t turn, BoardSumMsg& out);
bool net_board_seen_past(uint64_t battle, uint32_t turn);
bool net_send_boardreq(const BoardReqMsg& m);
// Forgets what the host sent for battles that are over -- the boards, the rolls and the level box. Called when a save is loaded: a node entered again keeps its battle id, and a stale board of turn 0 would
// otherwise be applied before the host's new one arrives.
void net_clear_battle_caches();
// The host's weather names that came with that level (same mailbox): true when the level message for `node_id` has arrived.
bool net_host_weather(uint64_t node_id, char (*names)[kWeatherLen], uint8_t& n);

// The desync dump. Sent at most once per divergence, so it has no throughput
// budget to respect and no dedupe to do -- by the time it goes out the run is
// already over.
bool net_send_statedump(const StateDumpMsg& m);
bool net_send_peerlog(const PeerLogMsg& m);
bool net_send_audit(const AuditMsg& m);
bool net_send_uqd(const UqdMsg& m);
bool net_send_props(const PropsMsg& m);
bool net_send_rngl(const RnglMsg& m);
bool net_send_chat(const ChatMsg& m);
bool net_send_draw(const DrawMsg& m);
bool net_send_treasure(const TreasureMsg& m);
bool net_send_housecoop(const HouseCoopMsg& m);
bool net_send_setting(const SettingMsg& m);
bool net_send_ping(const PingMsg& m);
bool net_send_equip(const EquipMsg& m);
// proto 100: the cat a Soul Jar of this player's holds (mgmp_souljar), to every other peer. `data` is borrowed for the duration.
bool net_send_soulcat(const SoulCatMsg& m);
bool net_send_nodevote(const NodeVoteMsg& m);
bool net_send_deep(const DeepMsg& m);
bool net_send_halt(const HaltMsg& h);
bool net_send_refuse(const char* reason);
// Point-to-point variants, for the handshake: with several clients, a WELCOME
// broadcast would re-welcome everyone already playing.
bool net_send_hello_to(uint8_t peer, const Hello& h);
// One peer only -- replaying a battle in progress to a joiner. See the note on
// the definition for why this must not be a broadcast.
bool net_send_action_to(uint8_t peer, const ActionMsg& a);
bool net_send_enter_node_to(uint8_t peer, const EnterNodeMsg& m);
bool net_send_welcome_to(uint8_t peer, const Welcome& w);
bool net_send_refuse_to(uint8_t peer, const char* reason);

// Pops one decoded message, or returns false if the queue is empty. Call it
// from the game thread; drain in a loop until it returns false.
bool net_poll(NetMsg& out);

// Diagnostics for the banner and the desync dump.
struct NetStats {
    uint32_t sent = 0, received = 0, dropped = 0;
    uint64_t bytes_sent = 0, bytes_received = 0;
};
NetStats net_stats();
// Bytes this peer has queued for that peer and not yet handed to the socket (the link's outgoing queue, 0.1.50). For the PERF line (0.1.84).
size_t net_queued_bytes(uint8_t peer);

} // namespace mgmp
