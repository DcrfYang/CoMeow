// mgmp_diag.cpp -- see mgmp_diag.h.
#include "mgmp_diag.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "mgmp_log.h"
#include "mgmp_lockstep.h"
#include "mgmp_net.h"
#include "mgmp_proto.h"
#include "mgmp_rng.h"
#include "mgmp_tuning.h"

namespace mgmp {

// ============================================================================================================================================
// THE RNG LEDGER
// ============================================================================================================================================
namespace {
struct RnglRec { RnglMsg m; bool have = false; };
RnglRec g_mine[64], g_peer[kMaxPeers][64];
uint64_t g_battle = 0;
uint32_t g_seq = 0, g_compared = 0, g_mismatched = 0, g_detailed = 0;
uint32_t g_loose_mismatched = 0, g_loose_lines = 0;
bool g_armed = false, g_shared = false, g_env_sent = false;

void rngl_reset(uint64_t battle) {
    for (auto& r : g_mine) r = RnglRec{};
    for (auto& row : g_peer) for (auto& r : row) r = RnglRec{};
    g_battle = battle; g_seq = g_compared = g_mismatched = g_detailed = 0; g_loose_mismatched = g_loose_lines = 0; g_shared = false; g_env_sent = false;
}

const char* fn_name(uint32_t key) {
    switch (key >> 28) { case 0: return "randint"; case 1: return "randfloat"; case 2: return "rand2"; case 3: return "rollchance"; default: return "draw"; }
}

// The call sites whose draw counts differ, as text: merged by key (both lists are sorted). A site missing from a list that was cut short (`more`) may only be missing from the message.
constexpr uint32_t kLooseBit = 0x08000000u;
size_t site_diff(const RnglMsg& a0, const RnglMsg& b0, bool loose, char* out, size_t cap) {
    // the sites of one phase only
    RnglMsg a = a0, b = b0;
    auto filter = [&](RnglMsg& m) {
        uint8_t k = 0;
        for (uint32_t i = 0; i < m.sites; ++i) if (((m.key[i] & kLooseBit) != 0) == loose) { m.key[k] = m.key[i]; m.count[k] = m.count[i]; ++k; }
        m.sites = k;
    };
    filter(a); filter(b);
    size_t w = 0;
    uint32_t i = 0, j = 0, shown = 0, total = 0;
    auto put = [&](uint32_t key, long long here, long long there, bool unsure) {
        ++total;
        if (shown >= 8 || w + 100 >= cap) return;
        ++shown;
        const int n = _snprintf_s(out + w, cap - w, _TRUNCATE, "%s[%s site rva 0x%07X: here %lld, there %lld%s]", w ? " " : "", fn_name(key), key & 0x07FFFFFFu, here, there, unsure ? " (listing cut)" : "");
        if (n > 0) w += (size_t)n;
    };
    while (i < a.sites || j < b.sites) {
        if (j >= b.sites || (i < a.sites && a.key[i] < b.key[j])) { put(a.key[i], a.count[i], 0, b.more != 0); ++i; }
        else if (i >= a.sites || b.key[j] < a.key[i]) { put(b.key[j], 0, b.count[j], a.more != 0); ++j; }
        else { if (a.count[i] != b.count[j]) put(a.key[i], a.count[i], b.count[j], false); ++i; ++j; }
    }
    if (total > shown && w + 40 < cap) _snprintf_s(out + w, cap - w, _TRUNCATE, " ... and %u more site(s)", total - shown);
    if (!w) _snprintf_s(out, cap, _TRUNCATE, "no listed site differs in count (the order of the draws differs)");
    return w;
}

void rngl_check(uint32_t seq, uint8_t from) {
    RnglRec& a = g_mine[seq % 64];
    RnglRec& b = g_peer[from][seq % 64];
    if (!a.have || !b.have || a.m.seq != seq || b.m.seq != seq || a.m.battle != b.m.battle) return;
    b.have = false;                                       // compared once
    ++g_compared;
    // BETWEEN ACTIONS (the AI's decisions; the owner's aim previews too): reported on its own and more quietly -- a preview on one peer is expected, an AI decision that differs is the bug
    if (a.m.n_loose != b.m.n_loose || a.m.digest_loose != b.m.digest_loose) {
        ++g_loose_mismatched;
        if (g_loose_lines < 4) {
            ++g_loose_lines;
            char d[900];
            site_diff(a.m, b.m, true, d, sizeof(d));
            log_line_lvl(LogLevel::Warn, "RNGL", "RNG LEDGER (between actions) differs: battle %016llx turn %u before action %u (flush #%u): THIS peer drew %u time(s), peer %u drew %u -- %s -- the draws the AI's decisions make, and "
                         "the aim previews on the cat's owner's peer, land here; an owner's preview is expected, a decision that differs is not (the stream is reseeded at the next action, so it cannot spread)",
                         (unsigned long long)a.m.battle, a.m.turn, a.m.action, a.m.seq, a.m.n_loose, (unsigned)from, b.m.n_loose, d);
        }
    }
    if (a.m.n == b.m.n && a.m.digest == b.m.digest) return;
    ++g_mismatched;
    if (g_detailed >= 4) return;                          // the first ones say it all; a cascade of misaligned windows after them does not
    ++g_detailed;
    char diff[900];
    site_diff(a.m, b.m, false, diff, sizeof(diff));
    log_line_lvl(LogLevel::Error, "RNGL", "!! RNG LEDGER MISMATCH battle %016llx turn %u after action %u (flush #%u): THIS peer drew %u time(s) from %u call site(s) (order digest %08x) | peer %u drew %u time(s) from %u site(s) "
                 "(digest %08x) -- what differs: %s -- the shared stream was served a different number of draws, or in another order, inside this action; the site is the draw's return address (see RNG-STREAM-SITES for the "
                 "function that holds it)", (unsigned long long)a.m.battle, a.m.turn, a.m.action, a.m.seq, a.m.n, (unsigned)a.m.sites + (a.m.more ? 1u : 0u), a.m.digest, (unsigned)from, b.m.n,
                 (unsigned)b.m.sites + (b.m.more ? 1u : 0u), b.m.digest, diff);
    if (!g_shared) { g_shared = true; lockstep_share_log("a draw-ledger mismatch"); }
}
} // namespace

void rngl_flush(uint32_t turn, uint32_t action) {
    if (!net_active() || net_peer_count() < 2 || !lockstep_active()) { if (g_armed) { rng_ledger_arm(false); g_armed = false; } return; }
    // Only the fight's own draws: the battle is snapshotted, its character list is live and an enemy is standing. Same condition on both peers (the state is the same).
    const bool on = lockstep_in_battle() && lockstep_fight_up() && !lockstep_enemies_all_down();
    if (!on) { if (g_armed) { rng_ledger_arm(false); g_armed = false; } return; }
    const uint64_t battle = lockstep_battle_id();
    if (battle != g_battle) { rngl_reset(battle); g_armed = false; }
    if (!g_armed) {                                       // the first window of a battle is not a whole one: start counting here, say who this peer is, compare from the next flush on
        rng_ledger_arm(true); g_armed = true;
        if (!g_env_sent) {
            g_env_sent = true;
            static bool logged_here = false;
            char text[1400];
            const size_t n = diag_environment_text(text, sizeof(text));
            if (!logged_here) { logged_here = true; log_line("ENV", "%s", text); }
            if (n) {
                PeerLogMsg m{};
                m.battle_id = battle; m.turn = turn;
                strncpy_s(m.why, sizeof(m.why), "environment of this peer", _TRUNCATE);
                m.size = (uint32_t)n; m.data = (uint8_t*)text;
                net_send_peerlog(m);
            }
        }
        return;
    }
    RngLedger L;
    rng_ledger_take(L);
    RnglMsg m;
    m.battle = battle; m.seq = ++g_seq; m.turn = turn; m.action = action; m.n = L.n; m.digest = L.digest; m.n_loose = L.n_loose; m.digest_loose = L.digest_loose;
    m.sites = (uint8_t)(L.sites < kRnglSites ? L.sites : kRnglSites);
    m.more = (L.sites - m.sites) + (L.over ? 1u : 0u);
    for (uint32_t i = 0; i < m.sites; ++i) { m.key[i] = L.key[i]; m.count[i] = L.count[i]; }
    RnglRec& r = g_mine[m.seq % 64];
    r.m = m; r.have = true;
    net_send_rngl(m);
    for (uint8_t p = 0; p < kMaxPeers; ++p) rngl_check(m.seq, p);
}

void rngl_on_peer(uint8_t from, const RnglMsg& m) {
    if (from >= kMaxPeers || !lockstep_active() || m.battle != lockstep_battle_id()) return;
    if (m.battle != g_battle) rngl_reset(m.battle);
    RnglRec& r = g_peer[from][m.seq % 64];
    r.m = m; r.have = true;
    rngl_check(m.seq, from);
}

void rngl_stats(uint32_t& compared, uint32_t& mismatched, uint32_t& loose) {
    compared = g_battle == lockstep_battle_id() ? g_compared : 0;
    mismatched = g_battle == lockstep_battle_id() ? g_mismatched : 0;
    loose = g_battle == lockstep_battle_id() ? g_loose_mismatched : 0;
}

// ============================================================================================================================================
// THE DEEP DIGEST
// ============================================================================================================================================
namespace {
struct DeepMine { uint64_t battle = 0; uint32_t turn = ~0u; uint32_t n = 0; DeepRow row[kDeepUnits]; uint32_t digest[kDeepUnits] = {}; };
struct DeepPeer { DeepMsg m; bool have = false; };
DeepMine d_mine[8];
DeepPeer d_peer[kMaxPeers][8];
uint64_t d_battle = 0;
uint32_t d_compared = 0, d_mismatched = 0, d_lines = 0;
bool d_shared = false;

uint32_t row_digest(const DeepRow& r) {
    if (!r.ok) return 0;
    uint32_t h = 2166136261u;
    const int32_t v[13] = { r.speed, r.key_a, r.key_b, r.init_base, r.maxhp, r.bonus, r.stat[0], r.stat[1], r.stat[2], r.stat[3], r.stat[4], r.stat[5], r.stat[6] };
    for (int i = 0; i < 13; ++i) { h = (h ^ (uint32_t)v[i]) * 16777619u; h ^= h >> 15; }
    return h ? h : 1u;
}
void deep_reset(uint64_t battle) {
    for (auto& r : d_mine) r = DeepMine{};
    for (auto& row : d_peer) for (auto& r : row) r = DeepPeer{};
    d_battle = battle; d_compared = d_mismatched = d_lines = 0; d_shared = false;
}
void deep_check(uint32_t turn, uint8_t from) {
    DeepMine& a = d_mine[turn % 8];
    DeepPeer& b = d_peer[from][turn % 8];
    if (a.battle != d_battle || a.turn != turn || !b.have || b.m.turn != turn || b.m.battle != a.battle) return;
    b.have = false;
    ++d_compared;
    if (a.n != b.m.n) {
        ++d_mismatched;
        if (d_lines < 8) { ++d_lines; log_line_lvl(LogLevel::Error, "DEEP", "!! DERIVED VALUES: turn %u: this peer has %u unit(s) at the boundary, peer %u has %u", turn, a.n, (unsigned)from, (unsigned)b.m.n); }
        return;
    }
    uint32_t bad = 0;
    for (uint32_t i = 0; i < a.n; ++i) {
        if (!a.digest[i] || !b.m.digest[i] || a.digest[i] == b.m.digest[i]) continue;
        if (!bad) ++d_mismatched;
        ++bad;
        if (d_lines >= 8) continue;
        ++d_lines;
        const DeepRow& r = a.row[i];
        log_line_lvl(LogLevel::Error, "DEEP", "!! DERIVED VALUES DIFFER turn %u, unit %u (this peer: speed %d, turn-order keys %d/%d, base %d, max hp %d, stat bonus %d, stats [%d %d %d %d %d %d %d], digest %08x) -- "
                     "peer %u has digest %08x for the same unit (its own values are in the log tail it shares); the state hash does not cover these, and the host's board repairs the keys and speed afterwards", turn, i,
                     r.speed, r.key_a, r.key_b, r.init_base, r.maxhp, r.bonus, r.stat[0], r.stat[1], r.stat[2], r.stat[3], r.stat[4], r.stat[5], r.stat[6], a.digest[i], (unsigned)from, b.m.digest[i]);
    }
    if (bad && !d_shared) { d_shared = true; lockstep_share_log("a derived-value difference"); }
}
} // namespace

void deep_publish(uint64_t battle, uint32_t turn, const DeepRow* rows, uint32_t n) {
    if (!battle || !rows || !net_active() || net_peer_count() < 2 || !lockstep_active()) return;
    if (battle != d_battle) deep_reset(battle);
    if (n > kDeepUnits) n = kDeepUnits;
    DeepMine& a = d_mine[turn % 8];
    a = DeepMine{};
    a.battle = battle; a.turn = turn; a.n = n;
    DeepMsg m;
    m.battle = battle; m.turn = turn; m.n = (uint8_t)n;
    for (uint32_t i = 0; i < n; ++i) { a.row[i] = rows[i]; a.digest[i] = m.digest[i] = row_digest(rows[i]); }
    net_send_deep(m);
    for (uint8_t p = 0; p < kMaxPeers; ++p) deep_check(turn, p);
}

void deep_on_peer(uint8_t from, const DeepMsg& m) {
    if (from >= kMaxPeers || !lockstep_active() || m.battle != lockstep_battle_id()) return;
    if (m.battle != d_battle) deep_reset(m.battle);
    DeepPeer& r = d_peer[from][m.turn % 8];
    r.m = m; r.have = true;
    deep_check(m.turn, from);
}

void deep_stats(uint32_t& compared, uint32_t& mismatched) {
    compared = d_battle == lockstep_battle_id() ? d_compared : 0;
    mismatched = d_battle == lockstep_battle_id() ? d_mismatched : 0;
}

// ============================================================================================================================================
// THE ENVIRONMENT
// ============================================================================================================================================
namespace {
uint64_t file_fnv(const wchar_t* path, uint64_t& size) {
    size = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    uint64_t h = 1469598103934665603ULL;
    static uint8_t buf[1 << 16];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) {
        size += got;
        for (DWORD i = 0; i < got; ++i) { h ^= buf[i]; h *= 1099511628211ULL; }
    }
    CloseHandle(f);
    return h;
}
} // namespace

