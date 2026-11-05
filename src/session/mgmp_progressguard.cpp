// mgmp_progressguard.cpp -- see mgmp_progressguard.h.
#include "mgmp_progressguard.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

#include "sqlite/sqlite3.h"
#include "mgmp_i18n.h"
#include "mgmp_menu.h"      // ui_error_popup: a file or save problem the player is shown (0.1.169)
#include "mgmp_lockstep.h"
#include "mgmp_log.h"
#include "mgmp_net.h"
#include "mgmp_room.h"
#include "mgmp_savefile.h"
#include "mgmp_unlocks.h"

namespace mgmp {
namespace pg {

const char* const kUnlockCatNames[kUnlockCats] = { "class", "ability", "passive", "item", "boss", "radio", "level group" };

namespace {
std::string utf8(const wchar_t* s) {
    if (!s) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string r((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, &r[0], n, nullptr, nullptr);
    r.resize((size_t)n - 1);
    return r;
}
struct Rd {
    const uint8_t* p; size_t n, at = 0;
    bool u32(uint32_t& v) { if (at + 4 > n) return false; memcpy(&v, p + at, 4); at += 4; return true; }
    bool u64(uint64_t& v) { if (at + 8 > n) return false; memcpy(&v, p + at, 8); at += 8; return true; }
    bool str(std::string& s) { uint64_t l = 0; if (!u64(l) || l > 4096 || at + l > n) return false; s.assign((const char*)p + at, (size_t)l); at += (size_t)l; return true; }
    bool list(std::vector<std::string>& v) {
        uint64_t c = 0;
        if (!u64(c) || c > 100000) return false;
        v.clear(); v.reserve((size_t)c);
        for (uint64_t i = 0; i < c; ++i) { std::string s; if (!str(s)) return false; v.push_back(std::move(s)); }
        return true;
    }
};
void put32(std::vector<uint8_t>& b, uint32_t v) { const uint8_t* q = (const uint8_t*)&v; b.insert(b.end(), q, q + 4); }
void put64(std::vector<uint8_t>& b, uint64_t v) { const uint8_t* q = (const uint8_t*)&v; b.insert(b.end(), q, q + 8); }
void putstr(std::vector<uint8_t>& b, const std::string& s) { put64(b, s.size()); b.insert(b.end(), s.begin(), s.end()); }
void putlist(std::vector<uint8_t>& b, const std::vector<std::string>& v) { put64(b, v.size()); for (const std::string& s : v) putstr(b, s); }
bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }
} // namespace

// unlocks: [u32 version] then the lists to the end of the blob ([u64 n] n x [u64 len][bytes]); seen 7 lists (version 3)
bool parse_unlocks(const uint8_t* p, size_t n, Unlocks& out) {
    out = Unlocks{};
    Rd r{ p, n };
    if (!r.u32(out.ver)) return false;
    while (r.at < n) {
        std::vector<std::string> l;
        if (!r.list(l) || out.cats.size() >= 32) { out = Unlocks{}; return false; }
        out.cats.push_back(std::move(l));
    }
    out.ok = true;
    return true;
}
std::vector<uint8_t> write_unlocks(const Unlocks& u) {
    std::vector<uint8_t> b;
    put32(b, u.ver);
    for (const auto& l : u.cats) putlist(b, l);
    return b;
}
// house_unlocks: [u32 version][string the house in use] then lists to the end (seen: the unlocked rooms, then an empty list)
bool parse_house(const uint8_t* p, size_t n, House& out) {
    out = House{};
    Rd r{ p, n };
    if (!r.u32(out.ver) || !r.str(out.current)) { out = House{}; return false; }
    while (r.at < n) {
        std::vector<std::string> l;
        if (!r.list(l) || out.lists.size() >= 16) { out = House{}; return false; }
        out.lists.push_back(std::move(l));
    }
    out.ok = true;
    return true;
}
std::vector<uint8_t> write_house(const House& h) {
    std::vector<uint8_t> b;
    put32(b, h.ver);
    putstr(b, h.current);
    for (const auto& l : h.lists) putlist(b, l);
    return b;
}

bool read_save(const wchar_t* path, Snap& out, std::string& why) {
    out = Snap{};
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(utf8(path).c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK || !db) {
        why = db ? sqlite3_errmsg(db) : "open failed";
        if (db) sqlite3_close(db);
        return false;
    }
    sqlite3_busy_timeout(db, 2000);
    bool ok = true;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT key, data FROM properties", -1, &st, nullptr) == SQLITE_OK) {
        int rc;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            const char* k = (const char*)sqlite3_column_text(st, 0);
            if (!k) continue;
            const int t = sqlite3_column_type(st, 1);
            if (t == SQLITE_INTEGER) {
                const int64_t v = sqlite3_column_int64(st, 1);
                if (strcmp(k, "house_gold") == 0) { out.gold = v; out.have_gold = true; }
                else if (strcmp(k, "house_food") == 0) { out.food = v; out.have_food = true; }
                if (unlocks_progress_key(k)) out.props[k] = v;
            } else if (t == SQLITE_TEXT && strcmp(k, "owner_steamid") == 0) {
                const char* v = (const char*)sqlite3_column_text(st, 1);
                out.owner = v ? v : "";
            }
        }
        if (rc != SQLITE_DONE) { ok = false; why = sqlite3_errmsg(db); }
    } else { ok = false; why = sqlite3_errmsg(db); }
    sqlite3_finalize(st); st = nullptr;
    if (ok && sqlite3_prepare_v2(db, "SELECT key, data FROM files WHERE key IN ('unlocks','house_unlocks')", -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char* k = (const char*)sqlite3_column_text(st, 0);
            const uint8_t* b = (const uint8_t*)sqlite3_column_blob(st, 1);
            const size_t n = (size_t)sqlite3_column_bytes(st, 1);
            if (!k || !b) continue;
            if (strcmp(k, "unlocks") == 0) parse_unlocks(b, n, out.un);
            else parse_house(b, n, out.hu);
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    out.ok = ok;
    return ok;
}

void compute_loss(const Snap& floor, const Snap& cur, Loss& out) {
    out = Loss{};
    for (const auto& kv : floor.props) {
        auto it = cur.props.find(kv.first);
        if (it == cur.props.end() || it->second < kv.second) out.props.push_back(kv);
    }
    if (floor.un.ok && cur.un.ok)
        for (size_t c = 0; c < floor.un.cats.size(); ++c)
            for (const std::string& s : floor.un.cats[c])
                if (c >= cur.un.cats.size() || !has(cur.un.cats[c], s)) out.unlocks.push_back({ (int)c, s });
    if (floor.hu.ok && cur.hu.ok)
        for (size_t l = 0; l < floor.hu.lists.size(); ++l)
            for (const std::string& s : floor.hu.lists[l])
                if (l >= cur.hu.lists.size() || !has(cur.hu.lists[l], s)) out.house.push_back({ (int)l, s });
}

void apply_loss(Snap& cur, const Loss& loss) {
    for (const auto& kv : loss.props) cur.props[kv.first] = kv.second;
    for (const auto& e : loss.unlocks) { if ((size_t)e.first >= cur.un.cats.size()) cur.un.cats.resize((size_t)e.first + 1); cur.un.cats[(size_t)e.first].push_back(e.second); }
    for (const auto& e : loss.house) { if ((size_t)e.first >= cur.hu.lists.size()) cur.hu.lists.resize((size_t)e.first + 1); cur.hu.lists[(size_t)e.first].push_back(e.second); }
}

bool write_fix(const wchar_t* path, const Snap& fixed, const Loss& loss, std::string& why) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(utf8(path).c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK || !db) {
        why = db ? sqlite3_errmsg(db) : "open failed";
        if (db) sqlite3_close(db);
        return false;
    }
    sqlite3_busy_timeout(db, 3000);
    bool ok = sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK;
    if (!ok) why = sqlite3_errmsg(db);
    sqlite3_stmt* st = nullptr;
    if (ok && !loss.props.empty()) {
        ok = sqlite3_prepare_v2(db, "INSERT INTO properties(key, data) VALUES(?1, ?2) ON CONFLICT(key) DO UPDATE SET data = excluded.data", -1, &st, nullptr) == SQLITE_OK;
        for (size_t i = 0; ok && i < loss.props.size(); ++i) {
            sqlite3_reset(st);
            sqlite3_bind_text(st, 1, loss.props[i].first.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 2, loss.props[i].second);
            ok = sqlite3_step(st) == SQLITE_DONE;
        }
        if (!ok) why = sqlite3_errmsg(db);
        sqlite3_finalize(st); st = nullptr;
    }
    auto put_file = [&](const char* key, const std::vector<uint8_t>& blob) {
        if (!ok) return;
        ok = sqlite3_prepare_v2(db, "INSERT INTO files(key, data) VALUES(?1, ?2) ON CONFLICT(key) DO UPDATE SET data = excluded.data", -1, &st, nullptr) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
            sqlite3_bind_blob(st, 2, blob.data(), (int)blob.size(), SQLITE_TRANSIENT);
            ok = sqlite3_step(st) == SQLITE_DONE;
        }
        if (!ok) why = sqlite3_errmsg(db);
        sqlite3_finalize(st); st = nullptr;
    };
    if (!loss.unlocks.empty() && fixed.un.ok) put_file("unlocks", write_unlocks(fixed.un));
    if (!loss.house.empty() && fixed.hu.ok) put_file("house_unlocks", write_house(fixed.hu));
    if (ok) { ok = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK; if (!ok) why = sqlite3_errmsg(db); }
    if (!ok) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    return ok;
}

