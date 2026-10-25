// mgmp_perf.cpp -- see the header.
#include "mgmp_perf.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

#include "mgmp_log.h"
#include "mgmp_net.h"
#include "mgmp_proto.h"

namespace mgmp {
namespace {

constexpr ULONGLONG kPingEveryMs = 2000;
constexpr ULONGLONG kLogEveryMs  = 10000;
constexpr ULONGLONG kQuietLogMs  = 60000;     // when nothing is notable: one line a minute
constexpr uint32_t  kSlowMs = 100, kVerySlowMs = 500, kNotableMs = 150, kNotableRttMs = 100;

SRWLOCK g_lock = SRWLOCK_INIT;

struct Frames {          // the frames of a window
    uint32_t n = 0, slow = 0, vslow = 0, max_ms = 0;
    double   sum_ms = 0, mod_sum_ms = 0, mod_max_ms = 0;
};
struct PeerStats {       // the other peer's last report
    bool     have = false;
    uint32_t frames = 0, avg_x10 = 0, max_ms = 0, slow = 0, mod_max_x10 = 0;
    uint8_t  n_rtt = 0, rtt_peer[kMaxPeers] = {};      // proto 105: its round trip to each member that answers it
    uint32_t rtt_ms[kMaxPeers] = {};
    ULONGLONG at = 0;
};
struct Rtt { uint32_t n = 0, last = 0, min = 0xFFFFFFFFu, max = 0; double sum = 0; };

Frames    g_cur2;            // the 2 s window being filled (game thread)
Frames    g_last2;           // the last COMPLETE 2 s window: what the next ping carries
Frames    g_log10;           // the 10 s window of the log line (game thread)
ULONGLONG g_win2_at = 0, g_ping_at = 0, g_log_at = 0, g_quiet_at = 0;
LONGLONG  g_prev_qpc = 0;
uint32_t  g_seq = 0;
Rtt       g_rtt[kMaxPeers];  // by peer id, since the last line (receive threads write, the game thread reads)
PeerStats g_peer[kMaxPeers];
// 0.1.84 (room HA2K, 2026-10-24: "stuck on the map" was a Steam link whose round trip went 1 s -> 17 s -> 29 s): when each ping went out, and per peer the newest one
// answered and the last round trip -- so a link that has stopped answering is seen while it is stopped, not only when its late answers arrive.
constexpr uint32_t kSentRing = 64;
ULONGLONG g_sent_at[kSentRing] = {};
uint32_t  g_answered[kMaxPeers] = {};
uint32_t  g_last_rtt[kMaxPeers] = {};
bool      g_answered_any[kMaxPeers] = {};
NetStats  g_stats_at_log{};

// How long the oldest ping this peer has not answered has been waiting (0: none outstanding). Under g_lock.
uint32_t unanswered_ms(uint8_t p, ULONGLONG now) {
    if (!g_answered_any[p] || g_seq <= g_answered[p]) return 0;
    uint32_t first = g_answered[p] + 1;
    if (g_seq - first >= kSentRing) first = g_seq - kSentRing + 1;
    const ULONGLONG at = g_sent_at[first % kSentRing];
    return at && now > at ? (uint32_t)(now - at) : 0;
}

// This peer's round trip to that one as the panel shows it: the last ping's, or, once a ping has waited 3 s or more, how long it has waited (a link that has stopped
// answering shows how long it has been silent, growing, not its last good figure). Under g_lock; false: it never answered.
bool direct_ms(uint8_t p, ULONGLONG now, uint32_t& ms) {
    if (p >= kMaxPeers || !g_answered_any[p]) return false;
    ms = g_last_rtt[p];
    const uint32_t w = unanswered_ms(p, now);
    if (w >= 3000 && w > ms) ms = w;
    return true;
}

// proto 105: this peer's round trips, for the others (a client reads the host's to know the path through it to another client). Under g_lock.
void fill_rtts(PingMsg& m, ULONGLONG now) {
    m.n_rtt = 0;
    for (uint8_t p = 0; p < kMaxPeers; ++p) {
        uint32_t ms = 0;
        if (!direct_ms(p, now, ms)) continue;
        m.rtt_peer[m.n_rtt] = p; m.rtt_ms[m.n_rtt] = ms; ++m.n_rtt;
    }
}

double qpc_ms(LONGLONG a, LONGLONG b) {
    static LONGLONG freq = 0;
    if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart ? f.QuadPart : 1; }
    return (double)(b - a) * 1000.0 / (double)freq;
}
void add(Frames& f, double dt_ms, double mod_ms) {
    ++f.n; f.sum_ms += dt_ms;
    if (dt_ms >= kSlowMs) ++f.slow;
    if (dt_ms >= kVerySlowMs) ++f.vslow;
    if ((uint32_t)dt_ms > f.max_ms) f.max_ms = (uint32_t)dt_ms;
    f.mod_sum_ms += mod_ms;
    if (mod_ms > f.mod_max_ms) f.mod_max_ms = mod_ms;
}
bool in_room() { return net_active() && net_peer_count() >= 2; }

void fill_stats(PingMsg& m) {
    const Frames& f = g_last2;
    m.frames = f.n;
    m.avg_x10 = f.n ? (uint32_t)(f.sum_ms * 10.0 / f.n) : 0;
    m.max_ms = f.max_ms;
    m.slow = f.slow;
    m.mod_max_x10 = (uint32_t)(f.mod_max_ms * 10.0);
}

} // namespace

