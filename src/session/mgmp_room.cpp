// mgmp_room.cpp -- see mgmp_room.h.
#include "mgmp_room.h"
#include "mgmp_i18n.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "mgmp_catview.h"
#include "mgmp_checkpoint.h"
#include "mgmp_checkpoint_io.h"
#include "mgmp_config.h"
#include "mgmp_leave.h"
#include "mgmp_lockstep.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_menu.h"
#include "mgmp_net.h"
#include "mgmp_page.h"
#include "mgmp_proto.h"
#include "mgmp_resolve.h"
#include "mgmp_session.h"
#include "mgmp_signal.h"

namespace mgmp {
namespace {

constexpr uint32_t  kRows          = 8;
constexpr ULONGLONG kNameEveryMs   = 3000;
constexpr ULONGLONG kAwayLeadMs    = 1500;   // the room hears "away" before the transport drops
constexpr ULONGLONG kRedialEveryMs = 3000;
constexpr int       kRedialTries   = 20;
constexpr ULONGLONG kToastRepeatMs = 2500;

// MewDirector+0x5A8 is the House's progression object: it is what save_adventure (0x1403BABB0) hands to
// the house serializer (0x1401D2550), which stores +0x458 as "next_house_boss" (std::string) and +0x478 as
// "house_boss_countdown" (int); the house's own countdown sign (0x1401E3900) shows "!!!"
// (HOUSEBOSS_COUNTDOWN_TODAY) when +0x478 is 0 and a boss is named.
constexpr uintptr_t kDir_House            = 0x5A8;
constexpr uintptr_t kHouse_NextBossSize   = 0x458 + 0x10;   // std::string size
constexpr uintptr_t kHouse_BossCountdown  = 0x478;

enum class Solo : uint8_t { None, Leaving, Playing, Done };

struct State {
    char      name[kMaxPeers][32] = {};
    bool      have_name[kMaxPeers] = {};
    ULONGLONG name_sent = 0;
    uint8_t   name_self = kNoPeer;
    uint8_t   name_count = 0;

    bool      was_locked = false;

    // membership as it was last frame, for the dropped-player notices
    uint8_t   prev_ids[kMaxPeers] = {};
    uint8_t   prev_n = 0;
    int       prev_row[kMaxPeers] = { -1, -1, -1, -1 };     // by id
    bool      prev_away[kMaxPeers] = {};                    // by id
    PageState prev_page[kMaxPeers] = {};                    // by id
    bool      said[kRows] = {};                             // by row: announced in this game already
    // Once a drop has been announced, players walking back to the main menu are FOLLOWING that notice;
    // only real disconnections are announced after it.
    bool      broken = false;

    char      notice[4][1024] = {};
    uint32_t  notices = 0;
    char      toast[512] = {};
    bool      toast_set = false;
    char      toast_last[512] = {};
    ULONGLONG toast_at = 0;

    Solo      solo = Solo::None;
    ULONGLONG solo_at = 0;
    int       solo_slot = -1;
    // "The boss is over" is only believed after "the boss is today" has been read in this solo, and only
    // once it has held for a while -- a House still being built reads as no boss at all.
    bool      solo_seen_today = false;
    ULONGLONG solo_clear_since = 0;

