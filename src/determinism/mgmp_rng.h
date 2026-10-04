// mgmp_rng.h -- the xoshiro256 draw recorder.
//
// Mewgenics has a real RNG API that takes the stream state as an explicit
// parameter, which is what makes fencing possible at all:
//
//   randint  (int    n, u64* state) -> int      @ 0x14094B0B0   ecx, rdx  -> eax
//   randfloat(double m, u64* state) -> double   @ 0x140158B80   xmm0, rdx -> xmm0
//   rand2    (double m, u64* state) -> __m128   @ 0x14094B230   xmm0, rdx -> xmm0
//
// `rand2` advances the state TWICE per call. It is tagged RNG_TWO in the record
// so the decoder does not mistake the second round's absence for a divergence.
//
// The shared "global" stream lives in thread-local storage at
// [gs:0x58][0] + 0x178. A draw that passes that exact address is a draw that
// both peers must agree on; a draw that passes anything else is using a scratch
// stream and cannot desync anyone. That single pointer comparison is the whole
// filter, and it is why `rng_global_only` can default to on without losing
// anything that matters.
//
// ---------------------------------------------------------------------------
// WHAT THIS DOES NOT SEE  -- read before trusting a clean diff
//
// Some call sites call these functions; others have the xoshiro round inlined
// into them by the compiler. Hooking the API catches the former and is blind to
// the latter. So an empty diff from this recorder is *not* proof of
// determinism -- it is proof that the ~210 call-based draw sites agree.
//
// The inlined sites are findable (byte-scan for `ror r64,19` plus `shl r64,17`,
// or xrefs to the 2^-53 constant at 0x141137238) and the design notes records the
// method. Closing that gap means either patching inlined sites individually or
// hashing the TLS state block at frame boundaries to catch drift from any
// source. The second is much cheaper and is the natural next step if Run C
// comes back clean but a real desync still shows up later.
#pragma once

#include <cstdint>

namespace mgmp {

// The TLS global stream, or nullptr if TLS is not set up on this thread yet.
uint64_t* rng_global_stream();

// Must be called before the detours run: call sites are recorded as RVAs and
// that needs the module base.
void rng_set_base(uintptr_t base);

// Detours. Installed by hooks_install() alongside the phase-1 hooks; the
// originals live here so the detours can tail-call them.
void* rng_detour_randint();
void* rng_detour_randfloat();
void* rng_detour_rand2();
void* rng_detour_rollchance();

void** rng_original_randint();
void** rng_original_randfloat();
void** rng_original_rand2();
void** rng_original_rollchance();

// THE LEDGER (proto 80): while armed, every draw on the shared stream made by the thread that armed it is counted by call site, and its (function, site) goes into an order-sensitive digest. The values
// drawn are NOT in it: two peers drawing the same sequence from different streams still agree, and the stream itself is compared elsewhere. mgmp_diag takes it once per action and compares it with
// the other peer's, so a drift INSIDE an action is named by the call site that drew a different number of times.
constexpr uint32_t kLedgerSites = 96;
struct RngLedger {
    uint32_t n = 0, digest = 0;                   // draws made inside an apply-action call (rng_ledger_phase true), and the digest of their order
    uint32_t n_loose = 0, digest_loose = 0;       // the ones made outside one: the AI's decisions, and on the owner's peer the aim previews

    uint32_t sites = 0, over = 0;                 // distinct sites listed (sorted by key), and draws from sites that did not fit
    uint32_t key[kLedgerSites] = {};              // site RVA | outside-an-apply << 27 | function << 28
    uint32_t count[kLedgerSites] = {};
};
void rng_ledger_arm(bool on);                     // on: count this thread's draws from now (clears); off: stop
void rng_ledger_take(RngLedger& out);             // the window so far, sorted; clears it
bool rng_ledger_armed();
void rng_ledger_phase(bool in_apply);             // the hook of the game's apply-action call: true around it, false after (a no-op unless armed on this thread)

// Total draws seen, and of those, how many were on the global stream. Logged at
// each turn so the trace shows the recorder is alive even when the diff is
// empty.
void rng_counters(uint64_t* total, uint64_t* global);

} // namespace mgmp
