// mgmp_progressguard.h -- THE SAVE'S PROGRESS, FLOORED AT THE FILE (0.1.130).
//
// Reports, more than once: after playing with a friend a save had lost progress -- an area route locked (the Ice Age), the warehouse's upgrade level and money lower --
// and it stayed lost in single player too. 0.1.124 closed the paths that were found (the property setter's translation, the restore's merge); this module is the last
// line behind them, and it does not depend on knowing the path: the save FILE itself is compared with what it held when this player joined a room.
//
//   * At the first look in a room the save file is read (the player's own data, written before any room answer could reach it): every progress property
//     (unlocks_progress_key: flags, unlocks, upgrades -- only ever up), the seven unlock lists of the `unlocks` blob (classes, abilities, passives, items, bosses,
//     radios, level groups) and the house's unlocked rooms (`house_unlocks`). That is the floor.
//   * Every time the game writes the file (its write time moves), the file is read again. A progress property lower than the floor, or a name the floor's lists
//     have and the file's do not, is put back INTO THE FILE (one transaction; the file before it is kept beside the save, newest 5) and, for a property, into the
//     game's own copy as well (the setter), so the next save writes it right. The floor grows with every look (the union), never shrinks.
//   * Money and food go down in normal play, so they are not floored; a change that makes them EXACTLY the host's number is logged (PROGRESS ... the host's).
//   * The guard keeps watching the save after the room is gone, for the rest of the process: the game's own copy of an unlock list cannot be fixed in place, so
//     every later write is checked too. A save whose owner changed, or that lost half of everything at once (a new game in the slot), starts a new floor.
//
// Logs: PROGRESS (grep it). The snapshot line, one line per write that changed something (gains, losses, money/food with the host's), and an Error line for
// every repair naming each key and list entry put back.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace mgmp {
namespace pg {

constexpr int kUnlockCats = 7;
extern const char* const kUnlockCatNames[kUnlockCats];

struct Unlocks { bool ok = false; uint32_t ver = 0; std::vector<std::vector<std::string>> cats; };
struct House   { bool ok = false; uint32_t ver = 0; std::string current; std::vector<std::vector<std::string>> lists; };
struct Snap {
    bool ok = false;
    std::map<std::string, int64_t> props;        // the progress properties (integer rows)
    std::string owner;                           // owner_steamid
    int64_t gold = 0, food = 0;
    bool have_gold = false, have_food = false;
    Unlocks un;
    House hu;
};
struct Loss {
    std::vector<std::pair<std::string, int64_t>> props;    // key -> the floor's value
    std::vector<std::pair<int, std::string>> unlocks;      // category -> name
    std::vector<std::pair<int, std::string>> house;        // list -> name
    bool any() const { return !props.empty() || !unlocks.empty() || !house.empty(); }
};

bool parse_unlocks(const uint8_t* p, size_t n, Unlocks& out);
std::vector<uint8_t> write_unlocks(const Unlocks& u);
bool parse_house(const uint8_t* p, size_t n, House& out);
std::vector<uint8_t> write_house(const House& h);
bool read_save(const wchar_t* path, Snap& out, std::string& why);
void compute_loss(const Snap& floor, const Snap& cur, Loss& out);
void apply_loss(Snap& cur, const Loss& loss);                   // what the file holds after the repair
bool write_fix(const wchar_t* path, const Snap& fixed, const Loss& loss, std::string& why);
void merge_floor(Snap& floor, const Snap& cur);
bool looks_like_another_save(const Snap& floor, const Snap& cur, std::string& why);
size_t unlock_total(const Snap& s);

} // namespace pg

void progressguard_tick();                 // ~1 s: arms in a room, checks the save file whenever the game has written it
void progressguard_test_reset();           // tests: forget everything
void progressguard_test_force_next();      // tests: the next tick runs now

} // namespace mgmp
