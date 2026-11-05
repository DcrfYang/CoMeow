// test_progressguard.cpp -- the save-file progress floor (mgmp_progressguard, 0.1.130), against a real SQLite file laid out like a Mewgenics save.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sqlite/sqlite3.h"
#include "mgmp_progressguard.h"
#include "mgmp_i18n.h"
#include "mgmp_log.h"

using namespace mgmp;

static int g_checks = 0, g_fail = 0;
#define CHECK(x) do { ++g_checks; if (!(x)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

// --- stubs ------------------------------------------------------------------------------------------------
static bool g_room = true;
static std::wstring g_path;
static std::vector<std::pair<std::string, int>> g_set;
static int g_toasts = 0;
static std::vector<std::string> g_log;
static std::vector<std::pair<int, std::string>> g_host_only;
namespace mgmp {
void log_line(const char* tag, const char* fmt, ...) { char b[2048]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a); g_log.push_back(std::string(tag) + " " + b); }
void log_line_lvl(LogLevel, const char* tag, const char* fmt, ...) { char b[2048]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a); g_log.push_back(std::string(tag) + " " + b); }
const char* log_stage_last() { return "test"; }
bool net_active() { return g_room; }
uint8_t net_peer_count() { return g_room ? 2 : 1; }
bool savefile_live_path(wchar_t* out, size_t n) { if (g_path.empty()) return false; wcsncpy_s(out, n, g_path.c_str(), _TRUNCATE); return true; }
bool unlocks_progress_key(const char* k) { return strncmp(k, "mapflag_", 8) == 0 || strcmp(k, "house_storage_upgrades") == 0; }
bool unlocks_set_own_progress(const char* key, int value) { g_set.push_back({ key, value }); return true; }
bool unlocks_host_property(const char* key, int32_t& v) { if (strcmp(key, "house_gold") == 0) { v = 5000; return true; } return false; }
void unlocks_host_only_take(std::vector<std::pair<int, std::string>>& out) { out.swap(g_host_only); g_host_only.clear(); }
bool lockstep_fight_up() { return false; }
void room_toast(const char*) { ++g_toasts; }
const char* tr(Tx) { return ""; }
void ui_error_popup(const char*, const char*) {}   // 0.1.169: the file / save problem popup
}

// --- a save file --------------------------------------------------------------------------------------------
static pg::Unlocks base_unlocks() {
    pg::Unlocks u; u.ok = true; u.ver = 3;
    u.cats = { { "Colorless", "Fighter" }, { "HyperBeam" }, { "Bouncer" }, { "ScrappersHat", "ThiefHood" }, { "ratking" }, { "flush" }, { "bigsharklevels" } };
    return u;
}
static pg::House base_house() { pg::House h; h.ok = true; h.ver = 1; h.current = "House3"; h.lists = { { "Default", "SmallHouse_Attic" }, {} }; return h; }

