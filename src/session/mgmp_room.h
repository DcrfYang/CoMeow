// mgmp_room.h -- what the ROOM decides, as opposed to the session (2026-09-30).
//
// The lobby's room (mgmp_signal) and the game session (mgmp_session / mgmp_net) have different
// lifetimes: a player can stay in a room while its transport is down -- reconnecting, or away playing a
// house boss fight on its own. This module is the layer that looks at both:
//
//   * THE LOCK. The host locks the room before anybody may pick a save. A locked room is hidden from the
//     server's list and refuses joins; it can only be unlocked while every player stands on the main menu
//     or the save screen, and unlocking starts the save-selection round over.
//
//   * WHO IS WHO. The room list is in room order (P1 = the host, P2.. in join order); the session's ids
//     are the transport's and stop matching that order the moment anyone reconnects. Every peer says its
//     lobby name (ROOMCTL kRoomName), and rows are paired with ids by name, falling back to ascending ids.
//
//   * DROPPED PLAYERS. While this player is in a game (warehouse, collar, gear, chapter, the run), a peer
//     that disappears from the session or goes back to the main menu is announced once:
//     "PX玩家掉线，如果要继续游戏，请所有玩家回到主菜单重新开始游戏->选择存档并进入游戏".
//
//   * THE HOUSE BOSS IS PLAYED ALONE. On the day a house boss arrives (House+0x478 house_boss_countdown
//     == 0 with a boss named at House+0x458, House = MewDirector+0x5A8; the save file carries the same two
//     values as `house_boss_countdown` / `next_house_boss`), the fight is not replicated. That player
//     marks itself away in the room, drops the transport, loads its save (or stays in its warehouse) and
//     fights alone; back in the warehouse with the boss gone, it returns to the main menu, rejoins the
//     session and picks a save with everybody else again.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mgmp {

struct RoomCtlMsg;

void room_tick();                                       // from the frame hook
void room_on_message(uint8_t from, const RoomCtlMsg& m);

// --- the save screen ---------------------------------------------------------------------------------
// From savefile_on_slot_click, for a click made by a person (not the handshake's own load). `path` is the
// save the click names, or null when it could not be resolved. True = the click is consumed here.
bool room_on_slot_click(int slot, const wchar_t* path);

// Start the save-selection round over, here and on every other peer.
void room_abort_selection();

// --- the lock ----------------------------------------------------------------------------------------
bool room_is_host();
bool room_locked();
bool room_can_unlock();         // every player on the main menu or the save screen, nobody away
void room_request_lock(bool on);

// --- the room list -----------------------------------------------------------------------------------
// The session id of row `row` of the room list (room order), 0xFF when that player has none (away, still
// connecting, or this row is us).
uint8_t room_peer_for_row(uint32_t row);
// The row (0-based, so P = row + 1) of a session id, -1 when it cannot be placed.
int     room_row_of_peer(uint8_t id);
bool    room_row_away(uint32_t row);
int     room_self_row();

// --- the party-size limit (2026-10-01) -------------------------------------------------------------------
// Balance for bigger rooms: with 3 players each brings at most 3 cats, with 4 players at most 2; 2 players
// (and single player) are unchanged. 0 = no limit.
int  room_party_limit();
// From the House's departure (T_TryDepart) with the number of cats in the box. False = refused (and the
// player has been told the limit).
bool room_depart_allowed(uint32_t cats);

// A client's departure was refused because a selected cat wears a main-story item (the host alone carries
// those). Tells the player which item; always returns false so the caller can `return` it.
bool room_refuse_story_item(const char* item);

// The three one-line hints of the preparation phase (toasts; the same text is not repeated within a moment):
//  - a client tried to equip a main-story quest item,
//  - a client pressed a chapter (only the host chooses it),
//  - the host pressed a chapter while some player is not ready yet.
void room_say_story_only_host();
void room_say_chapter_client();
void room_say_chapter_wait();
// The host picked a chapter some player has not unlocked: 2 = somebody lacks chapter 2 (chapters 2 and 3 are closed), 3 = only chapter 3 is closed.
void room_say_chapter_locked_2();
void room_say_chapter_locked_3();
// A player without chapter 2 locked the gear screen: a client is READY and waits for the host, the host waits for every READY.
void room_say_no_chapter_ready();
void room_say_no_chapter_wait();

// A sync that waits for another player's data ran out of time: the player is told (a notice with an OK) that the client's connection to the
// host may have a problem and the games cannot be kept in sync. `what` names the data, for the log. Silent outside a session.
void room_sync_trouble(const char* what);

// --- solo house boss ---------------------------------------------------------------------------------
bool room_solo_active();        // this player is away fighting a house boss on its own

// --- what the panel shows ----------------------------------------------------------------------------
// A notice needs an OK; a toast is a line that fades.
bool room_notice(char* out, size_t cap);   // the front notice, if any
void room_notice_dismiss();
bool room_take_toast(char* out, size_t cap);

} // namespace mgmp
