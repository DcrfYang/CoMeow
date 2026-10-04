// test_config.cpp -- covers the mgmp.json reader.
//
// This one is here because the config parser is the only code in the mod that
// reads a file a HUMAN wrote, which makes it the only code whose input is not
// produced by something else we control. Every other parser in the project
// reads bytes another copy of the mod sent.
//
// The cases that matter are the ones where a setting is IGNORED: a value of the
// wrong type, a key that is out of range, a whole file that will not parse.
// Each of those has to keep the default and say so, because a config value that
// goes missing quietly is the exact failure the old ini's truthy() note was
// written about -- `record = 2` once parsed as false and produced a run with no
// capture and no complaint. Build and run:
//     cl /nologo /EHsc /std:c++17 /I..\src\core /I..\third_party test_config.cpp ..\src\core\mgmp_config.cpp && test_config.exe
#include "mgmp_config.h"
#include "mgmp_tuning.h"
#include "mgmp_addresses.h"   // the Target enum the hook assertions name

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <string>

using namespace mgmp;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    if (!ok) { printf("  FAIL  %s\n", what); ++g_fail; }
    else       printf("  ok    %s\n", what);
}

static wchar_t g_dir[MAX_PATH];

// Writes the given text as mgmp.json in a scratch directory and loads it.
// config_load has process-wide state, so every case reloads the whole thing --
// which is also how the mod uses it.
static const Config& load(const char* text) {
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\mgmp.json", g_dir);
    if (text) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    } else {
        DeleteFileW(path);
    }
    config_load(g_dir);
    return config();
}

static bool warned() { return config().parse_warnings[0] != 0; }