static void exec(sqlite3* db, const char* q) { char* e = nullptr; if (sqlite3_exec(db, q, nullptr, nullptr, &e) != SQLITE_OK) { std::printf("sql: %s (%s)\n", e ? e : "?", q); sqlite3_free(e); } }
static void put_blob(sqlite3* db, const char* key, const std::vector<uint8_t>& b) {
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "INSERT INTO files(key,data) VALUES(?1,?2) ON CONFLICT(key) DO UPDATE SET data=excluded.data", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC); sqlite3_bind_blob(st, 2, b.data(), (int)b.size(), SQLITE_TRANSIENT);
    sqlite3_step(st); sqlite3_finalize(st);
}
static sqlite3* open_rw() {
    char p[MAX_PATH]; WideCharToMultiByte(CP_UTF8, 0, g_path.c_str(), -1, p, MAX_PATH, nullptr, nullptr);
    sqlite3* db = nullptr; sqlite3_open_v2(p, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr); return db;
}
static ULONGLONG g_fake_time = 0;
static void touch() {          // the game's write: a new write time (set explicitly; the clock's resolution would not tell two writes apart)
    HANDLE h = CreateFileW(g_path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    FILETIME ft; ULARGE_INTEGER u; u.QuadPart = 133000000000000000ull + (++g_fake_time) * 10000000ull; ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
    SetFileTime(h, nullptr, nullptr, &ft); CloseHandle(h);
}
static void tick() { progressguard_test_force_next(); progressguard_tick(); }
static int64_t prop(const char* k) {
    sqlite3* db = open_rw(); sqlite3_stmt* st = nullptr; int64_t v = -999;
    sqlite3_prepare_v2(db, "SELECT data FROM properties WHERE key=?1", -1, &st, nullptr); sqlite3_bind_text(st, 1, k, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st); sqlite3_close(db); return v;
}
static bool file_has(int cat, const char* name) {
    pg::Snap s; std::string why; pg::read_save(g_path.c_str(), s, why);
    if (!s.un.ok || (size_t)cat >= s.un.cats.size()) return false;
    for (auto& n : s.un.cats[(size_t)cat]) if (n == name) return true;
    return false;
}
static bool house_has(const char* name) {
    pg::Snap s; std::string why; pg::read_save(g_path.c_str(), s, why);
    if (!s.hu.ok || s.hu.lists.empty()) return false;
    for (auto& n : s.hu.lists[0]) if (n == name) return true;
    return false;
}
static size_t count_log(const char* needle) { size_t n = 0; for (auto& l : g_log) if (l.find(needle) != std::string::npos) ++n; return n; }

int main() {
    wchar_t dir[MAX_PATH]; GetTempPathW(MAX_PATH, dir);
    g_path = std::wstring(dir) + L"mgmp_pg_test.sav";
    DeleteFileW(g_path.c_str());
    {   // the files' formats round-trip byte for byte
        pg::Unlocks u = base_unlocks(); auto b = pg::write_unlocks(u); pg::Unlocks r; CHECK(pg::parse_unlocks(b.data(), b.size(), r) && r.cats == u.cats && pg::write_unlocks(r) == b);
        pg::House h = base_house(); auto hb = pg::write_house(h); pg::House hr; CHECK(pg::parse_house(hb.data(), hb.size(), hr) && hr.current == "House3" && hr.lists == h.lists && pg::write_house(hr) == hb);
        uint8_t junk[5] = { 1, 2, 3, 4, 5 }; pg::Unlocks bad; CHECK(!pg::parse_unlocks(junk, sizeof(junk), bad) && !bad.ok);
    }
    {
        sqlite3* db = open_rw();
        exec(db, "CREATE TABLE files (key TEXT PRIMARY KEY,data BLOB) STRICT");
        exec(db, "CREATE TABLE properties (key TEXT PRIMARY KEY,data ANY) STRICT");
        exec(db, "INSERT INTO properties VALUES('mapflag_IceAgeUnlocked',1),('mapflag_DesertUnlocked',1),('house_storage_upgrades',6),('house_gold',1414),('house_food',583),('owner_steamid','7656')");
        put_blob(db, "unlocks", pg::write_unlocks(base_unlocks()));
        put_blob(db, "house_unlocks", pg::write_house(base_house()));
        sqlite3_close(db);
    }
    touch();
    // not in a room: nothing is guarded
    g_room = false; tick(); CHECK(count_log("guarding") == 0);
    // the room: the floor is taken
    g_room = true; tick(); CHECK(count_log("guarding") == 1);
    tick(); CHECK(count_log("REPAIR") == 0);                    // the file did not change: nothing read again
    // the game writes the save with progress lowered, an unlock and a house room gone, and the gold made the host's
    {
        sqlite3* db = open_rw();
        exec(db, "UPDATE properties SET data=0 WHERE key='mapflag_IceAgeUnlocked'");
        exec(db, "UPDATE properties SET data=3 WHERE key='house_storage_upgrades'");
        exec(db, "UPDATE properties SET data=5000 WHERE key='house_gold'");
        pg::Unlocks u = base_unlocks(); u.cats[3] = { "ThiefHood" }; put_blob(db, "unlocks", pg::write_unlocks(u));
        pg::House h = base_house(); h.lists[0] = { "Default" }; put_blob(db, "house_unlocks", pg::write_house(h));
        sqlite3_close(db);
    }
    touch(); tick();
    CHECK(count_log("REPAIR #1") == 1);
    CHECK(prop("mapflag_IceAgeUnlocked") == 1 && prop("house_storage_upgrades") == 6);
    CHECK(file_has(3, "ScrappersHat") && file_has(3, "ThiefHood") && house_has("SmallHouse_Attic"));
    CHECK(g_set.size() == 2 && g_toasts == 1);
    CHECK(prop("house_gold") == 5000 && count_log("became exactly the host's") == 1);   // money: logged, never changed back
    {   // the save as it was before the repair is kept beside it
        WIN32_FIND_DATAW fd{}; HANDLE h = FindFirstFileW((g_path + L".before-mgmp-progressguard-*").c_str(), &fd);
        CHECK(h != INVALID_HANDLE_VALUE); if (h != INVALID_HANDLE_VALUE) FindClose(h);
    }
    // a gain is taken into the floor; losing it later is repaired too
    { sqlite3* db = open_rw(); pg::Unlocks u = base_unlocks(); u.cats[3].push_back("NewHat"); put_blob(db, "unlocks", pg::write_unlocks(u)); exec(db, "UPDATE properties SET data=7 WHERE key='house_storage_upgrades'"); sqlite3_close(db); }
    touch(); tick(); CHECK(count_log("REPAIR #2") == 0 && count_log("+item:NewHat") >= 1);
    { sqlite3* db = open_rw(); put_blob(db, "unlocks", pg::write_unlocks(base_unlocks())); sqlite3_close(db); }
    touch(); tick(); CHECK(count_log("REPAIR #2") == 1 && file_has(3, "NewHat") && prop("house_storage_upgrades") == 7);
    // after the room the guard keeps watching
    g_room = false;
    { sqlite3* db = open_rw(); exec(db, "UPDATE properties SET data=0 WHERE key='mapflag_DesertUnlocked'"); sqlite3_close(db); }
    touch(); tick(); CHECK(count_log("REPAIR #3") == 1 && prop("mapflag_DesertUnlocked") == 1 && count_log("the room is gone") == 1);
    // the host-only names the window answered: still locked here -> said
    g_host_only = { { 3, "GoldenHat" }, { 3, "ThiefHood" } };
    touch(); tick(); CHECK(count_log("still locked in this save: item:GoldenHat") == 1);
    // another save in the slot (owner changed): a new floor, nothing put back
    { sqlite3* db = open_rw(); exec(db, "UPDATE properties SET data='9999' WHERE key='owner_steamid'"); exec(db, "UPDATE properties SET data=0 WHERE key='house_storage_upgrades'"); sqlite3_close(db); }
    touch(); tick(); CHECK(count_log("looks like another save") == 1 && prop("house_storage_upgrades") == 0 && count_log("REPAIR #4") == 0);
    touch(); tick(); CHECK(count_log("REPAIR #4") == 0);

    // clean up
    WIN32_FIND_DATAW fd{}; HANDLE h = FindFirstFileW((g_path + L".before-mgmp-progressguard-*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) { do DeleteFileW((std::wstring(dir) + fd.cFileName).c_str()); while (FindNextFileW(h, &fd)); FindClose(h); }
    DeleteFileW(g_path.c_str());
    if (g_fail || getenv("PG_VERBOSE")) for (auto& l : g_log) std::printf("  log: %s\n", l.c_str());
    std::printf("test_progressguard: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
