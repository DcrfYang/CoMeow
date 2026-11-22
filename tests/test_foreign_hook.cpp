// test_foreign_hook.cpp -- 0.1.244: another mod loader's hook on a function's first bytes (mgmp_foreign_hook.h).
//   1. On the real game image: FRAME / NEXTTURN / STATUSMENU with an E9, FF 25 or mov-rax-jmp hook written over their first bytes are still
//      recognised (by the resolver and by the loader's park check); a jump inside the image, or other bytes after the jump, are not.
//   2. MinHook chains on top of such a jump: our detour runs, then the other hooker's, then the original.
#include "mgmp_foreign_hook.h"
#include "mgmp_sigscan.h"
#include "mgmp_sigs.generated.h"
#include "mgmp_addresses.h"
#include "MinHook.h"
#include <windows.h>
#include <cstdio>
#include <cstring>

using namespace mgmp;

static int g_fail = 0, g_pass = 0;
static void check(bool ok, const char* what) { if (ok) ++g_pass; else { ++g_fail; printf("  FAIL  %s\n", what); } }

static const SigTargetDesc* sig_named(const char* name) {
    for (const auto& s : kTargetSigs) if (strcmp(s.name, name) == 0) return &s;
    return nullptr;
}

// --- part 2: a "game function", another loader's hook on it, and ours on top -------------------------------------------------------------
static int g_order[8]; static int g_n = 0;
static __declspec(noinline) int game_fn(int x) { g_order[g_n++] = 3; return x * 2; }
typedef int (*fn_t)(int);
static fn_t g_other_tramp = nullptr;   // the other loader's way back to the game
static fn_t o_ours = nullptr;
static int other_hook(int x) { g_order[g_n++] = 2; return g_other_tramp(x) + 1; }
static int our_hook(int x) { g_order[g_n++] = 1; return o_ours(x) + 10; }