void perf_reset() {
    AcquireSRWLockExclusive(&g_lock);
    g_cur2 = g_last2 = g_log10 = Frames{};
    for (uint8_t p = 0; p < kMaxPeers; ++p) { g_rtt[p] = Rtt{}; g_peer[p] = PeerStats{}; g_answered[p] = 0; g_last_rtt[p] = 0; g_answered_any[p] = false; }
    for (auto& t : g_sent_at) t = 0;
    g_win2_at = g_ping_at = g_log_at = g_quiet_at = 0;
    g_prev_qpc = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

void perf_frame_begin(long long* t_in) {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    *t_in = now.QuadPart;
    if (!g_prev_qpc) { g_prev_qpc = now.QuadPart; return; }
    const double dt = qpc_ms(g_prev_qpc, now.QuadPart);
    g_prev_qpc = now.QuadPart;
    if (dt > 60000.0) return;                 // a pause / a load: not a frame
    add(g_cur2, dt, 0.0);
}

void perf_frame_mod_done(long long t_in) {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    const double mod = qpc_ms(t_in, now.QuadPart);
    g_cur2.mod_sum_ms += mod;
    if (mod > g_cur2.mod_max_ms) g_cur2.mod_max_ms = mod;
}

void perf_tick() {
    if (!in_room()) { if (g_ping_at) perf_reset(); return; }
    const ULONGLONG now = GetTickCount64();
    if (!g_win2_at) g_win2_at = g_ping_at = g_log_at = g_quiet_at = now;
    // the 2 s window closes: it becomes what the next ping carries, and joins the 10 s window
    if (now - g_win2_at >= kPingEveryMs) {
        AcquireSRWLockExclusive(&g_lock);
        g_last2 = g_cur2;
        g_log10.n += g_cur2.n; g_log10.sum_ms += g_cur2.sum_ms; g_log10.slow += g_cur2.slow; g_log10.vslow += g_cur2.vslow;
        if (g_cur2.max_ms > g_log10.max_ms) g_log10.max_ms = g_cur2.max_ms;
        g_log10.mod_sum_ms += g_cur2.mod_sum_ms;
        if (g_cur2.mod_max_ms > g_log10.mod_max_ms) g_log10.mod_max_ms = g_cur2.mod_max_ms;
        g_cur2 = Frames{};
        ++g_seq;
        g_sent_at[g_seq % kSentRing] = now;
        ReleaseSRWLockExclusive(&g_lock);
        g_win2_at = now;
        PingMsg m{};
        m.kind = 0; m.seq = g_seq; m.t = (uint32_t)now;
        fill_stats(m);
        AcquireSRWLockShared(&g_lock);
        fill_rtts(m, now);
        ReleaseSRWLockShared(&g_lock);
        net_send_ping(m);
    }
    if (now - g_log_at < kLogEveryMs) return;
    g_log_at = now;
    Frames f; Rtt rtt[kMaxPeers]; PeerStats peer[kMaxPeers]; uint32_t waiting[kMaxPeers] = {};
    AcquireSRWLockExclusive(&g_lock);
    f = g_log10;
    for (uint8_t p = 0; p < kMaxPeers; ++p) { rtt[p] = g_rtt[p]; peer[p] = g_peer[p]; g_rtt[p] = Rtt{}; waiting[p] = unanswered_ms(p, now); }
    g_log10 = Frames{};
    ReleaseSRWLockExclusive(&g_lock);
    // 0.1.84: what this peer put on the wire in the window, and what is still in its own outgoing queue -- a long round trip with little traffic is the carrier, a full queue is us
    const NetStats st = net_stats();
    const uint64_t sent_b = st.bytes_sent - g_stats_at_log.bytes_sent, recv_b = st.bytes_received - g_stats_at_log.bytes_received;
    g_stats_at_log = st;
    bool notable = f.max_ms >= kNotableMs, bad = f.vslow != 0;
    for (uint8_t p = 0; p < kMaxPeers; ++p) {
        const bool fresh = peer[p].have && now - peer[p].at < 30000;
        if (rtt[p].n && rtt[p].max >= kNotableRttMs) notable = true;
        if (fresh && peer[p].max_ms >= kNotableMs) notable = true;
        if ((rtt[p].n && rtt[p].max >= 1000) || (fresh && peer[p].max_ms >= 1000) || waiting[p] >= 3000) notable = bad = true;
    }
    if (!notable && now - g_quiet_at < kQuietLogMs) return;
    g_quiet_at = now;
    char line[900];
    int off = _snprintf_s(line, sizeof(line), _TRUNCATE, "this game: %u frames (avg %.1f ms, longest %u ms, %u over %u ms, %u over %u ms), the mod's part of a frame: avg %.2f longest %.1f ms",
                          f.n, f.n ? f.sum_ms / f.n : 0.0, f.max_ms, f.slow, kSlowMs, f.vslow, kVerySlowMs, f.n ? f.mod_sum_ms / f.n : 0.0, f.mod_max_ms);
    for (uint8_t p = 0; p < kMaxPeers && off > 0 && off < (int)sizeof(line) - 330; ++p) {
        if (!rtt[p].n && !peer[p].have) continue;
        const bool fresh = peer[p].have && now - peer[p].at < 30000;
        if (rtt[p].n) off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, " | peer %u: round trip last %u avg %.0f min %u max %u ms (%u pings)", (unsigned)p, rtt[p].last, rtt[p].sum / rtt[p].n, rtt[p].min, rtt[p].max, rtt[p].n);
        else off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, " | peer %u: no answer to a ping in this interval", (unsigned)p);
        if (fresh) off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, ", its game (last 2 s): %u frames, avg %.1f longest %u ms, %u over %u ms, mod part longest %.1f ms",
                                      peer[p].frames, peer[p].avg_x10 / 10.0, peer[p].max_ms, peer[p].slow, kSlowMs, peer[p].mod_max_x10 / 10.0);
        if (waiting[p] >= 3000) off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, ", !! a ping has waited %u ms for its answer", waiting[p]);
        const size_t q = net_queued_bytes(p);
        if (q) off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, ", %zu byte(s) still queued here for it", q);
    }
    if (off > 0 && off < (int)sizeof(line) - 80)
        off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, " | traffic: sent %.1f KB, received %.1f KB in %.0f s", sent_b / 1024.0, recv_b / 1024.0, kLogEveryMs / 1000.0);
    log_line_lvl(bad ? LogLevel::Warn : LogLevel::Info, "PERF", "%s", line);
}

