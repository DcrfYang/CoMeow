#include "mgmp_rng.h"
#include "mgmp_config.h"
#include "mgmp_tuning.h"
#include "mgmp_record.h"
#include "mgmp_lockstep.h"

#include <windows.h>
#include <intrin.h>
#include <cstring>
#include <utility>

namespace mgmp {
namespace {

typedef int     (__fastcall* fn_randint)  (int    n, uint64_t* state);
typedef double  (__fastcall* fn_randfloat)(double m, uint64_t* state);
typedef __m128  (__fastcall* fn_rand2)    (double m, uint64_t* state);
typedef bool    (__fastcall* fn_rollchance)(double p, double scale, uint64_t* state);

fn_randint   o_randint   = nullptr;
fn_randfloat o_randfloat = nullptr;
fn_rand2     o_rand2     = nullptr;
fn_rollchance o_rollchance = nullptr;

uintptr_t g_base = 0;

volatile LONG64 g_total  = 0;
volatile LONG64 g_global = 0;

// the ledger (see the header). Single writer: only the thread that armed it adds, so there is no lock; the take also runs on that thread.
volatile LONG g_led_on = 0;
DWORD    g_led_tid = 0;
uint32_t g_led_n = 0, g_led_dig = 2166136261u, g_led_over = 0, g_led_sites = 0;
uint32_t g_led_nl = 0, g_led_digl = 2166136261u;
bool     g_led_apply = false;
uint32_t g_led_key[kLedgerSites] = {}, g_led_cnt[kLedgerSites] = {};
uint32_t g_led_last = 0;               // index of the site hit last: draws come in runs from one site

// Return address -> RVA. Absolute addresses would differ between runs under
// ASLR and make every record differ; the RVA is stable for a pinned build.
inline uint32_t site_rva(void* ret) {
    uintptr_t a = (uintptr_t)ret;
    uintptr_t b = g_base;
    if (!b || a < b) return 0;
    uintptr_t d = a - b;
    return d > 0xFFFFFFFFull ? 0 : (uint32_t)d;
}

// The common tail of all three detours: decide whether this draw matters,
// count it, and record it.
//
// Ordering note: `s0` must be sampled BEFORE the original runs, because the
// original mutates the state in place. That is the entire reason these detours
// cannot be written as a simple "call original, then log".
inline void ledger_add(uint8_t fn, uint32_t rva) {
    const uint32_t key = (rva & 0x07FFFFFFu) | (g_led_apply ? 0u : 0x08000000u) | ((uint32_t)(fn & 15) << 28);
    if (g_led_apply) { ++g_led_n; g_led_dig = (g_led_dig ^ key) * 16777619u; }
    else { ++g_led_nl; g_led_digl = (g_led_digl ^ key) * 16777619u; }
    if (g_led_last < g_led_sites && g_led_key[g_led_last] == key) { ++g_led_cnt[g_led_last]; return; }
    for (uint32_t i = 0; i < g_led_sites; ++i) if (g_led_key[i] == key) { g_led_last = i; ++g_led_cnt[i]; return; }
    if (g_led_sites < kLedgerSites) { g_led_key[g_led_sites] = key; g_led_cnt[g_led_sites] = 1; g_led_last = g_led_sites++; }
    else ++g_led_over;
}

inline void note(uint8_t fn, void* ret, uint64_t s0, uint64_t result, bool global,
                 const void* state) {
    InterlockedIncrement64(&g_total);
    if (global) {
        InterlockedIncrement64(&g_global);
        if (g_led_on && GetCurrentThreadId() == g_led_tid) ledger_add(fn, site_rva(ret));
    }

    if (!record_active()) return;
    if (!global && tune::kRngGlobalOnly) {
        record_note_skipped();
        return;
    }
    record_rng(fn, site_rva(ret), s0, result, global, state);
}

} // namespace

uint64_t* rng_global_stream() {
    // gs:[0x58] is the TEB's ThreadLocalStoragePointer; slot 0 is the main
    // module's static TLS block, and the shared stream sits at +0x178 in it.
    char** tls = (char**)__readgsqword(0x58);
    if (!tls) return nullptr;
    char* block = tls[0];
    if (!block) return nullptr;
    return (uint64_t*)(block + 0x178);
}

// ---- detours --------------------------------------------------------------
//
// Each one reads state[0] first, forwards, then records. `state` is supplied by
// the caller and is always a live 32-byte array, so the read needs no SEH
// guard -- if it were bad the game would already have crashed inside the
// original on the very next instruction.

int __fastcall h_randint(int n, uint64_t* state) {
    void*    ret = _ReturnAddress();
    bool     glb = (state == rng_global_stream());
    uint64_t s0  = state ? state[0] : 0;
    int      r   = o_randint(n, state);
    note(RNG_INT, ret, s0, (uint64_t)(uint32_t)r, glb, state);
    return r;
}

double __fastcall h_randfloat(double m, uint64_t* state) {
    void*    ret = _ReturnAddress();
    bool     glb = (state == rng_global_stream());
    uint64_t s0  = state ? state[0] : 0;
    double   r   = o_randfloat(m, state);
    uint64_t bits;
    memcpy(&bits, &r, sizeof(bits));
    note(RNG_FLOAT, ret, s0, bits, glb, state);
    return r;
}

__m128 __fastcall h_rand2(double m, uint64_t* state) {
    void*    ret = _ReturnAddress();
    bool     glb = (state == rng_global_stream());
    uint64_t s0  = state ? state[0] : 0;
    __m128   r   = o_rand2(m, state);
    uint64_t bits;
    memcpy(&bits, &r, sizeof(bits));       // low half is enough as a checksum
    note(RNG_TWO, ret, s0, bits, glb, state);
    return r;
}

// RollChance(p, scale, state) -- the proc/crit gate.
//
// Recorded even though the draw it makes is ALSO recorded by the rand2 hook
// underneath it. That is the point: this frame is the only one that knows which
// passive rolled, because rand2's return address lands inside RollChance and
// RollChance's lands in the caller. The decoder can pair them by sequence.
//
// When p >= 1.0 it short-circuits and takes no draw at all. That case is still
// recorded (result = 1, s0 unchanged) because "a proc that was guaranteed"
// still has to match between peers -- a peer that thought it was 0.9 would take
// a draw here and desync the stream position, not just the outcome.
bool __fastcall h_rollchance(double p, double scale, uint64_t* state) {
    void*    ret = _ReturnAddress();
    bool     glb = (state == rng_global_stream());
    uint64_t s0  = state ? state[0] : 0;
    bool     r   = o_rollchance(p, scale, state);
    note(RNG_ROLL, ret, s0, (uint64_t)r, glb, state);
    // The coin a melee kill drops (the effect at 0x2C3060): this roll's odds come from data that is not the same on every peer, so the HOST's answer is the one everybody uses.
    const uint32_t rva = site_rva(ret);
    if (glb && rva >= 0x2C3060 && rva < 0x2C3488) r = lockstep_roll_resolve(kRollSiteCoinDrop, p, scale, r);
    return r;
}

void* rng_detour_randint()   { return (void*)&h_randint;   }
void* rng_detour_randfloat() { return (void*)&h_randfloat; }
void* rng_detour_rand2()     { return (void*)&h_rand2;     }
void* rng_detour_rollchance(){ return (void*)&h_rollchance;}

void** rng_original_randint()   { return (void**)&o_randint;   }
void** rng_original_randfloat() { return (void**)&o_randfloat; }
void** rng_original_rand2()     { return (void**)&o_rand2;     }
void** rng_original_rollchance(){ return (void**)&o_rollchance;}

void rng_counters(uint64_t* total, uint64_t* global) {
    if (total)  *total  = (uint64_t)InterlockedCompareExchange64(&g_total, 0, 0);
    if (global) *global = (uint64_t)InterlockedCompareExchange64(&g_global, 0, 0);
}

void rng_set_base(uintptr_t base) { g_base = base; }

void rng_ledger_arm(bool on) {
    InterlockedExchange(&g_led_on, 0);
    g_led_n = 0; g_led_dig = 2166136261u; g_led_over = 0; g_led_sites = 0; g_led_last = 0; g_led_nl = 0; g_led_digl = 2166136261u; g_led_apply = false;
    g_led_tid = on ? GetCurrentThreadId() : 0;
    if (on) InterlockedExchange(&g_led_on, 1);
}
bool rng_ledger_armed() { return g_led_on != 0 && g_led_tid == GetCurrentThreadId(); }
void rng_ledger_take(RngLedger& out) {
    out = RngLedger{};
    out.n = g_led_n; out.digest = g_led_dig; out.n_loose = g_led_nl; out.digest_loose = g_led_digl; out.sites = g_led_sites; out.over = g_led_over;
    for (uint32_t i = 0; i < g_led_sites; ++i) { out.key[i] = g_led_key[i]; out.count[i] = g_led_cnt[i]; }
    for (uint32_t i = 1; i < out.sites; ++i)          // insertion sort by key: the same table on both peers reads the same
        for (uint32_t j = i; j > 0 && out.key[j] < out.key[j - 1]; --j) { std::swap(out.key[j], out.key[j - 1]); std::swap(out.count[j], out.count[j - 1]); }
    g_led_n = 0; g_led_dig = 2166136261u; g_led_over = 0; g_led_sites = 0; g_led_last = 0; g_led_nl = 0; g_led_digl = 2166136261u;
}
void rng_ledger_phase(bool in_apply) { if (g_led_on && g_led_tid == GetCurrentThreadId()) g_led_apply = in_apply; }

} // namespace mgmp