int main() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    swprintf_s(g_dir, L"%smgmp_test_config", tmp);
    CreateDirectoryW(g_dir, nullptr);

    printf("-- where the trace log goes: the log folder beside the dll --\n");
    {
        const std::wstring logdir = std::wstring(g_dir) + L"\\log";
        const std::wstring want = logdir + L"\\mgmp_trace.log";
        check(std::wstring(load(R"({})").log_path) == want, "the default is <dll dir>\\log\\mgmp_trace.log");
        check(GetFileAttributesW(logdir.c_str()) != INVALID_FILE_ATTRIBUTES, "and the folder is created");
        check(std::wstring(load(R"({ "log": "mgmp_trace.log" })").log_path) == want, "a bare name (what every shipped mgmp.json says) goes into the log folder");
        check(std::wstring(load(R"({ "log": "other.log" })").log_path) == logdir + L"\\other.log", "any bare name does");
        check(std::wstring(load(R"({ "log": "sub/x.log" })").log_path) == std::wstring(g_dir) + L"\\sub/x.log", "a path with a folder is relative to the dll dir, as before");
        check(std::wstring(load(R"({ "log": "C:\\logs\\x.log" })").log_path) == L"C:\\logs\\x.log", "an absolute path is used as it is");
        check(load(R"({ "log": "" })").log_path[0] == 0, "an empty value still means no log file");
    }

    printf("-- a missing file is a working configuration, but it says so --\n");
    {
        const Config& c = load(nullptr);
        check(_stricmp(c.net_role, "off") == 0, "role defaults to off");
        check(c.net_port == 27600,              "port defaults to 27600");
        check(c.net_control_auto,               "control defaults to auto");
        check(c.ui,                             "the panel defaults on");
        check(warned(),                         "and the absence is reported");
    }

    printf("-- the ordinary case --\n");
    {
        const Config& c = load(R"({
            "net":   { "role": "client", "addr": "10.0.0.4", "port": 27611 },
            "ui":    { "enabled": false, "key": "F4", "dev_tools": true },
            "debug": { "join_barrier": false, "follow_delay_ms": 2500 }
        })");
        check(_stricmp(c.net_role, "client") == 0, "role read");
        check(_stricmp(c.net_addr, "10.0.0.4") == 0, "addr read");
        check(c.net_port == 27611,             "port read");
        check(!c.ui,                           "panel turned off");
        check(c.ui_key == 0x73,                "F4 resolves to VK_F4 (0x73)");
        check(!c.net_join_barrier,             "join barrier turned off");
        check(c.net_follow_delay_ms == 2500,   "follow delay read");
        check(c.net_follow,                    "an unmentioned key keeps its default");
        check(std::strstr(c.parse_warnings, "signal.legacy_role") != nullptr,
              "a startup role without legacy opt-in reports the lobby migration");
    }

    printf("-- net.control has exactly two legal shapes --\n");
    {
        const Config& c = load(R"({ "net": { "control": [0, 2] } })");
        check(!c.net_control_auto,      "an array is an explicit split");
        check(c.net_control_count == 2, "both indices kept");
        check(c.net_control[0] == 0 && c.net_control[1] == 2, "in order");
    }
    {
        // The empty array is the observer case and must NOT collapse to auto:
        // "this peer decides for nothing" is a thing you can ask for.
        const Config& c = load(R"({ "net": { "control": [] } })");
        check(!c.net_control_auto,      "an empty array is not auto");
        check(c.net_control_count == 0, "and controls nothing");
    }
    {
        // This is what net_test.ps1 used to emit for -HostCats 0, because
        // PowerShell enumerates the output of an if-expression and a
        // single-element array came out as a bare number. The parser must not
        // guess: falling back to auto silently would mean the requested split
        // was never applied and nothing said so.
        const Config& c = load(R"({ "net": { "control": 0 } })");
        check(c.net_control_auto, "a bare number is refused, not guessed at");
        check(warned(),           "and the refusal is reported");
    }

    printf("-- a value of the wrong type keeps the default AND warns --\n");
    {
        const Config& c = load(R"({ "debug": { "join_barrier": "yes" } })");
        check(c.net_join_barrier, "the string 'yes' is not a boolean");
        check(warned(),           "reported");
    }
    {
        const Config& c = load(R"({ "net": { "port": 70000 } })");
        check(c.net_port == 27600, "an out-of-range port is refused");
        check(warned(),            "reported");
    }
    {
        const Config& c = load(R"({ "ui": { "key": "Escape" } })");
        check(c.ui_key == 0x70, "an unknown key name keeps F1");
        check(warned(),         "reported");
    }
    {
        const Config& c = load(R"({ "net": { "role": "server" } })");
        // Kept verbatim: session_start only acts on host/client, so an unknown
        // role is dormant. The point is that it is not SILENTLY dormant.
        check(!c.net_control_auto == false, "control untouched");
        check(warned(),                     "an unknown role is reported");
    }

    printf("-- a file that will not parse loses everything, loudly --\n");
    {
        const Config& c = load("{ \"net\": { \"role\": \"host\", } oops");
        check(_stricmp(c.net_role, "off") == 0,
              "nothing from a malformed file is applied");
        check(warned(), "and it is reported rather than half-applied");
    }

    printf("-- booleans accept 0/1, because people type them --\n");
    {
        const Config& c = load(R"({ "ui": { "dev_tools": true }, "debug": { "desync_halt": 0, "record": 1 } })");
        check(!c.net_desync_halt, "0 is false");
        check(c.record,           "1 is true");
    }

    printf("-- the capture is named after the log, so one name sets both --\n");
    {
        const Config& c = load(R"({ "log": "runF.log", "ui": { "dev_tools": true }, "debug": { "record": true } })");
        check(wcsstr(c.log_path,    L"runF.log") != nullptr, "log path honoured");
        check(wcsstr(c.record_path, L"runF.mgr") != nullptr,
              "and the capture takes the same stem");
    }

    printf("-- hooks the file cannot name are still implied correctly --\n");
    {
        const Config& c = load(R"({ "net": { "role": "host" }, "ui": { "enabled": false } })");
        check(c.hook[T_FrameBegin],
              "a configured role forces the frame hook that pumps the socket");
        check(c.hook[T_SFStoreBlob] == tune::kInvSync && c.hook[T_SFLoadBlob] == tune::kInvSync,
              "the blob accessors follow the inventory-sync feature flag");
        check(c.hook[T_EventChoice] && c.hook[T_EventUpdate],
              "and the choice screens");
        check(c.hook[T_SelectAct] == tune::kLocalSetup,
              "local setup installs the chapter commit barrier");
        check(c.hook[T_ChapterLower] == tune::kLocalSetup &&
              c.hook[T_ChapterRaise] == tune::kLocalSetup,
              "local setup also installs both difficulty callback guards");
        check(c.hook[T_SaveScumPenalty] == tune::kHookSaveScum,
              "while the game-modifying hooks follow mgmp_tuning.h alone");
    }
    {
        const Config& c = load(R"({ "ui": { "enabled": true } })");
        check(c.hook[T_FrameBegin],
              "the panel alone forces it too -- its connect buttons open a socket");
        check(!c.hook[T_RandInt],
              "but the RNG hooks stay off without debug.record");
    }
    {
        const Config& c = load(R"({ "ui": { "dev_tools": true }, "debug": { "record": true } })");
        check(c.hook[T_RandInt] && c.hook[T_RandFloat] &&
              c.hook[T_Rand2]   && c.hook[T_RollChance],
              "debug.record implies all four RNG hooks");
    }

    printf("-- release safety: developer switches are ignored without ui.dev_tools --\n");
    {
        const Config& c = load(R"({ "debug": { "debug_mode": true, "roster_shrink": 2, "test_weaken": true, "record": true,
                                    "join_barrier": false, "desync_halt": false, "follow": false, "follow_delay_ms": 900 },
                                    "ui": { "test_harness": true } })");
        check(!c.dev_tools && !c.debug_mode && c.roster_shrink == 0 && !c.test_weaken && !c.record && !c.ui_test,
              "debug mode, roster_shrink, test_weaken, record and the UI harness are off");
        check(c.net_join_barrier && c.net_desync_halt && c.net_follow && c.net_follow_delay_ms == 0,
              "the switches that make the peers play differently are forced to their safe values");
        check(warned(), "and the file is told it was ignored");
    }
    {
        const Config& c = load(R"({ "ui": { "dev_tools": true }, "debug": { "test_weaken": true, "roster_shrink": 2 } })");
        check(c.dev_tools && c.test_weaken && c.roster_shrink == 2, "with ui.dev_tools they are honoured");
    }
    {
        const Config& c = load(R"({ "debug": { "allow_unpinned_build": true } })");
        check(c.allow_unpinned && !c.dev_tools, "allow_unpinned_build is its own switch");
        check(!load("{}").allow_unpinned, "and is off by default");
        check(!config_set_debug_mode(true), "the loader's --debug cannot switch debug mode on in a release");
    }

    printf("-- ui.beta_notice: read, and rewritten in place without touching anything else --\n");
    {
        check(load("{}").beta_notice, "the beta notice defaults on");
        check(!load(R"({ "ui": { "beta_notice": false } })").beta_notice, "and is read from ui.beta_notice");

        const std::string a = config_rewrite_beta_notice(R"({ "ui": { "beta_notice": true, "key": "F4" } })", false);
        check(a == R"({ "ui": { "beta_notice": false, "key": "F4" } })", "an existing value is replaced in place");
        const std::string b = config_rewrite_beta_notice("{\n  // note: \"beta_notice\": true\n  \"ui\": { \"key\": \"F4\" }\n}", false);
        check(b.find("// note: \"beta_notice\": true") != std::string::npos && b.find("\"beta_notice\": false,") != std::string::npos,
              "a commented-out mention is not mistaken for the key; the key is added to the ui block");
        const std::string c = config_rewrite_beta_notice(R"({ "ui": {} })", false);
        check(c.find(',') == std::string::npos, "an empty ui block gets no trailing comma");
        const std::string d = config_rewrite_beta_notice(R"({ "net": { "port": 1 } })", false);
        check(d.find(R"("ui": { "beta_notice": false })") != std::string::npos, "a missing ui block is added");
        check(config_rewrite_beta_notice("[1,2]", false).empty() && config_rewrite_beta_notice("", false).empty(),
              "text that is not an object is refused");

        check(!load("{}").block_new_cats, "ui.block_new_cats defaults off");
        check(load(R"({ "ui": { "block_new_cats": true } })").block_new_cats, "and is read from ui.block_new_cats");
        const std::string e = config_rewrite_ui_bool(R"({ "ui": { "beta_notice": false } })", "block_new_cats", true);
        check(e.find("\"block_new_cats\": true") != std::string::npos && e.find("\"beta_notice\": false") != std::string::npos,
              "block_new_cats is added to the ui block beside the other keys");
        const std::string f = config_rewrite_ui_bool(e, "block_new_cats", false);
        check(f.find("\"block_new_cats\": false") != std::string::npos && f.find("true") == std::string::npos, "and replaced in place afterwards");

        // end to end through the file; the loader reads the result back
        load("{\n  // keep me\n  \"net\": { \"port\": 27611 },\n  \"ui\": { \"key\": \"F4\" }\n}\n");
        check(config_set_beta_notice(false), "config_set_beta_notice writes the file");
        config_load(g_dir);
        check(!config().beta_notice && config().net_port == 27611 && config().ui_key == 0x73,
              "the reload sees the new value and keeps the rest");
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\mgmp.json", g_dir);
        {
            std::ifstream in(path, std::ios::binary);   // closed before the file is deleted below
            const std::string now((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            check(now.find("// keep me") != std::string::npos, "and the user's comment survived");
        }

        // no file at all: one is created holding just this setting
        DeleteFileW(path);
        config_load(g_dir);
        check(config_set_beta_notice(false), "a missing file is created");
        config_load(g_dir);
        check(!config().beta_notice, "and reads back");
        DeleteFileW(path);
    }

    printf(g_fail ? "\n%d FAILED\n" : "\nall good\n", g_fail);
    return g_fail ? 1 : 0;
}
