// mgmp_rngfix.cpp -- see mgmp_rngfix.h.
#include "mgmp_rngfix.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "mgmp_log.h"
#include "mgmp_lockstep.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_rng.h"
#include "mgmp_unlocks.h"

namespace mgmp {

namespace {
bool in_room() { return net_active() && net_peer_count() >= 2; }

// A per-battle line budget: the per-frame fences can fire every frame for a whole battle.
struct Budget {
    uint64_t battle = ~0ull;
    uint32_t used = 0;
    uint64_t total = 0;
    bool take(uint32_t cap) {
        const uint64_t b = lockstep_battle_id();
        if (b != battle) { battle = b; used = 0; }
        ++total;
        if (used >= cap) return false;
        ++used;
        return true;
    }
};
Budget g_b369, g_b102, g_b270;
} // namespace

void rngfix_event_keyword(const char* rid, const char* keyword) {
    if (!in_room()) return;
    log_line("EVENT", "keyword `%s` (%s): the shared stream is reset before it draws (2026-10-11 fix; it used to run on whatever the option drew before it)", keyword, rid);
    unlocks_event_roll();
}

// R369. The per-frame call recomputes a RELATIVE delayed trigger's tile from the caster's current position; when that lands off the board it
// re-picks a tile with an inline xoshiro on the shared stream -- every frame, so the number of draws follows the frame rate. Only the warning
// marker is drawn from this call; the trigger itself recomputes at OnTurnBegin (the other call site, left on the shared stream). Put back.
void* rngfix_delayed_target(void* self, void* out, fn_rngfix_2 orig) {
    uint64_t* s = in_room() ? rng_global_stream() : nullptr;
    if (!s) return orig(self, out);
    uint64_t before[4];
    memcpy(before, s, sizeof(before));
    void* r = orig(self, out);
    if (memcmp(before, s, sizeof(before)) != 0) {
        memcpy(s, before, sizeof(before));
        if (g_b369.take(6))
            log_line("RNGFIX", "R369 fence: DelayedTrigger's per-frame target re-pick moved the shared stream (an off-board relative target) -- put back (%llu this session)%s",
                     (unsigned long long)g_b369.total, g_b369.used == 6 ? "; the rest of this battle is not logged" : "");
    }
    return r;
}

// R102. Character late_update picks a random target for the AI intent display while the brain is undecided -- every frame, on the shared stream.
void* rngfix_pick_index(void* out, void* vec, void* stream, fn_rngfix_3 orig) {
    uint64_t* s = in_room() ? rng_global_stream() : nullptr;
    if (!s || stream != (void*)s) return orig(out, vec, stream);
    uint64_t before[4];
    memcpy(before, s, sizeof(before));
    void* r = orig(out, vec, stream);
    if (memcmp(before, s, sizeof(before)) != 0) {
        memcpy(s, before, sizeof(before));
        if (g_b102.take(3))
            log_line("RNGFIX", "R102 fence: the per-frame AI intent display drew from the shared stream -- put back (%llu this session; the research could not tell if this "
                     "happens during an action -- if this line shows up mid-battle, it did)", (unsigned long long)g_b102.total);
    }
    return r;
}

// R027. The vector is {u32, u32 count, Item** data}; an Item's name is the std::string at +8 (the build looks its GON up by it).
void rngfix_battle_backpack(void* vec) {
    if (!vec || !in_room()) return;
    uint32_t n = 0;
    void** data = nullptr;
    if (!mem_read((uint8_t*)vec + 4, &n, sizeof(n)) || !n) return;
    if (!mem_read((uint8_t*)vec + 8, &data, sizeof(data)) || !data || n > 4096) return;
    char names[600] = {};
    size_t w = 0;
    for (uint32_t i = 0; i < n && i < 16; ++i) {
        void* item = nullptr;
        char nm[64] = "?";
        if (mem_read(data + i, &item, sizeof(item)) && item) mem_read_std_string((uint8_t*)item + 8, nm, sizeof(nm));
        const int k = _snprintf_s(names + w, sizeof(names) - w, _TRUNCATE, "%s%s", w ? ", " : "", nm);
        if (k <= 0) break;
        w += (size_t)k;
    }
    const uint32_t zero = 0;
    if (!mem_write((uint8_t*)vec + 4, &zero, sizeof(zero))) return;
    // TODO(RNG-R027): ignored, not shared -- a CryogenicTimeChamber / ThrobbingGristle in a backpack does nothing in a room until the host sends its list.
    log_line("RNGFIX", "R027: the battle build would apply the global passives of %u item(s) in THIS player's backpack to the whole battle (%s%s) -- ignored in a room "
             "(each player's backpack is their own: the other peers would build the battle without them)", n, names, n > 16 ? ", ..." : "");
}

// R270, THE LEVEL'S NEXT WAVE (2026-10-11, desert111: the user's "every enemy came back once"). A level's placed objects carry a wave number (byte +6);
// the build spawns wave 0, and Level's per-frame update (slot 10, 0x1403611E0) spawns the next one -- Level+0x194 is the wave, +0x190 the last -- the
// frame it finds no kill_required enemy alive (0x140361020). That frame can fall in the middle of an action, at a different point of it on each
// peer, and the wave's draws (tiles, facings, picks) came from the shared stream there: the wave would land differently, and the rest of the action
// would draw from a moved stream. The wave now draws from a stream made of (battle, wave), and the shared stream is put back after it.
void* rngfix_wave(void* level, int32_t wave_arg, fn_rngfix_wave orig) {
    uint64_t* s = (in_room() && lockstep_active()) ? rng_global_stream() : nullptr;
    const uint64_t battle = lockstep_battle_id();
    if (!s && level && !in_room()) {   // single player: the stage runs as the game has it -- said, so a solo log shows when a level's wave came (2026-10-11)
        int32_t l = -1;
        mem_read((uint8_t*)level + 0x190, &l, sizeof(l));
        if (g_b270.take(12)) log_line("SOLO", "the level's extra-spawn stage runs: wave %d of %d (wave 0 is the build's own; a higher one spawns when no kill_required enemy is left)", wave_arg, l);
    }
    if (!s || !level || !battle) return orig(level, wave_arg);
    const int32_t wave = wave_arg;                               // the caller's own count (Level+0x194, already counted up)
    int32_t last = -1;
    mem_read((uint8_t*)level + 0x190, &last, sizeof(last));
    uint64_t saved[4];
    memcpy(saved, s, sizeof(saved));
    uint64_t x = battle ^ 0x6D676D7057415645ull ^ ((uint64_t)(uint32_t)wave * 0x9E3779B97F4A7C15ull);
    uint64_t st[4];
    for (int i = 0; i < 4; ++i) {
        uint64_t z = (x += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        st[i] = z ^ (z >> 31);
    }
    memcpy(s, st, sizeof(st));
    void* r = orig(level, wave_arg);
    memcpy(s, saved, sizeof(saved));
    if (g_b270.take(12))
        log_line("RNGFIX", "R270: the level's wave %d of %d spawned mid-battle (turn %u; no kill_required enemy was left) from a stream of its own %016llx (battle, wave) -- "
                 "the shared stream %016llx is put back", wave, last, (unsigned)lockstep_turn(), (unsigned long long)st[0], (unsigned long long)saved[0]);
    return r;
}

// MODELING CLAY (2026-10-11, the halt "cat 38 slot 4:4 holds 'wp_BentSpoon', peer sent 'wp_MeatCleaver'" after ten battles that agreed). The weapon
// "changes into a different weapon each time it's used": ModelingClayPassive's after-cast (slot 57 -> 0x140906F50) picks one from the
// combat_reward_hard pool (0x1408DEFC0, kind "weapon", the cat's luck) with draws made INLINE on the shared stream -- the ledger cannot see them,
// and it saw nothing differ in that battle (133 actions compared), nor did any unlock query. The pool is static data; what differed is where the
// shared stream stood when the throw landed (a lobbed projectile's landing is a frame, racing whatever else that action has in flight). The pick
// now draws from a stream of its own -- (battle, the cat's save id, the n-th change of that cat's clay in this battle), the same on every peer --
// and the shared stream is put back, so the change neither depends on the frame nor moves anything after it.
void* rngfix_clay(void* self, void* a, void* b, void* c, fn_rngfix_4 orig) {
    uint64_t* s = (in_room() && lockstep_active()) ? rng_global_stream() : nullptr;
    const uint64_t battle = lockstep_battle_id();
    if (!s || !self || !battle) return orig(self, a, b, c);
    uint64_t cat_id = 0;
    const void* chr = nullptr;
    const void* cat = nullptr;
    if (mem_read((const uint8_t*)self + 0x38, &chr, sizeof(chr)) && chr && mem_read((const uint8_t*)chr + 0x88, &cat, sizeof(cat)) && cat)
        mem_read((const uint8_t*)cat + 0xC48, &cat_id, sizeof(cat_id));
    static uint64_t s_battle = 0;
    static uint64_t s_id[16] = {};
    static uint32_t s_n[16] = {};
    if (s_battle != battle) { s_battle = battle; memset(s_id, 0, sizeof(s_id)); memset(s_n, 0, sizeof(s_n)); }
    uint32_t k = 0;
    while (k < 16 && s_id[k] && s_id[k] != cat_id) ++k;
    if (k == 16) k = 15;
    s_id[k] = cat_id;
    const uint32_t n = ++s_n[k];
    uint64_t saved[4];
    memcpy(saved, s, sizeof(saved));
    uint64_t x = battle ^ 0x6D676D70434C4159ull ^ (cat_id * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)n * 0xD6E8FEB86659FD93ull);
    uint64_t st[4];
    for (int i = 0; i < 4; ++i) {
        uint64_t z = (x += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        st[i] = z ^ (z >> 31);
    }
    memcpy(s, st, sizeof(st));
    void* r = orig(self, a, b, c);
    memcpy(s, saved, sizeof(saved));
    log_line("RNGFIX", "ModelingClay of cat %llx changed (#%u in this battle, turn %u) from a stream of its own %016llx -- the shared stream %016llx is put back",
             (unsigned long long)cat_id, n, (unsigned)lockstep_turn(), (unsigned long long)st[0], (unsigned long long)saved[0]);
    return r;
}

// The research's flagged draw sites that call a draw function directly (the ledger keys a draw by its return address; sites behind an inline
// helper are invisible to it). Generated from 批次NN.md + 汇总.csv, sorted by rva.
namespace {
struct SiteTag { uint32_t rva; const char* tag; };
const SiteTag kSites[] = {
    { 0x00426C3, "R050/verify" },   // randint in the four-direction helper: also the owner's native aim preview
    { 0x009CF2D, "R452/P1" },
    { 0x010F28C, "R457/P1" },
    { 0x0110390, "R001/P1" },
    { 0x011081D, "R001/P1" },
    { 0x0110912, "R001/P1" },
    { 0x0110EC1, "R001/P1" },
    { 0x0111257, "R001/P1" },
    { 0x0111517, "R001/P1" },
    { 0x019C4A6, "R142/P1" },
    { 0x02646AA, "R182/P1" },
    { 0x02B9F99, "R189/P1" },
    { 0x02C32E3, "R192/P1" },
    { 0x02F04DC, "R203/P1" },
    { 0x02F083D, "R204/P1" },
    { 0x033BF96, "R261/P1" },
    { 0x035B576, "R027/P0" },
    { 0x0769699, "R297/P1" },
    { 0x07CC49B, "R002/P1" },
    { 0x07D4F9E, "R336/P1" },
    { 0x0802A8C, "R488/P1" },
    { 0x092880E, "R439/verify" },
};
} // namespace

const char* rngfix_site_tag(uint32_t rva) {
    for (const SiteTag& t : kSites)
        if (t.rva == rva) return t.tag;
    return nullptr;
}

} // namespace mgmp