    bool      host_was_away = false;
    ULONGLONG redial_at = 0;
    int       redial_tries = 0;
};
State g;

bool in_room() { return signal_room()[0] != 0; }

bool in_game(PageState p) {
    return p == PageState::House || p == PageState::Collar || p == PageState::Equipment ||
           p == PageState::Chapter || p == PageState::InGame;
}

void push_notice(const char* text) {
    for (uint32_t i = 0; i < g.notices; ++i) if (strcmp(g.notice[i], text) == 0) return;
    if (g.notices >= 4) return;
    strncpy_s(g.notice[g.notices++], sizeof(g.notice[0]), text, _TRUNCATE);
    log_line("ROOM", "notice: %s", text);
}

void toast(const char* text) {
    const ULONGLONG t = GetTickCount64();
    if (strcmp(text, g.toast_last) == 0 && t - g.toast_at < kToastRepeatMs) return;
    strncpy_s(g.toast, sizeof(g.toast), text, _TRUNCATE);
    strncpy_s(g.toast_last, sizeof(g.toast_last), text, _TRUNCATE);
    g.toast_set = true; g.toast_at = t;
}

uint32_t rows(SignalPeer* out) { return signal_peers(out, kRows); }

int self_row_of(const SignalPeer* p, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) if (strcmp(p[i].name, signal_name()) == 0) return (int)i;
    return -1;
}

// row -> session id, for every row. See the header: names first, then ascending ids.
void build_map(uint8_t* id_of_row, uint32_t& nrows) {
    SignalPeer p[kRows];
    nrows = rows(p);
    for (uint32_t i = 0; i < kRows; ++i) id_of_row[i] = kNoPeer;
    uint8_t ids[kMaxPeers] = {};
    uint8_t n = 0;
    if (net_active() && net_peer_ids(ids, kMaxPeers)) n = net_peer_count();
    if (n > kMaxPeers) n = kMaxPeers;
    const uint8_t me = net_self();
    const int self_row = self_row_of(p, nrows);
    if (self_row >= 0 && n) id_of_row[self_row] = me;
    bool used[kMaxPeers] = {};
    for (uint8_t j = 0; j < n; ++j) {
        const uint8_t id = ids[j];
        if (id == me || id >= kMaxPeers || !g.have_name[id]) continue;
        for (uint32_t r = 0; r < nrows; ++r) {
            if ((int)r == self_row || id_of_row[r] != kNoPeer) continue;
            if (strcmp(p[r].name, g.name[id]) != 0) continue;
            id_of_row[r] = id; used[j] = true;
            break;
        }
    }
    uint32_t r = 0;
    for (uint8_t j = 0; j < n; ++j) {
        if (used[j] || ids[j] == me) continue;
        while (r < nrows && ((int)r == self_row || id_of_row[r] != kNoPeer || p[r].away)) ++r;
        if (r >= nrows) break;
        id_of_row[r++] = ids[j];
    }
}

// Whether today is a house boss day in THIS peer's warehouse. `known` is false when the House is not
// there to read (the menu, a load).
bool houseboss_today_live(bool& known) {
    known = false;
    const uintptr_t at = addr_of_data(D_MewDirectorPtr);
    void* dir = nullptr;
    if (!at || !mem_read((const void*)at, &dir, sizeof(dir)) || !dir) return false;
    void* house = nullptr;
    if (!mem_read((const uint8_t*)dir + kDir_House, &house, sizeof(house)) || !house) return false;
    uint64_t name_size = 0; int32_t countdown = -1;
    if (!mem_read((const uint8_t*)house + kHouse_NextBossSize, &name_size, sizeof(name_size))) return false;
    if (!mem_read((const uint8_t*)house + kHouse_BossCountdown, &countdown, sizeof(countdown))) return false;
    if (name_size > 256) return false;               // not a string: do not guess
    known = true;
    return name_size > 0 && countdown == 0;
}

// The same question of a save file on disk.
bool houseboss_today_file(const wchar_t* path) {
    if (!path || !path[0]) return false;
    bool have = false; std::string name, count;
    if (!checkpoint_io::read_property(path, "next_house_boss", have, name) || !have) return false;
    // SQLite hands text back as text; an empty boss name is "none scheduled".
    while (!name.empty() && (name.back() == ' ' || name.back() == '\0')) name.pop_back();
    if (name.empty()) return false;
    if (!checkpoint_io::read_property(path, "house_boss_countdown", have, count) || !have) return false;
    return atoi(count.c_str()) == 0;
}

void send_name() {
    RoomCtlMsg m;
    m.kind = kRoomName;
    strncpy_s(m.name, sizeof(m.name), signal_name(), _TRUNCATE);
    net_send_roomctl(m);
    g.name_sent = GetTickCount64();
}

void come_back() {
    log_line("ROOM", "the house boss is done -- back in the room, rejoining the session");
    signal_set_away(false);
    g.solo = Solo::None;
    g.solo_slot = -1;
    if (room_is_host()) {
        session_request_host(config().net_port);
    } else {
        // Dial now, and keep dialling (tick_redial) if the host is not listening yet -- it may be away
        // on a boss day of its own.
        g.host_was_away = true; g.redial_tries = 0; g.redial_at = GetTickCount64();
        if (signal_host_addr()[0]) session_request_join(signal_host_addr(), signal_host_port());
    }
}

void start_solo(int slot) {
    g.solo = Solo::Leaving;
    g.solo_at = GetTickCount64();
    g.solo_slot = slot;
    g.solo_seen_today = slot < 0;      // started FROM the House: already read as today
    g.solo_clear_since = 0;
    signal_set_away(true);
    log_line("ROOM", "today is a house boss day on this save -- the fight is played alone (slot %d)", slot);
    push_notice(tr(Tx::R_BOSS_SOLO_START));
}

void tick_solo() {
    const MenuScreen scr = leave_menu_screen();
    switch (g.solo) {
        case Solo::None: {
            // A co-op day that ends in a warehouse on a boss day: the same rule, from the House.
            if (!net_active() || !in_room() || net_peer_count() < 2) return;
            if (scr != MenuScreen::House) return;
            bool known = false;
            if (houseboss_today_live(known) && known) start_solo(-1);
            return;
        }
        case Solo::Leaving:
            if (net_active()) {
                if (GetTickCount64() - g.solo_at > kAwayLeadMs) session_request_disconnect();
                return;
            }
            if (g.solo_slot >= 0) {
                if (scr != MenuScreen::SaveSelect) return;
                log_line("ROOM", "session down -- loading save slot %d alone for the house boss", g.solo_slot);
                g.solo = Solo::Playing;       // before the click, which asks room_on_slot_click again
                menu_request_slot(g.solo_slot);
                g.solo_slot = -1;
            } else {
                g.solo = Solo::Playing;
            }
            return;
        case Solo::Playing: {
            if (scr != MenuScreen::House) { g.solo_clear_since = 0; return; }
            bool known = false;
            const bool today = houseboss_today_live(known);
            if (!known) { g.solo_clear_since = 0; return; }
            if (today) { g.solo_seen_today = true; g.solo_clear_since = 0; return; }
            if (!g.solo_seen_today) return;
            const ULONGLONG t = GetTickCount64();
            if (!g.solo_clear_since) { g.solo_clear_since = t; return; }
            if (t - g.solo_clear_since < 2000) return;
            g.solo = Solo::Done;
            log_line("ROOM", "back in the warehouse and the house boss is no longer due -- the solo fight is over");
            push_notice(tr(Tx::R_BOSS_SOLO_DONE));
            return;
        }
        case Solo::Done:
            if (scr == MenuScreen::MainMenu || scr == MenuScreen::SaveSelect) come_back();
            return;
    }
}

// A client whose host went away and came back dials it again -- only then, so a refused or deliberate
// disconnect is never retried behind the player's back.
void tick_redial() {
    if (!in_room() || room_is_host() || g.solo != Solo::None) { g.host_was_away = false; return; }
    SignalPeer p[kRows];
    const uint32_t n = rows(p);
    bool host_away = false;
    for (uint32_t i = 0; i < n; ++i) if (_stricmp(p[i].role, "host") == 0) host_away = p[i].away;
    if (host_away) { g.host_was_away = true; g.redial_tries = 0; return; }
    if (!g.host_was_away) return;
    if (net_active()) { g.host_was_away = false; return; }
    const MenuScreen scr = leave_menu_screen();
    if (scr != MenuScreen::MainMenu && scr != MenuScreen::SaveSelect) return;
    const ULONGLONG t = GetTickCount64();
    if (t - g.redial_at < kRedialEveryMs || !signal_host_addr()[0]) return;
    if (++g.redial_tries > kRedialTries) { g.host_was_away = false; return; }
    g.redial_at = t;
    log_line("ROOM", "the host is back from its house boss -- dialling it again (%d)", g.redial_tries);
    session_request_join(signal_host_addr(), signal_host_port());
}

// A SAVE THAT IS ALREADY PAST THE DEPARTURE BOX (2026-10-05): the party-size check lives at the box ("departure refused: 3 cats in the box, 4-player room allows 2 each"), and a save that was stored on the collar,
// gear or chapter page is loaded straight onto that page with its cats chosen, so the check never runs. A player on one of those pages whose cats are more than the room allows is told to choose the save again.
// Said once per count (and again if the count changes); the page, the room size and the numbers are in the text.
void tick_party_limit() {
    static ULONGLONG s_next = 0;
    static uint32_t s_said_count = 0;
    const ULONGLONG t = GetTickCount64();
    if (t < s_next) return;
    s_next = t + 500;
    const int limit = room_party_limit();
    const PageState pg = page_self();
    uint32_t n = 0;
    if (!room_self_over_limit(&n)) { s_said_count = 0; return; }
    if (s_said_count == n) return;
    s_said_count = n;
    char text[640];
    _snprintf_s(text, sizeof(text), _TRUNCATE, tr(Tx::R_PARTY_RESELECT), (int)net_peer_count(), limit, (int)n, page_name(pg));
    push_notice(text);
    log_line("ROOM", "!! this peer is on '%s' with %u cat(s) chosen, over the %d a %d-player room allows -- told to choose the save again (the check at the departure box was skipped)", page_name(pg), n, limit, (int)net_peer_count());
}

// THE TWO SAVES BEFORE THE MAP (2026-10-05): the handshake journal also holds a certified save of every player from the moment all are in the warehouse (the preparation stage) and from the moment all are on
// the chapter page ("ready to start"). The checkpoint does the work; this only says, every frame, which of those pages this peer is on.
void tick_stage() {
    const PageState pg = page_self();
    checkpoint_stage_tick(pg == PageState::House ? kStagePrep : pg == PageState::Chapter ? kStageReady : 0);
}

void tick_notices() {
    SignalPeer p[kRows];
    const uint32_t nrows = rows(p);
    uint8_t ids[kMaxPeers] = {};
    uint8_t n = 0;
    if (net_active() && net_peer_ids(ids, kMaxPeers)) n = net_peer_count();
    if (n > kMaxPeers) n = kMaxPeers;
    const uint8_t me = net_self();
    const bool self_game = in_game(page_self());
    if (!self_game) { memset(g.said, 0, sizeof(g.said)); g.broken = false; }
    const bool watch = self_game && in_room() && g.solo == Solo::None;

    auto announce = [&](int row, bool away) {
        if (row < 0 || row >= (int)kRows || g.said[row]) return;
        g.said[row] = true;
        g.broken = true;
        char text[1024];
        if (away)
            _snprintf_s(text, sizeof(text), _TRUNCATE,
                        tr(Tx::R_PEER_AWAY_BOSS), row + 1, row + 1);
        else
            _snprintf_s(text, sizeof(text), _TRUNCATE,
                        tr(Tx::R_PEER_DROPPED), row + 1);
        // Dropped in the middle of a fight: say that the boss that would have ended the run is taken care of.
        if (!away && lockstep_fight_up()) {
            const size_t len = strlen(text);
            _snprintf_s(text + len, sizeof(text) - len, _TRUNCATE, " %s", tr(Tx::R_PEER_DROPPED_FIGHT));
        }
        push_notice(text);
    };

    // Gone from the session.
    for (uint8_t j = 0; j < g.prev_n; ++j) {
        const uint8_t id = g.prev_ids[j];
        if (id == me || id >= kMaxPeers) continue;
        bool still = false;
        for (uint8_t k = 0; k < n; ++k) if (ids[k] == id) still = true;
        if (still) continue;
        if (watch) announce(g.prev_row[id], g.prev_away[id]);
        g.have_name[id] = false;
        g.prev_page[id] = PageState::Unknown;
    }
    // Back to the main menu while this player is in a game.
    for (uint8_t k = 0; k < n; ++k) {
        const uint8_t id = ids[k];
        if (id == me || id >= kMaxPeers) continue;
        const PageState pg = page_of(id);
        if (watch && !g.broken && in_game(g.prev_page[id]) && pg == PageState::MainMenu)
            announce(room_row_of_peer(id), false);
        if (pg != PageState::Unknown) g.prev_page[id] = pg;
    }

    // Remember this frame's view.
    uint8_t id_of_row[kRows]; uint32_t nr = 0;
    build_map(id_of_row, nr);
    for (uint8_t k = 0; k < n; ++k) {
        const uint8_t id = ids[k];
        if (id >= kMaxPeers) continue;
        g.prev_row[id] = -1; g.prev_away[id] = false;
        for (uint32_t r = 0; r < nr; ++r)
            if (id_of_row[r] == id) { g.prev_row[id] = (int)r; g.prev_away[id] = p[r].away; }
    }
    // A player marks itself away a moment BEFORE its transport drops: keep the flag of the row that just
    // went away even when the id has already lost its row.
    for (uint32_t r = 0; r < nrows; ++r) {
        if (!p[r].away) continue;
        for (uint8_t id = 0; id < kMaxPeers; ++id) if (g.prev_row[id] == (int)r) g.prev_away[id] = true;
    }
    memcpy(g.prev_ids, ids, sizeof(ids));
    g.prev_n = n;
}

} // namespace

