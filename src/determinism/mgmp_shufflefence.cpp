#include "mgmp_shufflefence.h"
#include "mgmp_mem.h"
#include "mgmp_log.h"
#include "mgmp_lockstep.h"
#include "mgmp_net.h"
#include "MinHook.h"

#include <windows.h>
#include <intrin.h>
#include <cstring>
#include <cstdio>

namespace mgmp {
namespace {

constexpr uintptr_t kFn_Shuffle = 0x00086F00;
const uint8_t kHead[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x8B, 0x59, 0x04,
                          0x48, 0x8B, 0xFA, 0x48, 0x8B, 0xF1, 0x48, 0x83, 0xFB, 0x01 };
constexpr uint32_t kMaxElems = 254;

using fn_shuffle = void(__fastcall*)(void* vec, uint64_t* rng);
fn_shuffle g_orig = nullptr;
uintptr_t  g_base = 0;
uint32_t   g_logged = 0;
uint32_t   g_logged_gen = ~0u;
uint64_t   g_sorted = 0;

void __fastcall detour(void* vec, uint64_t* rng) {
    // only once THIS battle's roster is snapshotted (lockstep_battle_ready): in the battle-start window the roster is the last fight's, its indices are no one's
    if (vec && lockstep_battle_ready() && net_active() && net_peer_count() >= 2) {
        uint32_t n = 0; void** data = nullptr;
        if (mem_read((const uint8_t*)vec + 4, &n, 4) && n >= 2 && n <= kMaxElems && mem_read((const uint8_t*)vec + 8, &data, 8) && data) {
            void* el[kMaxElems]; int idx[kMaxElems];
            bool all = true;
            for (uint32_t i = 0; i < n && all; ++i) {
                if (!mem_read(data + i, &el[i], 8) || !el[i]) { all = false; break; }
                idx[i] = lockstep_unit_index(el[i]);
                if (idx[i] < 0) all = false;
            }
            if (all) {
                char before[300]; int w = 0; before[0] = 0;
                for (uint32_t i = 0; i < n && w >= 0 && w < (int)sizeof(before) - 8; ++i) w += _snprintf_s(before + w, sizeof(before) - w, _TRUNCATE, " %d", idx[i]);
                bool moved = false;
                for (uint32_t i = 1; i < n; ++i) {            // insertion sort by roster index (stable; n is a handful)
                    void* e = el[i]; const int k = idx[i]; uint32_t j = i;
                    while (j > 0 && idx[j - 1] > k) { el[j] = el[j - 1]; idx[j] = idx[j - 1]; --j; moved = true; }
                    el[j] = e; idx[j] = k;
                }
                if (moved) {
                    for (uint32_t i = 0; i < n; ++i) mem_write(data + i, &el[i], 8);
                    ++g_sorted;
                }
                const uint32_t gen = lockstep_battle_gen();
                if (gen != g_logged_gen) { g_logged_gen = gen; g_logged = 0; }
                if (g_logged < 40) {
                    ++g_logged;
                    log_line("SHUFFLE", "a shuffle of %u battle unit(s) (called from %X, turn %u): this game's order [%s ] %s", n,
                             (unsigned)((uintptr_t)_ReturnAddress() - g_base), lockstep_turn(), before, moved ? "-> put in roster order first (the order every peer shares)" : "-- already in roster order");
                }
            }
        }
    }
    g_orig(vec, rng);
}

} // namespace

int shufflefence_install(uintptr_t base) {
    if (!base) return 0;
    g_base = base;
    uint8_t head[sizeof(kHead)] = {};
    uint8_t* at = (uint8_t*)(base + kFn_Shuffle);
    if (!mem_read(at, head, sizeof(head)) || memcmp(head, kHead, sizeof(head)) != 0) {
        log_raw("  [!] SHUFFLE  the unit-list shuffle rva 0x%07X: the first bytes are not this build's -- not wrapped", (unsigned)kFn_Shuffle);
        return 0;
    }
    if (MH_CreateHook(at, (void*)&detour, (void**)&g_orig) != MH_OK || MH_EnableHook(at) != MH_OK) {
        log_raw("  [!] SHUFFLE  the unit-list shuffle: MinHook refused -- not wrapped");
        return 0;
    }
    log_raw("  [+] SHUFFLE  a shuffle of battle units (rva 0x%07X, FleshGolem's allies ...) starts from roster order in a room's battle", (unsigned)kFn_Shuffle);
    return 1;
}

} // namespace mgmp