void merge_floor(Snap& floor, const Snap& cur) {
    for (const auto& kv : cur.props) { auto it = floor.props.find(kv.first); if (it == floor.props.end() || it->second < kv.second) floor.props[kv.first] = kv.second; }
    if (cur.un.ok) {
        if (!floor.un.ok) floor.un = cur.un;
        else {
            if (floor.un.cats.size() < cur.un.cats.size()) floor.un.cats.resize(cur.un.cats.size());
            for (size_t c = 0; c < cur.un.cats.size(); ++c) for (const std::string& s : cur.un.cats[c]) if (!has(floor.un.cats[c], s)) floor.un.cats[c].push_back(s);
        }
    }
    if (cur.hu.ok) {
        if (!floor.hu.ok) floor.hu = cur.hu;
        else {
            if (floor.hu.lists.size() < cur.hu.lists.size()) floor.hu.lists.resize(cur.hu.lists.size());
            for (size_t l = 0; l < cur.hu.lists.size(); ++l) for (const std::string& s : cur.hu.lists[l]) if (!has(floor.hu.lists[l], s)) floor.hu.lists[l].push_back(s);
        }
    }
    if (!cur.owner.empty()) floor.owner = cur.owner;
}

size_t unlock_total(const Snap& s) { size_t n = 0; for (const auto& l : s.un.cats) n += l.size(); return n; }

