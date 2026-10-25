// mgmp_poolfence.cpp -- see mgmp_poolfence.h.
#include "mgmp_poolfence.h"

#include <windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <utility>

#include "MinHook.h"
#include "mgmp_log.h"
#include "mgmp_lockstep.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_rng.h"

namespace mgmp {
namespace {

// The arguments were checked on the pinned build (scratchpad stackargs.py, 2026-10-23): no float argument or return; R115 and R237 take a 5th
// argument on the stack, the others four registers at most. Five slots are forwarded for all of them -- one more than a function reads is harmless.
struct PoolTarget { uint32_t rva; uint8_t head[16]; const char* what; };
const PoolTarget kTargets[] = {
    { 0x148870, { 0x48, 0x89, 0x5C, 0x24, 0x20, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41 }, "R108 ability pool shuffle (banned_abilities / AnyUnlocked)" },
    { 0x14A490, { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56 }, "R111 AnyUnlocked shuffle" },
    { 0x14B500, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 }, "R113 AnyUnlocked pool shuffle" },
    { 0x14BD10, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 }, "R114 AnyUnlocked shuffle" },
    { 0x14CE60, { 0x44, 0x89, 0x4C, 0x24, 0x20, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x48 }, "R115 EvolveAbilityFromPool" },
    { 0x30EEC0, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x4C }, "R237 ScrambleLastUsedSpell" },
    { 0x8DFA90, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55 }, "R404 item random_class_ability" },
    { 0x8DFD10, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55 }, "R405 item random_class_passive" },
};
constexpr int kN = (int)(sizeof(kTargets) / sizeof(kTargets[0]));

using fn5 = void* (__fastcall*)(void*, void*, void*, void*, void*);
fn5 g_orig[kN] = {};
uint32_t g_logged = 0;

void* fenced_call(int i, void* a, void* b, void* c, void* d, void* e) {
    uint64_t* s = (net_active() && net_peer_count() >= 2) ? rng_global_stream() : nullptr;
    uint64_t saved[4] = {};
    uint64_t seed = 0;
    if (!s || !mem_read(s, saved, sizeof(saved)) || !lockstep_stream_for_pick(0x504F4F4C00000000ull | kTargets[i].rva, &seed))
        return g_orig[i](a, b, c, d, e);
    rng_fence_enter();
    void* r = g_orig[i](a, b, c, d, e);
    rng_fence_leave();
    mem_write(s, saved, sizeof(saved));
    if (g_logged < 60) {
        ++g_logged;
        log_line("POOLFENCE", "%s: its pick drawn from a stream every peer derives alike (%016llx); the shared stream %016llx is put back",
                 kTargets[i].what, (unsigned long long)seed, (unsigned long long)saved[0]);
    }
    return r;
}

template <int I>
void* __fastcall pool_detour(void* a, void* b, void* c, void* d, void* e) { return fenced_call(I, a, b, c, d, e); }
template <int... I>
constexpr std::array<void*, sizeof...(I)> make_detours(std::integer_sequence<int, I...>) { return { { (void*)&pool_detour<I>... } }; }
const std::array<void*, kN> kDetours = make_detours(std::make_integer_sequence<int, kN>{});

} // namespace

int poolfence_install(uintptr_t base) {
    if (!base) return 0;
    int ok = 0, refused = 0;
    for (int i = 0; i < kN; ++i) {
        const PoolTarget& t = kTargets[i];
        uint8_t* at = (uint8_t*)(base + t.rva);
        uint8_t head[16] = {};
        if (!mem_read(at, head, sizeof(head)) || memcmp(head, t.head, sizeof(head)) != 0) {
            ++refused;
            log_raw("  [!] POOLFENCE %-44s rva 0x%07X: the first bytes are not the pinned build's -- not wrapped", t.what, t.rva);
            continue;
        }
        if (MH_CreateHook(at, kDetours[i], (void**)&g_orig[i]) != MH_OK || MH_EnableHook(at) != MH_OK) {
            ++refused;
            log_raw("  [!] POOLFENCE %-44s rva 0x%07X: MinHook refused -- not wrapped", t.what, t.rva);
            continue;
        }
        ++ok;
    }
    log_raw("  [+] POOLFENCE %d of %d ability-pool picks wrapped with a derived-stream fence (%d refused)", ok, kN, refused);
    return ok;
}

} // namespace mgmp