size_t diag_environment_text(char* out, size_t cap) {
    if (!out || !cap) return 0;
    // the mod's own file and the game's: measured once (hashing 7 MB is cheap, the game's 100 MB is not -- its PE header says which build it is)
    static bool measured = false;
    static char dll[160], exe[200], os[80], loc[40];
    static unsigned cpus = 0;
    if (!measured) {
        measured = true;
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&diag_environment_text, &self);
        wchar_t path[MAX_PATH] = {};
        uint64_t size = 0, h = 0;
        if (self && GetModuleFileNameW(self, path, MAX_PATH)) h = file_fnv(path, size);
        _snprintf_s(dll, sizeof(dll), _TRUNCATE, "mgmp.dll %llu bytes, fnv %016llx", (unsigned long long)size, (unsigned long long)h);

        const uint8_t* base = (const uint8_t*)GetModuleHandleW(nullptr);
        uint32_t stamp = 0, sum = 0, image = 0;
        if (base && ((const IMAGE_DOS_HEADER*)base)->e_magic == IMAGE_DOS_SIGNATURE) {
            const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE) { stamp = nt->FileHeader.TimeDateStamp; sum = nt->OptionalHeader.CheckSum; image = nt->OptionalHeader.SizeOfImage; }
        }
        wchar_t exep[MAX_PATH] = {};
        uint64_t exesize = 0;
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetModuleFileNameW(nullptr, exep, MAX_PATH) && GetFileAttributesExW(exep, GetFileExInfoStandard, &fa)) exesize = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        _snprintf_s(exe, sizeof(exe), _TRUNCATE, "game exe %llu bytes, pe-time %08x, pe-checksum %08x, image %u", (unsigned long long)exesize, stamp, sum, image);

        typedef LONG (WINAPI* fn_rtlver)(OSVERSIONINFOW*);
        OSVERSIONINFOW vi{}; vi.dwOSVersionInfoSize = sizeof(vi);
        fn_rtlver rv = (fn_rtlver)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
        if (rv && rv(&vi) == 0) _snprintf_s(os, sizeof(os), _TRUNCATE, "windows %lu.%lu build %lu", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
        else _snprintf_s(os, sizeof(os), _TRUNCATE, "windows ?");
        SYSTEM_INFO si{}; GetSystemInfo(&si); cpus = si.dwNumberOfProcessors;
        wchar_t wl[LOCALE_NAME_MAX_LENGTH] = {};
        if (GetUserDefaultLocaleName(wl, LOCALE_NAME_MAX_LENGTH)) WideCharToMultiByte(CP_UTF8, 0, wl, -1, loc, sizeof(loc) - 1, nullptr, nullptr);
    }
    const int n = _snprintf_s(out, cap, _TRUNCATE,
        "ENV role=%s self=%u proto=%u sim=%u | %s | %s | %s, %u cpu(s), locale %s | switches: debounce=%d board_repair_players=%d reseed_per_turn=%d rng_global_only=%d",
        net_role() == NetRole::Host ? "host" : "client", (unsigned)net_self(), (unsigned)kProtoVersion, (unsigned)tune::kSimRevision, exe, dll, os, cpus, loc,
        (int)tune::kDesyncDebounce, (int)tune::kBoardRepairPlayerCats, (int)tune::kReseedPerTurn, (int)tune::kRngGlobalOnly);
    return n > 0 ? (size_t)n : 0;
}

} // namespace mgmp
