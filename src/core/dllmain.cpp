// mgmp.dll -- Mewgenics multiplayer mod, L0 loader payload.
//
// Phase 1 scope (see the design notes): get code running inside the process at fixed
// RVAs and trace the turn/command boundaries. No networking, no determinism
// work, no state changes.
#include "mgmp_addresses.h"
#include "mgmp_crash.h"
#include "mgmp_config.h"
#include "mgmp_debug.h"
#include "mgmp_tuning.h"
#include "mgmp_hooks.h"
#include "mgmp_resolve.h"
#include "mgmp_log.h"
#include "mgmp_prevrun.h"
#include "mgmp_paths.h"
#include "mgmp_record.h"
#include "mgmp_replay.h"
#include "mgmp_session.h"
#include "mgmp_signal.h"
#include "mgmp_ui.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace {

wchar_t g_dll_dir[MAX_PATH] = {};
wchar_t g_log_dir[MAX_PATH] = {};     // <dll dir>\log (mgmp_paths.h); the dll dir itself when that could not be made

// Raw-Win32 breadcrumb, independent of the CRT and of the trace log itself.
// When injection goes wrong the interesting failures happen before the logger
// exists, and "no log file" is indistinguishable from "the thread never ran".
void breadcrumb(const char* what) {
    wchar_t path[MAX_PATH];
    if (g_log_dir[0])
        swprintf_s(path, L"%s\\mgmp_boot.log", g_log_dir);
    else
        wcscpy_s(path, L"mgmp_boot.log");

    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char  line[256];
    int   n = _snprintf_s(line, sizeof(line), _TRUNCATE, "%s\r\n", what);
    DWORD written = 0;
    if (n > 0) WriteFile(h, line, (DWORD)n, &written, nullptr);
    CloseHandle(h);
}

void compute_dll_dir(HMODULE self) {
    GetModuleFileNameW(self, g_dll_dir, MAX_PATH);
    wchar_t* slash = wcsrchr(g_dll_dir, L'\\');
    if (slash) *slash = 0;
    mgmp::paths_log_dir(g_dll_dir, g_log_dir, MAX_PATH);
}

