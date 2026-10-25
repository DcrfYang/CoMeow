// mgmp_gains.cpp -- see mgmp_gains.h.
#include "mgmp_gains.h"

#include <windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <utility>

#include "MinHook.h"
#include "mgmp_addresses.h"
#include "mgmp_lockstep.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_rngframe.h"

namespace mgmp {
namespace {

enum Kind : uint8_t {
    kPassive38,     // rcx = a passive; its carrier at +0x38
    kCharRdx,       // rdx = the character
    kEffCaster,     // an ability effect's wrapper: the caster (r9, else the 5th argument's ability +0x10)
    kEffTarget,     // ... the character the effect is on (rcx)
};
struct GainTarget { uint32_t rva; uint8_t head[8]; Kind kind; const char* what; };

// Each RVA's first eight bytes, as this game build has them (checked before wrapping, as mgmp_rngframe does).
const GainTarget kGainTargets[] = {
    { 0x071A80, { 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56 }, kPassive38, "RerollItemsOnBattleEnd" },
    { 0x24A970, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74 }, kPassive38, "FindExtraItemFromPoolOnBattleEnd" },
    { 0x7D5020, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x7C }, kPassive38, "ReturnBoundItemOnBattleEnd" },
    { 0x8C5620, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74 }, kPassive38, "ReclaimItemOnBreak" },
    { 0x0B3F70, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x4C, 0x89, 0x44 }, kCharRdx,   "trinket swap (Regurge)" },
    { 0x5E78C0, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x89, 0x54, 0x24 }, kEffCaster, "FindItem" },
    { 0x5E7AD0, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x89, 0x54, 0x24 }, kEffCaster, "FindItemFromPool" },
    { 0x61FF50, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x89, 0x54, 0x24 }, kEffCaster, "StealEquipment" },
    { 0x5E0CA0, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x4C, 0x89, 0x4C }, kEffTarget, "DuplicateRandomEquippedItem" },
    { 0x5DA770, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x4C, 0x89, 0x4C }, kEffTarget, "DestroyEquipmentAndAttachParasite" },
    { 0x5EDD50, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x89, 0x54, 0x24 }, kEffTarget, "GiveBoundItemToTarget" },
};
constexpr int kN = (int)(sizeof(kGainTargets) / sizeof(kGainTargets[0]));

constexpr uintptr_t kRa_RockyArmorInsert = 0x6F006;   // RockyArmorPassive (0x14006EC30, wrapped by mgmp_rngframe) -> Inventory::insert_item
constexpr uintptr_t kFn_RockyArmor = 0x6EC30;
constexpr uintptr_t kFn_InvInsert = 0x2E13F0;
constexpr uintptr_t kPassive_Owner = 0x38;
constexpr uintptr_t kAbility_Char = 0x10;
constexpr uintptr_t kChar_Ident = 0x958;               // the creation key (the same on every peer): for the log
constexpr uintptr_t kItem_Name = 0x08;
// Character::GainCoins(Character*, int): `test edx,edx; je ...` -- then, for a player-side character (+0xC94), the inventory's coins (+0x148).
constexpr uintptr_t kFn_GainCoins = 0x114E30;
constexpr uint8_t   kGainCoinsHead[8] = { 0x85, 0xD2, 0x0F, 0x84, 0xEB, 0x01, 0x00, 0x00 };
constexpr uintptr_t kGainCoins_InvLoad = 0x114E5C;    // mov r8, [rip+disp] -- the inventory global
constexpr uintptr_t kInv_Coins_Field = 0x148;

using fn8 = void* (__fastcall*)(void*, void*, void*, void*, void*, void*, void*, void*);
fn8 g_orig[kN] = {};
uintptr_t g_base = 0;
bool g_rocky_ok = false;
const void* const* g_inv_global = nullptr;
fn8 g_orig_coins = nullptr;

// The cat the item being produced is for: a stack, as one producer may run inside another.
struct Ctx { const void* chr; const char* what; };
Ctx g_stack[16];
int g_depth = 0;
unsigned g_logged = 0;

bool in_room() { return net_active() && net_peer_count() >= 2 && lockstep_active(); }

const void* read_ptr(const void* at) { const void* p = nullptr; if (at) mem_read(at, &p, sizeof(p)); return p; }

uint32_t ident(const void* chr) { uint32_t k = 0; if (chr) mem_read((const uint8_t*)chr + kChar_Ident, &k, sizeof(k)); return k; }

const void* responsible(Kind k, void* a, void* b, void* d, void* e) {
    switch (k) {
        case kPassive38: return read_ptr((const uint8_t*)a + kPassive_Owner);
        case kCharRdx:   return b;
        case kEffCaster: return d ? (const void*)d : (e ? read_ptr((const uint8_t*)e + kAbility_Char) : nullptr);
        case kEffTarget: return a;
    }
    return nullptr;
}

void* call(int i, void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h) {
    if (g_depth >= 16) return g_orig[i](a, b, c, d, e, f, g, h);
    g_stack[g_depth++] = Ctx{ responsible(kGainTargets[i].kind, a, b, d, e), kGainTargets[i].what };
    void* r = g_orig[i](a, b, c, d, e, f, g, h);
    --g_depth;
    return r;
}

template <int I>
void* __fastcall detour(void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h) { return call(I, a, b, c, d, e, f, g, h); }
template <int... I>
constexpr std::array<void*, sizeof...(I)> make_detours(std::integer_sequence<int, I...>) { return { { (void*)&detour<I>... } }; }
const std::array<void*, kN> kDetours = make_detours(std::make_integer_sequence<int, kN>{});

