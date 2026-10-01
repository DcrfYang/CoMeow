#include "mgmp_catview.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

#include "mgmp_catsync.h"
#include "mgmp_log.h"
#include "mgmp_lockstep.h"
#include "mgmp_net.h"
#include "mgmp_page.h"
#include "mgmp_proto.h"
#include "mgmp_tuning.h"

namespace mgmp {
namespace {

// The read. A party is four cats; once a second is far finer than the events that change
// it (an equip, a hit, a level), and each read serializes every cat through the game.
constexpr ULONGLONG kReadMs = 500;

// A message that has not changed is still re-sent this often, for the reasons the page
// heartbeat gives: a joiner mid-session, or a datagram lost between two clients.
constexpr ULONGLONG kHeartbeatMs = 3000;

struct Peer {
    CatBrief  cats[kBriefCats];
    uint8_t   n = 0;
};

Peer      g_self;
Peer      g_peer[kMaxPeers];
Peer      g_sent;
bool      g_sent_valid = false;
ULONGLONG g_next_read  = 0;
ULONGLONG g_last_send  = 0;

// The screens where a player's cats are a settled fact worth showing: the two gear pages
// and the chapter page (the party is chosen), and the run itself. The warehouse and the
// save screen are not among them -- the party there is whatever the last run left.
bool shows_cats(PageState p) {
    return p == PageState::Collar || p == PageState::Equipment ||
           p == PageState::Chapter || p == PageState::InGame;
}

bool session_live() {
    switch (net_state()) {
        case NetState::Listening:
        case NetState::Connecting:
        case NetState::Connected:
        case NetState::Ready:
            return true;
        default:
            return false;
    }
}

bool same(const Peer& a, const Peer& b) {
    if (a.n != b.n) return false;
    for (uint32_t i = 0; i < a.n; ++i)
        if (memcmp(&a.cats[i], &b.cats[i], sizeof(CatBrief)) != 0) return false;
    return true;
}

} // namespace

bool catview_page_shows(uint8_t page) { return shows_cats((PageState)page); }

uint32_t catview_self(const CatBrief** out) {
    if (out) *out = g_self.cats;
    return g_self.n;
}

uint32_t catview_of(uint8_t peer, const CatBrief** out) {
    if (peer >= kMaxPeers) { if (out) *out = nullptr; return 0; }
    if (out) *out = g_peer[peer].cats;
    return g_peer[peer].n;
}

void catview_reset() {
    g_self = Peer{};
    g_sent = Peer{};
    g_sent_valid = false;
    g_next_read = 0;
    g_last_send = 0;
    for (int i = 0; i < kMaxPeers; ++i) g_peer[i] = Peer{};
}

void catview_on_message(uint8_t from, const CatsMsg& m) {
    if (from >= kMaxPeers || m.count > kBriefCats) return;
    Peer& p = g_peer[from];
    if (p.n != m.count)
        log_line("CATS", "peer %u brings %u cat(s)", (unsigned)from, (unsigned)m.count);
    p.n = m.count;
    for (uint32_t i = 0; i < m.count; ++i) p.cats[i] = m.cats[i];
}

void catview_tick() {
    const ULONGLONG t = GetTickCount64();
    if (t < g_next_read) return;
    g_next_read = t + kReadMs;

    // NOT gated on the fight (an earlier version was, and the cards froze at the first battle for
    // good: lockstep_in_battle() stays true until the NEXT fight re-snapshots). The lists are read
    // whole and validated, a failed read keeps the previous cards, and a fight's own numbers come
    // from lockstep_live_health.
    {
        Peer now;
        bool read_ok = true;
        if (shows_cats(page_self())) {
            uint32_t n = 0;
            read_ok = catsync_local_briefs(now.cats, kBriefCats, n);
            now.n = (uint8_t)n;
            if (tune::kLogButtons && !same(now, g_self)) {
                for (uint32_t i = 0; i < now.n; ++i) {
                    const CatBrief& c = now.cats[i];
                    log_line("CATS", "self[%u] '%s' lv %d hp %d/%d class %s fur %d coat %d tex %d", i, c.name,
                             c.level, c.hp == kBriefFullHp ? -1 : c.hp, c.maxhp, brief_class_name(c.klass),
                             c.fur, c.coat, c.tex);
                }
            }
        }
        // A read that failed (the run's lists are mid-swap or mid-adopt) keeps the previous cards
        // instead of publishing "no cats" for a moment: cats leaving and joining is exactly when the
        // lists are being rewritten.
        if (read_ok) g_self = now;
    }

    if (!session_live()) { g_sent_valid = false; return; }
    const bool changed   = !g_sent_valid || !same(g_self, g_sent);
    const bool heartbeat = g_self.n > 0 && t - g_last_send > kHeartbeatMs;
    if (!changed && !heartbeat) return;

    CatsMsg m;
    m.count = g_self.n;
    for (uint32_t i = 0; i < g_self.n; ++i) m.cats[i] = g_self.cats[i];
    if (net_send_cats(m)) {
        g_sent = g_self;
        g_sent_valid = true;
        g_last_send = t;
    }
}

} // namespace mgmp
