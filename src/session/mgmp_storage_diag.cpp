// Read-only diagnostics for the warehouse/storage regression investigation.
#include "mgmp_storage_diag.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "mgmp_addresses.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_resolve.h"
#include "mgmp_savefile.h"
#include "mgmp_page.h"
#include "mgmp_follow.h"
#include "mgmp_lockstep.h"
#include "mgmp_unlocks.h"
#include <cstdlib>

namespace mgmp {
namespace {

constexpr uint8_t kPageHouse = 3;
constexpr uint8_t kPageCollar = 4;
constexpr uint8_t kPageEquipment = 5;
constexpr uintptr_t kBucketBytes = 64;
constexpr char kStorageProperty[] = "house_storage_upgrades";
// The House (MewDirector+0x5A0: the load 0x1403AEFD0 passes it to 0x1402062C0, which stores get("house_gold") at +0xB4, 0x14020685E).
constexpr uintptr_t kDir_House = 0x5A0;
constexpr uintptr_t kHouse_Gold = 0xB4;
constexpr int64_t kMissingProperty = INT64_MIN / 3;

struct GameStr {
    union { char buf[16]; char* ptr; };
    uint64_t size = 0;
    uint64_t cap = 0;
};
using fn_alloc = void* (__fastcall*)(size_t);
using fn_prop_int = int64_t (__fastcall*)(void*, GameStr*, int64_t);

uint8_t g_last_page = 0;
bool g_have_last_property = false;
int64_t g_last_local = 0;
int64_t g_last_answer = 0;
bool g_last_override = false;
bool g_internal_read = false;

uint64_t fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

const char* page_label(uint8_t p) {
    switch (p) {
        case kPageHouse: return "house";
        case kPageCollar: return "collar";
        case kPageEquipment: return "equipment";
        case 1: return "main_menu";
        case 2: return "save_slots";
        case 6: return "chapter";
        case 7: return "in_game";
        default: return "unknown";
    }
}

void utf8_path(const wchar_t* in, char* out, size_t cap) {
    if (!out || !cap) return;
    out[0] = 0;
    if (!in || !in[0]) return;
    WideCharToMultiByte(CP_UTF8, 0, in, -1, out, (int)cap, nullptr, nullptr);
    out[cap - 1] = 0;
}

bool make_key(GameStr& s, const char* text, fn_alloc alloc) {
    if (!text || !alloc) return false;
    const size_t n = strlen(text);
    memset(&s, 0, sizeof(s));
    if (n <= 15) {
        memcpy(s.buf, text, n + 1);
        s.size = n;
        s.cap = 15;
        return true;
    }
    size_t cap = n | 15;
    if (cap < 22) cap = 22;
    void* p = nullptr;
    __try { p = alloc(cap + 1); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!p) return false;
    memcpy(p, text, n + 1);
    s.ptr = (char*)p;
    s.size = n;
    s.cap = cap;
    return true;
}

bool director(void*& out) {
    out = nullptr;
    const uintptr_t at = addr_of_data(D_MewDirectorPtr);
    return at && mem_read((const void*)at, &out, sizeof(out)) && out;
}

struct BucketInfo {
    bool ok = false;
    uint32_t count = 0;
    uint64_t header_hash = 0;
};

BucketInfo bucket(void* inv, uintptr_t off) {
    BucketInfo out;
    if (!inv) return out;
    uint8_t raw[kBucketBytes] = {};
    if (!mem_read((const uint8_t*)inv + off, raw, sizeof(raw))) return out;
    memcpy(&out.count, raw + kBucket_Count, sizeof(out.count));
    // The intrusive-list sentinel is an allocator address. Exclude it so the
    // digest remains useful across restarts; keep the count and remaining
    // bucket header fields so a clear/rebuild is visible.
    memset(raw + kBucket_Head, 0, sizeof(void*));
    out.header_hash = fnv1a(raw, sizeof(raw));
    out.ok = true;
    return out;
}

bool read_property(void* dir, int64_t& value, const char* name = kStorageProperty) {
    value = 0;
    if (!dir) return false;
    auto get = (fn_prop_int)addr_of_call(C_PropGetInt);
    auto alloc = (fn_alloc)addr_of_call(C_GameStrAlloc);
    if (!get || !alloc) return false;
    GameStr key;
    if (!make_key(key, name, alloc)) return false;
    bool ok = false;
    g_internal_read = true;
    __try {
        value = get((uint8_t*)dir + kDir_SaveProps, &key, kMissingProperty);
        ok = value != kMissingProperty;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    g_internal_read = false;
    return ok;
}

// ============================================================================================================================================
// WHERE ITEMS COME FROM (0.1.52)
// ============================================================================================================================================
// The callers of Inventory::insert_item (0x1402E13F0) and of the helpers that call it, by function (RVA ranges of this build, from the disassembly).
struct ItemSource { uint32_t lo, hi; const char* what; bool helper; };   // helper: a general function -- the frame above it says more
const ItemSource kItemSources[] = {
    { 0x06EC30, 0x06F096, "RockyArmorPassive (a passive drops an item)", false },
    { 0x070F90, 0x071303, "DropSoulJarOnDeath (a death drops a soul jar)", false },
    { 0x071A80, 0x072190, "RerollItemsOnBattleEnd (battle-end reroll, combat_reward_hard)", false },
    { 0x0B3F70, 0x0B41A3, "trinket swap (Regurge)", false },
    { 0x0BC2D0, 0x0BD92C, "a cat is made: class bonus_items", false },
    { 0x0C3A00, 0x0C42EF, "cat item slot (lock_item_slot / grant)", true },
    { 0x0C4FC0, 0x0C5268, "cat bonus_items", true },
    { 0x0CA2E0, 0x0CA3AC, "a cat's gear goes to the inventory (equip swap)", true },
    { 0x0CA3B0, 0x0CA509, "a cat's gear goes to the inventory (slot replaced)", true },
    { 0x0CA510, 0x0CA5B5, "gear taken off a cat", true },
    { 0x0CA5C0, 0x0CA7B9, "CatData::equip_if_slot_free", true },
    { 0x0D3450, 0x0D372F, "the adventure starts: bonus_items", false },
    { 0x10A4C0, 0x10A600, "equipment broke in combat", false },
    { 0x1C18E0, 0x1C35AD, "House / NPC reward (house_upgrade, tracy_special_item ...)", false },
    { 0x1CC610, 0x1CE496, "progression unlock reward", false },
    { 0x1CEAE0, 0x1CF6EE, "quest item (quest_item_alias / legacy_quest)", false },
    { 0x1CFCB0, 0x1CFDCA, "progression reward item", true },
    { 0x1CFDD0, 0x1D09E3, "Beanies quest item", false },
    { 0x1D0ED0, 0x1D160C, "Beanies quest reward", false },
    { 0x217FB0, 0x21828B, "GiveBoundItemToTarget", false },
    { 0x24A970, 0x24AC01, "FindExtraItemFromPoolOnBattleEnd", false },
    { 0x2E15E0, 0x2E1659, "add an item (helper)", true },
    { 0x304030, 0x304356, "FindItem (an ability)", false },
    { 0x304360, 0x3047FF, "FindItemFromPool (an ability)", false },
    { 0x304800, 0x304C27, "DuplicateRandomEquippedItem", false },
    { 0x308970, 0x3093E0, "DestroyEquipmentAndAttachParasite", false },
    { 0x30A050, 0x30ACED, "StealEquipment", false },
    { 0x318DB0, 0x319168, "debug test map", false },
    { 0x3835D0, 0x383E35, "level-up option (LevelUpScreen::select_option)", false },
    { 0x391C20, 0x3929DF, "a map node is entered (MapScreen::EnterNode)", false },
    { 0x3A8570, 0x3A9261, "debug: adding items", false },
    { 0x3A9270, 0x3A9696, "debug console: Added", false },
    { 0x3A96A0, 0x3A9852, "debug console: Added", false },
    { 0x3B8290, 0x3B94E4, "BATTLE REWARD (combat_reward_easy/normal/miniboss/boss)", false },
    { 0x796070, 0x7971BB, "TREASURE CHEST (Shop::initShop takes every item of a treasure room)", false },
    { 0x79A3F0, 0x79A600, "SHOP PURCHASE (Shop_BuyItem)", false },
    { 0x79A600, 0x79A6B0, "shop: obtain", true },
    { 0x79ACB0, 0x79B211, "Shop::ObtainItem", true },
    { 0x7D5020, 0x7D5333, "ReturnBoundItemOnBattleEnd", false },
    { 0x7E2420, 0x7E2AF1, "BossRewards", false },
    { 0x88B480, 0x88BB14, "TacticsTest", false },
    { 0x8C5620, 0x8C577A, "ReclaimItemOnBreak", false },
    { 0x8D0A20, 0x8D1D08, "event (TrailerEvent)", false },
    { 0x91F790, 0x9205F7, "event: quest item / head / face / neck", false },
    { 0x920600, 0x9210E6, "EVENT RESULT: get_item / get_weapon / get_cursed_item", true },
    { 0x9173A0, 0x919752, "EVENT (get_item / get_item_from_pool)", false },
    { 0x930860, 0x9310A5, "EVENT REWARD (good/bad, common/rare)", false },
    { 0x9380A0, 0x93823A, "event effect", true },
    { 0x938240, 0x938639, "event effect", true },
};
const ItemSource* item_source(uintptr_t rva) {
    for (const ItemSource& s : kItemSources) if (rva >= s.lo && rva < s.hi) return &s;
    return nullptr;
}
const char* loc_name(int32_t loc) {
    switch (loc) {
        case 0: return "backpack";
        case 1: return "storage";
        case 2: return "trash";
        case 3: return "a cat or the backpack";
        default: return "?";
    }
}
const char* node_kind(uint32_t t) {
    static const char* k[] = { "none", "?1", "enter", "exit", "home", "battle", "hard", "miniboss", "boss", "event", "optional_event", "special_event",
                               "shop", "treasure", "furniturebox", "foodbox", "bonus", "empty", "?18" };
    return t < sizeof(k) / sizeof(k[0]) ? k[t] : "?";
}
uint32_t bucket_count(void* inv, uintptr_t off) {
    uint32_t n = 0;
    if (inv) mem_read((const uint8_t*)inv + off + kBucket_Count, &n, sizeof(n));
    return n;
}
void* my_inventory() {
    void* dir = nullptr; void* inv = nullptr;
    if (director(dir) && dir) mem_read((uint8_t*)dir + kDir_Inventory, &inv, sizeof(inv));
    return inv;
}
void context_text(char* out, size_t cap) {
    uint32_t idx = 0;
    const bool have = follow_current_node(idx);
    _snprintf_s(out, cap, _TRUNCATE, "node %s%u (%s) | page %s | battle %016llx | turn %u", have ? "" : "?", have ? idx : 0u, node_kind(follow_here_type()),
                page_name(page_self()), (unsigned long long)lockstep_battle_id(), (unsigned)log_turn());
}

// The names in one bucket: the intrusive list Inventory::collect walks (0x1402E1660): the sentinel at bucket+8, node = *node, the Equipment at node+0x18,
// its name the std::string at +8.
constexpr uint32_t kMaxItems = 400;
struct NameList { uint32_t n = 0; bool ok = false; char name[kMaxItems][40]; };
bool read_bucket(void* inv, uintptr_t off, NameList& out) {
    out.n = 0; out.ok = false;
    const uint8_t* sentinel = nullptr;
    if (!inv || !mem_read((const uint8_t*)inv + off + kBucket_Head, &sentinel, sizeof(sentinel)) || !sentinel) return false;
    const uint8_t* node = nullptr;
    if (!mem_read(sentinel, &node, sizeof(node))) return false;
    for (uint32_t guard = 0; node && node != sentinel && guard < 4096; ++guard) {
        if (out.n < kMaxItems) {
            char* nm = out.name[out.n++];
            if (!mem_read_std_string(node + 0x18 + 8, nm, 40) || !nm[0]) strcpy_s(nm, 40, "?");
        }
        if (!mem_read(node, &node, sizeof(node))) return false;
    }
    out.ok = node == sentinel;
    return out.ok;
}
int name_cmp(const void* a, const void* b) { return strcmp((const char*)a, (const char*)b); }

struct Bucket { uintptr_t off; const char* label; };
const Bucket kBuckets[3] = { { kInv_Backpack, "backpack" }, { kInv_Storage, "storage" }, { kInv_Trash, "trash" } };
NameList g_last[3];
bool g_have_last[3] = {};
void* g_last_inv = nullptr;

// added / removed between two sorted lists, as "+[a, b x2] -[c]"
void diff_text(const NameList& was, const NameList& now, char* out, size_t cap, uint32_t& plus, uint32_t& minus) {
    plus = minus = 0;
    char add[700] = {}, del[700] = {}; size_t wa = 0, wd = 0;
    auto put = [](char* buf, size_t bcap, size_t& w, const char* name, uint32_t times) {
        if (w + 50 >= bcap) { if (w + 5 < bcap) { strcpy_s(buf + w, bcap - w, " ..."); w += 4; } return; }
        const int k = times > 1 ? _snprintf_s(buf + w, bcap - w, _TRUNCATE, "%s%s x%u", w ? ", " : "", name, times)
                                : _snprintf_s(buf + w, bcap - w, _TRUNCATE, "%s%s", w ? ", " : "", name);
        if (k > 0) w += (size_t)k;
    };
    uint32_t i = 0, j = 0;
    while (i < was.n || j < now.n) {
        const int cmp = i >= was.n ? 1 : j >= now.n ? -1 : strcmp(was.name[i], now.name[j]);
        if (cmp == 0) { ++i; ++j; continue; }
        if (cmp < 0) {      // only in the old list: removed (count the run of the same name)
            uint32_t k = i; while (k < was.n && strcmp(was.name[k], was.name[i]) == 0) ++k;
            uint32_t m = j; while (m < now.n && strcmp(now.name[m], was.name[i]) == 0) ++m;
            (void)m;
            put(del, sizeof(del), wd, was.name[i], 1); ++minus; ++i;
        } else {
            put(add, sizeof(add), wa, now.name[j], 1); ++plus; ++j;
        }
    }
    _snprintf_s(out, cap, _TRUNCATE, "+[%s] -[%s]", add, del);
}

void log_full(const char* label, const NameList& l, const char* why) {
    char line[900]; size_t w = 0; uint32_t first = 0;
    for (uint32_t i = 0; i < l.n; ++i) {
        if (w == 0) first = i;
        const int k = _snprintf_s(line + w, sizeof(line) - w, _TRUNCATE, "%s%s", w ? ", " : "", l.name[i]);
        if (k > 0) w += (size_t)k;
        if (w > sizeof(line) - 60 || i + 1 == l.n || i + 1 - first >= 24) {
            log_line("ITEMS", "  %s (%s) %u-%u of %u: %s", label, why, first + 1, i + 1, l.n, line);
            w = 0;
        }
    }
    if (!l.n) log_line("ITEMS", "  %s (%s): empty", label, why);
}

// THE WAREHOUSE MONEY AND THE STASH (0.1.55): the House's gold in memory, the save's house_gold / house_food / house_storage_upgrades /
// adventure_coins, and the inventory's coins -- each line with the unlock window's state (a load inside an open window is what took the
// host's money). The previous line's numbers are kept so a change is called out with where it happened between.
struct HouseNums { bool have = false, mem_ok = false; int32_t gold_mem = 0; int64_t gold = 0, food = 0, upg = 0, coins = 0; int32_t coins_mem = 0; char why[64] = {}; };
HouseNums g_house_last;

void house_line(const char* why) {
    void* dir = nullptr;
    if (!director(dir)) { log_line("HOUSE", "%s: no MewDirector (no save loaded)", why ? why : "?"); return; }
    HouseNums n;
    const uint8_t* house = nullptr;
    const bool mem_ok = mem_read((const uint8_t*)dir + kDir_House, &house, sizeof(house)) && house && mem_read(house + kHouse_Gold, &n.gold_mem, sizeof(n.gold_mem));
    const bool g_ok = read_property(dir, n.gold, "house_gold");
    const bool f_ok = read_property(dir, n.food, "house_food");
    const bool u_ok = read_property(dir, n.upg, kStorageProperty);
    const bool c_ok = read_property(dir, n.coins, "adventure_coins");
    // 0.1.83: a property the save has no row for reads as the getter's sentinel, never as a number of coins
    if (!g_ok) n.gold = 0;
    if (!f_ok) n.food = 0;
    if (!u_ok) n.upg = 0;
    if (!c_ok) n.coins = 0;
    void* inv = nullptr;
    const bool inv_ok = mem_read((const uint8_t*)dir + kDir_Inventory, &inv, sizeof(inv)) && inv && mem_read((const uint8_t*)inv + kInv_Coins, &n.coins_mem, sizeof(n.coins_mem));
    const char* w = unlocks_window_why();
    char text[96];
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%s", w ? w : "");
    log_line("HOUSE", "%s: warehouse gold %s%d (in the House) / %s%lld (in the save) | house_food %s%lld | storage upgrades %s%lld | carried coins %s%d (inventory) / %s%lld (save) | role %s, window %s%s%s",
             why ? why : "?", mem_ok ? "" : "?", mem_ok ? n.gold_mem : 0, g_ok ? "" : "?", (long long)(g_ok ? n.gold : 0), f_ok ? "" : "?", (long long)(f_ok ? n.food : 0),
             u_ok ? "" : "?", (long long)(u_ok ? n.upg : 0), inv_ok ? "" : "?", inv_ok ? n.coins_mem : 0, c_ok ? "" : "?", (long long)(c_ok ? n.coins : 0),
             net_role() == NetRole::Host ? "host" : net_role() == NetRole::Client ? "client" : "alone", w ? "OPEN (" : "closed", text, w ? ")" : "");
    n.have = g_ok;
    n.mem_ok = mem_ok && n.gold_mem != 0;
    strncpy_s(n.why, sizeof(n.why), why ? why : "?", _TRUNCATE);
    // 0.1.83: compared like with like -- the save's number with the save's (the House in memory is only filled after a load's own read, so right at
    // "save_loaded" it still holds 0, which compared against the save's figure was a false "!! warehouse gold" line at every load)
    if (g_house_last.have && n.have) {
        const int64_t before = g_house_last.gold;
        const int64_t now = n.gold;
        if (before != now) {
            const int64_t d = now - before;
            const bool big = (before > 0 && (d < 0 ? -d : d) * 2 > before) || (d < 0 ? -d : d) >= 500;
            log_line_lvl(big ? LogLevel::Warn : LogLevel::Info, "HOUSE", "%swarehouse gold %lld -> %lld (%+lld) between '%s' and '%s'%s", big ? "!! " : "",
                         (long long)before, (long long)now, (long long)d, g_house_last.why, n.why, w ? " -- with the unlock window open" : "");
        }
    }
    if (n.have) g_house_last = n;
}

void emit_snapshot(const char* why, uint8_t page) {
    house_line(why);
    void* dir = nullptr;
    uint8_t slot = 0;
    const bool have_slot = savefile_selected_slot(&slot);
    wchar_t wpath[MAX_PATH] = {};
    char path[MAX_PATH * 3] = {};
    if (savefile_live_path(wpath, MAX_PATH)) utf8_path(wpath, path, sizeof(path));

    int64_t prop = 0;
    const bool prop_ok = director(dir) && read_property(dir, prop);
    void* inv = nullptr;
    if (dir) mem_read((uint8_t*)dir + kDir_Inventory, &inv, sizeof(inv));
    const BucketInfo backpack = bucket(inv, kInv_Backpack);
    const BucketInfo storage = bucket(inv, kInv_Storage);
    const BucketInfo trash = bucket(inv, kInv_Trash);
    log_line("STORAGE", "%s page=%s role=%s self=%u slot=%s%u path=%s property=%s%lld backpack=%s%u/%016llx storage=%s%u/%016llx trash=%s%u/%016llx",
             why ? why : "snapshot", page_label(page), net_role() == NetRole::Host ? "host" : "client", (unsigned)net_self(),
             have_slot ? "" : "?", have_slot ? (unsigned)slot : 0u, path[0] ? path : "-",
             prop_ok ? "" : "?", (long long)(prop_ok ? prop : 0),
             backpack.ok ? "" : "?", backpack.count, (unsigned long long)backpack.header_hash,
             storage.ok ? "" : "?", storage.count, (unsigned long long)storage.header_hash,
             trash.ok ? "" : "?", trash.count, (unsigned long long)trash.header_hash);
}

} // namespace

void storage_diag_items(const char* why, bool full) {
    void* inv = my_inventory();
    if (!inv) return;
    if (inv != g_last_inv) { for (bool& h : g_have_last) h = false; g_last_inv = inv; }      // another inventory object: a new load
    char ctx[200]; context_text(ctx, sizeof(ctx));
    for (int b = 0; b < 3; ++b) {
        static NameList now;
        if (!read_bucket(inv, kBuckets[b].off, now)) continue;
        qsort(now.name, now.n, sizeof(now.name[0]), name_cmp);
        if (!g_have_last[b]) {
            log_line("ITEMS", "%s: %u item(s) (%s) | %s", kBuckets[b].label, now.n, why, ctx);
            if (full || b == 0) log_full(kBuckets[b].label, now, why);
        } else {
            char d[1500]; uint32_t plus = 0, minus = 0;
            diff_text(g_last[b], now, d, sizeof(d), plus, minus);
            if (plus || minus) {
                log_line("ITEMS", "%s changed (%s): %s -- %u -> %u item(s) | %s", kBuckets[b].label, why, d, g_last[b].n, now.n, ctx);
                if (full) log_full(kBuckets[b].label, now, why);
            }
        }
        memcpy(&g_last[b], &now, sizeof(now));
        g_have_last[b] = true;
    }
}

void storage_diag_item_insert_before(ItemInsertNote& note, void* inv, void* item, int32_t loc, int64_t arg, void* ret, void* const* frames, unsigned nframes) {
    note = ItemInsertNote{};
    if (!item) return;
    note.on = true;
    note.inv = inv; note.loc = loc; note.arg = arg;
    if (!mem_read_std_string((const uint8_t*)item + 8, note.name, sizeof(note.name)) || !note.name[0]) strcpy_s(note.name, "?");
    mem_read((const uint8_t*)item + 0x50, &note.flags, sizeof(note.flags));
    mem_read((const uint8_t*)item + 0x48, &note.durability, sizeof(note.durability));
    note.mine = inv && inv == my_inventory();
    for (int b = 0; b < 3; ++b) note.before[b] = bucket_count(inv, kBuckets[b].off);
    // the caller chain: each frame named where it is known; the first non-helper frame is where the item really came from
    static const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    uintptr_t chain[8] = {}; unsigned n = 0;
    const uintptr_t r0 = (uintptr_t)ret;
    if (!nframes || (uintptr_t)frames[0] != r0) chain[n++] = r0;
    for (unsigned i = 0; i < nframes && n < 8; ++i) chain[n++] = (uintptr_t)frames[i];
    size_t w = 0; const char* main_src = nullptr;
    for (unsigned i = 0; i < n && i < 6; ++i) {
        const uintptr_t a = chain[i];
        const bool in_game = a >= base && a < base + 0x2000000;
        const uintptr_t rva = in_game ? a - base : 0;
        const ItemSource* src = in_game ? item_source(rva) : nullptr;
        if (src && !main_src && !src->helper) main_src = src->what;
        const int k = in_game ? _snprintf_s(note.source + w, sizeof(note.source) - w, _TRUNCATE, "%s0x%06llX%s%s%s", w ? " <- " : "", (unsigned long long)rva,
                                            src ? " (" : "", src ? src->what : "", src ? ")" : "")
                              : _snprintf_s(note.source + w, sizeof(note.source) - w, _TRUNCATE, "%s[outside the game]", w ? " <- " : "");
        if (k <= 0) break;
        w += (size_t)k;
    }
    if (main_src) {
        char tmp[420];
        _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s | chain: %s", main_src, note.source);
        strcpy_s(note.source, tmp);
    } else {
        char tmp[420];
        _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "unnamed caller | chain: %s", note.source);
        strcpy_s(note.source, tmp);
    }
}

void storage_diag_item_insert_after(const ItemInsertNote& note) {
    if (!note.on) return;
    uint32_t after[3] = {};
    for (int b = 0; b < 3; ++b) after[b] = bucket_count(note.inv, kBuckets[b].off);
    char where[80] = {};
    for (int b = 0; b < 3; ++b)
        if (after[b] != note.before[b]) { _snprintf_s(where, sizeof(where), _TRUNCATE, "%s %u -> %u", kBuckets[b].label, note.before[b], after[b]); break; }
    char ctx[200]; context_text(ctx, sizeof(ctx));
    log_line("ITEMGET", "+ '%s' -> %s (loc %d%s%llx) %s| %s | %s | durability %d flags %x%s",
             note.name, loc_name(note.loc), note.loc, note.loc == 3 ? ", cat " : ", arg ", (unsigned long long)note.arg,
             note.mine ? "" : "[NOT this player's inventory] ", where[0] ? where : "no count changed (refused, or it went onto a cat)", note.source, note.durability, note.flags,
             "");
    log_line("ITEMGET", "  ^ %s", ctx);
}

void storage_diag_page(uint8_t page) {
    if (page == g_last_page) return;
    const uint8_t old = g_last_page;
    g_last_page = page;
    const bool relevant = old == kPageHouse || old == kPageCollar || old == kPageEquipment ||
                          page == kPageHouse || page == kPageCollar || page == kPageEquipment;
    if (!relevant) return;
    log_line("STORAGE", "page transition %s -> %s", page_label(old), page_label(page));
    emit_snapshot(page == kPageHouse ? "house_enter" : page == kPageEquipment ? "equipment_enter" : "page_change", page);
    char why[64];
    _snprintf_s(why, sizeof(why), _TRUNCATE, "page %s -> %s", page_label(old), page_label(page));
    storage_diag_items(why, page == kPageHouse);
}

void storage_diag_house(const char* why) { house_line(why); }

void storage_diag_save_loaded() {
    emit_snapshot("save_loaded", g_last_page);
    for (bool& h : g_have_last) h = false;          // a load: list everything afresh
    storage_diag_items("a save was loaded", false);
}

void storage_diag_property_get(const char* key, int64_t local, int64_t answer, bool overridden) {
    if (g_internal_read || !key || strcmp(key, kStorageProperty) != 0) return;
    if (!g_have_last_property || local != g_last_local || answer != g_last_answer || overridden != g_last_override) {
        g_have_last_property = true;
        g_last_local = local;
        g_last_answer = answer;
        g_last_override = overridden;
        log_line("STORAGE", "property_get key=%s local=%lld answer=%lld overridden=%d page=%s",
                 kStorageProperty, (long long)local, (long long)answer, overridden ? 1 : 0, page_label(g_last_page));
    }
}

void storage_diag_property_set(const char* key, int value) {
    if (!key || strcmp(key, kStorageProperty) != 0) return;
    log_line("STORAGE", "property_set key=%s value=%d page=%s", kStorageProperty, value, page_label(g_last_page));
    emit_snapshot("property_set", g_last_page);
}

} // namespace mgmp
