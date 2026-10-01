// The abandon-adventure vote (mgmp_abandon.cpp), several peers in one process: every peer keeps its own
// copy of the module's state, and the fake net delivers each peer-authored message to every other peer
// (which is what the host's relay does). Only the transport and the game's own abandon are fake.
#include "../src/session/mgmp_abandon.cpp"
#include <deque>
#include <functional>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

using namespace mgmp;
static unsigned checks = 0;
#define CHECK(x) do { ++checks; if(!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while(0)

namespace {
struct Peer { State st; uint16_t nonce = 0; AbandonNotice notice; bool notice_set = false; unsigned ran = 0; };
Peer peers[kMaxPeers];
unsigned current = 0, live = 3;
bool active = true, native_ok = true;
struct Packet { unsigned from, to; AbandonMsg msg; };
std::deque<Packet> packets;

void at(unsigned who, const std::function<void()>& fn) {
    Peer& p = peers[who];
    current = who; g_st = p.st; g_nonce = p.nonce; g_notice = p.notice; g_notice_set = p.notice_set;
    fn();
    p.st = g_st; p.nonce = g_nonce; p.notice = g_notice; p.notice_set = g_notice_set;
}
void drain() {
    unsigned budget = 200;
    while (!packets.empty()) {
        CHECK(budget--);
        Packet p = packets.front(); packets.pop_front();
        // through the real wire format, like the transport would
        uint8_t buf[64]; const uint32_t n = enc_abandon(buf, sizeof(buf), p.msg);
        CHECK(n);
        Reader r(buf + 1, n - 1); AbandonMsg m; CHECK(dec_abandon(r, m));
        at(p.to, [&] { abandon_on_message((uint8_t)p.from, m); });
    }
}
void reset(unsigned n) {
    live = n; active = true; native_ok = true; packets.clear();
    for (auto& p : peers) p = Peer{};
}
bool press(unsigned who) { bool r = false; at(who, [&] { r = abandon_on_try(); }); return r; }
AbandonPhase phase(unsigned who) { AbandonView v; AbandonPhase ph = AbandonPhase::None; at(who, [&] { if (abandon_view(v)) ph = v.phase; }); return ph; }
}

namespace mgmp {
bool net_active() { return active; }
uint8_t net_self() { return (uint8_t)current; }
uint8_t net_peer_count() { return (uint8_t)live; }
bool net_peer_ids(uint8_t* out, uint8_t cap) { if (cap < live) return false; for (unsigned i = 0; i < live; ++i) out[i] = (uint8_t)i; return true; }
bool net_send_abandon(const AbandonMsg& m) {
    for (unsigned i = 0; i < live; ++i) if (i != current) packets.push_back({ current, i, m });
    return true;
}
bool abandon_native_ready() { return native_ok; }
bool abandon_run_native() { ++peers[current].ran; return true; }
void log_line(const char*, const char* fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); }
}

