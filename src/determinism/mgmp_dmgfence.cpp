// mgmp_dmgfence.cpp -- see mgmp_dmgfence.h.
#include "mgmp_dmgfence.h"

#include <windows.h>
#include <cstring>
#include <unordered_map>

#include "MinHook.h"
#include "mgmp_lockstep.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_rng.h"
#include "mgmp_tuning.h"

namespace mgmp {
namespace {

constexpr uintptr_t kFn_TakeDamage = 0x110120;
constexpr uint8_t   kHead[8] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x55 };
constexpr uintptr_t kDmg_Source = 0x00;       // DamageInstance: its source object (an ability, a status ...)
constexpr uintptr_t kSource_Char = 0x98;      // ... whose attacker (read so by R001 itself, 0x140110313)
constexpr uintptr_t kChar_Ident = 0x958;      // the creation key: the same on every peer for the same unit

using fn8 = void* (__fastcall*)(void*, void*, void*, void*, void*, void*, void*, void*);
fn8 g_orig = nullptr;

uint64_t g_battle = 0, g_key = 0;
std::unordered_map<uint64_t, uint32_t> g_n;    // (attacker, target) -> instances so far in this action
uint64_t g_calls = 0, g_drew = 0;
uint32_t g_logged = 0;

bool in_play() { return net_active() && net_peer_count() >= 2 && lockstep_active() && lockstep_battle_id() != 0; }

uint32_t ident(const void* chr) { uint32_t k = 0; if (chr) mem_read((const uint8_t*)chr + kChar_Ident, &k, sizeof(k)); return k; }

uint64_t mix(uint64_t& x) {
    uint64_t z = (x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void* __fastcall detour(void* target, void* dmg, void* c, void* d, void* e, void* f, void* g, void* h) {
    if (!tune::kDmgFence || !in_play()) return g_orig(target, dmg, c, d, e, f, g, h);
    uint64_t* s = rng_global_stream();
    if (!s) return g_orig(target, dmg, c, d, e, f, g, h);
    const uint64_t battle = lockstep_battle_id();
    const uint64_t key = lockstep_reseed_key();
    if (battle != g_battle) { g_battle = battle; g_key = ~0ull; g_calls = g_drew = 0; g_logged = 0; }
    if (key != g_key) { g_key = key; g_n.clear(); }
    const void* src = nullptr;
    if (dmg) {
        const void* so = nullptr;
        if (mem_read((const uint8_t*)dmg + kDmg_Source, &so, sizeof(so)) && so) mem_read((const uint8_t*)so + kSource_Char, &src, sizeof(src));
    }
    const uint32_t a = ident(src), t = ident(target);
    const uint64_t pair = ((uint64_t)a << 32) | t;
    const uint32_t n = g_n[pair]++;
    uint64_t x = battle ^ 0x444D4746454E4345ull ^ (key * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)a * 0xC2B2AE3D27D4EB4Full) ^ ((uint64_t)t * 0x165667B19E3779F9ull) ^
                 ((uint64_t)n * 0xD6E8FEB86659FD93ull);
    uint64_t saved[4], seed[4];
    memcpy(saved, s, sizeof(saved));
    for (int k = 0; k < 4; ++k) seed[k] = mix(x);
    memcpy(s, seed, sizeof(seed));
    rng_fence_enter();
    void* r = g_orig(target, dmg, c, d, e, f, g, h);
    rng_fence_leave();
    const bool drew = memcmp(s, seed, sizeof(seed)) != 0;
    memcpy(s, saved, sizeof(saved));
    ++g_calls;
    if (drew) ++g_drew;
    if (g_logged < 30 || (g_calls % 500) == 0) {
        ++g_logged;
        log_line("DMGFENCE", "damage #%u of %08x on %08x in action %016llx: rolled from its own stream (seed %016llx)%s -- the shared stream %016llx is put back [%llu instance(s), %llu drew, this battle]",
                 n, a, t, (unsigned long long)key, (unsigned long long)seed[0], drew ? "" : ", drew nothing", (unsigned long long)saved[0],
                 (unsigned long long)g_calls, (unsigned long long)g_drew);
    }
    return r;
}

} // namespace

int dmgfence_install(uintptr_t base) {
    if (!tune::kDmgFence || !base) return 0;
    uint8_t head[8] = {};
    uint8_t* at = (uint8_t*)(base + kFn_TakeDamage);
    if (!mem_read(at, head, sizeof(head)) || memcmp(head, kHead, sizeof(head)) != 0) {
        log_raw("  [!] DMGFENCE Character damage resolution (R001) rva 0x%07X: the first bytes are not this build's -- not wrapped", (unsigned)kFn_TakeDamage);
        return 0;
    }
    if (MH_CreateHook(at, (void*)&detour, (void**)&g_orig) != MH_OK || MH_EnableHook(at) != MH_OK) {
        log_raw("  [!] DMGFENCE Character damage resolution (R001): MinHook refused -- not wrapped");
        return 0;
    }
    log_raw("  [+] DMGFENCE every damage instance (R001, rva 0x%07X) rolls from a stream of its own in a room's battle", (unsigned)kFn_TakeDamage);
    return 1;
}

} // namespace mgmp
