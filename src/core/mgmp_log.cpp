#include "mgmp_log.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <share.h>

namespace mgmp {
namespace {

CRITICAL_SECTION g_cs;
bool             g_cs_ready = false;
FILE*            g_file     = nullptr;
wchar_t          g_path[MAX_PATH] = {};   // the stamped file name, for log_current_path
bool             g_console  = false;
LONG volatile    g_seq      = 0;
LONG volatile    g_turn     = 0;

// --- the ring ---------------------------------------------------------------
//
// Written under g_cs, which the receive thread already contends for, so this
// adds no new lock and no new ordering. Fixed-size entries rather than a packed
// character ring: a line is at most 4 KB but the median is well under 100
// bytes, and a fixed stride means the fetch below is a memcpy per entry instead
// of a parse.

constexpr uint32_t kRingCap = 4096;

LogEntry g_ring[kRingCap];
uint32_t g_ring_written = 0;   // total ever written; the oldest live entry is
                               // g_ring_written - min(g_ring_written, kRingCap)

namespace {
constexpr int kStageRing = 64;
struct StageMark { ULONGLONG at = 0; char text[160] = {}; };
StageMark g_stage[kStageRing];
LONG volatile g_stage_next = 0;

void stage_push_text(const char* text) {
    const LONG slot = InterlockedIncrement(&g_stage_next) - 1;
    StageMark& m = g_stage[slot % kStageRing];
    m.at = GetTickCount64();
    strncpy_s(m.text, sizeof(m.text), text, _TRUNCATE);
}

void stage_push(const char* fmt, va_list ap) {
    char text[160];
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, fmt, ap);
    stage_push_text(text);
}

// The log tags whose lines are phase changes of a session and are remembered in the timeline without any call site naming them: a crash then says which of these came last.
bool stage_tag(const char* tag) {
    static const char* kTags[] = { "SETUP", "SETTLE", "DEPART", "CHECKPOINT", "SAVEFILE", "PAGE", "LEAVE", "ROSTER", "ROOM", "SESSION", "SQUATTERS" };
    for (const char* t : kTags) if (strcmp(tag, t) == 0) return true;
    return false;
}
} // namespace


// Whether `hay` starts with `needle`, after skipping leading spaces.
bool starts_with(const char* hay, const char* needle) {
    while (*hay == ' ') ++hay;
    return strncmp(hay, needle, strlen(needle)) == 0;
}

// Severity from the message body. See the note in mgmp_log.h for why this is a
// heuristic and not a field at every call site.
LogLevel classify(const char* tag, const char* body) {
    if (starts_with(body, "!!")) {
        // `!!` means "notable", not "fatal" -- these are the ones that end a
        // run or switch a feature off, and they are worth separating from the
        // ones that warn and carry on.
        static const char* kFatal[] = {
            "HALT", "MISMATCH", "faulted", "does not match the pinned build",
            "is OFF", "disabled", "unresolved", "refus", "not applied",
        };
        for (const char* k : kFatal)
            if (strstr(body, k)) return LogLevel::Error;
        return LogLevel::Warn;
    }

    // "on --" IS GONE, and it is worth saying why so it does not come back. It
    // was here to grade the modules' "<feature> on -- <what it does>" banners,
    // and every one of those now states its own level instead. What it still
    // matched was collateral: it is a SUBSTRING, so it fired on any sentence
    // ending a word in "on" before a dash -- "peer reticles on --", but also
    // "role set to host by the connect butt(on --) mgmp.json said off". The
    // first defeated the CURSOR tag rule eight lines below, which exists to keep
    // exactly that line quiet; the second graded a configuration warning as
    // good news.
    //
    // A heuristic that has to be true of the BODY is only safe when it cannot
    // be true by accident, and this one could.
    static const char* kGood[] = { "AGREES", "agreed", "accepted", "done" };
    for (const char* k : kGood)
        if (strstr(body, k)) return LogLevel::Good;

    // Wire traffic and frame ticks are volume, not events. They stay in the
    // ring -- filtering them out here would make the pane unable to show them
    // at all -- but they default to the dimmest colour and the pane can hide
    // the whole level with one checkbox.
    if (starts_with(body, "->") || starts_with(body, "<-")) return LogLevel::Trace;
    if (strcmp(tag, "FRAME") == 0 || strcmp(tag, "CURSOR") == 0 ||
        strcmp(tag, "TRACE") == 0)
        return LogLevel::Trace;

    return LogLevel::Info;
}

// Caller holds g_cs.
void ring_push_locked(uint32_t seq, uint32_t turn, const char* tag,
                      const char* body, LogLevel level) {
    LogEntry& e = g_ring[g_ring_written % kRingCap];
    e.seq   = seq;
    e.turn  = turn;
    e.level = level;
    strncpy(e.tag, tag ? tag : "", sizeof(e.tag) - 1);
    e.tag[sizeof(e.tag) - 1] = 0;
    strncpy(e.text, body ? body : "", sizeof(e.text) - 1);
    e.text[sizeof(e.text) - 1] = 0;
    // The file keeps the untruncated line; the pane is a view, and a 236-byte
    // window onto a 4 KB hexdump is the right trade for a fixed stride.
    ++g_ring_written;
}

void emit_locked(const char* s, size_t n) {
    if (g_file) {
        fwrite(s, 1, n, g_file);
        fflush(g_file);
    }
    if (g_console) {
        DWORD written = 0;
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h && h != INVALID_HANDLE_VALUE)
            WriteFile(h, s, (DWORD)n, &written, nullptr);
    }
}

} // namespace

