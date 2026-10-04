#include "mgmp_saveslots.h"
#include "mgmp_i18n.h"

#include <windows.h>
#include <ctime>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "json.hpp"

#include "mgmp_addresses.h"
#include "mgmp_checkpoint_io.h"
#include "mgmp_leave.h"
#include "mgmp_log.h"
#include "mgmp_savefile.h"

namespace mgmp {
namespace {

using Bytes = checkpoint_io::Bytes;

constexpr const wchar_t* kGameFile[kGameSlots] = {
    L"steamcampaign01.sav", L"steamcampaign02.sav", L"steamcampaign03.sav"
};
constexpr const wchar_t* kSidecar[] = { L"-wal", L"-shm", L"-journal" };

struct State {
    SaveBackup list[kSaveBackupCount];
    SaveBackup autos[kAutoSaveCount];
    char       msg[640] = {};
    bool       msg_load = false;
};
State g;

void say(const char* fmt, ...) {
    g.msg_load = false;
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(g.msg, sizeof(g.msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    // Failures go to the log at Warn so a report of "the load did nothing" has
    // a line behind it; confirmations are ordinary Info.
    if (g.msg[0] == '!') log_line_lvl(LogLevel::Warn, "SAVESLOTS", "%s", g.msg);
    else                 log_line("SAVESLOTS", "%s", g.msg);
}

// --- paths ------------------------------------------------------------------

bool game_dir(std::wstring& out) {
    wchar_t buf[MAX_PATH * 2] = {};
    if (!savefile_save_dir(buf, sizeof(buf) / sizeof(buf[0]))) return false;
    out = buf;
    while (!out.empty() && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
    return !out.empty();
}

bool exists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool ensure_dir(const std::wstring& p) {
    if (CreateDirectoryW(p.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring root_of(const std::wstring& game) { return game + L"\\mgmp_saveslots"; }

std::wstring slot_dir(const std::wstring& game, int i) {
    wchar_t leaf[32];
    swprintf_s(leaf, L"\\slot_%02d", i + 1);
    return root_of(game) + leaf;
}

std::wstring undo_dir(const std::wstring& game) { return root_of(game) + L"\\_undo"; }

// the auto queue: auto_01 (newest) .. auto_03
std::wstring auto_dir(const std::wstring& game, int i) {
    wchar_t leaf[32];
    swprintf_s(leaf, L"\\auto_%02d", i + 1);
    return root_of(game) + leaf;
}

// Empties a directory of plain files and removes it. Only ever pointed at our
// own folders, and only ever removes files directly inside them.
void wipe_dir(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                // A backup folder nests handshake\<identity>\ ; never a reparse point.
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0 ||
                    (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
                wipe_dir(dir + L"\\" + fd.cFileName);
                continue;
            }
            DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// staged -> final, replacing whatever was there. Not atomic across the whole
// folder (Windows has no such call), but the new contents are complete and
// verified before the old ones are touched, and a failure of the last step
// leaves the staged copy behind for the log line to name.
bool swap_dir(const std::wstring& staged, const std::wstring& final_dir) {
    if (exists(final_dir)) wipe_dir(final_dir);
    return MoveFileW(staged.c_str(), final_dir.c_str()) != 0;
}

std::string utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

// --- reading a slot's headline numbers ----------------------------------------

bool read_int(const std::wstring& file, const char* key, int64_t& out) {
    bool have = false; std::string text;
    if (!checkpoint_io::read_property(file, key, have, text) || !have) return false;
    // savefile_timer_adjusted is a REAL; the ones read here are integers, but a
    // real spelled "25190095.0" must not silently become 0.
    out = (int64_t)_strtoi64(text.c_str(), nullptr, 10);
    return true;
}

SaveSlotInfo describe(const std::wstring& file) {
    SaveSlotInfo s;
    if (!exists(file)) return s;
    s.present = true;
    int64_t v = 0;
    if (read_int(file, "save_file_percent", v)) s.percent = (int)v;
    if (read_int(file, "current_day", v))       s.day     = (int)v;
    if (read_int(file, "savefile_timer", v))    s.timer   = v;
    return s;
}

// --- meta.json ---------------------------------------------------------------

bool read_text(const std::wstring& path, std::string& out) {
    Bytes b;
    if (!checkpoint_io::read(path, b, 1u << 20)) return false;
    out.assign((const char*)b.data(), b.size());
    return true;
}

bool write_text(const std::wstring& path, const std::string& text) {
    return checkpoint_io::atomic_write(path, Bytes(text.begin(), text.end()));
}

std::string meta_to_json(const SaveBackup& b) {
    nlohmann::json j;
    j["name"]     = b.name;
    j["saved_at"] = b.saved_at;
    j["handshake"] = b.handshake;
    for (int s = 0; s < kGameSlots; ++s) {
        nlohmann::json e;
        e["present"] = b.slot[s].present;
        e["percent"] = b.slot[s].percent;
        e["day"]     = b.slot[s].day;
        e["timer"]   = b.slot[s].timer;
        j["slots"].push_back(e);
    }
    return j.dump(1);
}

bool meta_from_json(const std::string& text, SaveBackup& out) {
    try {
        auto j = nlohmann::json::parse(text);
        out = SaveBackup{};
        out.used = true;
        strncpy_s(out.name, sizeof(out.name), j.value("name", std::string()).c_str(), _TRUNCATE);
        out.saved_at = j.value("saved_at", (int64_t)0);
        out.handshake = j.value("handshake", -1);
        if (j.contains("slots") && j["slots"].is_array()) {
            for (int s = 0; s < kGameSlots && s < (int)j["slots"].size(); ++s) {
                const auto& e = j["slots"][s];
                out.slot[s].present = e.value("present", false);
                out.slot[s].percent = e.value("percent", -1);
                out.slot[s].day     = e.value("day", -1);
                out.slot[s].timer   = e.value("timer", (int64_t)-1);
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

void clean_name(const char* in, char* out, size_t cap) {
    std::string s = in ? in : "";
    // No control characters, and no leading/trailing blanks: the name goes in a
    // json string and on a button, and an invisible name is a name nobody finds.
    std::string t;
    for (unsigned char c : s) if (c >= 0x20 && c != 0x7F) t.push_back((char)c);
    size_t a = t.find_first_not_of(' ');
    size_t b = t.find_last_not_of(' ');
    t = (a == std::string::npos) ? std::string() : t.substr(a, b - a + 1);
    if (t.size() >= cap) {
        t.resize(cap - 1);
        // Do not leave a UTF-8 sequence cut in half.
        while (!t.empty() && ((unsigned char)t.back() & 0xC0) == 0x80) t.pop_back();
        if (!t.empty() && ((unsigned char)t.back() & 0x80)) t.pop_back();
    }
    strncpy_s(out, cap, t.c_str(), _TRUNCATE);
}

// --- the two directions --------------------------------------------------------

// --- the handshake queue ---------------------------------------------------------------------

// `name` is relative to mgmp_handshake\ : "<player identity>\<pairing>.0" and so on.
struct HsFile { std::wstring name; Bytes data; };
using HsList = std::vector<HsFile>;

std::wstring hs_root(const std::wstring& game) { return game + L"\\mgmp_handshake"; }

bool plain_name(const wchar_t* n) { return wcscmp(n, L".") != 0 && wcscmp(n, L"..") != 0; }

// Every player's queue folder under mgmp_handshake\ and the files directly inside each. A real machine
// holds ONE identity; two game instances on one machine (how this is tested) hold two, and a backup that
// took only one queue would leave the other one describing a later run than the slots it goes with.
// `dir` may be a backup's handshake\ folder as well as the live one. False: a file could not be read.
bool read_hs_tree(const std::wstring& root, HsList& out) {
    WIN32_FIND_DATAW sub;
    HANDLE hs = FindFirstFileW((root + L"\\*").c_str(), &sub);
    if (hs == INVALID_HANDLE_VALUE) return true;                 // no folder = no queue
    bool ok = true;
    do {
        if (!(sub.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !plain_name(sub.cFileName) ||
            (sub.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        const std::wstring d = root + L"\\" + sub.cFileName;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            HsFile f; f.name = std::wstring(sub.cFileName) + L"\\" + fd.cFileName;
            if (!checkpoint_io::read(root + L"\\" + f.name, f.data, 256u << 20)) {
                say(tr(Tx::SS_READ_HS_FAIL), f.name.c_str()); ok = false; break;
            }
            out.push_back(std::move(f));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    } while (ok && FindNextFileW(hs, &sub));
    FindClose(hs);
    return ok;
}

// Capture for a backup. There is always something to record: an empty list means "the queue was empty".
bool capture_handshake(const std::wstring& game, HsList& out, bool& have) {
    have = true;
    return read_hs_tree(hs_root(game), out);
}

// Write `files` under `root`, creating the identity folders as needed.
bool write_hs_tree(const std::wstring& root, const HsList& files) {
    if (!ensure_dir(root)) return false;
    for (const HsFile& f : files) {
        const size_t cut = f.name.find(L'\\');
        if (cut == std::wstring::npos) return false;
        if (!ensure_dir(root + L"\\" + f.name.substr(0, cut))) return false;
        if (!checkpoint_io::atomic_write(root + L"\\" + f.name, f.data)) { say(tr(Tx::SS_WRITE_HS_FAIL_F), f.name.c_str()); return false; }
    }
    return true;
}

// Put a recorded queue back: every file of the current queue goes, the recorded ones are written. Files
// only, inside mgmp_handshake\ -- nothing else is ever touched.
bool restore_handshake(const std::wstring& game, const HsList& files) {
    const std::wstring root = hs_root(game);
    WIN32_FIND_DATAW sub;
    HANDLE hs = FindFirstFileW((root + L"\\*").c_str(), &sub);
    if (hs != INVALID_HANDLE_VALUE) {
        do {
            if (!(sub.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !plain_name(sub.cFileName) ||
                (sub.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            const std::wstring d = root + L"\\" + sub.cFileName;
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (!DeleteFileW((d + L"\\" + fd.cFileName).c_str())) {
                    FindClose(h); FindClose(hs); say(tr(Tx::SS_CLEAR_HS_FAIL), fd.cFileName); return false;
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        } while (FindNextFileW(hs, &sub));
        FindClose(hs);
    }
    return write_hs_tree(root, files);
}

// The three game files, each captured as a consistent copy. `have[s]` false
// means the slot has no file. False return: a file exists and could not be
// captured, in which case nothing has been written anywhere.
bool capture_current(const std::wstring& game, Bytes out[kGameSlots], bool have[kGameSlots]) {
    for (int s = 0; s < kGameSlots; ++s) {
        const std::wstring f = game + L"\\" + kGameFile[s];
        have[s] = false;
        if (!exists(f)) continue;
        if (checkpoint_io::snapshot(f, out[s])) { have[s] = true; continue; }
        // The database would not open cleanly (corrupt, or locked exclusively).
        // Still worth having: a byte copy of a damaged save beats no copy when
        // this is the undo point for a load.
        if (checkpoint_io::read(f, out[s], 64u << 20)) { have[s] = true; continue; }
        say(tr(Tx::SS_READ_FAIL), kGameFile[s]);
        return false;
    }
    return true;
}

// Stages a folder holding `bytes` + meta, then swaps it in as `final_dir`.
bool write_folder(const std::wstring& game, const std::wstring& final_dir, const Bytes bytes[kGameSlots],
                  const bool have[kGameSlots], SaveBackup& meta, const HsList* hs = nullptr) {
    const std::wstring root = root_of(game);
    if (!ensure_dir(root)) { say(tr(Tx::SS_CREATE_FAIL), root.c_str()); return false; }

    const std::wstring stage = root + L"\\_stage";
    wipe_dir(stage);
    if (!ensure_dir(stage)) { say(tr(Tx::SS_TMP_FAIL)); return false; }

    for (int s = 0; s < kGameSlots; ++s) {
        meta.slot[s] = SaveSlotInfo{};
        if (!have[s]) continue;
        const std::wstring f = stage + L"\\" + kGameFile[s];
        if (!checkpoint_io::atomic_write(f, bytes[s])) {
            say(tr(Tx::SS_WRITE_FAIL), kGameFile[s]);
            wipe_dir(stage);
            return false;
        }
        meta.slot[s] = describe(f);
    }
    meta.used = true;
    meta.handshake = -1;
    if (hs) {
        if (!write_hs_tree(stage + L"\\handshake", *hs)) { say(tr(Tx::SS_WRITE_HS_FAIL)); wipe_dir(stage); return false; }
        meta.handshake = (int)hs->size();
    }
    if (!write_text(stage + L"\\meta.json", meta_to_json(meta))) {
        say(tr(Tx::SS_META_FAIL));
        wipe_dir(stage);
        return false;
    }
    if (!swap_dir(stage, final_dir)) {
        say(tr(Tx::SS_PLACE_FAIL), stage.c_str());
        return false;
    }
    return true;
}

// Is a game slot file free to be replaced?
bool target_free(const std::wstring& f) {
    for (const wchar_t* sfx : kSidecar)
        if (exists(f + sfx)) return false;
    if (!exists(f)) return true;
    // Exclusive access: FILE_SHARE_DELETE only fails if anyone else has the file
    // open for anything, which is exactly a game holding its database.
    HANDLE lease = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (lease == INVALID_HANDLE_VALUE) return false;
    CloseHandle(lease);
    return true;
}

// Puts the folder's files over the game's three slots. The caller has already
// taken the undo copy if one is wanted.
bool install_from(const std::wstring& game, const std::wstring& dir, const SaveBackup& meta) {
    Bytes bytes[kGameSlots];
    bool  want[kGameSlots] = {};

    // 1. Read and verify EVERYTHING before touching a single game file: a load
    //    that gets two slots in and then finds the third corrupt has made a mess
    //    that no undo point describes.
    for (int s = 0; s < kGameSlots; ++s) {
        if (!meta.slot[s].present) continue;
        const std::wstring f = dir + L"\\" + kGameFile[s];
        if (!exists(f)) { say(tr(Tx::SS_MISSING), kGameFile[s]); return false; }
        if (!checkpoint_io::snapshot(f, bytes[s])) {
            say(tr(Tx::SS_CORRUPT), kGameFile[s]);
            return false;
        }
        want[s] = true;
    }
    HsList hs;
    if (meta.handshake >= 0 && !read_hs_tree(dir + L"\\handshake", hs)) return false;

    // 2. Every target must be free.
    for (int s = 0; s < kGameSlots; ++s) {
        if (!target_free(game + L"\\" + kGameFile[s])) {
            say(tr(Tx::SS_IN_USE), kGameFile[s]);
            return false;
        }
    }

    // 3. Replace.
    for (int s = 0; s < kGameSlots; ++s) {
        const std::wstring f = game + L"\\" + kGameFile[s];
        if (want[s]) {
            if (!checkpoint_io::atomic_write(f, bytes[s])) {
                say(tr(Tx::SS_WRITE_PARTIAL), kGameFile[s]);
                return false;
            }
        } else if (exists(f)) {
            // The backup was taken with this slot empty.
            if (!DeleteFileW(f.c_str())) {
                say(tr(Tx::SS_CLEAR_FAIL), kGameFile[s]);
                return false;
            }
        }
    }
    // 4. The handshake queue that belongs to those slots.
    if (meta.handshake >= 0 && !restore_handshake(game, hs)) return false;
    return true;
}

bool load_meta(const std::wstring& dir, SaveBackup& out) {
    std::string text;
    return read_text(dir + L"\\meta.json", text) && meta_from_json(text, out);
}

// --- the one-file package -------------------------------------------------------------------------------

constexpr char     kPackMagic[8]   = { 'M', 'G', 'M', 'P', 'S', 'A', 'V', 'E' };
constexpr uint32_t kPackVersion    = 1;
constexpr uint32_t kPackMaxFiles   = 256;
constexpr uint64_t kPackMaxBytes   = 512ull << 20;

uint64_t fnv1a(const uint8_t* p, size_t n, uint64_t h = 1469598103934665603ull) {
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

struct PackFile { std::string path; Bytes data; };

// Every plain file under `dir`, recursively, as paths relative to it.
bool collect(const std::wstring& dir, const std::wstring& rel, std::vector<PackFile>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    do {
        if (!plain_name(fd.cFileName) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        const std::wstring child = dir + L"\\" + fd.cFileName;
        const std::wstring crel = rel.empty() ? std::wstring(fd.cFileName) : rel + L"/" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { ok = collect(child, crel, out); continue; }
        PackFile f; f.path = utf8(crel);
        if (!checkpoint_io::read(child, f.data, 256u << 20)) { say(tr(Tx::SS_READ_FAIL), child.c_str()); ok = false; }
        else out.push_back(std::move(f));
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

// A path from a package: relative, '/' separated, nothing that climbs out or names a device.
bool safe_path(const std::string& p) {
    if (p.empty() || p.size() > 200 || p[0] == '/' || p.find('\\') != std::string::npos ||
        p.find(':') != std::string::npos) return false;
    size_t a = 0;
    while (a <= p.size()) {
        size_t b = p.find('/', a); if (b == std::string::npos) b = p.size();
        const std::string part = p.substr(a, b - a);
        if (part.empty() || part == "." || part == "..") return false;
        for (unsigned char c : part) if (c < 0x20 || strchr("<>\"|?*", c)) return false;
        a = b + 1;
    }
    return true;
}

bool write_pack(const std::wstring& path, const std::vector<PackFile>& files) {
    Bytes out;
    auto put = [&](const void* p, size_t n) { const uint8_t* b = (const uint8_t*)p; out.insert(out.end(), b, b + n); };
    put(kPackMagic, 8);
    const uint32_t ver = kPackVersion, count = (uint32_t)files.size();
    put(&ver, 4); put(&count, 4);
    for (const PackFile& f : files) {
        const uint16_t ln = (uint16_t)f.path.size(); const uint64_t sz = f.data.size();
        put(&ln, 2); put(f.path.data(), ln); put(&sz, 8);
        if (sz) put(f.data.data(), (size_t)sz);
    }
    const uint64_t sum = fnv1a(out.data(), out.size());
    put(&sum, 8);
    return checkpoint_io::atomic_write(path, out);
}

bool read_pack(const std::wstring& path, std::vector<PackFile>& files) {
    Bytes in;
    if (!checkpoint_io::read(path, in, (uint32_t)kPackMaxBytes)) return false;
    if (in.size() < 8 + 4 + 4 + 8 || memcmp(in.data(), kPackMagic, 8) != 0) return false;
    uint64_t sum = 0; memcpy(&sum, in.data() + in.size() - 8, 8);
    if (fnv1a(in.data(), in.size() - 8) != sum) return false;
    size_t at = 8; const size_t end = in.size() - 8;
    auto get = [&](void* p, size_t n) { if (end - at < n) return false; memcpy(p, in.data() + at, n); at += n; return true; };
    uint32_t ver = 0, count = 0;
    if (!get(&ver, 4) || !get(&count, 4) || ver != kPackVersion || count > kPackMaxFiles) return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t ln = 0; uint64_t sz = 0; PackFile f;
        if (!get(&ln, 2) || end - at < ln) return false;
        f.path.assign((const char*)in.data() + at, ln); at += ln;
        if (!get(&sz, 8) || sz > end - at || !safe_path(f.path)) return false;
        f.data.assign(in.begin() + at, in.begin() + at + (size_t)sz); at += (size_t)sz;
        files.push_back(std::move(f));
    }
    return at == end;
}

} // namespace

// ---------------------------------------------------------------------------

const char* saveslots_message() { return g.msg; }
bool saveslots_message_is_load() { return g.msg_load && g.msg[0] != '!'; }

void saveslots_format_time(int64_t timer, char* out, size_t cap) {
    if (timer < 0) { _snprintf_s(out, cap, _TRUNCATE, "--:--:--"); return; }
    const int64_t secs = timer / 60;   // the game counts in 1/60 s
    _snprintf_s(out, cap, _TRUNCATE, "%lld:%02lld:%02lld",
                (long long)(secs / 3600), (long long)((secs / 60) % 60), (long long)(secs % 60));
}

void saveslots_refresh() {
    for (auto& b : g.list) b = SaveBackup{};
    for (auto& b : g.autos) b = SaveBackup{};
    std::wstring game;
    if (!game_dir(game)) return;
    for (int i = 0; i < kAutoSaveCount; ++i) {
        const std::wstring d = auto_dir(game, i);
        SaveBackup b;
        if (exists(d) && load_meta(d, b)) g.autos[i] = b;
    }
    for (int i = 0; i < kSaveBackupCount; ++i) {
        const std::wstring d = slot_dir(game, i);
        if (!exists(d)) continue;
        SaveBackup b;
        if (load_meta(d, b)) g.list[i] = b;
    }
}

const SaveBackup& saveslots_auto_get(int index) {
    static const SaveBackup kEmpty;
    return (index >= 0 && index < kAutoSaveCount) ? g.autos[index] : kEmpty;
}

const SaveBackup& saveslots_get(int index) {
    static const SaveBackup kEmpty;
    return (index >= 0 && index < kSaveBackupCount) ? g.list[index] : kEmpty;
}

void saveslots_current(SaveSlotInfo out[kGameSlots]) {
    std::wstring game;
    for (int s = 0; s < kGameSlots; ++s) out[s] = SaveSlotInfo{};
    if (!game_dir(game)) return;
    for (int s = 0; s < kGameSlots; ++s) out[s] = describe(game + L"\\" + kGameFile[s]);
}

bool saveslots_save(int index, const char* name) {
    if (index < 0 || index >= kSaveBackupCount) { say(tr(Tx::SS_NO_POS)); return false; }
    std::wstring game;
    if (!game_dir(game)) { say(tr(Tx::SS_NO_DIR)); return false; }

    Bytes bytes[kGameSlots]; bool have[kGameSlots] = {};
    if (!capture_current(game, bytes, have)) return false;
    if (!have[0] && !have[1] && !have[2]) { say(tr(Tx::SS_NOTHING)); return false; }
    HsList hs; bool have_hs = false;
    if (!capture_handshake(game, hs, have_hs)) return false;

    SaveBackup b;
    clean_name(name, b.name, sizeof(b.name));
    if (!b.name[0]) {
        char fallback[kSaveNameMax];
        _snprintf_s(fallback, sizeof(fallback), _TRUNCATE, tr(Tx::SS_DEFAULT_NAME), index + 1);
        strncpy_s(b.name, sizeof(b.name), fallback, _TRUNCATE);
    }
    b.saved_at = (int64_t)time(nullptr);

    if (!write_folder(game, slot_dir(game, index), bytes, have, b, have_hs ? &hs : nullptr)) return false;
    saveslots_refresh();
    if (have_hs) say(tr(Tx::SS_SAVED_HS), index + 1, b.name, (int)hs.size());
    else         say(tr(Tx::SS_SAVED), index + 1, b.name);
    return true;
}

namespace {
// Puts the position in `dir` over the game's three slots, with the undo point first. `autoq` only picks the words of the confirmation.
bool load_dir(const std::wstring& game, const std::wstring& dir, int index, bool autoq) {
    SaveBackup meta;
    if (!exists(dir) || !load_meta(dir, meta)) { say(tr(Tx::SS_EMPTY_POS), index + 1); return false; }

    // Refuse BEFORE the undo copy: no point overwriting the previous undo point
    // with a state we are about to fail to leave.
    for (int s = 0; s < kGameSlots; ++s) {
        if (!target_free(game + L"\\" + kGameFile[s])) {
            say(tr(Tx::SS_IN_USE), kGameFile[s]);
            return false;
        }
    }

    Bytes cur[kGameSlots]; bool have[kGameSlots] = {};
    if (!capture_current(game, cur, have)) return false;
    HsList cur_hs; bool have_cur_hs = false;
    if (!capture_handshake(game, cur_hs, have_cur_hs)) return false;
    SaveBackup undo;
    strncpy_s(undo.name, sizeof(undo.name), tr(Tx::SS_UNDO_NAME), _TRUNCATE);
    undo.saved_at = (int64_t)time(nullptr);
    if (!write_folder(game, undo_dir(game), cur, have, undo, have_cur_hs ? &cur_hs : nullptr)) return false;

    if (!install_from(game, dir, meta)) return false;
    if (autoq)                      say(tr(Tx::SS_AUTO_LOADED), index + 1, meta.name);
    else if (meta.handshake >= 0)   say(tr(Tx::SS_LOADED_HS), index + 1, meta.name, meta.handshake);
    else                            say(tr(Tx::SS_LOADED), index + 1, meta.name);
    g.msg_load = true;
    return true;
}
} // namespace

bool saveslots_load(int index) {
    if (index < 0 || index >= kSaveBackupCount) { say(tr(Tx::SS_NO_POS)); return false; }
    std::wstring game;
    if (!game_dir(game)) { say(tr(Tx::SS_NO_DIR)); return false; }
    return load_dir(game, slot_dir(game, index), index, false);
}

bool saveslots_auto_load(int index) {
    if (index < 0 || index >= kAutoSaveCount) { say(tr(Tx::SS_NO_POS)); return false; }
    std::wstring game;
    if (!game_dir(game)) { say(tr(Tx::SS_NO_DIR)); return false; }
    return load_dir(game, auto_dir(game, index), index, true);
}

bool saveslots_auto_export(int index, const wchar_t* path) {
    if (index < 0 || index >= kAutoSaveCount || !path || !path[0]) { say(tr(Tx::SS_NO_POS)); return false; }
    std::wstring game;
    if (!game_dir(game)) { say(tr(Tx::SS_NO_DIR)); return false; }
    const std::wstring dir = auto_dir(game, index);
    SaveBackup meta;
    if (!exists(dir) || !load_meta(dir, meta)) { say(tr(Tx::SS_EMPTY_POS), index + 1); return false; }
    std::vector<PackFile> files;
    if (!collect(dir, L"", files)) return false;
    if (!write_pack(path, files)) { say(tr(Tx::SS_EXPORT_FAIL), path); return false; }
    say(tr(Tx::SS_AUTO_EXPORTED), index + 1, path);
    return true;
}

namespace {
// A folder rename can fail for a moment right after files were written into it (a virus scanner or the search indexer holds the new files open): tried again for about a second before it counts as failed.
bool move_retry(const std::wstring& from, const std::wstring& to) {
    for (int tries = 0; tries < 20; ++tries) {
        if (MoveFileW(from.c_str(), to.c_str())) return true;
        Sleep(50);
    }
    return false;
}
void wipe_retry(const std::wstring& dir) {
    for (int tries = 0; tries < 20 && exists(dir); ++tries) { wipe_dir(dir); if (exists(dir)) Sleep(50); }
}
} // namespace

bool saveslots_auto_push() {
    std::wstring game;
    if (!game_dir(game)) { log_line("SAVESLOTS", "auto save skipped: the game's save folder is not known"); return false; }
    Bytes bytes[kGameSlots]; bool have[kGameSlots] = {};
    if (!capture_current(game, bytes, have)) return false;
    if (!have[0] && !have[1] && !have[2]) { log_line("SAVESLOTS", "auto save skipped: no save slot has a file"); return false; }
    HsList hs; bool have_hs = false;
    if (!capture_handshake(game, hs, have_hs)) return false;

    SaveBackup b;
    b.saved_at = (int64_t)time(nullptr);
    char when[40] = {};
    { time_t t = (time_t)b.saved_at; tm lt{}; localtime_s(&lt, &t); strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &lt); }
    _snprintf_s(b.name, sizeof(b.name), _TRUNCATE, tr(Tx::AUTO_NAME), when);

    // the new one is written whole into its own folder first; only then does the queue shift (the oldest is deleted, each other one moves back a place, the new one becomes 01)
    const std::wstring fresh = root_of(game) + L"\\auto_new";
    if (!write_folder(game, fresh, bytes, have, b, have_hs ? &hs : nullptr)) return false;
    wipe_retry(auto_dir(game, kAutoSaveCount - 1));
    for (int i = kAutoSaveCount - 2; i >= 0; --i)
        if (exists(auto_dir(game, i)) && !move_retry(auto_dir(game, i), auto_dir(game, i + 1)))
            log_line_lvl(LogLevel::Warn, "SAVESLOTS", "!! auto save queue: position %d could not move back one place", i + 1);
    if (!move_retry(fresh, auto_dir(game, 0))) {
        log_line_lvl(LogLevel::Warn, "SAVESLOTS", "!! auto save could not be placed as position 1 (the new copy stays in auto_new)");
        return false;
    }
    saveslots_refresh();
    say(tr(Tx::SS_AUTO_SAVED), b.name);
    return true;
}

void saveslots_auto_tick() {
    // A RUN SETTLED AND THE PLAYER IS BACK IN THE WAREHOUSE: the map scene was live (a run), and now the House scene is, with no map. Two scene walks a second at most; the push waits a few seconds so the
    // game's own save at that moment has been written. The title screen disarms it (a run that was left, then a save loaded into the House, is not a settlement).
    static ULONGLONG next = 0, push_at = 0;
    static bool armed = false;
    const ULONGLONG t = GetTickCount64();
    if (t < next) return;
    next = t + 500;
    if (push_at) {
        if (t >= push_at) { push_at = 0; log_line("SAVESLOTS", "back in the warehouse after a settled run: the current saves go into the auto save queue"); saveslots_auto_push(); }
        return;
    }
    if (leave_scene_live(kScene_Map)) { armed = true; return; }
    if (leave_scene_live(kScene_MainMenu)) { armed = false; return; }
    if (armed && leave_scene_live(kScene_House)) { armed = false; push_at = t + 3000; }
}

bool saveslots_delete(int index) {
    if (index < 0 || index >= kSaveBackupCount) { say(tr(Tx::SS_NO_POS)); return false; }
    std::wstring game;
    if (!game_dir(game)) { say(tr(Tx::SS_NO_DIR)); return false; }
    const std::wstring dir = slot_dir(game, index);
    if (!exists(dir)) { say(tr(Tx::SS_ALREADY_EMPTY), index + 1); return false; }
    wipe_dir(dir);
    if (exists(dir)) { say(tr(Tx::SS_DELETE_FAIL), index + 1); return false; }
    saveslots_refresh();
    say(tr(Tx::SS_DELETED), index + 1);
    return true;
}

bool saveslots_rename(int index, const char* name) {
    if (index < 0 || index >= kSaveBackupCount) { say(tr(Tx::SS_NO_POS)); return false; }
    std::wstring game;
    if (!game_dir(game)) { say(tr(Tx::SS_NO_DIR)); return false; }
    const std::wstring dir = slot_dir(game, index);
    SaveBackup b;
    if (!load_meta(dir, b)) { say(tr(Tx::SS_EMPTY_POS), index + 1); return false; }
    char clean[kSaveNameMax];
    clean_name(name, clean, sizeof(clean));
    if (!clean[0]) { say(tr(Tx::SS_NAME_EMPTY)); return false; }
    strncpy_s(b.name, sizeof(b.name), clean, _TRUNCATE);
    if (!write_text(dir + L"\\meta.json", meta_to_json(b))) { say(tr(Tx::SS_RENAME_FAIL)); return false; }
    saveslots_refresh();
    say(tr(Tx::SS_RENAMED), index + 1, b.name);
    return true;
}

bool saveslots_export(int index, const wchar_t* path) {
    if (index < 0 || index >= kSaveBackupCount || !path || !path[0]) { say(tr(Tx::SS_NO_POS)); return false; }
    std::wstring game;
    if (!game_dir(game)) { say(tr(Tx::SS_NO_DIR)); return false; }
    const std::wstring dir = slot_dir(game, index);
    SaveBackup meta;
    if (!exists(dir) || !load_meta(dir, meta)) { say(tr(Tx::SS_EMPTY_POS), index + 1); return false; }
    std::vector<PackFile> files;
    if (!collect(dir, L"", files)) return false;
    if (!write_pack(path, files)) { say(tr(Tx::SS_EXPORT_FAIL), path); return false; }
    say(tr(Tx::SS_EXPORTED), index + 1, path);
    return true;
}

bool saveslots_import(int index, const wchar_t* path) {
    if (index < 0 || index >= kSaveBackupCount || !path || !path[0]) { say(tr(Tx::SS_NO_POS)); return false; }
    std::wstring game;
    if (!game_dir(game)) { say(tr(Tx::SS_NO_DIR)); return false; }
    std::vector<PackFile> files;
    if (!read_pack(path, files)) { say(tr(Tx::SS_IMPORT_BAD), path); return false; }

    // Stage it, check it as a position would be checked, and only then put it in place.
    const std::wstring root = root_of(game);
    if (!ensure_dir(root)) { say(tr(Tx::SS_CREATE_FAIL), root.c_str()); return false; }
    const std::wstring stage = root + L"\\_import";
    wipe_dir(stage);
    if (!ensure_dir(stage)) { say(tr(Tx::SS_TMP_FAIL)); return false; }
    for (const PackFile& f : files) {
        std::wstring rel = wide(f.path);
        for (wchar_t& c : rel) if (c == L'/') c = L'\\';
        // Folders on the way.
        for (size_t cut = rel.find(L'\\'); cut != std::wstring::npos; cut = rel.find(L'\\', cut + 1))
            ensure_dir(stage + L"\\" + rel.substr(0, cut));
        if (!checkpoint_io::atomic_write(stage + L"\\" + rel, f.data)) {
            say(tr(Tx::SS_WRITE_FAIL), rel.c_str()); wipe_dir(stage); return false;
        }
    }
    SaveBackup meta;
    bool ok = load_meta(stage, meta);
    for (int s = 0; ok && s < kGameSlots; ++s) {
        if (!meta.slot[s].present) continue;
        Bytes probe;
        ok = checkpoint_io::snapshot(stage + L"\\" + kGameFile[s], probe);
    }
    if (!ok) { wipe_dir(stage); say(tr(Tx::SS_IMPORT_BAD), path); return false; }
    if (!swap_dir(stage, slot_dir(game, index))) { say(tr(Tx::SS_PLACE_FAIL), stage.c_str()); return false; }
    saveslots_refresh();
    say(tr(Tx::SS_IMPORTED), index + 1, meta.name);
    return true;
}

bool saveslots_undo_available() {
    std::wstring game;
    if (!game_dir(game)) return false;
    SaveBackup b;
    return load_meta(undo_dir(game), b);
}

bool saveslots_undo() {
    std::wstring game;
    if (!game_dir(game)) { say(tr(Tx::SS_NO_DIR)); return false; }
    SaveBackup meta;
    if (!load_meta(undo_dir(game), meta)) { say(tr(Tx::SS_NO_UNDO)); return false; }
    if (!install_from(game, undo_dir(game), meta)) return false;
    say(tr(Tx::SS_UNDONE));
    return true;
}

} // namespace mgmp