void room_tick() {
    // Names: to everyone on a change of membership, else every few seconds.
    if (net_active() && net_peer_count() >= 2) {
        const uint8_t me = net_self(), cnt = net_peer_count();
        if (me != g.name_self || cnt != g.name_count || GetTickCount64() - g.name_sent > kNameEveryMs) {
            g.name_self = me; g.name_count = cnt;
            send_name();
        }
    } else {
        g.name_count = 0;
    }

    // Unlocking starts the save-selection round over, on every peer -- each sees the server say so.
    const bool locked = room_locked();
    if (g.was_locked && !locked && in_room()) {
        if (checkpoint_abort_round()) log_line("ROOM", "the room was unlocked -- save selection starts over");
    }
    g.was_locked = locked;

    // A player who goes back to the main menu with a pick still open has left the round: everybody picks
    // again (nobody could otherwise ever finish it).
    if (in_room() && checkpoint_round_open() && leave_menu_screen() == MenuScreen::MainMenu) {
        log_line("ROOM", "back on the main menu with a save picked -- the round starts over");
        room_abort_selection();
    }

    tick_solo();
    tick_redial();
    tick_party_limit();
    tick_stage();
    tick_notices();
}

void room_on_message(uint8_t from, const RoomCtlMsg& m) {
    if (from >= kMaxPeers) return;
    if (m.kind == kRoomName) {
        if (!g.have_name[from] || strcmp(g.name[from], m.name) != 0) {
            strncpy_s(g.name[from], sizeof(g.name[from]), m.name, _TRUNCATE);
            g.have_name[from] = true;
            log_line("ROOM", "peer %u is '%s' in the room", (unsigned)from, m.name);
        }
    } else if (m.kind == kRoomAbort) {
        if (checkpoint_abort_round()) {
            const int row = room_row_of_peer(from);
            char text[96];
            _snprintf_s(text, sizeof(text), _TRUNCATE, tr(Tx::R_REPICK_BY), row < 0 ? 0 : row + 1);
            toast(text);
        }
    }
}