int main() {
    printf("-- the wire format --\n");
    {
        uint8_t buf[16]; AbandonMsg m; m.kind = kAbandonAgree; m.proposer = 2; m.nonce = 0xABCD;
        const uint32_t n = enc_abandon(buf, sizeof(buf), m);
        CHECK(n == 5 && buf[0] == MSG_ABANDON);
        Reader r(buf + 1, n - 1); AbandonMsg d; CHECK(dec_abandon(r, d));
        CHECK(d.kind == kAbandonAgree && d.proposer == 2 && d.nonce == 0xABCD);
        m.kind = 0; CHECK(enc_abandon(buf, sizeof(buf), m) == 0);
        m.kind = 9; CHECK(enc_abandon(buf, sizeof(buf), m) == 0);
        uint8_t bad[5] = { MSG_ABANDON, 9, 0, 0, 0 }; Reader rb(bad + 1, 4); AbandonMsg x; CHECK(!dec_abandon(rb, x));
        CHECK(kProtoVersion >= 48);
    }

    printf("-- alone, or without the game call, the pause menu's own abandon runs --\n");
    reset(1); CHECK(!press(0));
    reset(3); active = false; CHECK(!press(0));
    reset(3); native_ok = false; CHECK(!press(1));

    printf("-- everyone agrees: every peer abandons once, nobody before --\n");
    reset(3);
    CHECK(press(1));                                   // swallowed by the vote
    CHECK(phase(1) == AbandonPhase::Proposing);
    drain();
    CHECK(phase(0) == AbandonPhase::Asking && phase(2) == AbandonPhase::Asking);
    { AbandonView v; at(0, [&] { CHECK(abandon_view(v)); CHECK(v.n == 3 && v.proposer_pos == 1 && v.self_pos == 0); CHECK(v.agreed[1] && !v.agreed[0] && !v.agreed[2]); }); }
    at(0, [&] { abandon_vote(true); }); drain();
    CHECK(phase(0) == AbandonPhase::Answered);
    CHECK(peers[0].ran == 0 && peers[1].ran == 0 && peers[2].ran == 0);
    at(2, [&] { abandon_vote(true); }); drain();
    CHECK(peers[0].ran == 1 && peers[1].ran == 1 && peers[2].ran == 1);
    CHECK(phase(0) == AbandonPhase::None && phase(1) == AbandonPhase::None && phase(2) == AbandonPhase::None);

    printf("-- one refusal ends it everywhere --\n");
    reset(3);
    CHECK(press(0)); drain();
    at(1, [&] { abandon_vote(true); }); drain();
    at(2, [&] { abandon_vote(false); }); drain();
    for (unsigned i = 0; i < 3; ++i) CHECK(peers[i].ran == 0 && phase(i) == AbandonPhase::None);
    { AbandonNotice nt; at(0, [&] { CHECK(abandon_take_notice(nt)); CHECK(nt.kind == AbandonNoticeKind::Denied && nt.pos == 2); }); }
    // and the button works again afterwards
    CHECK(press(2)); drain(); CHECK(phase(0) == AbandonPhase::Asking);

    printf("-- the proposer takes it back --\n");
    reset(2);
    CHECK(press(1)); drain();
    at(1, [&] { abandon_withdraw(); }); drain();
    CHECK(phase(0) == AbandonPhase::None && phase(1) == AbandonPhase::None);
    { AbandonNotice nt; at(0, [&] { CHECK(abandon_take_notice(nt)); CHECK(nt.kind == AbandonNoticeKind::Withdrawn && nt.pos == 1); }); }

    printf("-- two proposals at once: the lower id wins, and it still needs everyone --\n");
    reset(3);
    CHECK(press(2)); CHECK(press(1));                  // both pressed before either arrived
    drain();
    CHECK(phase(1) == AbandonPhase::Proposing);
    CHECK(phase(2) == AbandonPhase::Asking);           // peer 2 dropped its own and answers peer 1's
    CHECK(phase(0) == AbandonPhase::Asking);
    at(0, [&] { abandon_vote(true); }); at(2, [&] { abandon_vote(true); }); drain();
    CHECK(peers[0].ran == 1 && peers[1].ran == 1 && peers[2].ran == 1);

    printf("-- a press while a vote runs does not start another --\n");
    reset(3);
    CHECK(press(0)); drain();
    CHECK(press(1));
    { AbandonNotice nt; at(1, [&] { CHECK(abandon_take_notice(nt)); CHECK(nt.kind == AbandonNoticeKind::Busy); }); }
    drain();
    CHECK(phase(1) == AbandonPhase::Asking);

    printf("-- someone leaving cancels it --\n");
    reset(3);
    CHECK(press(0)); drain();
    live = 2;
    at(0, [&] { abandon_tick(); }); at(1, [&] { abandon_tick(); });
    CHECK(phase(0) == AbandonPhase::None && phase(1) == AbandonPhase::None);
    { AbandonNotice nt; at(1, [&] { CHECK(abandon_take_notice(nt)); CHECK(nt.kind == AbandonNoticeKind::Changed); }); }

    printf("-- nobody answering times out, the proposer telling the rest --\n");
    reset(3);
    CHECK(press(0)); drain();
    at(0, [&] { g_st.started = 1; abandon_tick(); }); drain();
    CHECK(phase(0) == AbandonPhase::None && phase(1) == AbandonPhase::None && phase(2) == AbandonPhase::None);

    printf("-- a stale answer from an older proposal counts for nothing --\n");
    reset(3);
    CHECK(press(0)); drain();
    AbandonMsg old; old.kind = kAbandonAgree; old.proposer = 0; old.nonce = 999;
    at(1, [&] { abandon_on_message(2, old); });
    at(1, [&] { AbandonView v; CHECK(abandon_view(v)); CHECK(!v.agreed[2]); });
    old.kind = kAbandonDeny; at(1, [&] { abandon_on_message(2, old); });
    CHECK(phase(1) == AbandonPhase::Asking);

    printf("abandon: %u checks passed\n", checks);
    return 0;
}