// A log per session is the right default for debugging and a slow leak for a player: keep the newest few dozen of this
// process's logs (same stem and extension, stamped names sort by time) and delete the rest. Never touches the file
// just opened, and a failure to delete is silent -- housekeeping must not be able to stop the game starting.
static void prune_old_logs(const wchar_t* path) {
    constexpr size_t kKeep = 30;
    wchar_t dir[MAX_PATH] = {}, pattern[MAX_PATH] = {};
    const wchar_t* slash = wcsrchr(path, L'\\');
    const wchar_t* dot = wcsrchr(path, L'.');
    if (!slash) return;
    wcsncpy_s(dir, path, (size_t)(slash - path + 1));
    const size_t stem = (dot && dot > slash) ? (size_t)(dot - (slash + 1)) : wcslen(slash + 1);
    _snwprintf_s(pattern, _TRUNCATE, L"%s%.*s_*%s", dir, (int)stem, slash + 1, (dot && dot > slash) ? dot : L"");
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    wchar_t names[256][64];
    size_t n = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n < 256 && wcslen(fd.cFileName) < 64)
            wcscpy_s(names[n++], fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (n <= kKeep) return;
    for (size_t i = 0; i < n; ++i)                       // oldest first (the stamp is in the name)
        for (size_t j = i + 1; j < n; ++j)
            if (wcscmp(names[j], names[i]) < 0) { wchar_t t[64]; wcscpy_s(t, names[i]); wcscpy_s(names[i], names[j]); wcscpy_s(names[j], t); }
    for (size_t i = 0; i + kKeep < n; ++i) {
        wchar_t full[MAX_PATH];
        _snwprintf_s(full, _TRUNCATE, L"%s%s", dir, names[i]);
        DeleteFileW(full);
    }
}