bool room_on_slot_click(int slot, const wchar_t* path) {
    // Away on a house boss: a boss-day save loads as it would alone; anything else means the fight is
    // over, so the click brings this player back to the room instead.
    if (g.solo == Solo::Playing || g.solo == Solo::Done) {
        if (houseboss_today_file(path)) return false;
        come_back();
        toast(tr(Tx::R_SOLO_BACK));
        return true;
    }
    if (g.solo == Solo::Leaving) return true;
    if (!in_room()) return false;
    // In a room, a save is only ever picked together. A player whose session is not up would otherwise
    // load alone and walk the room's host out of hosting.
    if (!net_active()) { toast(tr(Tx::R_NOT_CONNECTED)); return true; }

    if (houseboss_today_file(path)) { start_solo(slot); return true; }

    // Already picked in this round: the pick stands until somebody starts the round over.
    if (checkpoint_round_open()) {
        toast(tr(Tx::R_ALREADY_PICKED));
        return true;
    }

    if (!room_locked()) {
        toast(room_is_host() ? tr(Tx::R_LOCK_FIRST_HOST) : tr(Tx::R_LOCK_FIRST_CLIENT));
        return true;
    }
    SignalPeer p[kRows];
    const uint32_t n = rows(p);
    for (uint32_t i = 0; i < n; ++i) {
        if (!p[i].away) continue;
        char text[96];
        _snprintf_s(text, sizeof(text), _TRUNCATE, tr(Tx::R_WAIT_BOSS), i + 1);
        toast(text);
        return true;
    }
    if (n < 2) { toast(tr(Tx::R_NEED_TWO)); return true; }
    if (net_peer_count() < n) { toast(tr(Tx::R_WAIT_CONNECT)); return true; }
    return false;
}