bool looks_like_another_save(const Snap& floor, const Snap& cur, std::string& why) {
    if (!floor.owner.empty() && !cur.owner.empty() && floor.owner != cur.owner) { why = "its owner changed (" + floor.owner + " -> " + cur.owner + ")"; return true; }
    const size_t fu = unlock_total(floor), cu = unlock_total(cur), fp = floor.props.size(), cp = cur.props.size();
    if (fu >= 8 && fp >= 8 && cu * 2 < fu && cp * 2 < fp) {
        char b[160]; _snprintf_s(b, sizeof(b), _TRUNCATE, "it lost more than half of everything at once (unlocks %zu -> %zu, progress keys %zu -> %zu): a new game in this slot", fu, cu, fp, cp);
        why = b; return true;
    }
    return false;
}

} // namespace pg

namespace {
using namespace pg;

struct State {
    std::wstring path;
    bool armed = false;
    Snap floor, last;
    FILETIME mt{};
    bool had_room = false;
    unsigned repairs = 0, writes = 0, read_fails = 0;
    ULONGLONG next = 0, toast_at = 0;
    bool deferred_said = false;     // 0.1.163: the one line per room about a derived key left for the room's end
    std::string last_house;         // 0.1.163: the house rooms of the last house-only repair, and how many the same repairs went unlogged since
    unsigned house_quiet = 0;
    ULONGLONG house_said_at = 0;
} G;

// KEYS THE GAME DERIVES FROM WHAT IT SEES (0.1.163, 576 repairs of save_file_percent in 19 logs, up to 100 in one): in a room the unlock checks and the save
// properties answer with the host's, so a client's game computes its completion percentage from the host's progress (11 % for a player at 24 %) and writes it
// at every save; the guard put it back, the next save wrote 11 again. Nothing was lost, but every save was a repair. Such a key is not put back while the
// room lasts: once it is gone (the guard looks again at that moment), the floor puts it back into the file and the game's own copy, once.
bool deferred_in_room(const std::string& key) { return key == "save_file_percent"; }

bool mtime(const wchar_t* p, FILETIME& out) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &a)) return false;
    out = a.ftLastWriteTime;
    return true;
}
bool same(const FILETIME& a, const FILETIME& b) { return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime; }

