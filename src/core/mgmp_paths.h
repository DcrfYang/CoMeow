// mgmp_paths.h -- where the mod puts the files it WRITES for diagnosis: a "log" folder beside mgmp.dll (so beside CoMeow.exe), not the folder itself.
//
// In it: the trace logs (mgmp_trace_<time>.log), the crash dumps (.dmp beside their log), mgmp_boot.log, the cat dumps (mgmp_catdump_*.bin) and the "this run did not end cleanly" markers
// (mgmp_run_<pid>.lock, see mgmp_prevrun.h). The launcher and the updater keep their logs there too (comeow_launcher.log, comeow_updater.log). What stays in the folder itself is
// what the player or the program READS: mgmp.json, launcher.ini, the executables, mgmp-peer-id.bin.
#pragma once

#include <windows.h>
#include <cwchar>

namespace mgmp {

// <dll_dir>\log, created when missing. When that cannot be made (a read-only folder, a FILE called "log") the answer is dll_dir itself: a log in the old place beats no log.
inline void paths_log_dir(const wchar_t* dll_dir, wchar_t* out, size_t cap) {
    swprintf_s(out, cap, L"%s\\log", dll_dir);
    const DWORD a = GetFileAttributesW(out);
    if (a == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoryW(out, nullptr)) wcsncpy_s(out, cap, dll_dir, _TRUNCATE);
    } else if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        wcsncpy_s(out, cap, dll_dir, _TRUNCATE);
    }
}

// Files an older build left in the folder itself move into the log folder (never over a file that is already there), so one run of the new build tidies the old ones up.
// `names` are FindFirstFile patterns, e.g. L"mgmp_trace_*.log".
inline void paths_move_legacy(const wchar_t* dll_dir, const wchar_t* log_dir, const wchar_t* const* patterns, size_t count) {
    if (_wcsicmp(dll_dir, log_dir) == 0) return;       // no log folder could be made: nothing to move into
    for (size_t i = 0; i < count; ++i) {
        wchar_t pat[MAX_PATH];
        swprintf_s(pat, L"%s\\%s", dll_dir, patterns[i]);
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            wchar_t from[MAX_PATH], to[MAX_PATH];
            swprintf_s(from, L"%s\\%s", dll_dir, fd.cFileName);
            swprintf_s(to, L"%s\\%s", log_dir, fd.cFileName);
            // fails (and leaves it) when `to` exists or `from` is in use: housekeeping, never an error. The boot log is the one file that already exists over there (this run's
            // breadcrumbs went into the new folder first); the old copy is a few lines of an earlier start and is dropped.
            if (!MoveFileExW(from, to, 0) && GetLastError() == ERROR_ALREADY_EXISTS && _wcsicmp(fd.cFileName, L"mgmp_boot.log") == 0) DeleteFileW(from);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

} // namespace mgmp