void room_abort_selection() {
    checkpoint_abort_round();
    if (net_active()) {
        RoomCtlMsg m; m.kind = kRoomAbort;
        net_send_roomctl(m);
    }
}

bool room_is_host() { return in_room() && _stricmp(signal_role(), "host") == 0; }
bool room_locked()  { return signal_room_locked(); }

bool room_can_unlock() {
    if (in_game(page_self())) return false;
    SignalPeer p[kRows];
    const uint32_t n = rows(p);
    for (uint32_t i = 0; i < n; ++i) if (p[i].away) return false;
    uint8_t ids[kMaxPeers] = {};
    if (net_active() && net_peer_ids(ids, kMaxPeers)) {
        const uint8_t cnt = net_peer_count();
        for (uint8_t k = 0; k < cnt && k < kMaxPeers; ++k)
            if (ids[k] != net_self() && in_game(page_of(ids[k]))) return false;
    }
    return true;
}

void room_request_lock(bool on) {
    if (!room_is_host()) return;
    if (!on && !room_can_unlock()) {
        toast(tr(Tx::R_UNLOCK_DENIED));
        return;
    }
    signal_request_lock(on);
}

uint8_t room_peer_for_row(uint32_t row) {
    uint8_t id_of_row[kRows]; uint32_t n = 0;
    build_map(id_of_row, n);
    if (row >= n) return kNoPeer;
    return id_of_row[row] == net_self() ? kNoPeer : id_of_row[row];
}

