// mgmp_prevrun.h -- a game that died (crash, freeze and killed, power cut) cannot upload its own log afterwards, so the log is picked up from the NEXT run, and -- when the crash handler
// still runs -- sent at the moment of the crash.
//
// HOW A BAD END IS NOTICED. Every run puts mgmp_run_<pid>.lock beside mgmp.dll (it names the run's log) and removes it on a clean exit (DLL_PROCESS_DETACH, which a crash, a kill from the
// task manager and a power cut never reach). A lock whose process is gone is therefore a run that ended badly: its log (and the .dmp the crash handler wrote beside it) are what gets sent.
//
//   * prevrun_crash_upload   from the crash handler, after the dump is written: sends this run's log and dump, waiting a few seconds -- the process is going down anyway.
//   * prevrun_auto_start     at startup: sends the previous bad run's files in the background.
//   * prevrun_request        the same files as a request for the F2 window, when the automatic upload is off or failed (the player presses "upload").
// The lock is deleted by the upload itself once the server saved the files (LogUploadRequest::on_ok_delete), so nothing is sent twice. "ui.auto_upload_crash_log": false turns the automatic part off.
#pragma once

#include "mgmp_logupload.h"

namespace mgmp {

void prevrun_init(const wchar_t* dll_dir);       // after log_init: notes the dead runs' locks as pending and creates this run's lock
void prevrun_shutdown();                         // a clean exit: this run's lock goes away
bool prevrun_request(LogUploadRequest& req);     // the pending bad run's files, addressed to the configured server; false when there is none
bool prevrun_auto_start();                       // starts that upload in the background when the setting is on and a server is configured; true when it started
bool prevrun_auto_started();
void prevrun_dismiss();                          // the player does not want it uploaded: the pending runs are forgotten
void prevrun_crash_upload();                     // from the crash handler (see above); bounded in time

} // namespace mgmp
