// mgmp_roomhist.h -- which lobby rooms this player joined lately, so the room list can put them (and the rooms of the same host) first.
//
// A join is noted when the lobby confirms it (the room the player clicked is the room this peer is in). The record is a small text file beside mgmp.dll
// (mgmp-rooms.txt, one line per join: unix seconds, host name, room id), kept for one day: older lines are dropped whenever the file is written.
// "Recent" for a listed room means: this room was joined within a day, OR its host is the host of a room joined within a day (the host's other rooms are the likely ones to return to).
#pragma once

namespace mgmp {

// The player clicked Join on this room of the list (the host name is what the list shows). Remembered until the lobby says the join worked.
void roomhist_set_pending(const char* room_id, const char* host);

// Every frame (cheap): notes the pending join once this peer is actually in that room.
void roomhist_tick();

// Is this listed room one to put first? (joined within a day, or hosted by someone whose room was joined within a day)
bool roomhist_recent(const char* room_id, const char* host);

} // namespace mgmp
