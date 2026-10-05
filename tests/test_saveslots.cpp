// test_saveslots.cpp -- the save-backup positions carry the handshake queue with the three slots.
//
// Real SQLite files and real folders under a private root; only savefile_save_dir (where the game keeps
// its saves) and the logger are faked. Build and run: _buildcheck/test_saveslots.bat
#include "../src/session/mgmp_saveslots.cpp"
#include <cstdarg>
#include <cstdlib>

using namespace mgmp;
static unsigned checks = 0;
#define CHECK(x) do { ++checks; if(!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while(0)

static std::wstring g_root;

namespace mgmp {
bool savefile_save_dir(wchar_t* out, size_t cap) { wcsncpy_s(out, cap, g_root.c_str(), _TRUNCATE); return true; }
void log_line(const char*, const char* fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); }
void log_line_lvl(LogLevel, const char*, const char* fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); }
bool leave_scene_live(const char*) { return false; }
bool checkpoint_cleanup_due() { return false; }
void checkpoint_cleanup_deferred(const char*) {}     // the warehouse trigger is not under test: no scene is ever live here
// The journal's own reader is tested in test_checkpoint; here a file is a handshake save when the test says so (the file's bytes ARE the database).
struct FakeHs { std::wstring name; uint8_t slot; uint64_t seq, stamp; bool boss; uint8_t stage = 0; };
static std::vector<FakeHs> g_fake_hs;
bool checkpoint_peek_file(const std::wstring& path, uint64_t, HandshakeSaveInfo& info, std::vector<uint8_t>* db) {
    info = HandshakeSaveInfo{};
    for (const FakeHs& f : g_fake_hs) {
        if (path.size() < f.name.size() || path.compare(path.size() - f.name.size(), f.name.size(), f.name) != 0) continue;
        Bytes b; if (!checkpoint_io::read(path, b, 64u << 20)) return false;
        info.slot = f.slot; info.players = 3; info.after_boss = f.boss; info.stage = f.stage; info.run = 77; info.seq = f.seq; info.stamp = f.stamp;
        if (db) *db = std::move(b);
        return true;
    }
    return false;
}
}

