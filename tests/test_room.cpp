// The room layer (mgmp_room.cpp) against a fake lobby, a fake transport, fake pages and a fake House in
// memory: the lock gate, pairing rows with session ids by name, the dropped-player notices, and the solo
// house-boss round trip. One peer's view at a time -- the other peers exist only as what this one sees.
#include "../src/session/mgmp_room.cpp"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

using namespace mgmp;
static unsigned checks = 0;
#define CHECK(x) do { ++checks; if(!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while(0)

namespace {
// lobby
SignalPeer lobby[8]; uint32_t lobby_n = 0;
char room_id[16] = "ROOM", my_name[32] = "alice", my_role[8] = "host";
bool locked = false; int lock_asked = -1; int away_said = -1; bool fight_up = false;
// transport
bool active = true; uint8_t ids[4] = {0, 1}; uint8_t nids = 2, me = 0;
std::vector<RoomCtlMsg> sent;
// session requests
int asked_host = 0, asked_join = 0, asked_disc = 0;
// pages and screens
PageState self_page = PageState::SaveSlots; PageState pages[4] = {};
MenuScreen screen = MenuScreen::SaveSelect;
int requested_slot = -1;
// checkpoint
int aborts = 0; bool round_open = false;
// house memory: MewDirector* at a fake global, House* at +0x5A8, the string size at +0x468, countdown +0x478
alignas(16) uint8_t house[0x500] = {};
alignas(16) uint8_t director[0x600] = {};
void* director_ptr = nullptr;
// save files: path -> (boss name, countdown)
std::map<std::wstring, std::pair<std::string, std::string>> saves;

void set_house(bool today, uint64_t name_len = 7) {
    const uint64_t n = today ? name_len : 0; int32_t c = today ? 0 : 5;
    memcpy(house + 0x468, &n, 8); memcpy(house + 0x478, &c, 4);
}
void peers(std::initializer_list<const char*> names) {
    lobby_n = 0;
    for (const char* n : names) { SignalPeer p; strncpy_s(p.name, n, _TRUNCATE); strcpy_s(p.role, lobby_n ? "client" : "host"); lobby[lobby_n++] = p; }
}
void say_name(uint8_t from, const char* n) { RoomCtlMsg m; m.kind = kRoomName; strncpy_s(m.name, n, _TRUNCATE); room_on_message(from, m); }
std::string toast_text() { char t[160]; return room_take_toast(t, sizeof(t)) ? std::string(t) : std::string(); }
std::string notice_text() { char t[320]; return room_notice(t, sizeof(t)) ? std::string(t) : std::string(); }
void fresh() {
    g = State{}; sent.clear(); aborts = 0; round_open = false; asked_host = asked_join = asked_disc = 0; requested_slot = -1;
    locked = false; lock_asked = away_said = -1; active = true; me = 0; nids = 2; ids[0] = 0; ids[1] = 1;
    strcpy_s(my_name, "alice"); strcpy_s(my_role, "host"); peers({ "alice", "bob" });
    self_page = PageState::SaveSlots; for (auto& p : pages) p = PageState::SaveSlots; screen = MenuScreen::SaveSelect;
    set_house(false);
}
} // namespace