void log_init(const wchar_t* path, bool console) {
    if (!g_cs_ready) {
        InitializeCriticalSection(&g_cs);
        g_cs_ready = true;
    }
    if (console && !g_console) {
        if (AllocConsole()) {
            SetConsoleTitleW(L"mgmp trace");
            // Re-point the CRT's stdout at the new console so printf-style
            // output from anywhere in the DLL lands somewhere visible.
            FILE* dummy = nullptr;
            freopen_s(&dummy, "CONOUT$", "w", stdout);
        }
        g_console = true;
    }
    if (path && !g_file) {
        // Stamp the name with local time: <stem>_YYYYMMDD-HHMMSS<ext>.
        //
        // Every previous run overwrote the last one, which is fine while a
        // session is one run but actively harmful once two peers are launched
        // repeatedly -- the run that explained a desync is gone the moment you
        // relaunch to look again. Keeping them all is cheap; losing one is not.
        wchar_t stamped[MAX_PATH];
        SYSTEMTIME t{};
        GetLocalTime(&t);

        const wchar_t* dot   = wcsrchr(path, L'.');
        const wchar_t* slash = wcsrchr(path, L'\\');
        if (dot && (!slash || dot > slash)) {
            int stem = (int)(dot - path);
            _snwprintf_s(stamped, _TRUNCATE, L"%.*s_%04d%02d%02d-%02d%02d%02d%s",
                         stem, path, t.wYear, t.wMonth, t.wDay,
                         t.wHour, t.wMinute, t.wSecond, dot);
        } else {
            _snwprintf_s(stamped, _TRUNCATE, L"%s_%04d%02d%02d-%02d%02d%02d",
                         path, t.wYear, t.wMonth, t.wDay,
                         t.wHour, t.wMinute, t.wSecond);
        }

        // _SH_DENYWR, not fopen's exclusive default: the whole point of this
        // file is that another process tails it while the game is running.
        g_file = _wfsopen(stamped, L"w", _SH_DENYWR);
        if (g_file) wcsncpy_s(g_path, stamped, _TRUNCATE);
        // Into the file, not stdout: with no console there is nowhere else for
        // it to go, and a log whose first line names itself is what tells you
        // which of the stamped files you are looking at.
        // fprintf and not fwprintf: the first call sets the stream's
        // orientation and every other write here is byte-oriented fwrite, which
        // a wide-oriented stream would then refuse.
        if (g_file) fprintf(g_file, "log    : %ls\n", stamped);
        prune_old_logs(path);
        {   // the crash dumps and the cat dumps that sit beside the logs are kept to the same count
            wchar_t other[MAX_PATH] = {};
            wcsncpy_s(other, path, _TRUNCATE);
            wchar_t* d = wcsrchr(other, L'.');
            const wchar_t* sl = wcsrchr(other, L'\\');
            if (d && (!sl || d > sl)) { wcscpy_s(d, (size_t)(other + MAX_PATH - d), L".dmp"); prune_old_logs(other); }
            wcsncpy_s(other, path, _TRUNCATE);
            if (wchar_t* s2 = wcsrchr(other, L'\\')) { wcscpy_s(s2 + 1, (size_t)(other + MAX_PATH - (s2 + 1)), L"mgmp_catdump.bin"); prune_old_logs(other); }
        }
    }
}

void log_shutdown() {
    if (!g_cs_ready) return;
    EnterCriticalSection(&g_cs);
    if (g_file) { fflush(g_file); fclose(g_file); g_file = nullptr; }
    LeaveCriticalSection(&g_cs);
}

namespace {

// The one place a tagged line is formatted, so the file and the ring can never
// disagree about what was said. `level` is applied as given; classify() runs
// only when it is not stated.
void emit_line_v(const LogLevel* level, const char* tag, const char* fmt, va_list ap) {
    if (!g_cs_ready) return;

    char    buf[4096];
    LONG    seq = InterlockedIncrement(&g_seq);
    LONG    turn = g_turn;
    int     n   = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                              "%06ld %04ld %-9s ", seq, turn, tag);
    if (n < 0) return;

    const int body = n;   // where the message starts, for the ring

    int m = _vsnprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, fmt, ap);
    if (m < 0) m = (int)(sizeof(buf) - n - 1);   // truncated; emit what we have
    n += m;

    LogLevel lv = level ? *level : classify(tag, buf + body);

    // Terminate the body before appending the newline so the ring never stores
    // one -- ImGui renders a trailing newline as a blank line per entry.
    buf[n] = 0;

    if (stage_tag(tag)) {                      // a phase change: remembered for the crash report (the ring is lock-free)
        char mark[160];
        _snprintf_s(mark, sizeof(mark), _TRUNCATE, "%s %s", tag, buf + body);
        stage_push_text(mark);
    }
    EnterCriticalSection(&g_cs);
    ring_push_locked((uint32_t)seq, (uint32_t)turn, tag, buf + body, lv);
    if (n < (int)sizeof(buf) - 2) { buf[n++] = '\n'; buf[n] = 0; }
    emit_locked(buf, (size_t)n);
    LeaveCriticalSection(&g_cs);
}

} // namespace