static void sql_exec(const std::wstring& file, const char* command) {
    HMODULE mod = LoadLibraryW(L"winsqlite3.dll"); CHECK(mod);
    auto open = (int(*)(const char*, void**, int, const char*))GetProcAddress(mod, "sqlite3_open_v2");
    auto close = (int(*)(void*))GetProcAddress(mod, "sqlite3_close");
    auto exec = (int(*)(void*, const char*, void*, void*, char**))GetProcAddress(mod, "sqlite3_exec");
    std::string path(file.begin(), file.end()); void* db = nullptr;
    CHECK(open(path.c_str(), &db, 6, nullptr) == 0); CHECK(exec(db, command, nullptr, nullptr, nullptr) == 0); CHECK(close(db) == 0);
    FreeLibrary(mod);
}
static void put(const std::wstring& path, const char* text) {
    CHECK(checkpoint_io::atomic_write(path, Bytes(text, text + strlen(text))));
}
static std::string get(const std::wstring& path) {
    Bytes b; if (!checkpoint_io::read(path, b)) return "<missing>";
    return std::string((const char*)b.data(), b.size());
}
static bool present(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

int main() {
    wchar_t cwd[MAX_PATH]{}; GetCurrentDirectoryW(MAX_PATH, cwd);
    g_root = std::wstring(cwd) + L"\\_buildcheck\\saveslots-test-" + std::to_wstring(checkpoint_io::nonce());
    CHECK(CreateDirectoryW(g_root.c_str(), nullptr));
    for (int s = 0; s < kGameSlots; ++s)
        sql_exec(g_root + L"\\" + kGameFile[s], "CREATE TABLE properties(key TEXT PRIMARY KEY,data ANY); INSERT INTO properties VALUES('v',1);");
    const std::wstring hs = g_root + L"\\mgmp_handshake";
    CHECK(CreateDirectoryW(hs.c_str(), nullptr));
    CHECK(CreateDirectoryW((hs + L"\\111").c_str(), nullptr)); CHECK(CreateDirectoryW((hs + L"\\222").c_str(), nullptr));
    put(hs + L"\\111\\k.0", "host queue 0"); put(hs + L"\\111\\k.1", "host queue 1"); put(hs + L"\\111\\k.state", "host state");
    put(hs + L"\\222\\k.0", "guest queue 0");

    printf("-- a backup records every player's queue --\n");
    CHECK(saveslots_save(0, "before"));
    saveslots_refresh();
    CHECK(saveslots_get(0).used && saveslots_get(0).handshake == 4);
    const std::wstring slot1 = g_root + L"\\mgmp_saveslots\\slot_01\\handshake";
    CHECK(get(slot1 + L"\\111\\k.0") == "host queue 0" && get(slot1 + L"\\222\\k.0") == "guest queue 0");

    printf("-- the queue moves on, and loading the backup puts the old one back exactly --\n");
    put(hs + L"\\111\\k.0", "host queue 0 -- LATER"); put(hs + L"\\111\\k.2", "a file the backup never had");
    CHECK(DeleteFileW((hs + L"\\222\\k.0").c_str()));
    CHECK(CreateDirectoryW((hs + L"\\333").c_str(), nullptr)); put(hs + L"\\333\\k.0", "a third identity, created later");
    sql_exec(g_root + L"\\steamcampaign01.sav", "UPDATE properties SET data=2 WHERE key='v'");
    CHECK(saveslots_load(0));
    CHECK(get(hs + L"\\111\\k.0") == "host queue 0" && get(hs + L"\\111\\k.1") == "host queue 1" && get(hs + L"\\111\\k.state") == "host state");
    CHECK(get(hs + L"\\222\\k.0") == "guest queue 0");
    CHECK(!present(hs + L"\\111\\k.2") && !present(hs + L"\\333\\k.0"));

    printf("-- undo returns the queue the load replaced --\n");
    CHECK(saveslots_undo());
    CHECK(get(hs + L"\\111\\k.0") == "host queue 0 -- LATER" && get(hs + L"\\111\\k.2") == "a file the backup never had");
    CHECK(!present(hs + L"\\222\\k.0") && get(hs + L"\\333\\k.0") == "a third identity, created later");

    printf("-- a backup that recorded an EMPTY queue clears it on load --\n");
    // Empty the queue, back that up to position 2, refill it, load position 2.
    for (auto sub : { L"\\111", L"\\222", L"\\333" }) {
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((hs + sub + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) { do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((hs + sub + L"\\" + fd.cFileName).c_str()); } while (FindNextFileW(h, &fd)); FindClose(h); }
    }
    CHECK(saveslots_save(1, "empty queue")); saveslots_refresh();
    CHECK(saveslots_get(1).handshake == 0);
    put(hs + L"\\111\\k.0", "something new");
    CHECK(saveslots_load(1));
    CHECK(!present(hs + L"\\111\\k.0"));

    printf("-- an older backup (no queue recorded) leaves the current queue alone --\n");
    put(hs + L"\\111\\k.0", "current");
    const std::wstring meta = g_root + L"\\mgmp_saveslots\\slot_01\\meta.json";
    std::string text = get(meta); const size_t at = text.find("\"handshake\"");
    CHECK(at != std::string::npos);
    const size_t colon = text.find(':', at), eol = text.find_first_of(",\n", colon);
    text.replace(colon + 1, eol - colon - 1, " -1");
    put(meta, text.c_str());
    saveslots_refresh(); CHECK(saveslots_get(0).handshake == -1);
    CHECK(saveslots_load(0));
    CHECK(get(hs + L"\\111\\k.0") == "current");

    printf("-- export one position to ONE file, import it into another: the same backup --\n");
    {
        const std::wstring pack = g_root + L"\\exported.mgmpsave";
        CHECK(saveslots_export(0, pack.c_str()));
        CHECK(present(pack));
        CHECK(saveslots_import(7, pack.c_str()));
        saveslots_refresh();
        CHECK(saveslots_get(7).used && strcmp(saveslots_get(7).name, saveslots_get(0).name) == 0);
        CHECK(saveslots_get(7).handshake == saveslots_get(0).handshake);
        const std::wstring s1 = g_root + L"\\mgmp_saveslots\\slot_01", s8 = g_root + L"\\mgmp_saveslots\\slot_08";
        CHECK(get(s8 + L"\\steamcampaign01.sav") == get(s1 + L"\\steamcampaign01.sav"));
        CHECK(get(s8 + L"\\meta.json") == get(s1 + L"\\meta.json"));
        // and it loads like any other position
        CHECK(saveslots_load(7));

        printf("-- a damaged file is refused and changes nothing --\n");
        Bytes b; CHECK(checkpoint_io::read(pack, b, 64u << 20));
        Bytes bad = b; bad[bad.size() / 2] ^= 0x5A;
        const std::wstring badp = g_root + L"\\bad.mgmpsave";
        CHECK(checkpoint_io::atomic_write(badp, bad));
        CHECK(!saveslots_import(9, badp.c_str()));
        saveslots_refresh(); CHECK(!saveslots_get(9).used);
        CHECK(!saveslots_import(9, (g_root + L"\\missing.mgmpsave").c_str()));

        printf("-- a file whose paths climb out of the position is refused --\n");
        {
            Bytes evil; auto put = [&](const void* p, size_t n) { const uint8_t* q = (const uint8_t*)p; evil.insert(evil.end(), q, q + n); };
            put("MGMPSAVE", 8); uint32_t ver = 1, cnt = 1; put(&ver, 4); put(&cnt, 4);
            const char* name = "../../evil.txt"; uint16_t ln = (uint16_t)strlen(name); uint64_t sz = 3;
            put(&ln, 2); put(name, ln); put(&sz, 8); put("abc", 3);
            uint64_t h = 1469598103934665603ull; for (uint8_t c : evil) { h ^= c; h *= 1099511628211ull; }
            put(&h, 8);
            const std::wstring ep = g_root + L"\\evil.mgmpsave";
            CHECK(checkpoint_io::atomic_write(ep, evil));
            CHECK(!saveslots_import(9, ep.c_str()));
            CHECK(!present(g_root + L"\\evil.txt"));
        }
    }

    {   // THE AUTO SAVE QUEUE (2026-10-04): three positions apart from the twenty, newest first, the oldest is replaced; load or export only
        printf("-- the auto save queue keeps three, newest first, apart from the twenty --\n");
        const std::wstring root = g_root + L"\\mgmp_saveslots";
        CHECK(kAutoSaveCount == 3);
        for (int n = 1; n <= 7; ++n) {
            sql_exec(g_root + L"\\" + kGameFile[0], ("UPDATE properties SET data=" + std::to_string(100 + n) + " WHERE key='v';").c_str());
            CHECK(saveslots_auto_push());
        }
        saveslots_refresh();
        for (int i = 0; i < kAutoSaveCount; ++i) CHECK(saveslots_auto_get(i).used);
        CHECK(!present(root + L"\\auto_04") && !present(root + L"\\auto_new"));           // three, and no staging folder left behind
        const wchar_t* dirs[3] = { L"\\auto_01", L"\\auto_02", L"\\auto_03" };
        for (int i = 0; i < kAutoSaveCount; ++i) {                                         // 01 is the newest (107), 03 the oldest kept (105)
            bool have = false; std::string v;
            CHECK(checkpoint_io::read_property(root + dirs[i] + L"\\" + kGameFile[0], "v", have, v));
            CHECK(have && v == std::to_string(107 - i));
        }
        CHECK(saveslots_auto_get(0).handshake >= 0);                                       // the handshake queue travels with it, like any position
        printf("-- it can be exported, and the file imports into a position --\n");
        const std::wstring pack = g_root + L"\\auto.mgmpsave";
        CHECK(saveslots_auto_export(1, pack.c_str()));
        CHECK(present(pack));
        CHECK(!saveslots_auto_export(5, pack.c_str()));                                    // no such position
        CHECK(saveslots_import(17, pack.c_str()));
        {
            bool have = false; std::string v;
            CHECK(checkpoint_io::read_property(root + L"\\slot_18\\" + kGameFile[0], "v", have, v) && have && v == "106");
        }
        printf("-- loading one puts its saves back, with the undo point --\n");
        CHECK(saveslots_auto_load(0));
        {
            bool have = false; std::string v;
            CHECK(checkpoint_io::read_property(g_root + L"\\" + kGameFile[0], "v", have, v) && have && v == "107");
        }
        CHECK(!saveslots_auto_load(3));                                                    // out of range is refused
    }

    printf("-- restore from a handshake save (2026-10-05) --\n");
    {
        const std::wstring q = hs + L"\\444";
        CHECK(CreateDirectoryW(q.c_str(), nullptr));
        const unsigned long long t0 = 133700000000000000ull;               // FILETIME ticks; each node 10 minutes after the one before
        const struct { const wchar_t* f; int slot; int v; unsigned seq; bool boss; } mk[] = {
            { L"sync.0", 0, 303, 3, false }, { L"sync.1", 0, 302, 2, false }, { L"sync.2", 1, 311, 5, true }, { L"sync.3", 0, 301, 1, false } };
        for (const auto& m : mk) {
            const std::wstring f = q + L"\\" + m.f;
            sql_exec(f, ("CREATE TABLE properties(key TEXT PRIMARY KEY,data ANY); INSERT INTO properties VALUES('v'," + std::to_string(m.v) + "); INSERT INTO properties VALUES('current_day',9);").c_str());
            g_fake_hs.push_back({ m.f, (uint8_t)m.slot, m.seq, t0 + (unsigned long long)m.seq * 6000000000ull, m.boss });
        }
        {   // one of the two saves from before the map: its seq is not a node count
            const std::wstring f = q + L"\\sync.4";
            sql_exec(f, "CREATE TABLE properties(key TEXT PRIMARY KEY,data ANY); INSERT INTO properties VALUES('v',321);");
            g_fake_hs.push_back({ L"sync.4", 2, kStageSeqBase + kStagePrep, t0, false, kStagePrep });
        }
        put(q + L"\\sync.state", "the tombstone is not a save");
        put(q + L"\\sync.9", "not a database and not in the list");     // a file the journal reader refuses
        saveslots_sync_refresh();
        CHECK(saveslots_sync_count(0) == 3 && saveslots_sync_count(1) == 1 && saveslots_sync_count(2) == 1);
        CHECK(saveslots_sync_get(0, 0).seq == 3 && saveslots_sync_get(0, 1).seq == 2 && saveslots_sync_get(0, 2).seq == 1);   // newest first
        CHECK(saveslots_sync_get(0, 0).saved_at > saveslots_sync_get(0, 1).saved_at);
        CHECK(saveslots_sync_get(1, 0).after_boss && !saveslots_sync_get(0, 0).after_boss && saveslots_sync_get(0, 0).players == 3);
        CHECK(saveslots_sync_get(0, 0).day == 9);                                                  // the game's own number, read from the save
        CHECK(!present(g_root + L"\\mgmp_saveslots\\_sync_probe.sav"));                             // the probe file is gone again
        CHECK(saveslots_sync_get(2, 0).stage == kStagePrep && saveslots_sync_get(0, 0).stage == 0 && saveslots_sync_get(0, 9).seq == 0);   // out of range is an empty one

        sql_exec(g_root + L"\\steamcampaign01.sav", "UPDATE properties SET data=555 WHERE key='v'");
        const std::string before_queue = get(hs + L"\\111\\k.0");
        CHECK(saveslots_sync_restore(0, 1));                                                        // node 2
        {
            bool have = false; std::string v;
            CHECK(checkpoint_io::read_property(g_root + L"\\" + kGameFile[0], "v", have, v) && have && v == "302");
            CHECK(checkpoint_io::read_property(g_root + L"\\" + kGameFile[1], "v", have, v) && have && v != "311");   // the other slots are not touched
        }
        CHECK(saveslots_message_is_load());
        CHECK(get(hs + L"\\111\\k.0") == before_queue && present(q + L"\\sync.0"));                  // the queue is left alone
        CHECK(saveslots_undo_available());
        CHECK(saveslots_undo());                                                                    // the undo point has what the restore replaced
        {
            bool have = false; std::string v;
            CHECK(checkpoint_io::read_property(g_root + L"\\" + kGameFile[0], "v", have, v) && have && v == "555");
        }
        CHECK(DeleteFileW((q + L"\\sync.2").c_str()));                                              // gone since the page was drawn
        CHECK(!saveslots_sync_restore(1, 0));
        CHECK(saveslots_sync_count(1) == 0);                                                        // and the list is rescanned
        CHECK(!saveslots_sync_restore(3, 0) && !saveslots_sync_restore(0, 7));
        CHECK(saveslots_sync_restore(2, 0));                                                        // a save from before the map goes over its slot the same way
        {
            bool have = false; std::string v;
            CHECK(checkpoint_io::read_property(g_root + L"\\" + kGameFile[2], "v", have, v) && have && v == "321");
        }
    }

    printf("saveslots: %u checks passed\n", checks);
    return 0;
}
