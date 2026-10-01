// mgmp_saveslots.h -- the main menu's "save backup" list. Layer 5, player-facing.
//
// WHAT IT IS
//
// Twenty numbered positions. Each holds a complete copy of the game's three
// save slots (steamcampaign01/02/03.sav) plus a name and the time it was taken.
// The player can put the current three slots into a position, and can put a
// position back over the current three. That is the whole feature.
//
// WHY IT IS A COPY OF THE FILES AND NOT A SAVE FORMAT OF OUR OWN
//
// A slot is a self-contained sqlite file and the game reads nothing else, so a
// bit-for-bit copy of the three files IS "the whole save". Anything cleverer
// would have to decide which tables matter, and every table that was left out
// would be a bug report about a load that "mostly" worked.
//
// The copy is taken with sqlite's online backup API (checkpoint_io::snapshot),
// not with CopyFile: a live database can have a journal that CopyFile would
// leave behind, and the copy would then be a save that lost its last write.
//
// WHERE THEY LIVE
//
//   <game save dir>\mgmp_saveslots\slot_01 .. slot_20\   the positions
//   <game save dir>\mgmp_saveslots\_undo\                what the last LOAD replaced
//
// Beside the game's own files, so the backups follow the account they belong
// to and a second Steam account cannot load someone else's run.
//
// THE HANDSHAKE QUEUE TRAVELS WITH THEM (2026-09-30)
//
// A multiplayer run is continued from the confirmed saves this player holds for it (mgmp_checkpoint:
// mgmp_handshake\\<player identity>\\<pairing>.0 ..3, .state). They are the other half of "the save at that
// moment": restoring three slots from a backup while leaving the queue of a LATER run behind would offer
// the room saves that no longer belong to those slots. So a position also holds a copy of every file in
// this player's queue directory (slot_NN\\handshake\\), and loading a position puts them back, replacing
// what is there; the undo point takes them too.
//
// WHEN IT MAY RUN
//
// The caller decides (the main menu, and not while a multiplayer room is
// open). This module refuses on its own only what is unsafe for the FILES: a
// slot whose database is held open by a game (journal sidecar, or the file
// cannot be opened for replacement), and a backup that does not pass sqlite's
// integrity check. Nothing here touches the game's memory.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mgmp {

constexpr int kSaveBackupCount = 20;   // positions in the list
constexpr int kGameSlots       = 3;    // steamcampaign01..03
constexpr int kSaveNameMax     = 48;   // bytes, UTF-8, including the NUL

// What the game itself shows for one slot on its save screen.
struct SaveSlotInfo {
    bool     present = false;   // the slot has a save at all
    int      percent = -1;      // properties.save_file_percent; -1 = unreadable
    int      day     = -1;      // properties.current_day
    int64_t  timer   = -1;      // properties.savefile_timer, in 1/60 s
};

struct SaveBackup {
    bool         used = false;
    char         name[kSaveNameMax] = {};
    int64_t      saved_at = 0;          // unix seconds, local clock
    // How many files of this player's handshake queue (mgmp_handshake\\<identity>\\) were taken with the
    // three slots. -1 = not recorded (a backup from before 2026-09-30): loading it leaves the current
    // queue alone. 0 = recorded, and there was none -- loading it clears the queue.
    int          handshake = -1;
    SaveSlotInfo slot[kGameSlots];
};

// Rescans the directory. Cheap (20 small json files), so the menu calls it when
// the window opens and after every action rather than caching across frames.
void saveslots_refresh();

const SaveBackup& saveslots_get(int index);       // 0..19; an empty one out of range

// The live state of the three game slots, for the "current" header row.
void saveslots_current(SaveSlotInfo out[kGameSlots]);

// Each returns false and leaves saveslots_message() saying why. A message that
// begins with '!' is a failure, anything else is a confirmation.
bool saveslots_save(int index, const char* name);     // overwrites the position
bool saveslots_load(int index);                        // replaces the game's 3 slots
bool saveslots_delete(int index);
bool saveslots_rename(int index, const char* name);

// EXPORT / IMPORT (2026-10-01): one position <-> ONE file (*.mgmpsave) anywhere on disk. The file holds the
// whole position folder -- meta.json, the three .sav and the handshake queue -- so an import is exactly the
// backup that was exported, and it is loaded afterwards the usual way (with its undo point).
//
// Format, little-endian: "MGMPSAVE" | u32 version (1) | u32 file count | per file: u16 path length, UTF-8
// path relative to the position ('/' separated, no "..", no drive), u64 size, bytes | u64 FNV-1a of every
// byte before it. An import is refused unless the checksum, every path, meta.json and every save database
// it names are sound -- and it is written to a staging folder first, so a bad file never touches a position.
bool saveslots_export(int index, const wchar_t* path);
bool saveslots_import(int index, const wchar_t* path);

bool saveslots_undo_available();
bool saveslots_undo();                                 // put back what the last load replaced

const char* saveslots_message();
// Whether that message reports a successful load (the menu adds "you can press Play now").
bool saveslots_message_is_load();

// "H:MM:SS" the way the game's save screen prints play time, from 1/60 s ticks.
void saveslots_format_time(int64_t timer, char* out, size_t cap);

} // namespace mgmp