// COINS: another player's cat's gains and payments do not reach this player's coins.
void* __fastcall coins_detour(void* chr, void* amount, void* c, void* d, void* e, void* f, void* g, void* h) {
    const int32_t n = (int32_t)(intptr_t)amount;
    if (!n || !in_room() || !lockstep_peer_owns_character(chr) || !g_inv_global) return g_orig_coins(chr, amount, c, d, e, f, g, h);
    const void* inv = read_ptr(g_inv_global);
    int32_t before = 0;
    const bool have = inv && mem_read((const uint8_t*)inv + kInv_Coins_Field, &before, sizeof(before));
    void* r = g_orig_coins(chr, amount, c, d, e, f, g, h);
    if (have) {
        int32_t after = 0;
        mem_read((const uint8_t*)inv + kInv_Coins_Field, &after, sizeof(after));
        if (after != before) {
            mem_write((uint8_t*)inv + kInv_Coins_Field, &before, sizeof(before));
            if (g_logged < 200) {
                ++g_logged;
                log_line("GAINS", "coins: cat %08x (another player's) %s %d coin(s) -- this player's coins stay %d (the game made them %d); they are its owner's", ident(chr),
                         n > 0 ? "gained" : "paid", n > 0 ? n : -n, before, after);
            }
        }
    }
    return r;
}

bool check_head(uintptr_t rva, const uint8_t* want) {
    uint8_t head[8] = {};
    return mem_read((const void*)(g_base + rva), head, sizeof(head)) && memcmp(head, want, sizeof(head)) == 0;
}

bool call_target(uintptr_t ret, uintptr_t fn) {
    uint8_t op = 0; int32_t rel = 0;
    if (!mem_read((const void*)(g_base + ret - 5), &op, 1) || !mem_read((const void*)(g_base + ret - 4), &rel, 4)) return false;
    return op == 0xE8 && ret + (intptr_t)rel == fn;
}

} // namespace

int gains_install(uintptr_t base) {
    g_base = base;
    if (!base) return 0;
    int ok = 0, refused = 0;
    for (int i = 0; i < kN; ++i) {
        const GainTarget& t = kGainTargets[i];
        if (!check_head(t.rva, t.head)) { ++refused; log_raw("  [!] GAINS    %-34s rva 0x%07X: the first bytes are not this build's -- not wrapped", t.what, t.rva); continue; }
        uint8_t* at = (uint8_t*)(base + t.rva);
        if (MH_CreateHook(at, kDetours[i], (void**)&g_orig[i]) != MH_OK || MH_EnableHook(at) != MH_OK) {
            ++refused; log_raw("  [!] GAINS    %-34s rva 0x%07X: MinHook refused -- not wrapped", t.what, t.rva); continue;
        }
        ++ok;
    }
    g_rocky_ok = call_target(kRa_RockyArmorInsert, kFn_InvInsert);
    // the coins: the function's own load of the inventory global names it
    if (check_head(kFn_GainCoins, kGainCoinsHead)) {
        uint8_t ins[7] = {};
        if (mem_read((const void*)(base + kGainCoins_InvLoad), ins, sizeof(ins)) && ins[0] == 0x4C && ins[1] == 0x8B && ins[2] == 0x05) {
            int32_t disp = 0; memcpy(&disp, ins + 3, 4);
            g_inv_global = (const void* const*)(base + kGainCoins_InvLoad + 7 + disp);
        }
        uint8_t* at = (uint8_t*)(base + kFn_GainCoins);
        if (g_inv_global && MH_CreateHook(at, (void*)&coins_detour, (void**)&g_orig_coins) == MH_OK && MH_EnableHook(at) == MH_OK) ++ok;
        else { ++refused; log_raw("  [!] GAINS    Character::GainCoins: not wrapped (%s)", g_inv_global ? "MinHook refused" : "the inventory load is not where this build has it"); }
    } else { ++refused; log_raw("  [!] GAINS    Character::GainCoins rva 0x%07X: the first bytes are not this build's -- not wrapped", (unsigned)kFn_GainCoins); }
    log_raw("  [+] GAINS    %d of %d item/coin producers wrapped (%d refused)%s", ok, kN + 1, refused, g_rocky_ok ? "" : " -- RockyArmor's insert is not where this build has it");
    return ok;
}

bool gains_insert_skip(void* item, int32_t loc, uintptr_t ret) {
    if (!g_base || !in_room()) return false;
    const void* chr = nullptr;
    const char* what = nullptr;
    if (g_rocky_ok && ret - g_base == kRa_RockyArmorInsert) {
        chr = read_ptr((const uint8_t*)rngframe_current_passive() + kPassive_Owner);
        what = "RockyArmorPassive";
        if (!rngframe_current_passive()) chr = nullptr;
    } else if (g_depth > 0) {
        chr = g_stack[g_depth - 1].chr;
        what = g_stack[g_depth - 1].what;
    }
    if (!chr || !lockstep_peer_owns_character(chr)) return false;   // this player's own cat, an enemy, a summon, or no battle roster: the game's own way
    if (g_logged < 200) {
        ++g_logged;
        char name[64] = "?";
        if (item) mem_read_std_string((const uint8_t*)item + kItem_Name, name, sizeof(name));
        log_line("GAINS", "item: '%s' from %s is for cat %08x, another player's -- not put into this player's inventory (loc %d); its owner's game puts it into its own",
                 name, what ? what : "?", ident(chr), (int)loc);
    }
    return true;
}

} // namespace mgmp