int main() {
    printf("-- the recognizer on the real game image --\n");
    HMODULE exe = LoadLibraryExW(L"D:\\software\\steam\\Steam\\steamapps\\common\\Mewgenics\\Mewgenics.exe", nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!exe) { printf("  SKIP  the game exe is not here\n"); }
    else {
        const uintptr_t base = (uintptr_t)exe;
        const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
        const uintptr_t hi = base + nt->OptionalHeader.SizeOfImage;
        const char* names[] = { "FRAME", "NEXTTURN", "STATUSMENU" };
        for (const char* nm : names) {
            const SigTargetDesc* s = sig_named(nm);
            SigPattern p{}; char err[96] = {};
            if (!s || !sig_parse(s->pattern, &p, err, sizeof(err))) { check(false, nm); continue; }
            const uintptr_t at = base + s->rva_hint;
            uint8_t buf[64] = {};
            memcpy(buf, (const void*)at, p.len);
            char what[160];
            _snprintf_s(what, sizeof(what), _TRUNCATE, "%s: the pristine prologue matches and is not taken for a hook", nm);
            check(sig_matches_at(buf, p) && !foreign_hook_len(buf, at, base, hi), what);
            const uintptr_t outside = base - 0x10000000;           // a relay below the image, where a loader allocates it
            uint8_t e9[64]; memcpy(e9, buf, sizeof(e9));
            const int32_t rel = (int32_t)(int64_t)(outside - (at + 5));
            e9[0] = 0xE9; memcpy(e9 + 1, &rel, 4);
            _snprintf_s(what, sizeof(what), _TRUNCATE, "%s: an E9 hook leaving the image is recognised", nm);
            check(!sig_matches_at(e9, p) && foreign_hook_matches(e9, at, base, hi, p.bytes, p.mask, p.len), what);
            uint8_t ff[64]; memcpy(ff, buf, sizeof(ff));
            ff[0] = 0xFF; ff[1] = 0x25; memset(ff + 2, 0, 4); const uint64_t d = outside; memcpy(ff + 6, &d, 8);
            _snprintf_s(what, sizeof(what), _TRUNCATE, "%s: an FF 25 (absolute) hook is recognised", nm);
            check(foreign_hook_matches(ff, at, base, hi, p.bytes, p.mask, p.len), what);
            uint8_t mv[64]; memcpy(mv, buf, sizeof(mv));
            mv[0] = 0x48; mv[1] = 0xB8; memcpy(mv + 2, &d, 8); mv[10] = 0xFF; mv[11] = 0xE0;
            _snprintf_s(what, sizeof(what), _TRUNCATE, "%s: a mov rax / jmp rax hook is recognised", nm);
            check(foreign_hook_matches(mv, at, base, hi, p.bytes, p.mask, p.len), what);
            uint8_t in[64]; memcpy(in, buf, sizeof(in));
            const int32_t rel_in = (int32_t)(int64_t)((base + 0x1000) - (at + 5));
            in[0] = 0xE9; memcpy(in + 1, &rel_in, 4);
            _snprintf_s(what, sizeof(what), _TRUNCATE, "%s: a jump inside the game image is NOT taken for a hook", nm);
            check(!foreign_hook_matches(in, at, base, hi, p.bytes, p.mask, p.len), what);
            uint8_t other[64]; memcpy(other, e9, sizeof(other));
            other[p.len - 1] ^= 0x5A;
            _snprintf_s(what, sizeof(what), _TRUNCATE, "%s: a hooked function whose remaining bytes differ (another build) is NOT accepted", nm);
            check(!foreign_hook_matches(other, at, base, hi, p.bytes, p.mask, p.len), what);
        }
        // the loader's park check uses kParkSigLong, which is the FRAME signature's literal bytes
        const uintptr_t park = base + kParkRva;
        uint8_t pb[32]; memcpy(pb, (const void*)park, sizeof(pb));
        check(memcmp(pb, kParkSig, sizeof(kParkSig)) == 0 && memcmp(pb, kParkSigLong, sizeof(kParkSigLong)) == 0, "park point: the pristine FrameBegin is kParkSig / kParkSigLong");
        const int32_t rel = (int32_t)(int64_t)((base - 0x10000000) - (park + 5));
        pb[0] = 0xE9; memcpy(pb + 1, &rel, 4);
        check(foreign_hook_matches(pb, park, base, hi, kParkSigLong, nullptr, (int)sizeof(kParkSigLong)), "park point: FrameBegin hooked by another loader is still the park point");
        uint64_t d = base - 0x10000000;
        pb[0] = 0xFF; pb[1] = 0x25; memset(pb + 2, 0, 4); memcpy(pb + 6, &d, 8);
        check(foreign_hook_matches(pb, park, base, hi, kParkSigLong, nullptr, (int)sizeof(kParkSigLong)), "park point: an absolute (FF 25) hook leaves enough to check");
        FreeLibrary(exe);
    }

    printf("-- MinHook chains on top of another loader's jump --\n");
    MH_Initialize();
    // the other loader's hook on a "game function": its first bytes are an absolute jump (FF 25) to that loader's handler, which calls the game
    uint8_t* fn = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    const uint64_t to = (uint64_t)(uintptr_t)&other_hook;
    fn[0] = 0xFF; fn[1] = 0x25; memset(fn + 2, 0, 4); memcpy(fn + 6, &to, 8);
    memset(fn + 14, 0xCC, 16);
    FlushInstructionCache(GetCurrentProcess(), fn, 32);
    g_other_tramp = &game_fn;
    check(foreign_hook_len(fn, (uintptr_t)fn, 0x140000000ull, 0x141574000ull) == 14, "the stand-in hook is recognised as another loader's");
    check(MH_CreateHook(fn, (void*)&our_hook, (void**)&o_ours) == MH_OK && MH_EnableHook(fn) == MH_OK, "our hook goes on top of the jump");
    g_n = 0;
    const int r = ((fn_t)fn)(5);
    check(r == 5 * 2 + 1 + 10, "the call ran all three: game * 2, +1 (theirs), +10 (ours)");
    check(g_n == 3 && g_order[0] == 1 && g_order[1] == 2 && g_order[2] == 3, "in the order ours, theirs, the game");
    MH_Uninitialize();

    printf("%s: %d passed, %d failed\n", g_fail ? "FAILED" : "PASSED", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
