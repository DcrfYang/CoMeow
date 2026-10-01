// mgmp_debug.cpp -- see mgmp_debug.h for where the flag comes from and why the
// restore runs on the exit path.
//
// THE ONE RULE HERE: nothing in debug_shutdown() may call the shell, load a
// library or allocate through a path that could take a lock. It runs from
// DllMain's DLL_PROCESS_DETACH, i.e. with the loader lock held, and the whole
// point of it is to be the last thing that happens. So the directory and both
// full paths are resolved in debug_init() and cached, and the exit path is
// CopyFileW + SetFileTime + log.

#include "mgmp_debug.h"
#include "mgmp_config.h"
#include "mgmp_log.h"
#include "mgmp_savefile.h"   // savefile_save_dir -- the game's own save directory

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace mgmp {
namespace {

// The marker the loader writes. One byte: '1' on, '0' off, absent = "the
// command line said nothing, ask the config file".
const wchar_t* kMarkerName = L"mgmp_debug.on";

struct State {
    bool    on = false;
    wchar_t dir[MAX_PATH]  = {};
    wchar_t from[MAX_PATH] = {};
    wchar_t to[MAX_PATH]   = {};
};

State g;

// The DLL's own directory, without depending on dllmain.cpp's static: this
// module must be correct whether or not anything else remembered to tell it.
bool dll_dir(wchar_t* out, size_t cap) {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&dll_dir, &self))
        return false;
    if (!GetModuleFileNameW(self, out, (DWORD)cap)) return false;
    if (wchar_t* slash = wcsrchr(out, L'\\')) { *slash = 0; return true; }
    return false;
}

// 1 = the loader said debug, 0 = it said no-debug, -1 = no marker at all.
int marker_state(const wchar_t* dir) {
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\%s", dir, kMarkerName);

    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;

    char buf[8] = {};
    DWORD got = 0;
    const bool read_ok = ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr) != FALSE;
    CloseHandle(h);

    // A marker that exists but cannot be read is treated as OFF rather than as
    // "no opinion": a corrupt flag file must not be able to turn ON a feature
    // that overwrites saves.
    if (!read_ok || got == 0) return 0;
    return buf[0] == '1' ? 1 : 0;
}

} // namespace

void debug_init() {
    const Config& cfg = config();
    bool on = cfg.debug_mode;

    wchar_t dir[MAX_PATH] = {};
    if (dll_dir(dir, MAX_PATH)) {
        const int m = marker_state(dir);
        if (m >= 0) {
            const bool want = (m == 1);
            if (config_set_debug_mode(want))
                log_line("DEBUG", "debug mode %s comes from the loader's --%s "
                                  "(it overrides mgmp.json)",
                         want ? "ON" : "OFF", want ? "debug" : "no-debug");
            else
                log_line("DEBUG", "the loader's --%s agrees with mgmp.json",
                         want ? "debug" : "no-debug");
            on = cfg.debug_mode;   // config_set_debug_mode refuses to turn it on without ui.dev_tools
        }
    }

    if (!on) return;

    g.on = true;

    // LOUD, and above the detail: this is the only feature in the mod that
    // writes to a player's save directory on its own initiative. A test rig
    // whose log does not say it was one is a test whose results nobody can
    // interpret later -- the same rule the savescum and desync-halt banners
    // follow.
    log_line_lvl(LogLevel::Warn, "DEBUG",
                 "!! DEBUG MODE -- on exit, '%hs' will be OVERWRITTEN with a copy of "
                 "'%hs'. This process is a test rig, not a play session.",
                 cfg.debug_slot_to, cfg.debug_slot_from);
    log_line("DEBUG", "turn it off with mgmp_loader.exe --no-debug, or "
                      "debug.debug_mode = false in mgmp.json");

    // Resolved HERE, not in the exit path: the resolver goes through
    // SHGetFolderPathW and this thread is not holding the loader lock.
    if (!savefile_save_dir(g.dir, MAX_PATH)) {
        log_line_lvl(LogLevel::Warn, "DEBUG",
                     "!! the game's save directory could not be resolved -- the "
                     "slot restore is OFF for this process");
        g.on = false;
        return;
    }

    _snwprintf_s(g.from, _TRUNCATE, L"%s\\%hs", g.dir, cfg.debug_slot_from);
    _snwprintf_s(g.to,   _TRUNCATE, L"%s\\%hs", g.dir, cfg.debug_slot_to);
    log_line("DEBUG", "restore armed: %ls -> %ls", g.from, g.to);
    log_line("DEBUG", "a plain file copy -- a .sav carries no slot identity of its "
                      "own, so the filename is all of it");
    log_line("DEBUG", "it runs at DLL_PROCESS_DETACH, so a KILLED process (task "
                      "manager, crash) skips it entirely");
}

