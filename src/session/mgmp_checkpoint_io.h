#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace mgmp { namespace checkpoint_io {
using Bytes = std::vector<uint8_t>;
bool read(const std::wstring& path, Bytes& out, uint32_t limit = 5u << 20);
bool atomic_write(const std::wstring& path, const Bytes& bytes);
bool snapshot(const std::wstring& source, Bytes& out);
// What the last snapshot() did when it failed, or how it succeeded if it had to take the file as it is (empty after a plain backup). For a caller's log line.
const char* snapshot_why();
// Whether a file is a SQLite database the game can be given: it has the SQLite header and opens and answers a query -- WITHOUT sqlite's full integrity check, which snapshot() applies and which a save the game
// itself loads happily can still fail. Fills `out` with the bytes. For restoring a backup, where refusing a save the game would have opened is worse than handing it over.
bool loadable(const std::wstring& source, Bytes& out);
bool in_run(const std::wstring& source, bool& running);
// True when this save's adventure has already ENTERED the map: files.trollengine_state's event count is
// nonzero, OR properties.adventure_started is 1 (the count stays 0 until the first node resolves, so on
// its own it called a fresh map save "preparation"). Both are 0 in a save written during departure
// preparation -- cat, gear and chapter screens. A false RETURN means the question could not be answered
// from this file (caller decides the fallback).
bool on_map(const std::wstring& source, bool& started);
// Fresh-session map pre-sync. read_file_row pulls one `files` row as raw
// bytes (have=false when the table or the row is absent -- a clean save; a
// false return means the database itself is unreadable). write_file_row is
// the pre-load overwrite of the receiving peer's own row, shipped as an SQL
// blob literal so no new sqlite entry points are needed.
bool read_file_row(const std::wstring& source, const char* key, int& have, Bytes& out);
bool write_file_row(const std::wstring& target, const char* key, const Bytes& in);
// require_run: the database must be a save that is in a run (on_adventure) -- every node save is. The two saves from before the map (mgmp_checkpoint kStagePrep / kStageReady) are not necessarily.
bool restore(const std::wstring& target, const Bytes& bytes, bool require_run = true);
// One `properties` row as text (integers and reals arrive as their decimal
// spelling). have=false when the row is absent; a false RETURN means the
// database itself could not be opened or read. Read-only, so it is safe on a
// file the game has open. Used by the save-backup list to show progress.
bool read_property(const std::wstring& source, const char* key, bool& have, std::string& out);
// Every row of `properties` (key, data as text), read-only. Used to hand a client the host's WHOLE property table (mgmp_unlocks): the game's own in-memory table is only a cache of the keys read so far.
bool read_all_properties(const std::wstring& source, std::vector<std::pair<std::string, std::string>>& out);
uint64_t nonce();
// The player's persistent identity: a random id kept in mgmp-peer-id.bin NEXT TO THE DLL. If that file is missing (a fresh folder -- the mod was updated by deleting the old folder and
// unpacking the new zip) and `adopt_root` (the mgmp_handshake folder of the save directory, one sub-folder per identity) holds exactly ONE earlier identity, that one is taken over
// (*adopted = true) so the recovery records of the run in progress are still found; with several (two instances on one machine) a new id is made, as before.
bool identity(uint64_t& id, const wchar_t* adopt_root = nullptr, bool* adopted = nullptr);
// The SteamID64 that appears as a folder of the game's save directory (...\Mewgenics\7656119xxxxxxxxxx\saves), or 0 when there is none.
uint64_t steam_id_from_dir(const wchar_t* dir);
} }