namespace mgmp {
uint32_t signal_peers(SignalPeer* out, uint32_t cap) { uint32_t n = lobby_n < cap ? lobby_n : cap; for (uint32_t i = 0; i < n; ++i) out[i] = lobby[i]; return n; }
const char* signal_room() { return room_id; }
const char* signal_name() { return my_name; }
const char* signal_role() { return my_role; }
const char* signal_host_addr() { return "10.0.0.1"; }
uint16_t signal_host_port() { return 27600; }
bool signal_room_locked() { return room_id[0] && locked; }
void signal_request_lock(bool on) { lock_asked = on ? 1 : 0; }
void signal_set_away(bool on) { away_said = on ? 1 : 0; for (uint32_t i = 0; i < lobby_n; ++i) if (!strcmp(lobby[i].name, my_name)) lobby[i].away = on; }
bool net_active() { return active; }
bool lockstep_fight_up() { return fight_up; }
bool net_peer_ids(uint8_t* out, uint8_t cap) { if (!active || cap < nids) return false; memcpy(out, ids, nids); return true; }
uint8_t net_peer_count() { return active ? nids : 0; }
uint8_t net_self() { return me; }
bool net_send_roomctl(const RoomCtlMsg& m) { sent.push_back(m); return true; }
void session_request_host(uint16_t) { ++asked_host; }
void session_request_join(const char*, uint16_t) { ++asked_join; }
void session_request_disconnect() { ++asked_disc; }
PageState page_self() { return self_page; }
const char* page_name(PageState p) { return p == PageState::Collar ? "collar" : p == PageState::Equipment ? "gear" : p == PageState::Chapter ? "chapter" : "other"; }
uint32_t cats_chosen = 0;                                   // what the panel's own cards say this peer brings
uint32_t catview_self(const CatBrief** out) { if (out) *out = nullptr; return cats_chosen; }
uint32_t catview_of(uint8_t, const CatBrief** out) { if (out) *out = nullptr; return 0; }
PageState page_of(uint8_t id) { return id < 4 ? pages[id] : PageState::Unknown; }
MenuScreen leave_menu_screen() { return screen; }
void menu_request_slot(int s) { requested_slot = s; }
bool checkpoint_abort_round() { ++aborts; round_open = false; return true; }
bool checkpoint_round_open() { return round_open; }
void checkpoint_stage_tick(uint8_t) {}
uintptr_t addr_of_data(DataSym d) { return d == D_MewDirectorPtr ? (uintptr_t)&director_ptr : 0; }
bool mem_read(const void* src, void* dst, size_t n) { memcpy(dst, src, n); return true; }
const Config& config() { static Config c; return c; }
void log_line(const char*, const char* fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); }
namespace checkpoint_io {
bool read_property(const std::wstring& path, const char* key, bool& have, std::string& out) {
    auto it = saves.find(path); have = false;
    if (it == saves.end()) return true;
    have = true; out = !strcmp(key, "next_house_boss") ? it->second.first : it->second.second;
    return true;
}
}
}