void debug_shutdown() {
    if (!g.on) return;

    if (GetFileAttributesW(g.from) == INVALID_FILE_ATTRIBUTES) {
        log_line_lvl(LogLevel::Warn, "DEBUG",
                     "!! the source slot %ls does not exist -- nothing was restored",
                     g.from);
        return;
    }

    // WRITE A TEMPORARY, THEN REPLACE ATOMICALLY.
    //
    // Two instances on one machine SHARE ONE SAVE DIRECTORY -- the game resolves
    // it through SDL_GetPrefPath, and no environment variable can redirect it --
    // so with debug mode on for both (a reasonable thing to want: they have the
    // same slot 1 by construction) both processes are copying one source onto
    // one destination at almost the same moment. A plain CopyFile opens the
    // destination with CREATE_ALWAYS, so whichever of the two is slower can
    // leave a half-written slot 2 behind, and nothing would say so.
    //
    // MoveFileEx with REPLACE_EXISTING is a rename: a reader sees either the old
    // file or the whole new one, never a mixture. The temporary carries the
    // process id, because two instances sharing one temporary name is the very
    // race this exists to remove.
    wchar_t tmp[MAX_PATH];
    _snwprintf_s(tmp, _TRUNCATE, L"%s.%lu.tmp", g.to, GetCurrentProcessId());

    if (!CopyFileW(g.from, tmp, /*bFailIfExists=*/FALSE)) {
        log_line_lvl(LogLevel::Warn, "DEBUG",
                     "!! restoring %ls from %ls FAILED (%lu) -- the slot is left "
                     "exactly as the game wrote it",
                     g.to, g.from, GetLastError());
        return;
    }

    // Stamp the temporary as written NOW, before it becomes the slot. Steam
    // Cloud resolves conflicts on what changed, and CopyFile preserves the
    // SOURCE's timestamps -- so without this the fresh copy would present
    // itself as older than the file it replaced.
    //
    // GetSystemTimeAsFileTime, NOT GetLocalTime + SystemTimeToFileTime: the
    // latter hands SetFileTime a local-time value where it expects UTC, and it
    // wrote a timestamp one time-zone offset into the future. Caught by running
    // it -- the copy showed up 8 hours ahead of the clock on the wall.
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    {
        HANDLE h = CreateFileW(tmp, FILE_WRITE_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            SetFileTime(h, nullptr, nullptr, &ft);
            CloseHandle(h);
        }
    }

    // VERIFY THE BYTES LANDED before the temporary is promoted. A destination
    // that is present but short is worse than no copy at all, and from the
    // caller's side the two look identical -- so the check happens while the bad
    // copy is still a file nobody reads.
    LARGE_INTEGER src{}, dst{};
    HANDLE hs = CreateFileW(g.from, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE hd = CreateFileW(tmp,    GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool have_sizes = hs != INVALID_HANDLE_VALUE && hd != INVALID_HANDLE_VALUE &&
                            GetFileSizeEx(hs, &src) && GetFileSizeEx(hd, &dst);
    if (hs != INVALID_HANDLE_VALUE) CloseHandle(hs);
    if (hd != INVALID_HANDLE_VALUE) CloseHandle(hd);

    if (!have_sizes || src.QuadPart != dst.QuadPart) {
        log_line_lvl(LogLevel::Warn, "DEBUG",
                     "!! the copy of %ls came out the wrong size (%lld vs %lld) -- "
                     "the slot is left as the game wrote it",
                     g.from, have_sizes ? dst.QuadPart : -1LL,
                     have_sizes ? src.QuadPart : -1LL);
        DeleteFileW(tmp);
        return;
    }

    if (!MoveFileExW(tmp, g.to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        // Someone is holding the slot open -- the other instance, most likely.
        // Leaving the previous slot in place is the right failure: it is a whole
        // file from a moment ago, not a half-written one from now.
        log_line_lvl(LogLevel::Warn, "DEBUG",
                     "!! could not replace %ls (%lu) -- the slot keeps its previous "
                     "contents; the temporary is removed",
                     g.to, GetLastError());
        DeleteFileW(tmp);
        return;
    }

    log_line("DEBUG", "restored %ls from %ls (%lld bytes)", g.to, g.from, src.QuadPart);
}

} // namespace mgmp
