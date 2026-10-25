// test_loghist.cpp -- 0.1.93: the history of logs (mgmp_log: log_history, the logver/logend lines, the queue of 5 besides this run's).
// Builds mgmp_log.cpp alone; works in a fresh folder under %TEMP%.
#include "mgmp_log.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

using namespace mgmp;

static int g_fail = 0, g_checks = 0;
static void check(bool ok, const char* what) { ++g_checks; if (!ok) { ++g_fail; std::printf("FAIL: %s\n", what); } else std::printf("  ok    %s\n", what); }

static void put(const std::wstring& path, const char* text) {
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (f) { fputs(text, f); fclose(f); }
}

int main() {
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"mgmp_loghist_test";
    CreateDirectoryW(dir.c_str(), nullptr);
    // a clean folder
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) { do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + L"\\" + fd.cFileName).c_str()); } while (FindNextFileW(h, &fd)); FindClose(h); }

    // eight earlier runs: the oldest two must go (this run's + 5 are kept)
    put(dir + L"\\mgmp_trace_20261020-100000.log", "log    : x\n");
    put(dir + L"\\mgmp_trace_20261020-110000.log", "log    : x\n");
    put(dir + L"\\mgmp_trace_20261021-100000.log", "log    : x\n");                                                  // before 0.1.93: unknown
    put(dir + L"\\mgmp_trace_20261022-100000.log", "log    : x\nlogver : 2\n000001 line\n");                          // died
    put(dir + L"\\mgmp_trace_20261023-100000.log", "log    : x\nlogver : 2\n000001 line\nlogend : clean exit\n");    // clean
    put(dir + L"\\mgmp_trace_20261023-100000.dmp", "MDMP");
    put(dir + L"\\mgmp_trace_20261024-090000.log", "log    : x\nlogver : 2\n");
    put(dir + L"\\mgmp_trace_20261024-100000.log", "log    : x\nlogver : 2\nlogend : clean exit at x\n");
    put(dir + L"\\mgmp_trace_20261024-110000.log", "log    : x\nlogver : 2\n");
    put(dir + L"\\mgmp_other_20261024-110000.log", "another stem");

    const std::wstring base = dir + L"\\mgmp_trace.log";
    log_init(base.c_str(), false);
    wchar_t cur[MAX_PATH] = {};
    check(log_current_path(cur, MAX_PATH), "this run has a log");

    LogHistoryEntry e[8];
    const int n = log_history(e, 8);
    check(n == 5, "five earlier logs are kept besides this run's (eight were there)");
    check(GetFileAttributesW((dir + L"\\mgmp_trace_20261020-100000.log").c_str()) == INVALID_FILE_ATTRIBUTES &&
          GetFileAttributesW((dir + L"\\mgmp_trace_20261020-110000.log").c_str()) == INVALID_FILE_ATTRIBUTES &&
          GetFileAttributesW((dir + L"\\mgmp_trace_20261021-100000.log").c_str()) == INVALID_FILE_ATTRIBUTES, "the oldest went first (a queue)");
    check(GetFileAttributesW((dir + L"\\mgmp_other_20261024-110000.log").c_str()) != INVALID_FILE_ATTRIBUTES, "another name's logs are not touched");
    if (n == 5) {
        check(wcsstr(e[0].path, L"20261024-110000") != nullptr && wcsstr(e[4].path, L"20261022-100000") != nullptr, "newest first, this run's own not listed");
        check(e[0].have_start && e[0].start.year == 2026 && e[0].start.month == 10 && e[0].start.day == 24 && e[0].start.hour == 11, "the start is read from the name");
        check(e[0].have_end && e[0].end.year >= 2026, "the end is the file's last write");
        check(e[0].ended == 0, "a versioned log without its logend line: did not end cleanly");
        check(e[1].ended == 1, "a logend line: a clean exit");
        check(e[3].ended == 1 && wcsstr(e[3].dump, L"20261023-100000.dmp") != nullptr, "its crash dump is found beside it");
        check(e[4].ended == 0 && !e[4].dump[0], "no dump: none named");
    }
    // this run's own markers
    log_line("TEST", "a line");
    log_mark_clean_exit();
    log_shutdown();
    {
        FILE* f = nullptr; _wfopen_s(&f, cur, L"rb");
        char buf[4096] = {}; size_t got = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0; if (f) fclose(f);
        buf[got] = 0;
        check(strstr(buf, "logver :") != nullptr, "a new log names its format at the top");
        check(strstr(buf, "logend : clean exit") != nullptr, "a clean exit ends it with the logend line");
    }
    std::printf("test_loghist: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
