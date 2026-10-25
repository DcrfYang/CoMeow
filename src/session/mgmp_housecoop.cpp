// mgmp_housecoop.cpp -- see mgmp_housecoop.h.
#include "mgmp_housecoop.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "mgmp_addresses.h"
#include "mgmp_catsync.h"
#include "mgmp_i18n.h"
#include "mgmp_lockstep.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_page.h"
#include "mgmp_proto.h"
#include "mgmp_resolve.h"
#include "mgmp_rng.h"
#include "mgmp_room.h"
#include "mgmp_savefile.h"
#include "mgmp_saveslots.h"
#include "mgmp_session.h"
#include "mgmp_setup.h"
#include "mgmp_unlocks.h"

namespace mgmp {
namespace {

constexpr ULONGLONG kStatusEveryMs = 1000;
constexpr ULONGLONG kStatusFreshMs = 3500;
constexpr ULONGLONG kAutoSaveAfterMs = 4000;   // after the fight: the auto save once the warehouse is up and the game's own writes are done

// MewDirector+0x5A8 is GlobalProgressionData (the House's progression, mgmp_room.cpp); its house boss fields as the house serializer
// (0x1401D2550) stores them: next_house_boss (std::string), house_boss_countdown, house_boss_times_fought, house_boss_difficulty.
constexpr uintptr_t kDir_House      = 0x5A8;
constexpr uintptr_t kDir_Day        = 0x580;   // current_day (u64), +1 in the overnight
constexpr uintptr_t kGpd_Boss       = 0x458;
constexpr uintptr_t kGpd_Countdown  = 0x478;
constexpr uintptr_t kGpd_Times      = 0x47C;
constexpr uintptr_t kGpd_Difficulty = 0x480;

struct Boss {
    bool    ok = false;
    char    name[kHcBossLen] = {};
    int32_t countdown = -1, times = 0, diff = 0;
    bool today() const { return ok && name[0] && countdown == 0; }
};

struct PeerStatus { bool known = false; ULONGLONG at = 0; uint8_t flags = 0; char boss[kHcBossLen] = {}; };

enum class Role : uint8_t { None, Px, Helper };
enum class Phase : uint8_t { Idle, Asking, Voted, Accepted, Inserted, Fighting };

const char* phase_name(Phase p) {
    switch (p) {
        case Phase::Idle: return "idle"; case Phase::Asking: return "asking"; case Phase::Voted: return "voted";
        case Phase::Accepted: return "accepted"; case Phase::Inserted: return "inserted"; case Phase::Fighting: return "fighting";
    }
    return "?";
}

// The fight start's closure data as the game reads it (T_HouseFight): the director, then the boss name as an MSVC std::string.
struct GameStr { union { char buf[16]; const char* ptr; }; uint64_t size; uint64_t cap; };
struct FightData { void* director; GameStr boss; };

struct State {
    PeerStatus peer[kMaxPeers];
    ULONGLONG  status_at = 0;

    Role     role = Role::None;
    Phase    phase = Phase::Idle;
    uint32_t round = 0;
    uint8_t  px = kNoPeer;
    char     boss[kHcBossLen] = {};
    int32_t  times = 0, diff = 0;
    uint8_t  members[kMaxPeers] = {};       // the peers when the question was asked
    uint8_t  member_n = 0;
    bool     vote_in[kMaxPeers] = {}, vote_yes[kMaxPeers] = {};

    // PX: the boss day it chose to fight alone (not asked again)
    uint64_t declined_day = ~0ull;
    char     declined_boss[kHcBossLen] = {};

    // a helper: the inserted day, and its own values while PX's boss is in its House
    bool     inserting = false;
    bool     snap = false;
    Boss     orig;

    // the held fight start
    void   (*fight_orig)(void*) = nullptr;
    void*    fight_dir = nullptr;
    char     fight_boss[64] = {};
    char     fight_heap[64] = {};

