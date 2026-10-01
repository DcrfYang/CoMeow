// mgmp_abandon.cpp -- see mgmp_abandon.h.
#include "mgmp_abandon.h"

#include <windows.h>
#include <cstring>

#include "mgmp_log.h"
#include "mgmp_net.h"
#include "mgmp_proto.h"

namespace mgmp {
namespace {

// A vote nobody answers must not hold the pause menu's button forever.
constexpr ULONGLONG kProposerTimeoutMs = 60000;
constexpr ULONGLONG kVoterTimeoutMs    = 65000;    // outlives the proposer's, so its Cancel arrives first

struct State {
    bool      active = false;
    bool      mine = false;
    uint8_t   proposer = 0;
    uint16_t  nonce = 0;
    uint8_t   ids[kAbandonMaxPlayers] = {};
    uint8_t   n = 0;
    bool      agreed[kAbandonMaxPlayers] = {};
    bool      self_agreed = false;
    ULONGLONG started = 0;
};

State         g_st;
uint16_t      g_nonce = 0;
AbandonNotice g_notice;
bool          g_notice_set = false;

void notice(AbandonNoticeKind k, uint8_t pos = 0) {
    g_notice.kind = k; g_notice.pos = pos; g_notice_set = true;
}

// The session's members, host first. False when there is no membership to speak of.
bool members(uint8_t* ids, uint8_t& n) {
    uint8_t all[kMaxPeers] = {};
    if (!net_peer_ids(all, kMaxPeers)) return false;
    n = net_peer_count();
    if (n > kMaxPeers) n = kMaxPeers;
    if (n > kAbandonMaxPlayers) n = kAbandonMaxPlayers;
    for (uint8_t i = 0; i < n; ++i) ids[i] = all[i];
    return true;
}

int pos_in_state(uint8_t id) {
    for (uint8_t i = 0; i < g_st.n; ++i) if (g_st.ids[i] == id) return i;
    return -1;
}

bool same_members() {
    uint8_t ids[kAbandonMaxPlayers] = {}; uint8_t n = 0;
    if (!members(ids, n) || n != g_st.n) return false;
    return memcmp(ids, g_st.ids, n) == 0;
}

void end_vote() { g_st = State{}; }

bool begin(uint8_t proposer, uint16_t nonce, bool mine) {
    State s;
    if (!members(s.ids, s.n) || s.n < 2) return false;
    s.active = true; s.mine = mine; s.proposer = proposer; s.nonce = nonce;
    s.started = GetTickCount64();
    g_st = s;
    const int p = pos_in_state(proposer);
    if (p >= 0) g_st.agreed[p] = true;
    if (mine) g_st.self_agreed = true;
    return p >= 0;
}

bool matches(const AbandonMsg& m) {
    return g_st.active && m.proposer == g_st.proposer && m.nonce == g_st.nonce;
}

void send(uint8_t kind) {
    AbandonMsg m;
    m.kind = kind; m.proposer = g_st.proposer; m.nonce = g_st.nonce;
    net_send_abandon(m);
}

// Unanimous?
void check_all() {
    if (!g_st.active) return;
    for (uint8_t i = 0; i < g_st.n; ++i) if (!g_st.agreed[i]) return;
    log_line("ABANDON", "every player agreed (proposal %u from peer %u) -- abandoning the adventure",
             (unsigned)g_st.nonce, (unsigned)g_st.proposer);
    end_vote();
    notice(AbandonNoticeKind::Going);
    if (!abandon_run_native())
        log_line("ABANDON", "!! the game's abandon could not be started on this peer");
}

} // namespace

bool abandon_on_try() {
    if (!net_active() || net_peer_count() < 2) return false;
    if (!abandon_native_ready()) {
        log_line("ABANDON", "the game-side call is not resolved -- the pause menu's own abandon runs");
        return false;
    }
    if (g_st.active) {
        notice(AbandonNoticeKind::Busy);
        return true;
    }
    if (!begin(net_self(), ++g_nonce, true)) return false;
    log_line("ABANDON", "abandon proposed (nonce %u) -- asking %u other player(s)",
             (unsigned)g_st.nonce, (unsigned)(g_st.n - 1));
    send(kAbandonPropose);
    return true;
}

void abandon_on_message(uint8_t from, const AbandonMsg& m) {
    if (!net_active()) return;
    switch (m.kind) {
        case kAbandonPropose: {
            if (m.proposer != from) return;                 // a peer speaks for itself
            if (g_st.active) {
                if (matches(m)) return;                     // a repeat
                if (m.proposer > g_st.proposer) return;     // the lower id wins; theirs is dropped by them
                log_line("ABANDON", "proposal from peer %u outranks the one in progress -- adopting it",
                         (unsigned)m.proposer);
            }
            if (!begin(m.proposer, m.nonce, false)) return;
            log_line("ABANDON", "peer %u proposes to abandon the adventure -- asking this player",
                     (unsigned)m.proposer);
            return;
        }
        case kAbandonAgree: {
            if (!matches(m)) return;
            const int p = pos_in_state(from);
            if (p < 0) return;
            g_st.agreed[p] = true;
            check_all();
            return;
        }
        case kAbandonDeny: {
            if (!matches(m)) return;
            const int p = pos_in_state(from);
            if (p < 0) return;
            log_line("ABANDON", "peer %u refused -- the proposal is off", (unsigned)from);
            end_vote();
            notice(AbandonNoticeKind::Denied, (uint8_t)p);
            return;
        }
        case kAbandonCancel: {
            if (!matches(m) || from != g_st.proposer) return;
            const int p = pos_in_state(from);
            log_line("ABANDON", "peer %u withdrew the proposal", (unsigned)from);
            end_vote();
            notice(AbandonNoticeKind::Withdrawn, (uint8_t)(p < 0 ? 0 : p));
            return;
        }
        default:
            return;
    }
}

void abandon_vote(bool agree) {
    if (!g_st.active || g_st.mine || g_st.self_agreed) return;
    if (agree) {
        g_st.self_agreed = true;
        const int p = pos_in_state(net_self());
        if (p >= 0) g_st.agreed[p] = true;
        send(kAbandonAgree);
        check_all();
    } else {
        send(kAbandonDeny);
        end_vote();
    }
}

void abandon_withdraw() {
    if (!g_st.active || !g_st.mine) return;
    send(kAbandonCancel);
    const int p = pos_in_state(net_self());
    end_vote();
    notice(AbandonNoticeKind::Withdrawn, (uint8_t)(p < 0 ? 0 : p));
}

void abandon_tick() {
    if (!g_st.active) return;
    if (!net_active()) { end_vote(); return; }
    if (!same_members()) {
        log_line("ABANDON", "the session's membership changed -- the proposal is off");
        end_vote();
        notice(AbandonNoticeKind::Changed);
        return;
    }
    const ULONGLONG age = GetTickCount64() - g_st.started;
    if (age > (g_st.mine ? kProposerTimeoutMs : kVoterTimeoutMs)) {
        log_line("ABANDON", "nobody settled the proposal in time -- it is off");
        if (g_st.mine) send(kAbandonCancel);
        end_vote();
        notice(AbandonNoticeKind::TimedOut);
    }
}

void abandon_reset() {
    end_vote();
    g_notice_set = false;
}

bool abandon_view(AbandonView& out) {
    if (!g_st.active) return false;
    out = AbandonView{};
    out.n = g_st.n;
    const int self = pos_in_state(net_self());
    out.self_pos = (uint8_t)(self < 0 ? 0 : self);
    const int prop = pos_in_state(g_st.proposer);
    out.proposer_pos = (uint8_t)(prop < 0 ? 0 : prop);
    for (uint8_t i = 0; i < g_st.n; ++i) out.agreed[i] = g_st.agreed[i];
    out.phase = g_st.mine ? AbandonPhase::Proposing
              : g_st.self_agreed ? AbandonPhase::Answered : AbandonPhase::Asking;
    const ULONGLONG limit = g_st.mine ? kProposerTimeoutMs : kVoterTimeoutMs;
    const ULONGLONG age = GetTickCount64() - g_st.started;
    out.seconds_left = age >= limit ? 0 : (uint32_t)((limit - age + 999) / 1000);
    return true;
}

bool abandon_take_notice(AbandonNotice& out) {
    if (!g_notice_set) return false;
    out = g_notice; g_notice_set = false;
    return true;
}

} // namespace mgmp