int room_row_of_peer(uint8_t id) {
    uint8_t id_of_row[kRows]; uint32_t n = 0;
    build_map(id_of_row, n);
    for (uint32_t r = 0; r < n; ++r) if (id_of_row[r] == id) return (int)r;
    return -1;
}

bool room_row_away(uint32_t row) {
    SignalPeer p[kRows];
    const uint32_t n = rows(p);
    return row < n && p[row].away;
}

int room_self_row() {
    SignalPeer p[kRows];
    return self_row_of(p, rows(p));
}

bool room_solo_active() { return g.solo != Solo::None; }


int room_party_limit() {
    if (!net_active()) return 0;
    const uint8_t players = net_peer_count();
    if (players >= 4) return 2;
    if (players == 3) return 3;
    return 0;
}

bool room_self_over_limit(uint32_t* count) {
    const int limit = room_party_limit();
    const PageState pg = page_self();
    if (count) *count = 0;
    if (!limit || !(pg == PageState::Collar || pg == PageState::Equipment || pg == PageState::Chapter)) return false;
    const CatBrief* cats = nullptr;
    const uint32_t n = catview_self(&cats);
    if (count) *count = n;
    return n > (uint32_t)limit;
}

int room_other_over_limit(uint32_t* count) {
    const int limit = room_party_limit();
    if (count) *count = 0;
    if (!limit || !net_active()) return -1;
    uint8_t ids[kMaxPeers] = {};
    if (!net_peer_ids(ids, kMaxPeers)) return -1;
    const uint8_t cnt = net_peer_count();
    for (uint8_t k = 0; k < cnt && k < kMaxPeers; ++k) {
        if (ids[k] == net_self()) continue;
        const PageState pg = page_of(ids[k]);
        if (!(pg == PageState::Collar || pg == PageState::Equipment || pg == PageState::Chapter)) continue;
        const CatBrief* cats = nullptr;
        const uint32_t n = catview_of(ids[k], &cats);
        if (n > (uint32_t)limit) { if (count) *count = n; return room_row_of_peer(ids[k]); }
    }
    return -1;
}