void log_line(const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit_line_v(nullptr, tag, fmt, ap);
    va_end(ap);
}

void log_line_lvl(LogLevel level, const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit_line_v(&level, tag, fmt, ap);
    va_end(ap);
}

uint32_t log_ring_fetch(uint32_t& cursor, LogEntry* out, uint32_t cap,
                        uint32_t* dropped_out) {
    if (dropped_out) *dropped_out = 0;
    if (!g_cs_ready || !out || !cap) return 0;

    uint32_t written = 0;
    EnterCriticalSection(&g_cs);

    // Indices into the total-ever-written sequence, so wrap-around arithmetic
    // happens in one place: [oldest, g_ring_written) is what the ring still
    // holds.
    uint32_t oldest = g_ring_written > kRingCap ? g_ring_written - kRingCap : 0;
    uint32_t from   = cursor > oldest ? cursor : oldest;

    if (dropped_out && cursor < oldest) *dropped_out = oldest - cursor;

    for (uint32_t i = from; i < g_ring_written && written < cap; ++i)
        out[written++] = g_ring[i % kRingCap];

    cursor = from + written;
    LeaveCriticalSection(&g_cs);
    return written;
}

void log_stage_quiet(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    stage_push(fmt, ap);
    va_end(ap);
}

void log_stage(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    stage_push(fmt, ap);
    va_end(ap);
    const LONG last = g_stage_next - 1;
    if (last >= 0) log_line("STAGE", "%s", g_stage[last % kStageRing].text);
}

const char* log_stage_last() {
    const LONG last = g_stage_next - 1;
    return last >= 0 ? g_stage[last % kStageRing].text : "";
}

void log_stage_dump(const char* tag) {
    const LONG total = g_stage_next;
    const LONG first = total > kStageRing ? total - kStageRing : 0;
    const ULONGLONG now = GetTickCount64();
    for (LONG i = first; i < total; ++i) {
        const StageMark& m = g_stage[i % kStageRing];
        log_line(tag, "  stage -%llu ms: %s", (unsigned long long)(now - m.at), m.text);
    }
}

void log_raw(const char* fmt, ...) {
    if (!g_cs_ready) return;

    char    buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)sizeof(buf) - 2;
    buf[n] = 0;

    // The banner goes in the ring too -- it is the single most useful thing to
    // see in the pane on a fresh launch, because it says which hooks armed and
    // which two switches are editing the game. `[!]` is the banner's own marker
    // and means the same thing `!!` means on a tagged line.
    LogLevel lv = starts_with(buf, "[!]") ? LogLevel::Warn : LogLevel::Info;

    EnterCriticalSection(&g_cs);
    // g_seq is READ, not bumped. It is a diff key for the file format -- two
    // instances are diffed line by line on it -- and the host and client emit
    // different numbers of banner lines, so bumping here would offset one
    // peer's whole log against the other's. The ring orders by write index
    // anyway; seq here is only ever displayed.
    ring_push_locked((uint32_t)g_seq, (uint32_t)g_turn, "", buf, lv);
    if (n < (int)sizeof(buf) - 2) { buf[n++] = '\n'; buf[n] = 0; }
    emit_locked(buf, (size_t)n);
    LeaveCriticalSection(&g_cs);
}

void     log_set_turn(uint32_t t) { InterlockedExchange(&g_turn, (LONG)t); }
uint32_t log_turn()               { return (uint32_t)g_turn; }
uint32_t log_bump_turn()          { return (uint32_t)InterlockedIncrement(&g_turn); }


bool log_current_path(wchar_t* out, size_t cap) {
    if (!g_path[0] || !cap) return false;
    wcsncpy_s(out, cap, g_path, _TRUNCATE);
    return true;
}

} // namespace mgmp
