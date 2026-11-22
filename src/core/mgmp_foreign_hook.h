#pragma once
// mgmp_foreign_hook.h -- another mod loader's hook on a function's first bytes (0.1.244).
//
// WHY (2026-11-20, user request): the Combat Roster Panel mod loads through Mewjector (a version.dll proxy). Its DllMain runs while the
// process is being loaded -- before the game's entry point, so before either of our park points -- and it hooks the same three functions
// we do (FrameBegin, TurnControl::NextTurn, StatusMenu::update) with the same signatures. Their first bytes are then a jump into its
// relay, and two of our checks read that as "not the build this mod is made for":
//   - the loader's park point (FrameBegin): the normal launch KILLED the game ("park point prologue does not match"), auto-inject refused;
//   - the resolver: FRAME and NEXTTURN unresolved, both critical -- no lockstep.
// A jump at the very start whose target lies outside the game image, followed by the function's own bytes, is such a hook. It is accepted
// as the function, and MinHook chains on top of it: our detour runs first, our trampoline calls the other loader's chain, which calls the
// game. Header-only: the loader (mgmp_loader.exe) uses it too.
#include <cstdint>

namespace mgmp {

// The length of the jump another hooker wrote at `at` (the bytes after it are the function's own), or 0 when `at` does not start with
// one that leaves the image [img_lo, img_hi). `at` is readable for 16 bytes; `self` is the address of those bytes in the target process
// (the loader reads a copy of them out of another process).
inline int foreign_hook_len(const uint8_t* at, uintptr_t self, uintptr_t img_lo, uintptr_t img_hi) {
    uint64_t dest = 0;
    int len = 0;
    if (at[0] == 0xE9) {                                                             // jmp rel32
        int32_t rel = 0;
        for (int i = 0; i < 4; ++i) rel |= (int32_t)((uint32_t)at[1 + i] << (8 * i));
        dest = (uint64_t)(self + 5 + (int64_t)rel);
        len = 5;
    } else if (at[0] == 0xFF && at[1] == 0x25 && !at[2] && !at[3] && !at[4] && !at[5]) {   // jmp [rip+0] ; dq dest
        for (int i = 0; i < 8; ++i) dest |= (uint64_t)at[6 + i] << (8 * i);
        len = 14;
    } else if (at[0] == 0x48 && at[1] == 0xB8 && at[10] == 0xFF && at[11] == 0xE0) {   // mov rax, imm64 ; jmp rax
        for (int i = 0; i < 8; ++i) dest |= (uint64_t)at[2 + i] << (8 * i);
        len = 12;
    } else {
        return 0;
    }
    if (dest >= img_lo && dest < img_hi) return 0;        // a jump inside the game: the game's own code, not somebody's hook
    return len;
}

// Whether `at` is the function `want` (want_mask: 1 = must match, 0 = wildcard; n bytes) with another loader's jump on its first bytes:
// every literal byte after the jump matches, a 0x90/0xCC fill inside the first 16 bytes is allowed (some hookers pad the stolen
// instructions), and at least `min_checked` literal bytes were compared.
inline bool foreign_hook_matches(const uint8_t* at, uintptr_t self, uintptr_t img_lo, uintptr_t img_hi,
                                 const uint8_t* want, const uint8_t* want_mask, int n, int min_checked = 6) {
    const int skip = foreign_hook_len(at, self, img_lo, img_hi);
    if (!skip) return false;
    int checked = 0;
    for (int i = skip; i < n; ++i) {
        if (want_mask && !want_mask[i]) continue;
        if (at[i] == want[i]) { ++checked; continue; }
        if (i < 16 && (at[i] == 0x90 || at[i] == 0xCC)) continue;
        return false;
    }
    return checked >= min_checked;
}

} // namespace mgmp
