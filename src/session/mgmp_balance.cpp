// mgmp_balance.cpp -- see mgmp_balance.h.
#include "mgmp_balance.h"
#include "mgmp_config.h"
#include "mgmp_addresses.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"

namespace mgmp {

namespace {
// The health the game uses while a character has not been given its own: anything above this is "unset" and is
// clamped to the maximum by the first recompute.
constexpr int32_t kUnsetHp = 100000000;
unsigned g_logged = 0;
} // namespace

uint64_t balance_ruleset_id() {
    uint64_t h = 0xcbf29ce484222325ull;
    const uint32_t parts[] = { (uint32_t)kEnemyScale, config().test_weaken ? 1u : 0u, 0x4d47u /* 'MG' ruleset v1 */ };
    for (uint32_t p : parts) for (int i = 0; i < 4; ++i) { h ^= (p >> (8 * i)) & 0xFF; h *= 0x100000001b3ull; }
    return h;
}

bool balance_is_enemy_faction(int32_t f) {
    return f == 2 || (f >= 8 && f <= 12);
}

bool balance_scale_enemy(void* chr, int32_t hp_before, uint32_t scale_half) {
    if (!chr) return false;
    const uint8_t* c = (const uint8_t*)chr;
    int32_t faction = 0, con = 0, bonus = 0, base = 0, shield = 0, hp = 0;
    if (!mem_read(c + kChr_Faction, &faction, 4) || !balance_is_enemy_faction(faction)) return false;
    if (!mem_read(c + kChr_BaseCon, &con, 4) || !mem_read(c + kChr_HpBonus, &bonus, 4) ||
        !mem_read(c + kChr_HpBonusBase, &base, 4) || !mem_read(c + kChr_Shield, &shield, 4) ||
        !mem_read(c + kChr_Hp, &hp, 4))
        return false;
    // A sane character only: a wild value here means the layout is not what this was written against.
    if (con < 0 || con > 1000 || bonus < -100000 || bonus > 100000 || base < -100000 || base > 100000) return false;

    const int64_t h = scale_half < 4 ? 4 : scale_half > 16 ? 16 : scale_half;   // the multiplier is h / 2
    // x * h / 2, rounded to the nearest whole number (a .5 upward; floor division, so a negative x rounds the same way)
    const auto mul = [h](int64_t x) -> int64_t { const int64_t t = x * h + 1; return t >= 0 ? t / 2 : -((-t + 1) / 2); };
    // maximum = 4*con + bonus  ->  scaled  =>  bonus' = scaled maximum - 4*con
    const int64_t c4 = 4 * (int64_t)con;
    const int32_t bonus2 = (int32_t)(mul(c4 + bonus) - c4);
    const int32_t base2  = (int32_t)(mul(c4 + base) - c4);
    int32_t shield2 = shield;
    if (shield > 0 && shield < 100000) shield2 = (int32_t)mul(shield);
    int32_t hp2 = hp;
    if (hp > 0 && hp < kUnsetHp && hp != hp_before) hp2 = (int32_t)mul(hp);   // initial_health, written by the loader

    bool ok = mem_write((uint8_t*)chr + kChr_HpBonus, &bonus2, 4) && mem_write((uint8_t*)chr + kChr_HpBonusBase, &base2, 4);
    if (shield2 != shield) ok = mem_write((uint8_t*)chr + kChr_Shield, &shield2, 4) && ok;
    if (hp2 != hp) ok = mem_write((uint8_t*)chr + kChr_Hp, &hp2, 4) && ok;
    if (g_logged < 400) {
        ++g_logged;
        log_line("BALANCE", "enemy x%.1f: faction %d con %d  health bonus %d -> %d  armor %d -> %d  initial hp %d -> %d%s",
                 h / 2.0, faction, con, bonus, bonus2, shield, shield2, hp, hp2, ok ? "" : "  (WRITE FAILED)");
    }
    return ok;
}

} // namespace mgmp
