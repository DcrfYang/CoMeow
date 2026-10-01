// mgmp_logupload.h -- sends this run's game logs to a signaling server that collects them (the F2 panel).
//
// Self-contained on purpose: it takes the file paths it is told to send and talks the server's log protocol
// (see the PROTOCOL block in server/mgmp_server.cpp: logbegin / logchunk / logend), so it needs nothing from the
// rest of the mod and can be driven from a test. One upload at a time, on its own worker thread; the panel polls
// logupload_status() every frame.
#pragma once

#include <windows.h>
#include <cstdint>

namespace mgmp {

constexpr int kLogUploadMaxFiles = 3;
constexpr uint32_t kLogUploadFileMax = 16u << 20;   // a longer file is sent as its LAST 16 MB (the end is what matters)
constexpr int kLogUploadDescMax = 1200;             // bytes of the player's description

struct LogUploadRequest {
    char     addr[64]   = {};          // the server; "localhost" if empty
    uint16_t port       = 27700;
    char     player[32] = {};
    char     desc[kLogUploadDescMax + 1] = {};   // what went wrong, in the player's words (optional)
    char     info[160]  = {};          // a one-line fingerprint (protocol, role, room) the panel fills in
    wchar_t  files[kLogUploadMaxFiles][MAX_PATH] = {};
    int      nfiles     = 0;
};

enum class LogUploadState : uint8_t { Idle, Working, Done, Failed };
enum class LogUploadError : uint8_t {
    None,
    NoFile,      // none of the files could be read
    Connect,     // could not reach the server
    ServerOld,   // it answered but does not collect logs
    Refused,     // it said no (rate limit, storage full, ...) -- `detail` is its reason
    Lost,        // the connection broke or the server stopped answering
};

struct LogUploadStatus {
    LogUploadState state   = LogUploadState::Idle;
    LogUploadError error   = LogUploadError::None;
    uint32_t       percent = 0;        // while Working
    char           detail[200] = {};   // Done: the folder id the server stored it under; Refused: the server's reason
};

// Starts the upload. False if one is already running. The request is copied.
bool            logupload_start(const LogUploadRequest& req);
LogUploadStatus logupload_status();

// Back to Idle (only when not Working), so the panel can clear a finished result.
void            logupload_reset();

// Blocks until the worker is done; for tests and shutdown.
void            logupload_wait(uint32_t timeout_ms);

// "name (size)" facts for the panel's preview line: the file's name part and its size, 0 if unreadable.
uint32_t        logupload_file_size(const wchar_t* path);

} // namespace mgmp
