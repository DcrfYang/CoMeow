// mgmp_roomhist.cpp -- see mgmp_roomhist.h.
#include "mgmp_roomhist.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "mgmp_config.h"
#include "mgmp_log.h"
#include "mgmp_signal.h"

namespace mgmp {
namespace {

constexpr int64_t  kKeepSeconds = 24 * 3600;
constexpr size_t   kKeepLines   = 60;

struct Entry { int64_t at = 0; std::string host, id; };

std::vector<Entry> g_list;
bool        g_loaded = false;
char        g_pending_id[16] = {}, g_pending_host[32] = {};
char        g_noted_id[16] = {};

// Beside mgmp.dll (the mod's own folder, from the config) and nowhere else: an unknown folder means no history, never a file in the game's folder. (The first version looked for the last backslash with
// L"\/" -- an escape that is only a slash -- found none, and so wrote a RELATIVE name, which landed in the game's folder, where the launcher reported it as another mod.)
std::wstring file_path() {
    const wchar_t* dir = config_dll_dir();
    if (!dir || !dir[0]) return std::wstring();
    std::wstring p(dir);
    p.push_back(static_cast<wchar_t>(92));         // a backslash, spelled so no shell or editor can eat it
    p += L"mgmp-rooms.txt";
    return p;
}

std::string clean(const char* s) {
    std::string out;
    for (const char* c = s ? s : ""; *c; ++c) if (*c != '\t' && *c != '\r' && *c != '\n') out.push_back(*c);
    return out;
}

void prune(int64_t now) {
    std::vector<Entry> keep;
    for (const Entry& e : g_list) if (now - e.at < kKeepSeconds && e.at <= now + 3600) keep.push_back(e);
    g_list.swap(keep);
    if (g_list.size() > kKeepLines) g_list.erase(g_list.begin(), g_list.end() - kKeepLines);
}

void load() {
    if (g_loaded) return;
    g_loaded = true;
    const std::wstring path = file_path();
    if (path.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* t1 = strchr(line, '\t');
        if (!t1) continue;
        char* t2 = strchr(t1 + 1, '\t');
        if (!t2) continue;
        *t1 = 0; *t2 = 0;
        char* nl = strpbrk(t2 + 1, "\r\n"); if (nl) *nl = 0;
        Entry e; e.at = _strtoi64(line, nullptr, 10); e.host = t1 + 1; e.id = t2 + 1;
        if (e.at > 0) g_list.push_back(e);
    }
    fclose(f);
    prune((int64_t)time(nullptr));
}

void save() {
    const std::wstring path = file_path();
    if (path.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return;
    for (const Entry& e : g_list) fprintf(f, "%lld\t%s\t%s\n", (long long)e.at, e.host.c_str(), e.id.c_str());
    fclose(f);
}

} // namespace

void roomhist_set_pending(const char* room_id, const char* host) {
    strncpy_s(g_pending_id, sizeof(g_pending_id), room_id ? room_id : "", _TRUNCATE);
    strncpy_s(g_pending_host, sizeof(g_pending_host), host ? host : "", _TRUNCATE);
}

void roomhist_tick() {
    if (!g_pending_id[0]) return;
    const char* in = signal_room();
    if (!in || !in[0] || strcmp(in, g_pending_id) != 0 || strcmp(g_noted_id, g_pending_id) == 0) return;
    load();
    const int64_t now = (int64_t)time(nullptr);
    Entry e; e.at = now; e.host = clean(g_pending_host); e.id = clean(g_pending_id);
    g_list.push_back(e);
    prune(now);
    save();
    strncpy_s(g_noted_id, sizeof(g_noted_id), g_pending_id, _TRUNCATE);
    log_line("MENU", "room %s (host '%s') joined: remembered for a day, the list puts it and its host's other rooms first", g_pending_id, g_pending_host);
}

bool roomhist_recent(const char* room_id, const char* host) {
    load();
    const int64_t now = (int64_t)time(nullptr);
    for (const Entry& e : g_list) {
        if (now - e.at >= kKeepSeconds) continue;
        if (room_id && room_id[0] && e.id == room_id) return true;
        if (host && host[0] && e.host == host) return true;
    }
    return false;
}

} // namespace mgmp
