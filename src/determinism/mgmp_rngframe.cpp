// mgmp_rngframe.cpp -- see mgmp_rngframe.h.
#include "mgmp_rngframe.h"

#include <windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <utility>

#include "MinHook.h"
#include "mgmp_log.h"
#include "mgmp_lockstep.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_rng.h"
#include "mgmp_rtti.h"
#include "mgmp_tuning.h"

namespace mgmp {

namespace {

// kind: 0 fires during an action (n counts this unit's drawing calls this turn), 1 per frame (n = 0), 2 initialisation / battle start: the
// unit's creation key may not be assigned yet, and the build may still carry the previous battle's turn number, so the seed is (battle,
// handler, n) with n counted over the whole battle -- the build calls these in the same order on every peer.
struct FrameTarget { uint32_t rva; uint8_t head[8]; uint16_t slot; uint8_t kind; const char* cls; };
#include "mgmp_rngframe_table.inc"
constexpr int kN = (int)(sizeof(kFrameTargets) / sizeof(kFrameTargets[0]));

// Eight argument slots are forwarded: four registers and four stack slots. The automatic checks excluded every handler that reads a float
// argument or returns a float, so integer forwarding carries everything these handlers take.
using fn8 = void* (__fastcall*)(void*, void*, void*, void*, void*, void*, void*, void*);
fn8 g_orig[kN] = {};

constexpr uintptr_t kPassive_Owner = 0x38;    // Passive -> the Character that carries it (read the same way by ModelingClay's handler, 0x140906F76)
constexpr uintptr_t kChar_Ident    = 0x958;   // the creation key: the same value on every peer for the same unit (mgmp_lockstep's roster uses it)

uint64_t g_battle = 0;
uint32_t g_turn = ~0u;
std::unordered_map<uint64_t, uint32_t> g_n;   // (unit key, handler) -> drawing calls this turn
std::unordered_map<uint32_t, uint32_t> g_n_init;   // handler -> drawing calls this battle (kind 2)
std::unordered_map<uintptr_t, bool> g_is_char;   // owner vtable -> "is a Character" (asked of RTTI once per vtable; never cached per POINTER --
                                                  // the game reuses a dead unit's address for a new one, measured in the 2026-10-11 logs)
uint32_t g_logged = 0;
uint64_t g_fenced = 0, g_drew = 0;

bool in_play() { return net_active() && net_peer_count() >= 2 && lockstep_active() && lockstep_battle_id() != 0; }

uint32_t unit_key(const void* passive) {
    const void* owner = nullptr;
    if (!mem_read((const uint8_t*)passive + kPassive_Owner, &owner, sizeof(owner)) || !owner) return 0;
    uintptr_t vt = 0;
    if (!mem_read(owner, &vt, sizeof(vt)) || !vt) return 0;
    auto it = g_is_char.find(vt);
    if (it == g_is_char.end()) {
        char cls[96] = {};
        const bool ch = rtti_class_name(owner, cls, sizeof(cls)) && strcmp(cls, "glaiel::Character") == 0;
        it = g_is_char.emplace(vt, ch).first;
    }
    uint32_t key = 0;         // a non-Character owner (a tile, an object) keys as 0: never a pointer, so never different between peers
    if (it->second) mem_read((const uint8_t*)owner + kChar_Ident, &key, sizeof(key));
    return key;
}

uint64_t mix(uint64_t& x) {
    uint64_t z = (x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// The passive a wrapped handler is running for (its `this`), while it runs: mgmp_gains reads the carrier of the item a passive drops (RockyArmor).
const void* g_cur_passive = nullptr;
struct CurPassive {
    const void* prev;
    explicit CurPassive(const void* p) : prev(g_cur_passive) { g_cur_passive = p; }
    ~CurPassive() { g_cur_passive = prev; }
};

void* fenced_call(int i, void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h) {
    CurPassive cur(a);
    if (!tune::kRngFrame || !in_play()) return g_orig[i](a, b, c, d, e, f, g, h);
    uint64_t* s = rng_global_stream();
    if (!s) return g_orig[i](a, b, c, d, e, f, g, h);
    const uint64_t battle = lockstep_battle_id();
    const uint32_t turn = lockstep_turn();
    if (battle != g_battle) { g_battle = battle; g_turn = ~0u; g_logged = 0; g_fenced = g_drew = 0; g_n_init.clear(); }
    if (turn != g_turn) { g_turn = turn; g_n.clear(); }
    const FrameTarget& t = kFrameTargets[i];
    const bool init = t.kind == 2;
    const uint32_t unit = init ? 0u : unit_key(a);
    const uint64_t key = ((uint64_t)unit << 32) | (uint32_t)i;
    const uint32_t n = init ? g_n_init[(uint32_t)i] : (t.kind == 1 ? 0u : g_n[key]);
    uint64_t x = battle ^ 0x6D676D704652414Dull ^ ((uint64_t)(init ? 0u : turn) * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)unit * 0xC2B2AE3D27D4EB4Full) ^
                 ((uint64_t)t.rva * 0x165667B19E3779F9ull) ^ ((uint64_t)n * 0xD6E8FEB86659FD93ull);
    uint64_t saved[4], seed[4];
    memcpy(saved, s, sizeof(saved));
    for (int k = 0; k < 4; ++k) seed[k] = mix(x);
    memcpy(s, seed, sizeof(seed));
    rng_fence_enter();
    void* r = g_orig[i](a, b, c, d, e, f, g, h);
    rng_fence_leave();
    const bool drew = memcmp(s, seed, sizeof(seed)) != 0;
    memcpy(s, saved, sizeof(saved));
    ++g_fenced;
    if (drew) {
        ++g_drew;
        if (init) g_n_init[(uint32_t)i] = n + 1;
        else if (t.kind == 0) g_n[key] = n + 1;
        if (g_logged < 40 || (g_drew % 500) == 0) {
            ++g_logged;
            log_line("RNGFRAME", "%s (slot %u%s) of unit %08x drew from its own stream (turn %u, #%u, seed %016llx) -- the shared stream %016llx is put back [%llu fenced calls, %llu drew, this battle]",
                     t.cls, (unsigned)t.slot, t.kind == 1 ? ", per frame" : (init ? ", at initialisation" : ""), unit, (unsigned)turn, n, (unsigned long long)seed[0], (unsigned long long)saved[0],
                     (unsigned long long)g_fenced, (unsigned long long)g_drew);
        }
    }
    return r;
}

template <int I>
void* __fastcall frame_detour(void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h) {
    return fenced_call(I, a, b, c, d, e, f, g, h);
}
template <int... I>
constexpr std::array<void*, sizeof...(I)> make_detours(std::integer_sequence<int, I...>) { return { { (void*)&frame_detour<I>... } }; }
const std::array<void*, kN> kDetours = make_detours(std::make_integer_sequence<int, kN>{});

} // namespace

const void* rngframe_current_passive() { return g_cur_passive; }

int rngframe_install(uintptr_t base) {
    if (!tune::kRngFrame || !base) return 0;
    int ok = 0, refused = 0;
    for (int i = 0; i < kN; ++i) {
        const FrameTarget& t = kFrameTargets[i];
        uint8_t* at = (uint8_t*)(base + t.rva);
        uint8_t head[8] = {};
        if (!mem_read(at, head, sizeof(head)) || memcmp(head, t.head, sizeof(head)) != 0) {
            ++refused;
            log_raw("  [!] RNGFRAME %-36s rva 0x%07X: the first bytes are not the ones the table was made from -- not wrapped", t.cls, t.rva);
            continue;
        }
        if (MH_CreateHook(at, kDetours[i], (void**)&g_orig[i]) != MH_OK || MH_EnableHook(at) != MH_OK) {
            ++refused;
            log_raw("  [!] RNGFRAME %-36s rva 0x%07X: MinHook refused -- not wrapped", t.cls, t.rva);
            continue;
        }
        ++ok;
    }
    log_raw("  [+] RNGFRAME  %d of %d frame-driven passive handlers wrapped with a per-event stream fence (%d refused)", ok, kN, refused);
    return ok;
}

} // namespace mgmp