void room_say_party_over() {
    char text[320];
    _snprintf_s(text, sizeof(text), _TRUNCATE, tr(Tx::R_PARTY_OVER), room_party_limit(), (int)net_peer_count());
    toast(text);
}

void room_say_peer_over(int row) {
    char text[320];
    _snprintf_s(text, sizeof(text), _TRUNCATE, tr(Tx::R_PARTY_OVER_PEER), row < 0 ? 0 : row + 1, room_party_limit());
    toast(text);
}

bool room_depart_allowed(uint32_t cats) {
    const int limit = room_party_limit();
    if (!limit || cats <= (uint32_t)limit) return true;
    char text[256];
    _snprintf_s(text, sizeof(text), _TRUNCATE, tr(Tx::R_PARTY_LIMIT), (int)net_peer_count(), limit);
    toast(text);
    log_line("ROOM", "departure refused: %u cats in the box, %d-player room allows %d each",
             cats, (int)net_peer_count(), limit);
    return false;
}

bool room_refuse_story_item(const char* item) {
    char text[256];
    _snprintf_s(text, sizeof(text), _TRUNCATE, tr(Tx::R_STORY_ITEM), item && *item ? item : "?");
    toast(text);
    log_line("ROOM", "departure refused: a selected cat wears the main-story item '%s' (client)", item ? item : "?");
    return false;
}

void room_say_story_only_host() { toast(tr(Tx::R_STORY_ONLY_HOST)); }
void room_say_chapter_client() { toast(tr(Tx::R_CHAPTER_CLIENT)); }
void room_say_chapter_wait() { toast(tr(Tx::R_CHAPTER_WAIT)); }
void room_say_chapter_locked_2() { toast(tr(Tx::R_CHAPTER_LOCKED_2)); }
void room_say_chapter_locked_3() { toast(tr(Tx::R_CHAPTER_LOCKED_3)); }
void room_say_no_chapter_ready() { toast(tr(Tx::R_START_READY)); }
void room_say_no_chapter_wait() { toast(tr(Tx::R_START_WAIT)); }

void room_sync_trouble(const char* what) {
    if (!net_active() || net_peer_count() < 2) return;
    log_line("ROOM", "!! sync timed out (%s) -- the client's connection to the host may have a problem", what ? what : "?");
    push_notice(tr(Tx::R_SYNC_TROUBLE));
}

bool room_notice(char* out, size_t cap) {
    if (!g.notices) return false;
    strncpy_s(out, cap, g.notice[0], _TRUNCATE);
    return true;
}

void room_notice_dismiss() {
    if (!g.notices) return;
    for (uint32_t i = 1; i < g.notices; ++i) memcpy(g.notice[i - 1], g.notice[i], sizeof(g.notice[0]));
    --g.notices;
}

bool room_take_toast(char* out, size_t cap) {
    if (!g.toast_set) return false;
    strncpy_s(out, cap, g.toast, _TRUNCATE);
    g.toast_set = false;
    return true;
}

} // namespace mgmp
