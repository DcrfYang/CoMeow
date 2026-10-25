// mgmp_log.h -- line-atomic trace log.
//
// Format is chosen for diffing two instances against each other (phase 2), so
// every line starts with fixed-width, deterministic counters and nothing that
// varies with wall clock:
//
//   <seq:06> <turn:04> <TAG> field=value ...
//
// Raw pointers are inherently instance-specific; they are emitted only when
// tune::kPointers is on, and always at the end of the line so a diff can be
// taken on a prefix.
#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>

namespace mgmp {

void log_init(const wchar_t* path, bool console);
void log_shutdown();
// Puts what is buffered into the file now (0.1.50: lines are buffered). Call before anything reads the log file.
void log_flush();

// The full path of the file this run is writing (the stamped name). False if there is none (no log opened).
bool log_current_path(wchar_t* out, size_t cap);

// 0.1.93 THE HISTORY OF LOGS (F2's "upload an older log"). The folder keeps this run's log and the 5 before it (a queue: the oldest goes when a new run
// starts). A log opens with a "logver" line and a clean exit closes it with a "logend" line (log_mark_clean_exit, from DLL_PROCESS_DETACH only -- the crash
// path never writes it), so a log that has the first and not the second is a run that died. The lines are on disk within kFlushMs (100 ms) of being written,
// by a thread of their own, so a frozen or killed game leaves its log all but complete.
constexpr int kLogHistoryKeep = 5;
struct LogTimeStamp { int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0; };
struct LogHistoryEntry {
    wchar_t      path[260] = {};
    wchar_t      dump[260] = {};       // the crash dump beside it ("" when there is none)
    LogTimeStamp start;                // from the stamp in the name (when the game started)
    LogTimeStamp end;                  // the file's last write (when the game stopped writing: its end, or its crash)
    bool         have_start = false, have_end = false;
    uint64_t     size = 0;
    int          ended = -1;           // 1 a clean exit, 0 it did not end cleanly, -1 unknown (a log from before 0.1.93)
};
// The earlier runs' logs beside this run's (same name, another stamp), newest first; at most `cap`. This run's log is not among them.
int  log_history(LogHistoryEntry* out, int cap);
// A clean exit: the "logend" line, on disk at once. Only from DLL_PROCESS_DETACH.
void log_mark_clean_exit();

// Emits one complete line (newline appended, flushed).
void log_line(const char* tag, const char* fmt, ...);

// THE STAGE TIMELINE (2026-10-03). Every phase of a session -- departure, the setup barrier, a node, a battle, the settlement and each step inside it -- leaves one marker here. log_stage also writes
// a "STAGE" line; log_stage_quiet only remembers it. The last markers (with how long ago each was) are printed when the process dies, so a crash says WHICH phase it died in without anyone having to
// read the log back to find out.
void log_stage(const char* fmt, ...);
void log_stage_quiet(const char* fmt, ...);
void log_stage_dump(const char* tag);                 // the remembered markers, oldest first, through log_line
const char* log_stage_last();                          // the newest marker (empty before the first)

// No seq/turn prefix -- used for the startup banner.
void log_raw(const char* fmt, ...);

void     log_set_turn(uint32_t turn);
uint32_t log_turn();
uint32_t log_bump_turn();

// --- the in-memory ring, for the ImGui log pane ------------------------------
//
// The file stays the record of a run; this is the live view of it. Every line
// that reaches the file is also copied here with a severity, so the pane never
// has to re-parse the text and the two can never disagree about what was said.
//
// Severity is classified ONCE, at emit time, in classify() -- not per frame in
// the UI. The classifier is a heuristic because the codebase has exactly one
// severity marker: `!!`, at 110 call sites, and it deliberately covers both
// "this run is over" (!! HALT) and "this is odd, proceeding" (!! host offered
// 3 options, this peer built 4). Splitting those two apart is the whole reason
// the classifier looks at the body and not just the prefix.
//
// log_line_lvl() is the escape hatch: state the level and the heuristic is
// skipped. Use it where being wrong would be misleading, not everywhere.

enum class LogLevel : uint8_t {
    Trace = 0,   // high-frequency chatter -- wire traffic, frame ticks
    Info  = 1,   // the default
    Good  = 2,   // something agreed, connected or completed
    Warn  = 3,   // odd, but the run continues
    Error = 4,   // a halt, a mismatch, or a feature turning itself off
};

struct LogEntry {
    uint32_t seq   = 0;
    uint32_t turn  = 0;
    LogLevel level = LogLevel::Info;
    char     tag[12]  = {};
    char     text[236] = {};
};

// As log_line, but states the severity instead of having it inferred.
void log_line_lvl(LogLevel level, const char* tag, const char* fmt, ...);

// Copies out every entry newer than `cursor`, oldest first, up to `cap`, and
// advances `cursor` past them. Returns how many were written.
//
// A cursor, not a snapshot: the pane keeps its own list and asks only for what
// it has not seen, so the per-frame cost is the number of NEW lines -- which is
// zero on a quiet frame. Copying the whole ring every frame would be ~600 KB of
// memcpy at 60 Hz to show the same text again.
//
// `dropped_out`, if given, receives the number of lines that fell out of the
// ring between this call and the last -- a pane that silently skips lines is
// worse than no pane, so it is counted and shown.
//
// Pass cursor = 0 on the first call to receive the whole ring.
uint32_t log_ring_fetch(uint32_t& cursor, LogEntry* out, uint32_t cap,
                        uint32_t* dropped_out);

} // namespace mgmp
