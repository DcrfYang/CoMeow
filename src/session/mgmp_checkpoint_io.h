#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace mgmp { namespace checkpoint_io {
using Bytes = std::vector<uint8_t>;
bool read(const std::wstring& path, Bytes& out, uint32_t limit = 5u << 20);
bool atomic_write(const std::wstring& path, const Bytes& bytes);
bool snapshot(const std::wstring& source, Bytes& out);
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
bool restore(const std::wstring& target, const Bytes& bytes);
// One `properties` row as text (integers and reals arrive as their decimal
// spelling). have=false when the row is absent; a false RETURN means the
// database itself could not be opened or read. Read-only, so it is safe on a
// file the game has open. Used by the save-backup list to show progress.
bool read_property(const std::wstring& source, const char* key, bool& have, std::string& out);
uint64_t nonce();
bool identity(uint64_t& id);
} }
