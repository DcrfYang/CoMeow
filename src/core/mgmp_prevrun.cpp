// mgmp_prevrun.cpp -- see mgmp_prevrun.h.
#include "mgmp_prevrun.h"

#include "mgmp_config.h"
#include "mgmp_log.h"
#include "mgmp_paths.h"
#include "mgmp_proto.h"

#include <cstdio>
#include <cstring>

namespace mgmp {
namespace {

constexpr int kMaxStale = 8;
constexpr ULONGLONG kStaleAgeMs = 14ull * 24 * 3600 * 1000;   // a lock older than this is forgotten, not offered

struct Stale { wchar_t lock[MAX_PATH]; wchar_t log[MAX_PATH]; ULONGLONG when; };

wchar_t  g_dir[MAX_PATH] = {};
wchar_t  g_own[MAX_PATH] = {};
Stale    g_stale[kMaxStale] = {};
int      g_nstale = 0;
bool     g_auto_started = false;
LONG     g_crash_once = 0;

ULONGLONG ft_to_u64(const FILETIME& f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

bool alive(DWORD pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;     // exists but is not ours to open: treat as running
    const bool running = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return running;
}

bool exists(const wchar_t* p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

// "<pid>\n<log path in UTF-8>\n"
bool read_lock(const wchar_t* path, wchar_t* log, size_t cap) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[1400] = {}; DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);
    const char* nl = strchr(buf, '\n');
    if (!nl) return false;
    const char* p = nl + 1;
    char line[1100] = {};
    size_t n = 0;
    while (p[n] && p[n] != '\n' && p[n] != '\r' && n < sizeof(line) - 1) { line[n] = p[n]; ++n; }
    log[0] = 0;
    return n > 0 && MultiByteToWideChar(CP_UTF8, 0, line, -1, log, (int)cap) > 0;
}

void write_lock(const wchar_t* path) {
    wchar_t cur[MAX_PATH] = {};
    log_current_path(cur, MAX_PATH);
    char u8[1100] = {};
    WideCharToMultiByte(CP_UTF8, 0, cur, -1, u8, sizeof(u8) - 1, nullptr, nullptr);
    char text[1200];
    const int n = _snprintf_s(text, sizeof(text), _TRUNCATE, "%lu\n%s\n", GetCurrentProcessId(), u8);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    if (n > 0) WriteFile(h, text, (DWORD)n, &w, nullptr);
    CloseHandle(h);
}

// the log's path with another extension ("" when it has none)
void with_ext(const wchar_t* log, const wchar_t* ext, wchar_t* out) {
    wcsncpy_s(out, MAX_PATH, log, _TRUNCATE);
    wchar_t* dot = wcsrchr(out, L'.');
    wchar_t* slash = wcsrchr(out, L'\\');
    if (dot && (!slash || dot > slash)) wcscpy_s(dot, (size_t)(out + MAX_PATH - dot), ext);
    else out[0] = 0;
}

bool loopback_or_empty(const char* a) {
    return !a[0] || !_stricmp(a, "localhost") || !strncmp(a, "127.", 4) || !strcmp(a, "0.0.0.0");
}

void fill_server(LogUploadRequest& req) {
    const Config& c = config();
    strncpy_s(req.addr, sizeof(req.addr), c.signal_addr, _TRUNCATE);
    if (char* colon = strchr(req.addr, ':')) *colon = 0;      // "host:port" in the address field: the port field is the one used
    req.port = c.signal_port;
    if (c.signal_name[0]) strncpy_s(req.player, sizeof(req.player), c.signal_name, _TRUNCATE);
    else { DWORD n = sizeof(req.player); if (!GetComputerNameA(req.player, &n)) strcpy_s(req.player, "player"); }
}

} // namespace

void prevrun_init(const wchar_t* dll_dir) {
    paths_log_dir(dll_dir, g_dir, MAX_PATH);          // the markers live in the log folder (mgmp_paths.h)
    swprintf_s(g_own, L"%s\\mgmp_run_%lu.lock", g_dir, GetCurrentProcessId());
    g_nstale = 0;

    FILETIME nowft{}; GetSystemTimeAsFileTime(&nowft);
    const ULONGLONG now = ft_to_u64(nowft);
    // the log folder, and the folder itself, where an older build put them
    const wchar_t* const dirs[2] = { g_dir, dll_dir };
    for (int di = 0; di < 2; ++di) {
    if (di == 1 && _wcsicmp(g_dir, dll_dir) == 0) break;
    const wchar_t* scan_dir = dirs[di];
    wchar_t pat[MAX_PATH];
    swprintf_s(pat, L"%s\\mgmp_run_*.lock", scan_dir);
    WIN32_FIND_DATAW fd{};
    HANDLE f = FindFirstFileW(pat, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            DWORD pid = 0;
            if (swscanf_s(fd.cFileName, L"mgmp_run_%lu.lock", &pid) != 1 || pid == GetCurrentProcessId()) continue;
            wchar_t full[MAX_PATH];
            swprintf_s(full, L"%s\\%s", scan_dir, fd.cFileName);
            if (alive(pid)) continue;                                      // another game instance from this folder is still running
            const ULONGLONG when = ft_to_u64(fd.ftLastWriteTime);
            if (now > when && (now - when) / 10000 > kStaleAgeMs) { DeleteFileW(full); continue; }
            if (g_nstale >= kMaxStale) continue;
            Stale& s = g_stale[g_nstale];
            wcsncpy_s(s.lock, full, _TRUNCATE);
            if (!read_lock(full, s.log, MAX_PATH)) s.log[0] = 0;
            if (s.log[0] && !exists(s.log)) {         // the log was moved into the log folder after an older build wrote this marker
                const wchar_t* slash = wcsrchr(s.log, L'\\');
                wchar_t alt[MAX_PATH];
                swprintf_s(alt, L"%s\\%s", g_dir, slash ? slash + 1 : s.log);
                if (exists(alt)) wcsncpy_s(s.log, alt, _TRUNCATE);
            }
            s.when = when;
            wchar_t dmp[MAX_PATH] = {};
            with_ext(s.log, L".dmp", dmp);
            if (!(s.log[0] && exists(s.log)) && !(dmp[0] && exists(dmp))) { DeleteFileW(full); continue; }   // nothing left of that run to send
            ++g_nstale;
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    }
    write_lock(g_own);
    if (g_nstale) log_line("PREVRUN", "%d earlier run(s) of this folder did not end cleanly (crash, freeze, killed): their logs are pending upload", g_nstale);
}

void prevrun_shutdown() {
    if (g_own[0]) DeleteFileW(g_own);
}

bool prevrun_request(LogUploadRequest& req) {
    req = LogUploadRequest{};
    if (g_nstale == 0) return false;
    int newest = 0;
    for (int i = 1; i < g_nstale; ++i) if (g_stale[i].when > g_stale[newest].when) newest = i;
    const Stale& s = g_stale[newest];
    if (s.log[0] && exists(s.log)) wcsncpy_s(req.files[req.nfiles++], MAX_PATH, s.log, _TRUNCATE);
    wchar_t dmp[MAX_PATH] = {};
    with_ext(s.log, L".dmp", dmp);
    if (dmp[0] && exists(dmp) && req.nfiles < kLogUploadMaxFiles) wcsncpy_s(req.files[req.nfiles++], MAX_PATH, dmp, _TRUNCATE);
    if (req.nfiles == 0) {      // nothing left to send: the bookkeeping is stale
        prevrun_dismiss();
        return false;
    }
    fill_server(req);
    strncpy_s(req.desc, sizeof(req.desc), "(automatic) the previous run of the game ended without a clean exit: crash, freeze or killed", _TRUNCATE);
    _snprintf_s(req.info, sizeof(req.info), _TRUNCATE, "proto %u, previous run, %d file(s)", (unsigned)kProtoVersion, req.nfiles);
    for (int i = 0; i < g_nstale && req.ndelete < 4; ++i) wcsncpy_s(req.on_ok_delete[req.ndelete++], MAX_PATH, g_stale[i].lock, _TRUNCATE);
    return true;
}

void prevrun_dismiss() {
    for (int i = 0; i < g_nstale; ++i) DeleteFileW(g_stale[i].lock);
    g_nstale = 0;
}

bool prevrun_auto_start() {
    if (!config().auto_upload_crash_log || g_auto_started) return false;
    LogUploadRequest req;
    if (!prevrun_request(req) || loopback_or_empty(req.addr)) return false;     // no server configured: the player can still use F2 with an address
    if (!logupload_start(req)) return false;
    g_auto_started = true;
    log_line("PREVRUN", "uploading the previous run's log in the background (%d file(s)) to %s:%u", req.nfiles, req.addr, (unsigned)req.port);
    return true;
}

bool prevrun_auto_started() { return g_auto_started; }

void prevrun_crash_upload() {
    if (InterlockedCompareExchange(&g_crash_once, 1, 0) != 0) return;
    if (!config().auto_upload_crash_log) return;
    LogUploadRequest req;
    fill_server(req);
    if (loopback_or_empty(req.addr)) return;
    wchar_t cur[MAX_PATH] = {};
    if (!log_current_path(cur, MAX_PATH)) return;
    wcsncpy_s(req.files[req.nfiles++], MAX_PATH, cur, _TRUNCATE);
    wchar_t dmp[MAX_PATH] = {};
    with_ext(cur, L".dmp", dmp);
    if (dmp[0] && exists(dmp)) wcsncpy_s(req.files[req.nfiles++], MAX_PATH, dmp, _TRUNCATE);
    strncpy_s(req.desc, sizeof(req.desc), "(automatic) sent by the crash handler: the game crashed", _TRUNCATE);
    _snprintf_s(req.info, sizeof(req.info), _TRUNCATE, "proto %u, crash handler, %d file(s)", (unsigned)kProtoVersion, req.nfiles);
    if (g_own[0]) wcsncpy_s(req.on_ok_delete[req.ndelete++], MAX_PATH, g_own, _TRUNCATE);
    if (!logupload_start(req)) return;
    logupload_wait(12000);
    const LogUploadStatus st = logupload_status();
    if (st.state == LogUploadState::Done) log_line("CRASH", "  log upload from the crash handler: saved by the server, id %s", st.detail);
    else log_line("CRASH", "  log upload from the crash handler: %s", st.state == LogUploadState::Working ? "still running when the wait ended" : "failed");
}

} // namespace mgmp