std::string counts(const Snap& s) {
    char b[200]; int w = 0;
    w += _snprintf_s(b + w, sizeof(b) - w, _TRUNCATE, "%zu progress key(s), unlocks", s.props.size());
    for (size_t c = 0; c < s.un.cats.size() && w > 0 && (size_t)w < sizeof(b) - 16; ++c) w += _snprintf_s(b + w, sizeof(b) - w, _TRUNCATE, " %zu", s.un.cats[c].size());
    if (!s.un.ok && w > 0) w += _snprintf_s(b + w, sizeof(b) - w, _TRUNCATE, " (unreadable)");
    if (w > 0 && (size_t)w < sizeof(b) - 30) _snprintf_s(b + w, sizeof(b) - w, _TRUNCATE, ", house rooms %zu", s.hu.ok && !s.hu.lists.empty() ? s.hu.lists[0].size() : 0);
    return b;
}
int64_t prop_or(const Snap& s, const char* k, int64_t d) { auto it = s.props.find(k); return it == s.props.end() ? d : it->second; }

// THE BACKUP BEFORE A REPAIR: the file as it was, beside the save (newest 5).
void backup_before(const wchar_t* path) {
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t dst[MAX_PATH];
    _snwprintf_s(dst, _TRUNCATE, L"%s.before-mgmp-progressguard-%04u%02u%02u-%02u%02u%02u", path, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    sqlite3 *src = nullptr, *out = nullptr;
    bool ok = false;
    if (sqlite3_open_v2(utf8(path).c_str(), &src, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK &&
        sqlite3_open_v2(utf8(dst).c_str(), &out, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) == SQLITE_OK) {
        sqlite3_busy_timeout(src, 2000);
        if (sqlite3_backup* b = sqlite3_backup_init(out, "main", src, "main")) { ok = sqlite3_backup_step(b, -1) == SQLITE_DONE; sqlite3_backup_finish(b); }
    }
    if (src) sqlite3_close(src);
    if (out) sqlite3_close(out);
    std::string d = utf8(dst);
    log_line_lvl(ok ? LogLevel::Info : LogLevel::Warn, "PROGRESS", "%s the save as it was before the repair: %s", ok ? "kept" : "!! could not keep", d.c_str());
    // keep the newest 5
    wchar_t pat[MAX_PATH]; _snwprintf_s(pat, _TRUNCATE, L"%s.before-mgmp-progressguard-*", path);
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) { do names.push_back(fd.cFileName); while (FindNextFileW(h, &fd)); FindClose(h); }
    std::sort(names.begin(), names.end());
    std::wstring dir(path); const size_t slash = dir.find_last_of(L"\\/"); dir = slash == std::wstring::npos ? L"" : dir.substr(0, slash + 1);
    for (size_t i = 0; i + 5 < names.size(); ++i) DeleteFileW((dir + names[i]).c_str());
}

// What one write of the game changed, for the log: progress gained/lost, unlocks gained/lost, money and food (with the host's numbers in a room).
void log_changes(const Snap& a, const Snap& b, bool room) {
    char line[900]; int w = 0;
    auto add = [&](const char* fmt, auto... args) { if (w >= 0 && (size_t)w < sizeof(line) - 80) w += _snprintf_s(line + w, sizeof(line) - w, _TRUNCATE, fmt, args...); };
    unsigned up = 0, down = 0;
    for (const auto& kv : b.props) { auto it = a.props.find(kv.first); if (it == a.props.end() ? kv.second != 0 : it->second != kv.second) { (it != a.props.end() && kv.second < it->second) ? ++down : ++up; if (up + down <= 8) add(" %s %lld->%lld", kv.first.c_str(), it == a.props.end() ? 0LL : (long long)it->second, (long long)kv.second); } }
    for (const auto& kv : a.props) if (!b.props.count(kv.first)) { ++down; if (up + down <= 8) add(" %s %lld->(gone)", kv.first.c_str(), (long long)kv.second); }
    unsigned ug = 0, ul = 0;
    for (size_t c = 0; c < b.un.cats.size(); ++c) for (const std::string& s : b.un.cats[c]) if (c >= a.un.cats.size() || !has(a.un.cats[c], s)) { ++ug; if (ug <= 6) add(" +%s:%s", kUnlockCatNames[c < kUnlockCats ? c : 0], s.c_str()); }
    for (size_t c = 0; c < a.un.cats.size(); ++c) for (const std::string& s : a.un.cats[c]) if (c >= b.un.cats.size() || !has(b.un.cats[c], s)) { ++ul; if (ul <= 6) add(" -%s:%s", kUnlockCatNames[c < kUnlockCats ? c : 0], s.c_str()); }
    // 0.1.163 (the attic, SmallHouse_Attic, put back 39 times in one log at "the House's box sets off with 0 cat(s)", cause unknown): the house rooms the game
    // added or dropped in this write, and the house in use, so the next log says at which step a room comes and goes
    unsigned hg_ = 0, hl_ = 0;
    if (a.hu.ok && b.hu.ok) {
        for (size_t l = 0; l < b.hu.lists.size(); ++l) for (const std::string& s : b.hu.lists[l]) if (l >= a.hu.lists.size() || !has(a.hu.lists[l], s)) { ++hg_; if (hg_ <= 4) add(" +house:%s", s.c_str()); }
        for (size_t l = 0; l < a.hu.lists.size(); ++l) for (const std::string& s : a.hu.lists[l]) if (l >= b.hu.lists.size() || !has(b.hu.lists[l], s)) { ++hl_; if (hl_ <= 4) add(" -house:%s", s.c_str()); }
        if (a.hu.current != b.hu.current) { ++hg_; add(" house %s->%s", a.hu.current.c_str(), b.hu.current.c_str()); }
    }
    const bool money = (a.have_gold != b.have_gold || a.gold != b.gold) || (a.have_food != b.have_food || a.food != b.food);
    if (!up && !down && !ug && !ul && !hg_ && !hl_ && !money) return;
    int32_t hg = 0, hf = 0;
    const bool host_g = room && unlocks_host_property("house_gold", hg), host_f = room && unlocks_host_property("house_food", hf);
    log_line_lvl(down || ul || hl_ ? LogLevel::Warn : LogLevel::Info, "PROGRESS", "the game wrote the save (%s, stage '%s'): progress keys +%u / -%u, unlocks +%u / -%u, warehouse gold %lld->%lld%s, food %lld->%lld%s --%s",
             room ? "in a room" : "no room", log_stage_last(), up, down, ug, ul, (long long)a.gold, (long long)b.gold, host_g ? (std::string(" (host's ") + std::to_string(hg) + ")").c_str() : "",
             (long long)a.food, (long long)b.food, host_f ? (std::string(" (host's ") + std::to_string(hf) + ")").c_str() : "", w > 0 ? line : " (no detail)");
    if (host_g && b.have_gold && b.gold != a.gold && b.gold == (int64_t)hg && a.gold != (int64_t)hg)
        log_line_lvl(LogLevel::Error, "PROGRESS", "!! the warehouse gold of THIS save became exactly the host's number (%lld -> %lld) -- another player's money may have been written into this save (not changed back: money can go down in play)",
                 (long long)a.gold, (long long)b.gold);
    if (host_f && b.have_food && b.food != a.food && b.food == (int64_t)hf && a.food != (int64_t)hf)
        log_line_lvl(LogLevel::Error, "PROGRESS", "!! the warehouse food of THIS save became exactly the host's number (%lld -> %lld) -- another player's stock may have been written into this save (not changed back)",
                 (long long)a.food, (long long)b.food);
}

// The names the unlock window answered "UNLOCKED" for (the host's) while this save had them locked: if the save still lacks one, an unlock it earned inside the window may have been skipped.
void log_host_only(const Snap& cur) {
    std::vector<std::pair<int, std::string>> names;
    unlocks_host_only_take(names);
    if (names.empty() || !cur.un.ok) return;
    char line[700]; int w = 0; unsigned still = 0;
    for (const auto& e : names) {
        const int c = e.first;
        if (c < 0 || (size_t)c >= cur.un.cats.size() || has(cur.un.cats[(size_t)c], e.second)) continue;
        ++still;
        if (w >= 0 && (size_t)w < sizeof(line) - 60) w += _snprintf_s(line + w, sizeof(line) - w, _TRUNCATE, " %s:%s", kUnlockCatNames[c], e.second.c_str());
    }
    if (still)
        log_line_lvl(LogLevel::Warn, "PROGRESS", "%u name(s) the last window answered as unlocked from the HOST's lists are still locked in this save:%s -- expected for what only the host has; if this player earned one of them in that window, the unlock may have been skipped",
                 still, w > 0 ? line : "");
}
} // namespace

