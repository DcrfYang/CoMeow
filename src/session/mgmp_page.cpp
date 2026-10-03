#include "mgmp_page.h"
#include "mgmp_i18n.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

#include "mgmp_addresses.h"
#include "mgmp_leave.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_setup.h"

namespace mgmp {
namespace {

// How long a button counts as "up" after its last update. The same quantity, and
// the same reason, as mgmp_leave's kChapterFrames: a screen is present while its
// buttons tick, and a screen nobody is updating is a screen nobody is on.
constexpr ULONGLONG kLiveMs = 400;

// The poll. The scene list changes when a person clicks something, so twice a
// second is far finer than the event being watched.
constexpr ULONGLONG kPollMs = 250;

// A page that has not changed is still re-sent this often: a peer that joined
// mid-session, or a message lost between two clients in a 3/4-player room, must
// not leave someone's page stuck at whatever they last heard.
constexpr ULONGLONG kHeartbeatMs = 2000;

struct Seen { char name[40]; ULONGLONG ms; };
Seen      g_seen[96];
int       g_seen_n = 0;

PageState g_self     = PageState::Unknown;
PageState g_last_sent = PageState::Unknown;
ULONGLONG g_last_send = 0;
ULONGLONG g_next_poll = 0;

// Per peer id, as told to us. Unknown means "nobody has told me yet".
PageState g_peer[kMaxPeers]    = {};
ULONGLONG g_peer_ms[kMaxPeers] = {};
ULONGLONG g_peer_stamp[kMaxPeers] = {};

bool starts(const char* s, const char* p) { return strncmp(s, p, strlen(p)) == 0; }

void note(const char* name, ULONGLONG t) {
    for (int i = 0; i < g_seen_n; ++i)
        if (strcmp(g_seen[i].name, name) == 0) { g_seen[i].ms = t; return; }
    if (g_seen_n >= 96) return;
    strncpy_s(g_seen[g_seen_n].name, name, _TRUNCATE);
    g_seen[g_seen_n++].ms = t;
}

bool live(const char* name) {
    const ULONGLONG t = GetTickCount64();
    for (int i = 0; i < g_seen_n; ++i)
        if (strcmp(g_seen[i].name, name) == 0) return t - g_seen[i].ms < kLiveMs;
    return false;
}

// Any name under the prefix, ticking. Used for the gear slots, which are named
// per slot (trinket / weapon / head / face / neck / modifier) and whose set is
// the game's to change.
bool live_prefix(const char* prefix) {
    const ULONGLONG t = GetTickCount64();
    for (int i = 0; i < g_seen_n; ++i)
        if (starts(g_seen[i].name, prefix) && t - g_seen[i].ms < kLiveMs) return true;
    return false;
}

PageState detect() {
    // 1/2: the two screens the scene list names outright.
    const MenuScreen sc = leave_menu_screen();
    if (sc == MenuScreen::SaveSelect) return PageState::SaveSlots;
    if (sc == MenuScreen::MainMenu)   return PageState::MainMenu;

    // A gear-screen lock that is being held for the room (a player without chapter 2: the game has no chapter page for them):
    // for the room that player is on the chapter page, whatever the scenes underneath say.
    if (setup_no_chapter_pending()) return PageState::Chapter;

    // 3/4: the two gear screens, which are panels rather than scenes. The collar
    // has its own button and is asked first: it is the first of the two, and the
    // whole point of naming it separately is that the panel it belongs to is a
    // different screen from the gear one.
    if (live(kBtnName_InventoryCollar))                    return PageState::Collar;
    if (live_prefix("InventoryButton_") || live_prefix("EquippedButton_"))
        return PageState::Equipment;

    // 5: the chapter page, by its own buttons. Asked of THIS module's table rather
    // than of leave_at_chapter_page, whose heartbeat is counted in leave_pump -- see
    // the note on that function. Same handle, same reason: the page is identified by
    // the two difficulty buttons ticking.
    if (live(kBtnName_ChapterLower) || live(kBtnName_ChapterRaise)) return PageState::Chapter;

    // 6: the warehouse, where a run has not started.
    if (leave_scene_live(kScene_House)) return PageState::House;

    // 7: an adventure. A resumed save loads straight onto the map, which is why
    // this is asked after everything else rather than before it.
    if (leave_scene_live(kScene_Map)) return PageState::InGame;

    // A cutscene, a transition, or a scene list that did not read.
    return PageState::Unknown;
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

} // namespace

const char* page_name(PageState p) {
    switch (p) {
        case PageState::MainMenu:  return tr(Tx::PAGE_MAIN);
        case PageState::SaveSlots: return tr(Tx::PAGE_SAVES);
        case PageState::House:     return tr(Tx::PAGE_HOUSE);
        case PageState::Collar:    return tr(Tx::PAGE_COLLAR);
        case PageState::Equipment: return tr(Tx::PAGE_GEAR);
        case PageState::Chapter:   return tr(Tx::PAGE_CHAPTER);
        case PageState::InGame:    return tr(Tx::PAGE_INGAME);
        default:                   return tr(Tx::UNKNOWN);
    }
}

void page_on_button(void* button) {
    if (!button) return;
    char name[64];
    if (!mem_read_std_string((const uint8_t*)button + kBtn_Name, name, sizeof(name))) return;
    if (!name[0]) return;
    // Only the names this module can ask about are kept: the detour runs for every
    // button in the game, and the table is bounded.
    if (!starts(name, "InventoryButton_") && !starts(name, "EquippedButton_") &&
        !starts(name, "CatSelector_") && !starts(name, "Button_LowerDifficulty"))
        return;
    note(name, GetTickCount64());
}

void page_reset() {
    g_seen_n = 0;
    g_self = PageState::Unknown;
    g_last_sent = PageState::Unknown;
    g_last_send = 0;
    g_next_poll = 0;
    for (int i = 0; i < kMaxPeers; ++i) {
        g_peer[i] = PageState::Unknown;
        g_peer_ms[i] = 0;
        g_peer_stamp[i] = 0;
    }
}

PageState page_self() { return g_self; }

PageState page_of(uint8_t peer) {
    return peer < kMaxPeers ? g_peer[peer] : PageState::Unknown;
}

uint32_t page_age_ms(uint8_t peer) {
    if (peer >= kMaxPeers || !g_peer_stamp[peer]) return 0;
    return (uint32_t)(GetTickCount64() - g_peer_stamp[peer]);
}

void page_on_message(uint8_t from, uint8_t page) {
    if (from >= kMaxPeers || page > (uint8_t)PageState::InGame) return;
    const PageState p = (PageState)page;
    if (p != g_peer[from])
        log_line("PAGE", "peer %u is on '%s'", (unsigned)from, page_name(p));
    g_peer[from]      = p;
    g_peer_ms[from]   = GetTickCount64();
    g_peer_stamp[from] = g_peer_ms[from];
}

void page_tick() {
    const ULONGLONG t = GetTickCount64();
    if (t < g_next_poll) return;
    g_next_poll = t + kPollMs;

    const PageState now = detect();
    if (now != g_self) {
        if (now != PageState::Unknown)
            log_line("PAGE", "this peer is on '%s'", page_name(now));
        g_self = now;
    }

    if (!session_live()) return;
    const bool changed  = g_self != g_last_sent;
    const bool heartbeat = g_self != PageState::Unknown && t - g_last_send > kHeartbeatMs;
    if (!changed && !heartbeat) return;
    if (g_self == PageState::Unknown && !changed) return;   // do not heartbeat "unknown"

    PageMsg m;
    m.page = (uint8_t)g_self;
    if (net_send_page(m)) {
        g_last_sent = g_self;
        g_last_send = t;
    }
}

} // namespace mgmp