DWORD WINAPI init_thread(LPVOID) {
    using namespace mgmp;
    breadcrumb("init_thread: running");

    config_load(g_dll_dir);
    breadcrumb("init_thread: config loaded");
    {   // an older build left its logs in the folder itself: they move into the log folder
        static const wchar_t* const old_files[] = { L"mgmp_trace_*.log", L"mgmp_trace_*.dmp", L"mgmp_catdump_*.bin", L"mgmp_boot.log" };
        paths_move_legacy(g_dll_dir, g_log_dir, old_files, sizeof(old_files) / sizeof(old_files[0]));
    }

    log_init(config().log_path, tune::kConsole);
    breadcrumb("init_thread: log open");
    prevrun_init(g_dll_dir);       // notes runs that ended badly, and marks this one (removed again on a clean exit)
    prevrun_auto_start();          // and sends the last bad run's log in the background (ui.auto_upload_crash_log)

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    HMODULE   main_mod = GetModuleHandleW(nullptr);
    uintptr_t base     = (uintptr_t)main_mod;

    log_raw("=== mgmp phase 1 trace harness ===");
    log_raw("host   : %S", exe);
    log_raw("base   : %p  (pinned imagebase 0x140000000)", (void*)base);
    log_raw("pid    : %lu", GetCurrentProcessId());
    {   // WHICH mgmp.dll THIS IS: the file's own write time and size, so a log says which build produced it (the other peer's folder is updated by a launcher and cannot be assumed current)
        wchar_t self[MAX_PATH] = {};
        HMODULE me = nullptr;
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&init_thread, &me) &&
            GetModuleFileNameW(me, self, MAX_PATH) && GetFileAttributesExW(self, GetFileExInfoStandard, &fa)) {
            SYSTEMTIME st{};
            FileTimeToSystemTime(&fa.ftLastWriteTime, &st);
            log_raw("build  : mgmp.dll written %04d-%02d-%02d %02d:%02d:%02d UTC, %lu bytes", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, (unsigned long)fa.nFileSizeLow);
        }
    }
    log_raw("dumping %u bytes of TurnAction, pointers=%d",
            tune::kTaDump, (int)tune::kPointers);

    char err[256] = {};
    if (!hooks_verify_module(base, err, sizeof(err))) {
        log_raw("[!] REFUSING TO HOOK: %s", err);
        // Said on screen as well: a mod that silently does nothing looks like a mod that is broken.
        MessageBoxW(nullptr, L"mgmp: this version of Mewgenics is not the one the mod was made for, so the mod has "
                             L"switched itself off (the game runs unmodified).\n\nSee the mgmp log for details; update the mod, "
                             L"or set debug.allow_unpinned_build = true in mgmp.json at your own risk.",
                    L"mgmp", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        log_raw("[!] Re-run mod/tools/ida/gen_sigs.py against a .i64 of THIS");
        log_raw("[!] build to regenerate mgmp_sigs.generated.h.");
        return 1;
    }
    {
        int resolved = 0, moved = 0, failed = 0;
        mgmp::resolve_counts(&resolved, &moved, &failed);
        log_raw("addresses: %d resolved by signature, %d moved, %d unresolved%s",
                resolved, moved, failed,
                mgmp::resolve_is_pinned_build() ? "  (pinned build)" : "");
    }

    // Opened before the hooks go live, so no draw can arrive with nowhere to go.
    //
    // The recorder's state is always stated, never implied by the absence of a
    // line. `record = 2` once parsed as false and produced a run with no capture
    // and no complaint; the only defence against that class of mistake is for
    // the banner to say plainly which mode it is in.
    if (config().parse_warnings[0])
        log_raw("[!] mgmp.json: %s", config().parse_warnings);

    if (config().record) {
        const IMAGE_DOS_HEADER*   dos = (const IMAGE_DOS_HEADER*)base;
        const IMAGE_NT_HEADERS64* nt  = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
        record_set_image(base, nt->OptionalHeader.SizeOfImage);
        record_init(config().record_path, nt->OptionalHeader.SizeOfImage, config().record_note);
        if (record_active())
            log_raw("record : ON  -> %S   note='%s'",
                    config().record_path, config().record_note);
        else
            log_raw("[!] record: ON was requested but the stream did not open"
                    " -- continuing WITHOUT a capture");
    } else {
        log_raw("record : OFF (debug.record = true in mgmp.json captures a run)");
    }

    // Run B. Loaded after the recorder so a replay run captures its own stream
    // to diff against the one it is replaying -- replaying without recording
    // reproduces a battle but measures nothing.
    if (config().replay_path[0]) {
        replay_init(config().replay_path, tune::kReplayBrains);
        if (replay_active())
            log_raw("replay : ON  <- %S   inject into brains matching '%s'",
                    config().replay_path, tune::kReplayBrains);
        else
            log_raw("[!] replay: a capture was named but did not load"
                    " -- continuing WITHOUT injection");
        if (!config().record)
            log_raw("[!] replay is ON but record is OFF -- this run will"
                    " reproduce the battle without capturing anything to diff");
    } else {
        log_raw("replay : OFF (debug.replay = \"<capture>.mgr\" injects a run)");
    }

    // The debug panel. Started before the session so its log pane already
    // exists when the handshake starts talking, and independently of it because
    // it works -- and is worth having -- with no session at all.
    //
    // It draws from the swap takeover rather than from a frame hook, so unlike
    // everything below it does not need hook_framebegin, which net_role forces
    // on. That is what lets it come up in a plain single-player launch.
    ui_init();

    // Phase 4, plus the lobby. Started before the hooks so the frame hook has
    // somewhere to pump to on its very first call.
    //
    // The FALSE branch is no longer "multiplayer is off": every instance now
    // launches without a role, and the role comes from the lobby. It is still
    // an ordinary single-player process until somebody creates or joins a room,
    // and it stays one if nobody does.
    if (session_start()) {
        log_raw("net    : %s   control = %s", session_status(),
                config().net_control_count ? "see below" : "(nothing -- observer)");
        for (uint32_t i = 0; i < config().net_control_count; ++i)
            log_raw("         local cat %u", (unsigned)config().net_control[i]);
        if (config().replay_path[0])
            log_raw("[!] net and replay are both ON -- lockstep wins at"
                    " Brain::GetChoice and the replay FIFO will not advance");
    } else {
        log_raw("net    : neutral -- no role at launch; the lobby decides it");
        log_raw("         (signal.legacy_role = true restores net.role)");
    }

    log_raw("lobby  : %s -- %s", signal_state_name(signal_state()), signal_status());
    if (config().signal_on)
        log_raw("         server %s:%u, you are '%s'%s",
                config().signal_addr, (unsigned)config().signal_port,
                signal_name(),
                config().signal_room[0] ? " (auto room)" : "");

    // Debug mode. Decided here rather than at config load because the loader's
    // marker has to be compared against the file, and the answer belongs in the
    // banner next to the other "this run is not a play session" warnings.
    debug_init();

    // Say it loudly. Every other hook observes; this one edits the game, so a
    // capture taken with it on is not a capture of the shipped game and the log
    // must not let that go unnoticed weeks later.
    // Same rule, for the same reason: a run that does not stop at the first
    // hash mismatch is a run that cannot claim the peers stayed in sync, and
    // the log has to say so at the top rather than leave it to be inferred from
    // an absent HALT line weeks later.
    if (!config().net_desync_halt)
        log_raw("[!] desync HALT is DISABLED (debug.desync_halt = false) -- a hash"
                " mismatch will be reported and the run will CONTINUE; this run"
                " cannot show that the peers stayed in sync");

    // These two are compile-time now (mgmp_tuning.h), which makes announcing
    // them MORE important rather than less: a setting that cannot be checked by
    // opening a config file has to be visible in the log, or the only way to
    // find out what a run was made with is to find the binary that made it.
    if (config().hook[T_SaveScumPenalty])
        log_raw("[!] savescum penalty is DISABLED (tune::kHookSaveScum)"
                " -- this run does NOT match shipped behaviour");
    if (config().hook[T_TimeDelayTick])
        log_raw("[!] TimeDelayStatusApplication is TURN-DRIVEN (tune::kHookTimeDelay,"
                " %u turn(s)) -- this run does NOT match shipped behaviour",
                tune::kTimeDelayTurns);

    // Before the hooks, so a fault inside hooks_install itself is caught too.
    crash_install((uintptr_t)GetModuleHandleW(nullptr));

    int n = hooks_install();
    if (n <= 0) {
        log_raw("[!] no hooks installed (%d)", n);
        return 1;
    }
    log_raw("=== %d hooks live ===", n);

    // Tell the loader it can unpark the game. Without this the loader restores
    // the two bytes it patched into FrameBegin while MinHook may be writing its
    // own jump over the same bytes -- and FrameBegin is itself a hook target.
    wchar_t ev_name[64];
    swprintf_s(ev_name, L"Local\\mgmp_hooks_ready_%lu", GetCurrentProcessId());
    if (HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, ev_name)) {
        SetEvent(ev);
        // Deliberately leaked: the loader may not have waited on it yet.
    }
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(module);
        compute_dll_dir(module);
        breadcrumb("DllMain: PROCESS_ATTACH");
        // Do the real work off the loader lock. When injected into a suspended
        // process this still runs before the game's entry point gets going.
        CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
        breadcrumb("DllMain: init thread created");
        break;
    case DLL_PROCESS_DETACH:
        // FIRST, and before log_shutdown: this is the one thing here that
        // writes to the player's save directory, so it needs the log alive to
        // say what it did. It is also the reason the ordering below is not
        // arbitrary -- everything after this line can only read files or close
        // handles.
        //
        // It runs after the game's own saves were written, which is guaranteed
        // by being on this path at all: the game writes before ExitProcess, and
        // DLL detach happens during it. A killed process never gets here.
        mgmp::debug_shutdown();
        mgmp::hooks_uninstall();
        // After the hooks are gone, so nothing can append to a closed stream.
        mgmp::session_shutdown();
        mgmp::replay_shutdown();
        mgmp::record_shutdown();
        mgmp::log_shutdown();
        mgmp::prevrun_shutdown();      // a clean exit: this run's "ended badly" marker goes away
        break;
    }
    return TRUE;
}
