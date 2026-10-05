// mgmp_savemirror.cpp -- see mgmp_savemirror.h.
#include "mgmp_savemirror.h"

#include <windows.h>
#include <shlobj.h>
#include <string>

#include "mgmp_config.h"
#include "mgmp_log.h"

namespace mgmp {
namespace {

// 原存档, spelled as code points so no editor or code page can change the name.
const wchar_t kOriginalName[] = { 0x539F, 0x5B58, 0x6863, 0 };

struct Stats { unsigned files = 0, dirs = 0, failed = 0; unsigned long long bytes = 0; };

// The extended-length form (the \\?\ prefix): a deep folder of backups under a long mod path passes the 260-character limit, and "path not found" is what a plain call then answers (measured in a first test).
std::wstring ext(const std::wstring& p) {
    const wchar_t bs = 92;                                    // a backslash, spelled so no shell or editor can eat it
    const std::wstring prefix = std::wstring(2, bs) + L"?" + bs;
    if (p.compare(0, 4, prefix) == 0) return p;
    if (p.size() > 2 && p[0] == bs && p[1] == bs) return prefix + L"UNC" + bs + p.substr(2);
    return prefix + p;
}

bool is_dir(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(ext(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Copies `from` into `to` (created), recursively. A link is never followed. A file that is held for the moment (the game, a sync client, a scanner) is tried again a few times; any other failure is counted at once.
void copy_tree(const std::wstring& from, const std::wstring& to, Stats& st, int depth = 0) {
    if (depth > 12) return;
    CreateDirectoryW(ext(to).c_str(), nullptr);
    ++st.dirs;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(ext(from + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        const std::wstring src = from + L"\\" + name, dst = to + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { copy_tree(src, dst, st, depth + 1); continue; }
        bool ok = false;
        DWORD err = 0;
        for (int attempt = 0; attempt < 4 && !ok; ++attempt) {
            if (attempt) Sleep(300);
            ok = CopyFileW(ext(src).c_str(), ext(dst).c_str(), FALSE) != 0;
            err = ok ? 0 : GetLastError();
            if (!ok && err != ERROR_SHARING_VIOLATION && err != ERROR_LOCK_VIOLATION && err != ERROR_ACCESS_DENIED) break;
        }
        if (ok) { ++st.files; st.bytes += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow; }
        else { ++st.failed; log_line("SAVEMIRROR", "!! %ls could not be copied (error %lu)", src.c_str(), (unsigned long)err); }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Removes a leftover of a copy that was cut short. Only ever called on the .partial folder this module made.
void delete_tree(const std::wstring& dir, int depth = 0) {
    if (depth > 12) return;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(ext(dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring p = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { RemoveDirectoryW(ext(p).c_str()); continue; }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) delete_tree(p, depth + 1);
            else { SetFileAttributesW(ext(p).c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(ext(p).c_str()); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(ext(dir).c_str());
}

DWORD WINAPI mirror_thread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const wchar_t* dll_dir = config_dll_dir();
    if (!dll_dir || !dll_dir[0]) { log_line("SAVEMIRROR", "no mod folder is known -- the saves are not copied"); return 0; }
    const std::wstring dest = std::wstring(dll_dir) + L"\\" + kOriginalName;
    if (is_dir(dest)) { log_line("SAVEMIRROR", "%ls already exists -- the saves are not copied again", dest.c_str()); return 0; }

    wchar_t appdata[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appdata)) || !appdata[0]) {
        if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) { log_line("SAVEMIRROR", "!! the roaming app-data folder is not known -- the saves are not copied"); return 0; }
    }
    const std::wstring src = std::wstring(appdata) + L"\\Glaiel Games\\Mewgenics";
    if (!is_dir(src)) { log_line("SAVEMIRROR", "%ls does not exist -- nothing to copy", src.c_str()); return 0; }

    const std::wstring partial = dest + L".partial";
    if (is_dir(partial)) delete_tree(partial);          // a copy that was cut short: start it again
    const ULONGLONG t0 = GetTickCount64();
    log_line("SAVEMIRROR", "copying every account's save folder %ls -> %ls", src.c_str(), dest.c_str());
    Stats st;
    copy_tree(src, partial, st);
    if (!MoveFileExW(ext(partial).c_str(), ext(dest).c_str(), 0)) {
        log_line("SAVEMIRROR", "!! the finished copy could not be renamed to %ls (error %lu) -- it stays as %ls and is made again next time", dest.c_str(), (unsigned long)GetLastError(), partial.c_str());
        return 0;
    }
    log_line("SAVEMIRROR", "copied %u file(s), %u folder(s), %llu bytes in %llu ms%s", st.files, st.dirs, st.bytes, (unsigned long long)(GetTickCount64() - t0),
             st.failed ? " -- SOME FILES COULD NOT BE COPIED (see above)" : "");
    return 0;
}

} // namespace

void savemirror_start() {
    HANDLE h = CreateThread(nullptr, 0, mirror_thread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

} // namespace mgmp