void perf_on_ping(uint8_t from, const PingMsg& p) {
    if (from >= kMaxPeers) return;
    const uint32_t now = (uint32_t)GetTickCount64();
    AcquireSRWLockExclusive(&g_lock);
    PeerStats& s = g_peer[from];
    s.have = true; s.frames = p.frames; s.avg_x10 = p.avg_x10; s.max_ms = p.max_ms; s.slow = p.slow; s.mod_max_x10 = p.mod_max_x10;
    s.n_rtt = p.n_rtt > kMaxPeers ? kMaxPeers : p.n_rtt;
    for (uint8_t i = 0; i < s.n_rtt; ++i) { s.rtt_peer[i] = p.rtt_peer[i]; s.rtt_ms[i] = p.rtt_ms[i]; }
    s.at = GetTickCount64();
    if (p.kind == 1) {
        const uint32_t rtt = now - p.t;
        if (p.seq > g_answered[from] && p.seq <= g_seq) g_answered[from] = p.seq;
        g_answered_any[from] = true;
        g_last_rtt[from] = rtt;
        if (rtt < 60000) {
            Rtt& r = g_rtt[from];
            ++r.n; r.last = rtt; r.sum += rtt;
            if (rtt < r.min) r.min = rtt;
            if (rtt > r.max) r.max = rtt;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

uint32_t perf_link_slow_ms(uint8_t* who) {
    if (who) *who = kNoPeer;
    if (!in_room()) return 0;
    uint8_t ids[kMaxPeers] = {};
    if (!net_peer_ids(ids, kMaxPeers)) return 0;
    const uint8_t n = net_peer_count() > kMaxPeers ? kMaxPeers : net_peer_count(), self = net_self();
    const ULONGLONG now = GetTickCount64();
    uint32_t worst = 0;
    AcquireSRWLockShared(&g_lock);
    for (uint8_t i = 0; i < n; ++i) {
        const uint8_t p = ids[i];
        if (p >= kMaxPeers || p == self || !g_answered_any[p]) continue;     // a member that never answered is still connecting: not a slow link
        uint32_t ms = unanswered_ms(p, now);
        if (g_last_rtt[p] > ms) ms = g_last_rtt[p];
        if (ms > worst) { worst = ms; if (who) *who = p; }
    }
    ReleaseSRWLockShared(&g_lock);
    return worst;
}

bool perf_peer_latency_ms(uint8_t peer, uint32_t* ms, bool* via_host) {
    if (via_host) *via_host = false;
    if (peer >= kMaxPeers || peer == net_self() || !in_room()) return false;
    const ULONGLONG now = GetTickCount64();
    uint32_t v = 0;
    bool ok = false, through = false;
    AcquireSRWLockShared(&g_lock);
    if (direct_ms(peer, now, v)) ok = true;
    else if (net_role() != NetRole::Host && peer != kHostPeer) {
        // Another client: no link of its own -- its messages come through the host, so the path is this peer's round trip to the host plus the host's to it (proto 105)
        uint32_t mine = 0;
        const PeerStats& h = g_peer[kHostPeer];
        if (direct_ms(kHostPeer, now, mine) && h.have && now - h.at < 30000)
            for (uint8_t i = 0; i < h.n_rtt; ++i)
                if (h.rtt_peer[i] == peer) { v = mine + h.rtt_ms[i]; ok = through = true; break; }
    }
    ReleaseSRWLockShared(&g_lock);
    if (!ok) return false;
    if (ms) *ms = v;
    if (via_host) *via_host = through;
    return true;
}

uint32_t perf_make_reply(uint8_t* buf, uint32_t cap, const PingMsg& req) {
    PingMsg m{};
    m.kind = 1; m.seq = req.seq; m.t = req.t;          // the sender's stamp comes back untouched
    AcquireSRWLockShared(&g_lock);
    fill_stats(m);
    fill_rtts(m, GetTickCount64());
    ReleaseSRWLockShared(&g_lock);
    return enc_ping(buf, cap, m);
}

} // namespace mgmp
