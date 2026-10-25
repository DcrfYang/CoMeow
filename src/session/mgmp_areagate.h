// mgmp_areagate.h -- the host is warned before it leads the room into an area another player has not unlocked (0.1.53, proto 99).
//
// The room lets the host take every player into an area the host has unlocked (a later chapter map is the host's, and a story event's options
// follow the host's flags). What that does to the save of a player who has never been there is unknown, so before it happens the host is
// asked: "有其他玩家尚未解锁该区域，进入可能对存档出现未知的影响，是否还要继续？" with a "do not ask again in this game" box.
//
// The two ways into a new area (see 调研任务列表/结果/区域解锁与进入方式.md for the research):
//   * a map node of type exit (MapNode+0x138 == 3) -- its next_map (std::string at MapNode+0x50, "sewers.gon") is the area. Entered by
//     MapScreen::EnterNode, which the host's click reaches through h_EnterNode before the room is told.
//   * a story event option that begins a chapter -- the option's GON (option entry +0xE8) carries `hint_chapter_exit <area>` and a
//     `begin_chapter <area>.gon` in its result: the rift (Quest_DimensionXPortal) and the time machines (Quest_TimeMachine*). Committed by
//     WorldEvent's option commit, which the host's click reaches through choice_on_event_commit before it is published.
// Choosing an act on the chapter page (desert / lab) needs no question: the host may only choose an act every player has (mgmp_setup).
//
// Each client reports its own map flags (ChapterMsg kChapterFlags) whenever they change; the host compares the target's flag with them.
#pragma once

#include <cstdint>
#include "mgmp_proto.h"

namespace mgmp {

// Every frame (after session_update): a client reports its map flags when they changed, the host forgets players who left.
void areagate_tick();
// HOST: a client's map flags arrived (its READY, or its report).
void areagate_on_flags(uint8_t from, uint32_t flags, const char* how);

// HOST, h_EnterNode, before the room is told: true = the click is held and the question is up (the caller does nothing more).
bool areagate_hold_node(void* map, void* node);
// HOST, h_MapUpdate: the node to enter now that the host said "continue" (nullptr: none). Entering it goes through h_EnterNode again,
// which lets that one node through.
void* areagate_take_confirmed_node(void* map);

// HOST, choice_on_event_commit, before the choice is published: true = held, the question is up (the commit must not run).
bool areagate_hold_event(void* world_event, uint32_t index, const uint8_t* entry);
// HOST, the event screen's update: an option to commit now (it goes through choice_on_event_commit again, which lets it through).
bool areagate_take_confirmed_event(void* world_event, uint32_t& index);

// The question, for the overlay.
constexpr int kAreaGateMaxNames = kMaxPeers;
struct AreaGateView {
    int  area = -1;                              // index into the area table (areagate_area_tx_index), -1: none
    char area_id[24] = {};                       // "sewers", "iceage" ...
    uint8_t n = 0;                               // players without the flag
    char names[kAreaGateMaxNames][48] = {};      // their names (or "P2" ...)
    bool via_event = false;                      // a time machine / the rift, not a map exit
};
bool areagate_view(AreaGateView& out);
// The host's answer. `dont_ask_again`: no more questions until the game is restarted.
void areagate_answer(bool go, bool dont_ask_again);

} // namespace mgmp
