// The enemy-durability rule (mgmp_balance.cpp) on a fake Character in memory: which factions are enemies, the
// maximum-health arithmetic (4*constitution + bonus), the rebuild base, armor, initial health, and "never twice".
#include "../src/session/mgmp_balance.cpp"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

using namespace mgmp;
static unsigned checks = 0;
#define CHECK(x) do { ++checks; if(!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while(0)

namespace mgmp {
const Config& config() { static Config c; return c; }
bool mem_read(const void* src, void* dst, size_t n) { memcpy(dst, src, n); return true; }
bool mem_write(void* dst, const void* src, size_t n) { memcpy(dst, src, n); return true; }
void log_line(const char*, const char* fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); }
}

namespace {
alignas(16) uint8_t chr[0x1000];
int32_t& F(uintptr_t off) { return *(int32_t*)(chr + off); }
// A character as the loader leaves it: the definition said health H, constitution CON, shield S.
void make(int32_t faction, int32_t con, int32_t health, int32_t shield, int32_t hp = 0x3fffffff) {
    memset(chr, 0, sizeof(chr));
    F(kChr_Faction) = faction; F(kChr_BaseCon) = con;
    F(kChr_HpBonus) = health - 4 * con; F(kChr_HpBonusBase) = health - 4 * con;
    F(kChr_Shield) = shield; F(kChr_Hp) = hp;
}
int32_t max_hp(int32_t con_total) { return 4 * con_total + F(kChr_HpBonus); }
}

int main() {
    printf("-- factions --\n");
    CHECK(!balance_is_enemy_faction(0));      // none: rocks, walls, bombs
    CHECK(!balance_is_enemy_faction(1));      // allies: the player's cats and familiars
    CHECK(balance_is_enemy_faction(2));       // enemies
    CHECK(!balance_is_enemy_faction(3));      // solitary
    CHECK(!balance_is_enemy_faction(5));      // allies_to_all
    CHECK(!balance_is_enemy_faction(6));      // third party
    CHECK(!balance_is_enemy_faction(7));      // birds
    for (int f = 8; f <= 12; ++f) CHECK(balance_is_enemy_faction(f));
    CHECK(!balance_is_enemy_faction(13) && !balance_is_enemy_faction(-1) && !balance_is_enemy_faction(0x16));

    printf("-- an ordinary enemy: Rat, constitution 5, health 5 --\n");
    make(2, 5, 5, 0);
    CHECK(max_hp(5) == 5);
    CHECK(balance_scale_enemy(chr, 0x3fffffff));
    CHECK(max_hp(5) == 10);
    CHECK(F(kChr_HpBonusBase) == F(kChr_HpBonus));                  // the rebuild base moved with it
    CHECK(F(kChr_Hp) == 0x3fffffff);                                // unset stays unset: the game clamps it to 10 itself

    printf("-- a boss: health 300, shield 50 --\n");
    make(8, 5, 300, 50);
    balance_scale_enemy(chr, 0x3fffffff);
    CHECK(max_hp(5) == 600 && F(kChr_Shield) == 100);
    printf("-- an odd health keeps its exact double (no rounding) --\n");
    make(2, 5, 67, 0);
    balance_scale_enemy(chr, 0x3fffffff);
    CHECK(max_hp(5) == 134);

    printf("-- armor only: health 0, shield 100 -> still no health, armor 200 --\n");
    make(2, 5, 0, 100);
    balance_scale_enemy(chr, 0x3fffffff);
    CHECK(max_hp(5) == 0 && F(kChr_Shield) == 200);                  // 4*5 + (-20) = 0 -> the game's max(1, ..) makes it 1

    printf("-- no health key at all: constitution alone gives the maximum --\n");
    make(2, 5, 20, 0); F(kChr_HpBonus) = F(kChr_HpBonusBase) = 0;
    balance_scale_enemy(chr, 0x3fffffff);
    CHECK(max_hp(5) == 40);

    printf("-- initial_health: written by the loader, so doubled once --\n");
    make(2, 5, 100, 0, 30);
    CHECK(balance_scale_enemy(chr, 0x3fffffff) && F(kChr_Hp) == 60);
    printf("-- ... but a health that was already there before the loader is not doubled again --\n");
    make(2, 5, 100, 0, 30);
    balance_scale_enemy(chr, 30);
    CHECK(F(kChr_Hp) == 30);

    printf("-- not enemies: untouched --\n");
    for (int f : { 0, 1, 3, 6, 7 }) {
        make(f, 5, 40, 10);
        CHECK(!balance_scale_enemy(chr, 0x3fffffff));
        CHECK(max_hp(5) == 40 && F(kChr_Shield) == 10 && F(kChr_HpBonusBase) == 40 - 20);
    }

    printf("-- nonsense is left alone --\n");
    make(2, 5000, 40, 10);
    CHECK(!balance_scale_enemy(chr, 0));
    CHECK(!balance_scale_enemy(nullptr, 0));

    printf("-- the ruleset id the handshake compares --\n");
    CHECK(balance_ruleset_id() == balance_ruleset_id() && balance_ruleset_id() != 0);

    printf("-- the multiplier is a constant of the rule --\n");
    CHECK(kEnemyScale == 2);

    printf("OK %u checks\n", checks);
    return 0;
}