void progressguard_test_reset() { G = State{}; }
void progressguard_test_force_next() { G.next = 0; }

void progressguard_tick() {
    const ULONGLONG now = GetTickCount64();
    if (now < G.next) return;
    G.next = now + 1000;
    wchar_t path[MAX_PATH] = {};
    if (!savefile_live_path(path, MAX_PATH) || !path[0]) return;
    const bool room = net_active() && net_peer_count() >= 2;
    if (G.path != path) {
        if (G.armed) { std::string o = utf8(G.path.c_str()); log_line("PROGRESS", "another save is played now: the guard of '%s' ends (%u repair(s), %u write(s) checked)", o.c_str(), G.repairs, G.writes); }
        G = State{}; G.path = path; G.next = now + 1000;
    }
    if (!G.armed && !room) return;                               // the mod has not been near this save in a room: nothing to guard
    FILETIME mt{};
    if (!mtime(path, mt)) return;
    const bool room_changed = G.armed && room != G.had_room;
    if (G.armed && !room_changed && same(mt, G.mt)) return;
    Snap cur; std::string why;
    if (!read_save(path, cur, why)) {
        if (++G.read_fails <= 5) log_line_lvl(LogLevel::Warn, "PROGRESS", "!! the save could not be read (%s) -- tried again in a second", why.c_str());
        return;
    }
    G.read_fails = 0;
    const std::string pth = utf8(path);
    if (!G.armed) {
        G.armed = true; G.floor = cur; G.last = cur; G.mt = mt; G.had_room = room;
        log_line("PROGRESS", "guarding '%s' from now on (in a room): %s; owner %s; warehouse upgrades %lld, gold %lld, food %lld -- a progress key that goes down or an unlock that goes missing in this file is put back",
                 pth.c_str(), counts(cur).c_str(), cur.owner.empty() ? "?" : cur.owner.c_str(), (long long)prop_or(cur, "house_storage_upgrades", -1), (long long)cur.gold, (long long)cur.food);
        return;
    }
    ++G.writes;
    if (room_changed) log_line("PROGRESS", "%s: the guard keeps watching '%s' (%s)", room ? "a room again" : "the room is gone", pth.c_str(), counts(cur).c_str());
    std::string other;
    if (looks_like_another_save(G.floor, cur, other)) {
        log_line_lvl(LogLevel::Warn, "PROGRESS", "'%s' looks like another save now -- %s: its floor starts again from what it holds (%s)", pth.c_str(), other.c_str(), counts(cur).c_str());
        G.floor = cur; G.last = cur; G.mt = mt; G.had_room = room;
        return;
    }
    if (!same(mt, G.mt)) log_changes(G.last, cur, room);
    log_host_only(cur);
    Loss loss;
    compute_loss(G.floor, cur, loss);
    if (room) {   // 0.1.163: a derived key waits for the room's end (see deferred_in_room)
        size_t kept = 0;
        std::string left;
        for (size_t i = 0; i < loss.props.size(); ++i) {
            if (deferred_in_room(loss.props[i].first)) {
                auto it = cur.props.find(loss.props[i].first);
                left += " " + loss.props[i].first + " " + (it == cur.props.end() ? std::string("(gone)") : std::to_string(it->second)) + "->" + std::to_string(loss.props[i].second);
                continue;
            }
            loss.props[kept++] = loss.props[i];
        }
        loss.props.resize(kept);
        if (!left.empty() && !G.deferred_said) {
            G.deferred_said = true;
            log_line("PROGRESS", "a key the game derives from what it sees is lower in '%s' while in a room:%s -- the host's view of the run; put back once when the room is gone, not at every save",
                     pth.c_str(), left.c_str());
        }
    } else G.deferred_said = false;
    if (loss.any()) {
        backup_before(path);
        Snap fixed = cur;
        apply_loss(fixed, loss);
        if (!write_fix(path, fixed, loss, why)) {
            log_line_lvl(LogLevel::Error, "PROGRESS", "!! the repair of '%s' could not be written (%s) -- tried again at the next look", pth.c_str(), why.c_str());
            if (why.find("locked") == std::string::npos && why.find("busy") == std::string::npos) {   // 0.1.169: the player is told -- not for a lock the game holds a moment (retried in 3 s)
                char text[1024];
                _snprintf_s(text, sizeof(text), _TRUNCATE, tr(Tx::ERR_PROGRESS), pth.c_str(), why.c_str());
                ui_error_popup(tr(Tx::ERR_FILE_TITLE), text);
            }
            G.next = now + 3000;
            return;                                            // G.mt is not moved: the next tick tries again
        }
        ++G.repairs;
        // the game's own copy of the properties, so its next save writes them right (not inside a fight: the file is enough there, and the next write is looked at again)
        unsigned mem = 0;
        const bool fight = lockstep_fight_up();
        if (!fight) for (const auto& kv : loss.props) if (unlocks_set_own_progress(kv.first.c_str(), (int)(kv.second > 2000000000LL ? 2000000000LL : kv.second))) ++mem;
        char line[1200]; int w = 0;
        for (size_t i = 0; i < loss.props.size() && w >= 0 && (size_t)w < sizeof(line) - 80; ++i) {
            auto it = cur.props.find(loss.props[i].first);
            w += _snprintf_s(line + w, sizeof(line) - w, _TRUNCATE, " %s %s->%lld", loss.props[i].first.c_str(), it == cur.props.end() ? "(gone)" : std::to_string(it->second).c_str(), (long long)loss.props[i].second);
        }
        for (size_t i = 0; i < loss.unlocks.size() && w >= 0 && (size_t)w < sizeof(line) - 60; ++i)
            w += _snprintf_s(line + w, sizeof(line) - w, _TRUNCATE, " +%s:%s", kUnlockCatNames[loss.unlocks[i].first < kUnlockCats ? loss.unlocks[i].first : 0], loss.unlocks[i].second.c_str());
        for (size_t i = 0; i < loss.house.size() && w >= 0 && (size_t)w < sizeof(line) - 60; ++i)
            w += _snprintf_s(line + w, sizeof(line) - w, _TRUNCATE, " +house:%s", loss.house[i].second.c_str());
        // 0.1.163: the same house rooms put back again (the game's own copy lacks them until it loads the file, so every save of it is repaired): one line a minute
        bool quiet = false;
        if (loss.props.empty() && loss.unlocks.empty() && !loss.house.empty()) {
            std::string sig;
            for (const auto& e : loss.house) sig += e.second + ",";
            if (sig == G.last_house && now - G.house_said_at < 60000) { ++G.house_quiet; quiet = true; }
            else {
                if (G.house_quiet) log_line("PROGRESS", "(the same house room(s) %s were put back %u more time(s) since the last line -- each save of the game's own copy lacks them)", G.last_house.c_str(), G.house_quiet);
                G.last_house = sig; G.house_said_at = now; G.house_quiet = 0;
            }
        }
        if (!quiet)
        log_line_lvl(LogLevel::Error, "PROGRESS", "!! REPAIR #%u of '%s' (%s, stage '%s'): %zu progress key(s) lower than this player had, %zu unlock(s) and %zu house room(s) missing -- put back into the file%s:%s",
                 G.repairs, pth.c_str(), room ? "in a room" : "no room", log_stage_last(), loss.props.size(), loss.unlocks.size(), loss.house.size(),
                 loss.props.empty() ? "" : (fight ? " (the game's own copy after the fight)" : (std::string(" and ") + std::to_string(mem) + " into the game's own copy").c_str()), w > 0 ? line : "");
        if ((!loss.unlocks.empty() || !loss.house.empty()) && !quiet)
            log_line_lvl(LogLevel::Warn, "PROGRESS", "the game's own copy of the unlock lists cannot be changed in place: each later save of it is checked and put right again until the game loads the file");
        if (now - G.toast_at > 30000) { G.toast_at = now; room_toast(tr(Tx::PG_FIXED)); }
        cur = fixed;
        if (!mtime(path, mt)) mt = FILETIME{};
    }
    merge_floor(G.floor, cur);
    G.last = cur; G.mt = mt; G.had_room = room;
}

} // namespace mgmp
