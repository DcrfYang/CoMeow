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

    printf("saveslots: %u checks passed\n", checks);
    return 0;
}
