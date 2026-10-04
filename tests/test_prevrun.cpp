// test_prevrun.cpp -- the "this run ended badly" bookkeeping of src/core/mgmp_prevrun.cpp (the markers live in <dll dir>\log), with the log and the config stubbed.
//     _buildcheck\test_prevrun.bat
#include "mgmp_prevrun.h"
#include "mgmp_config.h"
#include "mgmp_log.h"
#include "mgmp_paths.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

using namespace mgmp;

namespace mgmp {
static Config g_cfg_stub;
const Config& config() { return g_cfg_stub; }
static wchar_t g_cur_log[MAX_PATH];
bool log_current_path(wchar_t* out, size_t cap) { wcsncpy_s(out, cap, g_cur_log, _TRUNCATE); return true; }
void log_line(const char*, const char*, ...) {}
}

static int fails = 0;
static void check(bool c, const char* m) { printf("  %s %s\n", c ? "ok  " : "FAIL", m); if (!c) ++fails; }
static bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static void put(const std::wstring& p, const std::string& text) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD w = 0; WriteFile(h, text.data(), (DWORD)text.size(), &w, nullptr); CloseHandle(h);
}
static std::string u8(const std::wstring& w) {
    char b[1100] = {}; WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, b, sizeof(b) - 1, nullptr, nullptr); return b;
}

int main() {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"mgmp_prevrun_test_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);

    // --- the log folder ---
    wchar_t ld[MAX_PATH] = {};
    paths_log_dir(dir.c_str(), ld, MAX_PATH);
    const std::wstring logdir = ld;
    check(logdir == dir + L"\\log" && exists(logdir), "the log folder is <dll dir>\\log and is created");
    // a FILE called "log": the old place is used instead of failing
    {
        const std::wstring d2 = dir + L"_b";
        CreateDirectoryW(d2.c_str(), nullptr);
        put(d2 + L"\\log", "x");
        wchar_t o2[MAX_PATH] = {};
        paths_log_dir(d2.c_str(), o2, MAX_PATH);
        check(d2 == o2, "when a file is in the way the dll dir itself is used");
    }
    // older builds' files move in, and never over what is already there
    {
        put(dir + L"\\mgmp_trace_20260101-000000.log", "legacy");
        put(dir + L"\\mgmp_trace_20260101-000000.dmp", "MDMP");
        put(dir + L"\\keep_me.txt", "stays");
        put(logdir + L"\\mgmp_boot.log", "new boot");
        put(dir + L"\\mgmp_boot.log", "old boot");
        static const wchar_t* const pats[] = { L"mgmp_trace_*.log", L"mgmp_trace_*.dmp", L"mgmp_boot.log" };
        paths_move_legacy(dir.c_str(), logdir.c_str(), pats, 3);
        check(exists(logdir + L"\\mgmp_trace_20260101-000000.log") && !exists(dir + L"\\mgmp_trace_20260101-000000.log"), "an old trace log moves into the log folder");
        check(exists(logdir + L"\\mgmp_trace_20260101-000000.dmp"), "and its dump");
        check(exists(dir + L"\\keep_me.txt"), "other files are not touched");
        check(!exists(dir + L"\\mgmp_boot.log"), "an old boot log is dropped when the new one is already there");
    }

    // --- the markers ---
    const std::wstring log1 = logdir + L"\\mgmp_trace_A.log", dmp1 = logdir + L"\\mgmp_trace_A.dmp";
    put(log1, "old log\n"); put(dmp1, "MDMP");
    swprintf_s(g_cur_log, L"%s\\mgmp_trace_NOW.log", logdir.c_str());
    strcpy_s(g_cfg_stub.signal_addr, "49.233.209.67:27700");

    // a run that died (a pid that does not exist), one that is still running (pid 4: not ours to open, so "alive"), one whose log is gone, one two weeks old,
    // and one an OLDER build left in the dll dir itself, naming a log that has since moved into the log folder
    put(logdir + L"\\mgmp_run_999991.lock", "999991\n" + u8(log1) + "\n");
    put(logdir + L"\\mgmp_run_4.lock", "4\n" + u8(log1) + "\n");
    put(logdir + L"\\mgmp_run_999992.lock", "999992\n" + u8(logdir + L"\\gone.log") + "\n");
    put(logdir + L"\\mgmp_run_999993.lock", "999993\n" + u8(log1) + "\n");
    {   // 20 days ago
        HANDLE h = CreateFileW((logdir + L"\\mgmp_run_999993.lock").c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        FILETIME now; GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER u; u.LowPart = now.dwLowDateTime; u.HighPart = now.dwHighDateTime; u.QuadPart -= 20ull * 24 * 3600 * 10000000ull;
        FILETIME old{ u.LowPart, u.HighPart };
        SetFileTime(h, nullptr, nullptr, &old); CloseHandle(h);
    }
    put(dir + L"\\mgmp_run_999994.lock", "999994\n" + u8(dir + L"\\mgmp_trace_20260101-000000.log") + "\n");

    prevrun_init(dir.c_str());
    check(exists(logdir + L"\\mgmp_run_" + std::to_wstring(GetCurrentProcessId()) + L".lock"), "this run's lock is created in the log folder");
    check(!exists(dir + L"\\mgmp_run_" + std::to_wstring(GetCurrentProcessId()) + L".lock"), "and not in the dll dir");
    check(!exists(logdir + L"\\mgmp_run_999993.lock"), "a lock two weeks old is dropped, not offered");
    check(exists(logdir + L"\\mgmp_run_4.lock"), "the lock of a running process is left alone");
    check(!exists(logdir + L"\\mgmp_run_999992.lock"), "a lock whose log (and dump) are gone is cleaned up and does not hide an older run that has files");

    LogUploadRequest req;
    check(prevrun_request(req), "the dead run is pending");
    bool legacyFound = false;
    for (int i = 0; i < req.ndelete; ++i) if (wcsstr(req.on_ok_delete[i], L"mgmp_run_999994.lock")) legacyFound = true;
    check(legacyFound, "a marker an older build left in the dll dir is picked up too (its moved log is found in the log folder)");
    check(req.nfiles >= 1 && wcsstr(req.files[0], L"\\log\\"), "the files are read from the log folder");
    bool lockListed = false;
    for (int i = 0; i < req.ndelete; ++i) if (wcsstr(req.on_ok_delete[i], L"mgmp_run_999991.lock")) lockListed = true;
    check(lockListed, "the dead lock is named for deletion once the server has the files");
    check(strcmp(req.addr, "49.233.209.67") == 0 && req.port == g_cfg_stub.signal_port, "addressed to the configured server (a port in the address is not part of the host)");

    prevrun_dismiss();
    check(!exists(logdir + L"\\mgmp_run_999991.lock") && !exists(dir + L"\\mgmp_run_999994.lock") && !prevrun_request(req), "dismissing forgets the pending runs and deletes their locks");

    prevrun_shutdown();
    check(!exists(logdir + L"\\mgmp_run_" + std::to_wstring(GetCurrentProcessId()) + L".lock"), "a clean exit removes this run's lock");

    prevrun_init(dir.c_str());
    check(!prevrun_request(req), "after a clean exit and a dismissed crash nothing is pending");
    prevrun_shutdown();

    printf(fails ? "FAILED\n" : "all good\n");
    return fails ? 1 : 0;
}