    ULONGLONG autosave_at = 0;
    bool      house_seen = false;           // the recovery check runs once per arrival in the warehouse
} g;

bool active_room() { return session_exp_house_coop() && net_active() && net_peer_count() >= 2; }
bool in_house() { return page_self() == PageState::House; }

uint8_t* director() {
    const uintptr_t at = addr_of_data(D_MewDirectorPtr);
    uint8_t* dir = nullptr;
    if (!at || !mem_read((const void*)at, &dir, sizeof(dir))) return nullptr;
    return dir;
}
uint8_t* gpd() {
    uint8_t* dir = director();
    uint8_t* p = nullptr;
    if (!dir || !mem_read(dir + kDir_House, &p, sizeof(p))) return nullptr;
    return p;
}
uint64_t current_day() {
    uint8_t* dir = director();
    uint64_t d = ~0ull;
    if (dir) mem_read(dir + kDir_Day, &d, sizeof(d));
    return d;
}

bool read_boss(Boss& b) {
    b = Boss{};
    uint8_t* p = gpd();
    if (!p) return false;
    uint64_t size = 0;
    if (!mem_read(p + kGpd_Boss + 0x10, &size, sizeof(size)) || size >= kHcBossLen) return false;
    if (size && !mem_read_std_string(p + kGpd_Boss, b.name, sizeof(b.name))) return false;
    if (!mem_read(p + kGpd_Countdown, &b.countdown, 4) || !mem_read(p + kGpd_Times, &b.times, 4) || !mem_read(p + kGpd_Difficulty, &b.diff, 4)) return false;
    b.ok = true;
    return true;
}

typedef void* (__fastcall* fn_str_assign)(void* s, const char* p, size_t n);
bool call_assign(fn_str_assign f, void* s, const char* p, size_t n) {
    __try { f(s, p, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool write_boss(const char* name, int32_t countdown, int32_t times, int32_t diff) {
    uint8_t* p = gpd();
    const auto assign = (fn_str_assign)addr_of_call(C_StrAssign);
    if (!p || !assign || !name) return false;
    if (!call_assign(assign, p + kGpd_Boss, name, strlen(name))) return false;
    return mem_write(p + kGpd_Countdown, &countdown, 4) && mem_write(p + kGpd_Times, &times, 4) && mem_write(p + kGpd_Difficulty, &diff, 4);
}

// --- the record beside the saves: a helper's own values while PX's boss is in its House ---------------------------------------------------
bool record_path(wchar_t* out, size_t cap) {
    wchar_t dir[MAX_PATH] = {};
    if (!savefile_save_dir(dir, MAX_PATH)) return false;
    return swprintf_s(out, cap, L"%s\\mgmp_housecoop_restore.txt", dir) > 0;
}
bool live_save(char* out, size_t cap) {
    wchar_t w[MAX_PATH] = {};
    if (!savefile_live_path(w, MAX_PATH)) return false;
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, nullptr, nullptr) > 0;
}
void record_write(const Boss& own) {
    wchar_t path[MAX_PATH]; char save[MAX_PATH * 3] = {};
    if (!record_path(path, MAX_PATH) || !live_save(save, sizeof(save))) { log_line_lvl(LogLevel::Warn, "HOUSECOOP", "!! the restore record could not be written (no save folder / live save known)"); return; }
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") || !f) { log_line_lvl(LogLevel::Warn, "HOUSECOOP", "!! the restore record could not be written"); return; }
    fprintf(f, "save=%s\nboss=%s\ncountdown=%d\ntimes=%d\ndiff=%d\npx_boss=%s\npx_times=%d\npx_diff=%d\n",
            save, own.name, own.countdown, own.times, own.diff, g.boss, g.times, g.diff);
    fclose(f);
}
void record_delete() {
    wchar_t path[MAX_PATH];
    if (record_path(path, MAX_PATH)) DeleteFileW(path);
}
struct Record { char save[MAX_PATH * 3] = {}; Boss own; Boss px; };
bool record_read(Record& r) {
    wchar_t path[MAX_PATH];
    if (!record_path(path, MAX_PATH)) return false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") || !f) return false;
    char line[MAX_PATH * 3 + 32];
    bool have_save = false, have_px = false;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char* k = line; const char* v = eq + 1;
        if (!strcmp(k, "save")) { strncpy_s(r.save, v, _TRUNCATE); have_save = true; }
        else if (!strcmp(k, "boss")) strncpy_s(r.own.name, v, _TRUNCATE);
        else if (!strcmp(k, "countdown")) r.own.countdown = atoi(v);
        else if (!strcmp(k, "times")) r.own.times = atoi(v);
        else if (!strcmp(k, "diff")) r.own.diff = atoi(v);
        else if (!strcmp(k, "px_boss")) { strncpy_s(r.px.name, v, _TRUNCATE); have_px = v[0] != 0; }
        else if (!strcmp(k, "px_times")) r.px.times = atoi(v);
        else if (!strcmp(k, "px_diff")) r.px.diff = atoi(v);
    }
    fclose(f);
    r.own.ok = r.px.ok = true; r.px.countdown = 0;
    return have_save && have_px;
}

int row_of(uint8_t id) {
    const int r = id == net_self() ? room_self_row() : room_row_of_peer(id);
    return r < 0 ? 0 : r + 1;
}

void send(uint8_t kind, uint8_t flags = 0) {
    HouseCoopMsg m;
    m.kind = kind; m.flags = flags; m.round = g.round;
    m.times_fought = g.times; m.difficulty = g.diff;
    strncpy_s(m.boss, g.boss, _TRUNCATE);
    if (!net_send_housecoop(m)) log_line_lvl(LogLevel::Warn, "HOUSECOOP", "!! message %u (round %08x) could not be sent", (unsigned)kind, g.round);
}

void send_status() {
    Boss b;
    const bool have = read_boss(b);
    const bool busy = g.phase >= Phase::Accepted || g.snap;
    HouseCoopMsg m;
    m.kind = kHcStatus;
    m.flags = (uint8_t)((in_house() ? kHcInHouse : 0) | (have && b.today() && !busy ? kHcToday : 0) | (busy ? kHcBusy : 0));
    if (have && b.today() && !busy) { strncpy_s(m.boss, b.name, _TRUNCATE); m.times_fought = b.times; m.difficulty = b.diff; }
    net_send_housecoop(m);
    g.status_at = GetTickCount64();
}

bool fresh(uint8_t id) {
    return id < kMaxPeers && g.peer[id].known && GetTickCount64() - g.peer[id].at < kStatusFreshMs;
}

void reset(const char* why) {
    if (g.phase != Phase::Idle || g.role != Role::None)
        log_line("HOUSECOOP", "co-op round %08x ends (%s; was %s, %s)", g.round, why, g.role == Role::Px ? "the helped player" : g.role == Role::Helper ? "a helper" : "-", phase_name(g.phase));
    g.role = Role::None; g.phase = Phase::Idle; g.round = 0; g.px = kNoPeer;
    g.member_n = 0; g.inserting = false;
    memset(g.vote_in, 0, sizeof(g.vote_in)); memset(g.vote_yes, 0, sizeof(g.vote_yes));
}

void decline_today() {
    Boss b;
    if (read_boss(b)) { g.declined_day = current_day(); strncpy_s(g.declined_boss, b.name, _TRUNCATE); }
}
bool declined(const Boss& b) {
    return g.declined_day == current_day() && strcmp(g.declined_boss, b.name) == 0;
}

void autosave(const char* why) {
    const bool game = catsync_save_game(why);
    const bool backup = saveslots_auto_push();
    log_line("HOUSECOOP", "auto save (%s): the game's save %s, the backup page's auto save %s", why, game ? "written" : "NOT written", backup ? "taken" : "NOT taken");
}

// A helper's own house boss values back in its House (after the fight, or a co-op that ended before it).
void restore(const char* why) {
    if (!g.snap) return;
    const bool ok = write_boss(g.orig.name, g.orig.countdown, g.orig.times, g.orig.diff);
    log_line_lvl(ok ? LogLevel::Info : LogLevel::Error, "HOUSECOOP", "%s: this House's own house boss is put back -- '%s', countdown %d, fought %d, difficulty %d (%s)",
                 why, g.orig.name, g.orig.countdown, g.orig.times, g.orig.diff, ok ? "ok" : "FAILED -- the backup page's auto save from the co-op's start has them");
    if (!ok) return;
    g.snap = false;
    catsync_save_game(why);
    record_delete();
}

// A game that did not come back: the record says this save still carries the helped player's boss.
void recover_if_needed() {
    Record r;
    if (g.snap || !record_read(r)) return;
    char save[MAX_PATH * 3] = {};
    if (!live_save(save, sizeof(save)) || _stricmp(save, r.save) != 0) return;   // another save: its record waits for it
    Boss live;
    if (!read_boss(live)) return;
    if (strcmp(live.name, r.px.name) != 0 || live.countdown != 0 || live.times != r.px.times || live.diff != r.px.diff) {
        log_line("HOUSECOOP", "a restore record for this save is stale (the House has '%s' countdown %d, not the helped player's '%s') -- dropped", live.name, live.countdown, r.px.name);
        record_delete();
        return;
    }
    g.orig = r.own; g.snap = true;
    restore("a co-op house boss of an earlier game did not finish");
    if (!g.snap) room_push_notice(tr(Tx::HC_RECOVERED));
}

void maybe_ask() {
    Boss b;
    if (!read_boss(b) || !b.today() || !in_house() || g.snap || declined(b)) return;
    uint8_t ids[kMaxPeers] = {};
    if (!net_peer_ids(ids, kMaxPeers)) return;
    const uint8_t n = net_peer_count() > kMaxPeers ? kMaxPeers : net_peer_count();
    for (uint8_t i = 0; i < n; ++i) {
        const uint8_t id = ids[i];
        if (id == net_self()) continue;
        if (!fresh(id)) return;
        const uint8_t f = g.peer[id].flags;
        if (!(f & kHcInHouse) || (f & kHcToday) || (f & kHcBusy)) return;
    }
    g.round = ((uint32_t)GetTickCount64() * 2654435761u) ^ ((uint32_t)net_self() << 28);
    if (!g.round) g.round = 1;
    g.role = Role::Px; g.phase = Phase::Asking; g.px = net_self();
    strncpy_s(g.boss, b.name, _TRUNCATE); g.times = b.times; g.diff = b.diff;
    memcpy(g.members, ids, sizeof(ids)); g.member_n = n;
    memset(g.vote_in, 0, sizeof(g.vote_in)); memset(g.vote_yes, 0, sizeof(g.vote_yes));
    send(kHcAsk);
    log_line("HOUSECOOP", "this save has the house boss '%s' today (fought %d, difficulty %d) and nobody else has -- the other %u player(s) are asked to help (round %08x)",
             g.boss, g.times, g.diff, (unsigned)(n - 1), g.round);
}

bool members_present() {
    uint8_t ids[kMaxPeers] = {};
    if (!net_peer_ids(ids, kMaxPeers)) return false;
    const uint8_t n = net_peer_count() > kMaxPeers ? kMaxPeers : net_peer_count();
    for (uint8_t j = 0; j < g.member_n; ++j) {
        bool here = false;
        for (uint8_t i = 0; i < n; ++i) if (ids[i] == g.members[j]) here = true;
        if (!here) return false;
    }
    return true;
}

void go() {
    g.phase = Phase::Accepted;
    send(kHcGo);
    log_line("HOUSECOOP", "every player helps: the co-op house boss '%s' is on (round %08x)", g.boss, g.round);
    autosave("the house boss co-op starts");
    room_push_notice(tr(Tx::HC_GO_PX));
}

// THE FIGHT, on every peer at once (setup_tick runs it once the host's command is in): the battle id from the committed stream, the client's
// unlock window, then the game's own start with the closure data it would have had.
void house_go(void*) {
    uint64_t id = 0;
    if (const uint64_t* s = rng_global_stream()) {
        uint64_t x = s[0] ^ (s[1] * 0x9E3779B97F4A7C15ull) ^ 0x484F555345424F53ull;   // 'HOUSEBOS'
        x ^= x >> 31; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 29;
        id = x;
    }
    if (!id) id = 0x484F555345424F53ull;
    lockstep_enter_battle(id);
    if (net_role() == NetRole::Client) unlocks_window_open("the house boss co-op fight", false);
    g.phase = Phase::Fighting;
    static FightData data;
    data = FightData{};
    data.director = g.fight_dir;
    const size_t len = strlen(g.fight_boss);
    if (len < 16) { memcpy(data.boss.buf, g.fight_boss, len + 1); data.boss.cap = 15; }
    else { memcpy(g.fight_heap, g.fight_boss, len + 1); data.boss.ptr = g.fight_heap; data.boss.cap = len; }
    data.boss.size = len;
    log_line("HOUSECOOP", "the co-op house boss fight '%s' starts on this screen (battle id %016llx)", g.fight_boss, (unsigned long long)id);
    if (g.fight_orig) g.fight_orig(&data);
}

} // namespace

// --------------------------------------------------------------------------------------------------------------------------------------------

void housecoop_tick() {
    const ULONGLONG t = GetTickCount64();
    if (in_house()) {
        if (!g.house_seen) { g.house_seen = true; housecoop_log_boss("arrived in the warehouse"); if (g.phase == Phase::Idle) recover_if_needed(); }
    } else {
        g.house_seen = false;
    }
    if (g.autosave_at && t >= g.autosave_at && in_house()) { g.autosave_at = 0; autosave("back home from the co-op house boss"); }

    if (!active_room()) {
        if (g.phase != Phase::Idle && g.phase != Phase::Fighting) {
            reset("the room's session or the feature is gone");
        }
        if (g.snap && g.phase == Phase::Idle && in_house()) { restore("the co-op ended before its fight"); if (!g.snap) room_push_notice(tr(Tx::HC_CANCELLED)); }
        return;
    }
    if (t - g.status_at >= kStatusEveryMs) send_status();

    if ((g.phase == Phase::Asking || g.phase == Phase::Voted || g.phase == Phase::Accepted || g.phase == Phase::Inserted) && !members_present()) {
        const bool was_px = g.role == Role::Px;
        send(kHcCancel);
        reset("a player of the co-op left");
        if (was_px) decline_today();
        room_push_notice(tr(Tx::HC_CANCELLED));
        return;
    }
    if (g.role == Role::None && g.phase == Phase::Idle) maybe_ask();
    if (g.snap && g.phase == Phase::Idle && in_house()) { restore("the co-op ended before its fight"); if (!g.snap) room_push_notice(tr(Tx::HC_CANCELLED)); }
}

void housecoop_on_message(uint8_t from, const HouseCoopMsg& m) {
    if (from >= kMaxPeers || from == net_self()) return;
    if (m.kind == kHcStatus) {
        PeerStatus& p = g.peer[from];
        p.known = true; p.at = GetTickCount64(); p.flags = m.flags;
        strncpy_s(p.boss, m.boss, _TRUNCATE);
        return;
    }
    if (!active_room()) return;
    char text[512];
    switch (m.kind) {
        case kHcAsk: {
            Boss b;
            const bool mine_today = read_boss(b) && b.today();
            if (g.phase != Phase::Idle || !in_house() || mine_today || g.snap) {
                log_line("HOUSECOOP", "peer %u asks for help with '%s' -- this player cannot (%s), answering no", (unsigned)from, m.boss,
                         g.phase != Phase::Idle ? "already in a round" : !in_house() ? "not in the warehouse" : mine_today ? "its own house boss is today" : "its House carries a co-op boss");
                HouseCoopMsg no = m; no.kind = kHcVote; no.flags = 0;
                net_send_housecoop(no);
                return;
            }
            g.role = Role::Helper; g.phase = Phase::Asking; g.px = from; g.round = m.round;
            strncpy_s(g.boss, m.boss, _TRUNCATE); g.times = m.times_fought; g.diff = m.difficulty;
            uint8_t ids[kMaxPeers] = {};
            if (net_peer_ids(ids, kMaxPeers)) { memcpy(g.members, ids, sizeof(ids)); g.member_n = net_peer_count() > kMaxPeers ? kMaxPeers : net_peer_count(); }
            log_line("HOUSECOOP", "P%d (peer %u) asks for help with its house boss '%s' (fought %d, difficulty %d, round %08x)", row_of(from), (unsigned)from, g.boss, g.times, g.diff, g.round);
            return;
        }
        case kHcVote: {
            if (g.role != Role::Px || g.phase != Phase::Asking || m.round != g.round) return;
            g.vote_in[from] = true; g.vote_yes[from] = (m.flags & 1) != 0;
            log_line("HOUSECOOP", "P%d (peer %u) answers %s", row_of(from), (unsigned)from, g.vote_yes[from] ? "YES" : "no");
            if (!g.vote_yes[from]) {
                send(kHcCancel);
                reset("a player will not help");
                decline_today();
                room_push_notice(tr(Tx::HC_DECLINED_PX));
                return;
            }
            for (uint8_t j = 0; j < g.member_n; ++j)
                if (g.members[j] != net_self() && !(g.members[j] < kMaxPeers && g.vote_in[g.members[j]] && g.vote_yes[g.members[j]])) return;
            go();
            return;
        }
        case kHcGo: {
            if (g.role != Role::Helper || from != g.px || m.round != g.round || (g.phase != Phase::Asking && g.phase != Phase::Voted)) return;
            strncpy_s(g.boss, m.boss, _TRUNCATE); g.times = m.times_fought; g.diff = m.difficulty;
            g.phase = Phase::Accepted;
            log_line("HOUSECOOP", "the co-op house boss of P%d is on: '%s' -- this player's next end of day is the inserted boss day", row_of(from), g.boss);
            autosave("helping with a house boss");
            _snprintf_s(text, sizeof(text), _TRUNCATE, tr(Tx::HC_GO_HELPER), row_of(from));
            room_push_notice(text);
            return;
        }
        case kHcCancel: {
            if (m.round != g.round || g.phase == Phase::Idle || g.phase == Phase::Fighting) return;
            const bool was_px = g.role == Role::Px;
            const int px_row = row_of(g.px);
            reset("cancelled by another player");
            if (was_px) { decline_today(); room_push_notice(tr(Tx::HC_DECLINED_PX)); }
            else { _snprintf_s(text, sizeof(text), _TRUNCATE, tr(Tx::HC_DECLINED), px_row); room_push_notice(text); }
            return;
        }
    }
}

bool housecoop_holds_solo() {
    if (!active_room()) return false;
    if (g.phase != Phase::Idle || g.snap) return true;
    Boss b;
    if (!read_boss(b) || !b.today() || declined(b)) return false;
    uint8_t ids[kMaxPeers] = {};
    if (net_peer_ids(ids, kMaxPeers)) {
        const uint8_t n = net_peer_count() > kMaxPeers ? kMaxPeers : net_peer_count();
        for (uint8_t i = 0; i < n; ++i)
            if (ids[i] != net_self() && fresh(ids[i]) && (g.peer[ids[i]].flags & kHcToday)) return false;   // two boss days: each alone, as before
    }
    return true;
}

// --- 0.1.92 DIAGNOSTICS ONLY (a tester: "with the mod on, the next house boss never comes; with it off it is there at once"). Reads, never writes. ---
// GlobalProgressionData's rematch list (0x1401C47A0 counts it down with the boss): a vector at +0x488/+0x490 of 0x30-byte entries -- the boss's name
// (std::string at +0) and its cooldown (+0x20). A boss day that passed unfought goes there (0x1401C7300, "rematch_cooldown").
namespace {
constexpr uintptr_t kGpd_RematchFirst = 0x488, kGpd_RematchLast = 0x490, kRematchEntry = 0x30, kRematch_Cooldown = 0x20;
const char* role_name(Role r) { return r == Role::Px ? "the helped player" : r == Role::Helper ? "a helper" : "none"; }
}

void housecoop_log_boss(const char* why) {
    Boss b;
    const bool have = read_boss(b);
    char rem[300] = {}; int w = 0; unsigned n = 0;
    if (uint8_t* p = gpd()) {
        uint8_t* first = nullptr; uint8_t* last = nullptr;
        if (mem_read(p + kGpd_RematchFirst, &first, sizeof(first)) && mem_read(p + kGpd_RematchLast, &last, sizeof(last)) && first && last >= first &&
            (uintptr_t)(last - first) % kRematchEntry == 0 && (uintptr_t)(last - first) / kRematchEntry < 64) {
            for (uint8_t* e = first; e < last; e += kRematchEntry, ++n) {
                char nm[48] = {}; int32_t cd = 0;
                mem_read_std_string(e, nm, sizeof(nm)); mem_read(e + kRematch_Cooldown, &cd, 4);
                if (w >= 0 && (size_t)w < sizeof(rem) - 64) w += _snprintf_s(rem + w, sizeof(rem) - w, _TRUNCATE, "%s'%s' cooldown %d", n ? ", " : "", nm, cd);
            }
        } else _snprintf_s(rem, sizeof(rem), _TRUNCATE, "unreadable");
    }
    wchar_t rp[MAX_PATH] = {};
    const bool record = record_path(rp, MAX_PATH) && GetFileAttributesW(rp) != INVALID_FILE_ATTRIBUTES;
    log_line("HOUSEBOSS", "%s: day %lld | %s | rematches (%u): %s | co-op: feature %s, in a room %s, role %s, phase %s, carries a co-op boss %s, inserting %s, restore record %s | page %s",
             why ? why : "?", (long long)current_day(),
             have ? (b.name[0] ? "" : "no house boss scheduled") : "the House cannot be read",
             n, n ? rem : "-",
             session_exp_house_coop() ? "on" : "off", active_room() ? "yes" : "no", role_name(g.role), phase_name(g.phase), g.snap ? "YES" : "no", g.inserting ? "yes" : "no",
             record ? "ON DISK" : "none", page_name(page_self()));
    if (have && b.name[0])
        log_line("HOUSEBOSS", "   next house boss '%s': countdown %d%s, fought %d time(s), difficulty %d", b.name, b.countdown,
                 b.countdown == 0 ? " (TODAY)" : b.countdown < 0 ? " (PAST -- below zero)" : "", b.times, b.diff);
}

// Why housecoop_holds_solo() says true right now (the same tests, in the same order), for the log; "" when it does not hold.
void housecoop_hold_reason(char* out, size_t cap) {
    out[0] = 0;
    if (!active_room()) return;
    if (g.phase != Phase::Idle) { _snprintf_s(out, cap, _TRUNCATE, "a co-op round is on (%s, %s)", role_name(g.role), phase_name(g.phase)); return; }
    if (g.snap) { _snprintf_s(out, cap, _TRUNCATE, "this House carries another player's co-op boss"); return; }
    Boss b;
    if (!read_boss(b) || !b.today() || declined(b)) return;
    uint8_t ids[kMaxPeers] = {};
    char who[200] = {}; int w = 0;
    if (net_peer_ids(ids, kMaxPeers)) {
        const uint8_t n = net_peer_count() > kMaxPeers ? kMaxPeers : net_peer_count();
        for (uint8_t i = 0; i < n; ++i) {
            const uint8_t id = ids[i];
            if (id == net_self()) continue;
            if (fresh(id) && (g.peer[id].flags & kHcToday)) return;
            const char* st = !fresh(id) ? "no recent status" : !(g.peer[id].flags & kHcInHouse) ? "not in its warehouse" : (g.peer[id].flags & kHcBusy) ? "busy" : "ready";
            if (w >= 0 && (size_t)w < sizeof(who) - 40) w += _snprintf_s(who + w, sizeof(who) - w, _TRUNCATE, "%speer %u %s", w ? ", " : "", (unsigned)id, st);
        }
    }
    _snprintf_s(out, cap, _TRUNCATE, "the house boss '%s' is today and the co-op waits to ask the others (%s)%s", b.name, who[0] ? who : "nobody else",
                in_house() ? "" : "; this player is not in the warehouse, so nobody is asked yet");
}

bool housecoop_skip_countdown() {
    if (g.role != Role::Helper || g.phase != Phase::Accepted || !active_room()) return false;
    g.inserting = true;
    log_line("HOUSECOOP", "the inserted day begins: this House's house boss countdown is left as it is");
    return true;
}

bool housecoop_overnight(void* out_vec) {
    if (!g.inserting || !out_vec) return false;
    const uint64_t zero[3] = {};
    mem_write(out_vec, zero, sizeof(zero));   // the overnight's result list: empty (it is zeroed first by the game too)
    Boss own;
    if (!read_boss(own)) {
        log_line_lvl(LogLevel::Error, "HOUSECOOP", "!! the inserted day: this House's house boss could not be read -- the helped player's boss is NOT put in");
        return true;
    }
    g.orig = own;
    record_write(own);
    if (!write_boss(g.boss, 0, g.times, g.diff)) {
        log_line_lvl(LogLevel::Error, "HOUSECOOP", "!! the inserted day: the helped player's boss could not be put in -- putting this House's own back");
        write_boss(own.name, own.countdown, own.times, own.diff);
        record_delete();
        return true;
    }
    g.snap = true;
    g.phase = Phase::Inserted;
    log_line("HOUSECOOP", "the inserted day: no overnight (day %llu stays), and the House has the house boss '%s' today (fought %d, difficulty %d); its own was '%s' countdown %d fought %d difficulty %d",
             (unsigned long long)current_day(), g.boss, g.times, g.diff, own.name, own.countdown, own.times, own.diff);
    return true;
}

bool housecoop_skip_strays() {
    if (!g.inserting) return false;
    g.inserting = false;
    log_line("HOUSECOOP", "the inserted day: no new strays");
    room_push_notice(tr(Tx::HC_INSERTED));
    return true;
}

bool housecoop_on_fight_start(void* data, void (*original)(void*)) {
    if (!active_room() || !data) return false;
    const bool mine = (g.role == Role::Px && g.phase == Phase::Accepted) || (g.role == Role::Helper && g.phase == Phase::Inserted);
    if (!mine) return false;
    if (setup_house_pending()) { log_line("HOUSECOOP", "the gear screen's done again while the fight is held -- ignored"); return true; }
    void* dir = nullptr;
    char name[64] = {};
    if (!mem_read(data, &dir, sizeof(dir)) || !dir || !mem_read_std_string((const uint8_t*)data + 8, name, sizeof(name)) || !name[0]) {
        log_line_lvl(LogLevel::Error, "HOUSECOOP", "!! the house boss fight's closure could not be read -- the game starts it on its own");
        return false;
    }
    if (strcmp(name, g.boss) != 0)
        log_line_lvl(LogLevel::Warn, "HOUSECOOP", "!! the fight starting is '%s', the co-op's boss is '%s'", name, g.boss);
    g.fight_orig = original; g.fight_dir = dir;
    strncpy_s(g.fight_boss, name, _TRUNCATE);
    if (!setup_on_house_gear_done(dir, house_go)) {
        log_line_lvl(LogLevel::Error, "HOUSECOOP", "!! the setup barrier did not take the house boss fight (no session setup, or a run already committed) -- the game starts it on its own");
        return false;
    }
    log_line("HOUSECOOP", "the house boss gear screen is done -- the fight waits for every player");
    return true;
}

bool housecoop_skip_beat_boss() {
    if (g.role == Role::Helper || g.snap) {
        log_line("HOUSECOOP", "BeatHouseBoss skipped: this player helped, the story is the helped player's");
        log_line("HOUSEBOSS", "BeatHouseBoss SKIPPED by the co-op (role %s, phase %s, carries a co-op boss %s, in a room %s) -- this save's next house boss is not scheduled by it",
                 role_name(g.role), phase_name(g.phase), g.snap ? "yes" : "no", active_room() ? "yes" : "no");
        return true;
    }
    return false;
}

bool housecoop_refuse_depart(uint32_t cats) {
    if (!active_room() || !cats) return false;   // an empty box is not a departure
    if (g.role == Role::Helper && g.phase == Phase::Accepted) { room_toast(tr(Tx::HC_NEXT_DAY_FIRST)); return true; }
    if ((g.role == Role::Px && g.phase == Phase::Accepted) || (g.role == Role::Helper && g.phase == Phase::Inserted)) return false;
    if (g.phase == Phase::Asking || g.phase == Phase::Voted || housecoop_holds_solo()) { room_toast(tr(Tx::HC_WAIT_DEPART)); return true; }
    return false;
}

void housecoop_settlement_begin() {
    if (g.phase != Phase::Fighting) return;
    unlocks_window_close("the house boss co-op fight is over");
    log_line("HOUSECOOP", "the co-op house boss fight is settled (%s)", g.role == Role::Px ? "the helped player: the story is settled here" : "a helper: no story");
}

void housecoop_after_settlement() {
    if (g.phase != Phase::Fighting && !g.snap) return;
    const bool helper = g.role == Role::Helper || g.snap;
    if (g.snap) restore("after the co-op house boss fight");
    reset("the fight is over");
    g.autosave_at = GetTickCount64() + kAutoSaveAfterMs;
    room_push_notice(tr(helper ? Tx::HC_RESTORED : Tx::HC_DONE_PX));
}

bool housecoop_view(HouseCoopView& out) {
    out = HouseCoopView{};
    if (!active_room()) return false;
    strncpy_s(out.boss, g.boss, _TRUNCATE);
    out.px_row = row_of(g.px == kNoPeer ? net_self() : g.px);
    if (setup_house_pending()) out.panel = HcPanel::GearWait;
    else if (g.role == Role::Helper && g.phase == Phase::Asking) out.panel = HcPanel::Ask;
    else if (g.role == Role::Helper && g.phase == Phase::Voted) out.panel = HcPanel::Voted;
    else if (g.role == Role::Px && g.phase == Phase::Asking) out.panel = HcPanel::WaitVotes;
    else if (g.role == Role::None && g.phase == Phase::Idle && in_house() && housecoop_holds_solo()) out.panel = HcPanel::WaitHouse;
    return out.panel != HcPanel::None;
}

void housecoop_answer(bool yes) {
    if (g.role != Role::Helper || g.phase != Phase::Asking) return;
    send(kHcVote, yes ? 1 : 0);
    log_line("HOUSECOOP", "this player %s P%d's house boss", yes ? "HELPS with" : "does not help with", row_of(g.px));
    if (yes) g.phase = Phase::Voted;
    else reset("this player will not help");
}

void housecoop_fight_alone() {
    if (g.role == Role::Px && g.phase == Phase::Asking) send(kHcCancel);
    else if (!(g.role == Role::None && g.phase == Phase::Idle)) return;
    reset("the helped player fights alone");
    decline_today();
    log_line("HOUSECOOP", "this player fights its house boss alone");
}

} // namespace mgmp