int main() {
    director_ptr = director;
    void* hp = house; memcpy(director + 0x5A8, &hp, 8);
    saves[L"boss.sav"] = { "hitler3", "0" };
    saves[L"soon.sav"] = { "hitler3", "3" };
    saves[L"plain.sav"] = { "", "0" };

    printf("-- the lock gates every pick --\n");
    fresh();
    CHECK(room_on_slot_click(0, L"plain.sav"));
    CHECK(toast_text() == "先锁定房间后再选择存档");
    strcpy_s(my_role, "client"); me = 1;
    CHECK(room_on_slot_click(0, L"plain.sav"));
    CHECK(toast_text() == "等待房主锁定房间后再选择存档");
    locked = true;
    CHECK(!room_on_slot_click(0, L"plain.sav"));
    CHECK(!room_on_slot_click(0, L"soon.sav"));            // the boss is days away: an ordinary pick
    nids = 1; ids[0] = 1;                                    // a room member whose session is not up
    CHECK(room_on_slot_click(0, L"plain.sav"));
    CHECK(toast_text() == "等待所有玩家连接后再选择存档");
    active = false;
    CHECK(room_on_slot_click(0, L"plain.sav"));             // in a room, never alone by accident
    room_id[0] = 0; CHECK(!room_on_slot_click(0, L"plain.sav")); strcpy_s(room_id, "ROOM");  // no room: untouched

    printf("-- unlock: only from the menus, and it starts the round over --\n");
    fresh(); locked = true;
    CHECK(room_is_host() && room_can_unlock());
    pages[1] = PageState::House;
    CHECK(!room_can_unlock());
    room_request_lock(false);
    CHECK(lock_asked == -1 && toast_text() == "仅当所有玩家都在主菜单或存档页面时才能解锁房间");
    pages[1] = PageState::MainMenu; self_page = PageState::Equipment; CHECK(!room_can_unlock());
    self_page = PageState::MainMenu; CHECK(room_can_unlock());
    room_request_lock(false); CHECK(lock_asked == 0);
    room_tick(); locked = false; room_tick();
    CHECK(aborts == 1);

    printf("-- rows are paired with ids by name, whatever order the ids came back in --\n");
    fresh();
    peers({ "alice", "bob", "carol" }); nids = 3; ids[0] = 0; ids[1] = 1; ids[2] = 2;
    // bob reconnected and is now id 2; carol kept 1
    say_name(2, "bob"); say_name(1, "carol");
    CHECK(room_peer_for_row(0) == kNoPeer);                 // us
    CHECK(room_peer_for_row(1) == 2 && room_peer_for_row(2) == 1);
    CHECK(room_row_of_peer(2) == 1 && room_row_of_peer(1) == 2 && room_self_row() == 0);
    // unnamed ids fall back to ascending order over the rows still free
    fresh(); peers({ "alice", "bob", "carol" }); nids = 3; ids[0] = 0; ids[1] = 1; ids[2] = 3;
    CHECK(room_peer_for_row(1) == 1 && room_peer_for_row(2) == 3);
    // a row that is away gets no id
    lobby[1].away = true;
    CHECK(room_peer_for_row(1) == kNoPeer && room_peer_for_row(2) == 1);
    // the names go out
    fresh(); room_tick();
    CHECK(!sent.empty() && sent.back().kind == kRoomName && !strcmp(sent.back().name, "alice"));

    printf("-- a player who drops mid-game is announced once, by P number --\n");
    fresh(); peers({ "alice", "bob", "carol" }); nids = 3; ids[0] = 0; ids[1] = 1; ids[2] = 2;
    say_name(1, "bob"); say_name(2, "carol");
    self_page = PageState::InGame; pages[1] = pages[2] = PageState::InGame;
    room_tick();
    nids = 2;                                                  // carol (id 2, P3) is gone
    room_tick();
    CHECK(notice_text() == "P3玩家掉线，如果要继续游戏，请所有玩家回到主菜单重新开始游戏->选择存档并进入游戏");
    room_notice_dismiss(); CHECK(notice_text().empty());
    room_tick(); CHECK(notice_text().empty());               // once
    // then bob heads back to the main menu, as the notice said: that is not another drop
    pages[1] = PageState::MainMenu; room_tick(); CHECK(notice_text().empty());
    // going back to the main menu mid-game counts as dropping out
    fresh(); self_page = PageState::House; pages[1] = PageState::House; say_name(1, "bob");
    room_tick(); pages[1] = PageState::MainMenu; room_tick();
    CHECK(notice_text() == "P2玩家掉线，如果要继续游戏，请所有玩家回到主菜单重新开始游戏->选择存档并进入游戏");
    // dropped in the middle of a fight: the notice adds the sentence about the house boss; outside a fight it does not
    fresh(); peers({ "alice", "bob" }); nids = 2; say_name(1, "bob"); self_page = PageState::InGame; pages[1] = PageState::InGame;
    room_tick(); fight_up = true; nids = 1; room_tick(); fight_up = false;
    CHECK(notice_text() == "P2玩家掉线，如果要继续游戏，请所有玩家回到主菜单重新开始游戏->选择存档并进入游戏 放心吧，本模组已经让史蒂文“下班了”。");
    // nothing is said on the menus
    fresh(); say_name(1, "bob"); room_tick(); nids = 1; room_tick(); CHECK(notice_text().empty());

    printf("-- the house boss: picked on its day, played alone, back in the room after --\n");
    fresh(); locked = false;                                   // not even locked: the boss day comes first
    strcpy_s(my_role, "client"); me = 1; say_name(0, "alice");
    strcpy_s(my_name, "bob");
    CHECK(room_on_slot_click(2, L"boss.sav"));
    CHECK(away_said == 1 && room_solo_active() && !notice_text().empty());
    room_notice_dismiss();
    room_tick(); CHECK(asked_disc == 0);                      // the room hears "away" first
    g.solo_at -= 5000; room_tick(); CHECK(asked_disc == 1);
    active = false; room_tick();
    CHECK(requested_slot == 2);                               // the load, now that it is alone
    CHECK(!room_on_slot_click(2, L"boss.sav"));              // ...and that click goes through
    // the warehouse on the day, then the fight, then the warehouse with no boss due
    screen = MenuScreen::House; set_house(true); room_tick();
    screen = MenuScreen::Other; room_tick();
    screen = MenuScreen::House; set_house(false); room_tick();
    CHECK(notice_text().empty());                             // not yet: it has to hold
    g.solo_clear_since -= 5000; room_tick();
    CHECK(notice_text() == "家园Boss战已结束。请回到主菜单并重新选择存档，与其他玩家继续联机。");
    room_notice_dismiss();
    screen = MenuScreen::MainMenu; room_tick();
    CHECK(!room_solo_active() && away_said == 0 && asked_join == 1);
    // a House still being built never ends the solo early
    fresh(); strcpy_s(my_name, "bob"); me = 1; strcpy_s(my_role, "client");
    CHECK(room_on_slot_click(0, L"boss.sav")); room_notice_dismiss(); g.solo_at -= 5000; room_tick(); active = false; room_tick();
    screen = MenuScreen::House; set_house(false); room_tick(); g.solo_clear_since -= 5000; room_tick();
    CHECK(room_solo_active() && notice_text().empty());
    // away, back on the menu without having finished: another save means "I am done", and rejoins
    screen = MenuScreen::SaveSelect;
    CHECK(room_on_slot_click(1, L"plain.sav"));
    CHECK(!room_solo_active() && asked_join == 1 && !toast_text().empty());

    printf("-- the boss day found in a co-op warehouse --\n");
    fresh(); self_page = PageState::House; screen = MenuScreen::House; set_house(true);
    room_tick();
    CHECK(room_solo_active() && away_said == 1);
    // and the others are told it is a boss fight, not a crash
    fresh(); peers({ "alice", "bob" }); say_name(1, "bob"); self_page = PageState::House; pages[1] = PageState::House;
    room_tick(); lobby[1].away = true; room_tick(); nids = 1; room_tick();
    CHECK(notice_text().find("P2 今天需要单人进行家园Boss战") == 0);

    printf("-- a client dials a host that comes back from its boss --\n");
    fresh(); strcpy_s(my_role, "client"); strcpy_s(my_name, "bob"); me = 1; screen = MenuScreen::MainMenu;
    lobby[0].away = true; room_tick();
    active = false; lobby[0].away = false; room_tick();
    CHECK(asked_join == 1);

    printf("-- a save already past the departure box, with more cats than the room allows (2026-10-05) --\n");
    for (int players : { 3, 4 }) {
        const uint32_t limit = players == 4 ? 2u : 3u;
        for (PageState pg : { PageState::Collar, PageState::Equipment, PageState::Chapter }) {
            fresh(); nids = (uint8_t)players; self_page = PageState::House; cats_chosen = 0; Sleep(520); room_tick();   // leaving the setup pages forgets what was said
            self_page = pg; cats_chosen = limit + 1;
            Sleep(520); room_tick();
            CHECK(!notice_text().empty());                               // told to choose the save again
            room_notice_dismiss(); Sleep(520); room_tick(); CHECK(notice_text().empty());   // said once for this count
        }
        fresh(); nids = (uint8_t)players; self_page = PageState::House; cats_chosen = 0; Sleep(520); room_tick();
        self_page = PageState::Collar; cats_chosen = limit;    // within the limit: nothing
        Sleep(520); room_tick(); CHECK(notice_text().empty());
        fresh(); nids = (uint8_t)players; self_page = PageState::House; cats_chosen = limit + 1;  // not past the box yet: the box checks it
        Sleep(520); room_tick(); CHECK(notice_text().empty());
    }
    fresh(); nids = 2; self_page = PageState::Collar; cats_chosen = 4; Sleep(520); room_tick(); CHECK(notice_text().empty());   // two players: no limit
    cats_chosen = 0;
    // the readings the gear screen, the client's READY and the host's choice use
    fresh(); nids = 4; self_page = PageState::Chapter; cats_chosen = 3;
    { uint32_t n = 0; CHECK(room_self_over_limit(&n) && n == 3); }
    cats_chosen = 2; CHECK(!room_self_over_limit());
    self_page = PageState::House; cats_chosen = 3; CHECK(!room_self_over_limit());
    cats_chosen = 0;

    printf("-- a second pick while one is open; the main menu drops the pick --\n");
    fresh(); locked = true; round_open = true;
    CHECK(room_on_slot_click(1, L"plain.sav"));
    CHECK(toast_text().find("重新选择存档") != std::string::npos);
    room_tick(); CHECK(aborts == 0);
    screen = MenuScreen::MainMenu; room_tick();
    CHECK(aborts == 1 && sent.back().kind == kRoomAbort);
    CHECK(!room_on_slot_click(1, L"plain.sav"));

    printf("-- the party limit at the House's door: 2 players 4, 3 players 3, 4 players 2 --\n");
    fresh();
    CHECK(room_party_limit() == 0 && room_depart_allowed(4));
    nids = 3; ids[2] = 2;
    CHECK(room_party_limit() == 3 && room_depart_allowed(3) && toast_text().empty());
    CHECK(!room_depart_allowed(4));
    CHECK(toast_text() == "当前为3人联机，每名玩家最多选择3只猫");
    nids = 4; ids[3] = 3;
    CHECK(room_party_limit() == 2 && room_depart_allowed(2) && !room_depart_allowed(3));
    CHECK(toast_text() == "当前为4人联机，每名玩家最多选择2只猫");
    active = false; CHECK(room_party_limit() == 0 && room_depart_allowed(4));   // alone: the game's own rules

    printf("-- a sync that ran out of time tells the player --\n");
    fresh(); room_sync_trouble("the host's board");
    CHECK(notice_text().find("连接") != std::string::npos && notice_text().find("同步") != std::string::npos);
    room_notice_dismiss(); CHECK(notice_text().empty());
    nids = 1; room_sync_trouble("the host's board"); CHECK(notice_text().empty());     // alone: nothing to be out of sync with
    fresh(); active = false; room_sync_trouble("the host's board"); CHECK(notice_text().empty()); active = true;

    printf("-- the text follows the game's language --\n");
    CHECK(i18n_set_lang_code("en") && i18n_lang() == kLangEn);
    fresh(); nids = 3; ids[2] = 2; CHECK(!room_depart_allowed(4));
    CHECK(toast_text() == "With 3 players, each player may bring at most 3 cats");
    CHECK(!strcmp(tr(Tx::R_LOCK_FIRST_HOST), "Lock the room before picking a save"));
    CHECK(!strcmp(tr_class(8), "Butcher") && !strcmp(tr_class(200), "Unknown"));
    CHECK(i18n_set_lang_code("ja") && i18n_lang() == kLangJa);
    CHECK(i18n_set_lang_code("pt-br") && i18n_lang() == kLangPt);
    CHECK(i18n_set_lang_code("sp") && i18n_lang() == kLangEs);
    CHECK(i18n_set_lang_code("xx") && i18n_lang() == kLangEn);            // unknown: English, as the game does
    CHECK(!i18n_set_lang_code("en"));                                      // no change
    for (int l = 0; l < kLangCount; ++l)
        for (int i = 0; i < (int)Tx::COUNT; ++i) CHECK(kTxTable[i][l] && kTxTable[i][l][0]);
    CHECK(i18n_set_lang_code("zh") && !strcmp(tr_class(8), "屠夫"));

    printf("-- a client may not set off with a cat that wears a main-story item --\n");
    fresh(); toast_text();
    CHECK(i18n_set_lang_code("en"));
    CHECK(!room_refuse_story_item("PutridLeech"));                 // refuses (returns false) ...
    { const std::string t = toast_text(); CHECK(t.find("PutridLeech") != std::string::npos); }   // ... and says which item
    CHECK(!room_refuse_story_item(nullptr));
    { const std::string t = toast_text(); CHECK(t.find("?") != std::string::npos); }
    CHECK(i18n_set_lang_code("zh")); CHECK(strstr(tr(Tx::R_STORY_ITEM), "%s"));

    printf("-- the one-line hints of the preparation phase --\n");
    CHECK(i18n_set_lang_code("en"));
    toast_text(); room_say_story_only_host();  { const std::string t = toast_text(); CHECK(t.find("host") != std::string::npos && t.find("quest") != std::string::npos); }
    room_say_chapter_client();                 { const std::string t = toast_text(); CHECK(t.find("host") != std::string::npos && t.find("chapter") != std::string::npos); }
    room_say_chapter_wait();                   { const std::string t = toast_text(); CHECK(t.find("ready") != std::string::npos); }
    CHECK(i18n_set_lang_code("zh"));
    room_say_chapter_wait();                   { const std::string t = toast_text(); CHECK(t.find("准备") != std::string::npos); }
    room_say_story_only_host();                { const std::string t = toast_text(); CHECK(t.find("仅host可选主线任务道具") != std::string::npos); }

    printf("-- start over --\n");
    fresh(); room_abort_selection();
    CHECK(aborts == 1 && sent.back().kind == kRoomAbort);
    { RoomCtlMsg m; m.kind = kRoomAbort; room_on_message(1, m); }
    CHECK(aborts == 2 && !toast_text().empty());

    printf("room: %u checks passed\n", checks);
    return 0;
}
