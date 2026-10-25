// mgmp_treasure.cpp -- EXPERIMENTAL: the shared treasure chest. See mgmp_treasure.h.
#include "mgmp_treasure.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

#include "mgmp_addresses.h"
#include "mgmp_follow.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_resolve.h"
#include "mgmp_rng.h"
#include "mgmp_room.h"
#include "mgmp_session.h"
#include "mgmp_unlocks.h"
#include "mgmp_i18n.h"     // tr: the hint in the room's banner

namespace mgmp {

namespace {

// glaiel::Shop (0x140794CF0 initShop, 0x140796070 its stock half, 0x140797EA0 update).
constexpr uintptr_t kShop_Def      = 0x50;   // GonObject*: the shop's definition
constexpr uintptr_t kShop_Stock    = 0x60;   // vector<ShopItem> {first, last, end}
constexpr uintptr_t kShop_Treasure = 0x79;   // u8: meta.treasure_room
constexpr uintptr_t kShop_Opened   = 0x7A;   // u8: the stock was taken (a treasure room then closes itself, Shop::update)
constexpr uintptr_t kShop_Given    = 0x7C;   // u8: the stock was handed in at init -- the stock half then rolls nothing (read only there)
constexpr uintptr_t kShop_Luck     = 0x90;   // double: what the roller is given (difficulty's bonus_itemroll_luck + the party's luck)
// ShopItem, 0xC0 bytes: +0x00 kind (1 = an item), +0x08 price, +0x0C stock, +0x10 std::string, +0x38 Item (0x60: +0x08 its gon name), +0x98 std::wstring shown name.
constexpr uintptr_t kShopItemSize  = 0xC0;
constexpr uintptr_t kSI_Kind = 0x00, kSI_Str = 0x10, kSI_ItemName = 0x38 + 0x08, kSI_Shown = 0x98;
constexpr uint32_t  kNodeTreasure  = 13;     // MapNode type "treasure" (mgmp_follow node_type_name)
// glaiel::GonObject (0xB0 bytes): its own name is a std::string at +0x88 (what GON's "Field ... missing" error prints, 0x140940D30).
constexpr uintptr_t kGon_Name     = 0x88;

// ONLY THE ORDINARY CHESTS ARE SHARED (2026-10-20, from the live test): the treasure nodes of data/shops/combat_shops.gon whose item comes from a
// pool (TreasureEasy: treasure_easy, TreasureHard: treasure_hard). The special chests -- money bags, furniture boxes, meteors, the glowing bone,
// the ancestors' skull, the irradiated object, the ability needle, the tutorial's stick -- name their item outright and stay as the game does them.
bool shared_chest(const char* def_name) { return !strcmp(def_name, "TreasureEasy") || !strcmp(def_name, "TreasureHard"); }
// The same question from an item, for a definition whose name cannot be read: the special chests' items (by their gon names).
bool special_item(const char* gon) {
    static const char* const kPrefix[] = { "MoneyBag", "FurnitureBox", "SmallMeteor", "LargeMeteor", "GlowingBone", "Ancestors", "IrradiatedObject",
                                           "LearnAbility", "LearnPassive", "TutorialStick" };
    for (const char* p : kPrefix) if (!strncmp(gon, p, strlen(p))) return true;
    return false;
}
constexpr ULONGLONG kClaimTimeoutMs = 5000;

struct Cand {
    alignas(16) uint8_t vec[24] = {};   // vector<ShopItem>, the game's allocation
    uint32_t count = 0;
    uint64_t key = 0;
    char     name[96] = {};
    char     desc[1024] = {};           // 0.1.87: the game's description of the item, as plain lines (treasure_desc_plain)
};

struct State {
    bool     open = false;
    void*    shop = nullptr;
    uint64_t shop_vt = 0;
    uint64_t node = 0;
    uint64_t known_node = 0;     // the node the takers / host keys below are about
    uint8_t  n = 0;
    Cand     c[kTreasureMaxSlots];
    uint8_t  taker[kTreasureMaxSlots];
    uint64_t host_keys[kTreasureMaxSlots] = {};
    bool     mismatch = false;
    int      pending = -1;
    ULONGLONG pending_at = 0;
    int      granted = -1;
    bool     skip = false;
    void   (*original)(void*) = nullptr;
    bool     releasing = false;
    // THE HOST'S PARTY LUCK (proto 101): the node it is for, and the value in millionths
    uint64_t luck_node = 0;
    int64_t  luck_e6 = 0;
    State() { for (uint8_t& t : taker) t = kTreasureNobody; }
} g;

TreasureLedger g_ledger;     // the host's
void vis_reset();            // the room's own row (below): forgotten, not touched
CRITICAL_SECTION g_cs;
INIT_ONCE g_cs_once = INIT_ONCE_STATIC_INIT;
BOOL CALLBACK init_cs(PINIT_ONCE, PVOID, PVOID*) { InitializeCriticalSection(&g_cs); return TRUE; }
struct Lock {
    Lock()  { InitOnceExecuteOnce(&g_cs_once, init_cs, nullptr, nullptr); EnterCriticalSection(&g_cs); }
    ~Lock() { LeaveCriticalSection(&g_cs); }
};

uint64_t mix(uint64_t& x) {
    uint64_t z = (x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
uint64_t fnv(uint64_t h, const void* p, size_t n) {
    for (size_t i = 0; i < n; ++i) { h ^= ((const uint8_t*)p)[i]; h *= 1099511628211ull; }
    return h;
}

uint8_t self_id() { return net_role() == NetRole::Host ? kHostPeer : net_self(); }

// --- the game's calls (SEH: functions of their own, no C++ objects) ---------------------------------------------------------------------------
typedef void* (__fastcall* fn_gen)(void* out, void* def, double luck, const void* initial, const void* key);
typedef void* (__fastcall* fn_move)(void* dst, void* src);
typedef void  (__fastcall* fn_free)(void* vec);
typedef void* (__fastcall* fn_str)(void* out, const char* s);
typedef void  (__fastcall* fn_strdtor)(void* s);

bool call_gen(void* out, void* def, double luck) {
    const uintptr_t gen = addr_of_call(C_ShopItemsGen), sctor = addr_of_call(C_StrFromCStr), sdtor = addr_of_call(C_GameStrDtor);
    if (!gen || !sctor || !sdtor) return false;
    __try {
        alignas(16) uint8_t key[32];
        alignas(16) uint8_t initial[24];
        memset(key, 0, sizeof(key)); memset(initial, 0, sizeof(initial));
        ((fn_str)sctor)(key, "breakdown");
        ((fn_gen)gen)(out, def, luck, initial, key);
        ((fn_strdtor)sdtor)(key);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool call_free(void* vec) {
    const uintptr_t fr = addr_of_call(C_ShopItemsFree);
    if (!fr) return false;
    __try { ((fn_free)fr)(vec); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool call_move(void* dst, void* src) {
    const uintptr_t mv = addr_of_call(C_ShopItemsMove);
    if (!mv) return false;
    __try { ((fn_move)mv)(dst, src); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool call_original(void (*fn)(void*), void* shop) {
    __try { fn(shop); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// THE PARTY'S LUCK, ASKED OF THE GAME (2026-10-22, the live test: the host rolled its chest with luck 1.30 and 1.40, the client with 0.50 both
// times, and every item differed). Shop::initShop gives the roller the difficulty's bonus_itemroll_luck plus 0x1403BA770(director): 0.1 x the
// sum of (LCK - 5) over the director's party list -- and a client's party list is ITS OWN cats (the host's are in its familiars). The shared
// stream is put back: the stats it reads are computed, not drawn, but nothing here may move it.
bool party_luck(double& out) {
    typedef double (__fastcall* fn_luck)(void*);
    const uintptr_t fn = addr_of_call(C_PartyLuck), dslot = addr_of_data(D_MewDirectorPtr);
    void* dir = nullptr;
    if (!fn || !dslot || !mem_read((const void*)dslot, &dir, sizeof(dir)) || !dir) return false;
    uint64_t* s = rng_global_stream();
    uint64_t saved[4] = {};
    const bool fence = s && mem_read(s, saved, sizeof(saved));
    bool ok = false;
    __try { out = ((fn_luck)fn)(dir); ok = out > -1000.0 && out < 1000.0; } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    if (fence) mem_write(s, saved, sizeof(saved));
    return ok;
}
// THE ITEM POOLS THE CHEST ROLLS FROM (0.1.75, room QLQC: the same luck 1.70 and the same first item, then 'Teleport' on the host and 'Ankh' on the
// client). The pools are not asked about unlocks at the roll: MewDirector::init builds them at every load (sub_1407AB3D0), keeping only the items
// the save had unlocked THEN, and every roll after that draws from those cached lists. Answering the unlock checks with the host's lists during
// the roll therefore changed nothing -- the client's pools still held what ITS save had unlocked (the Ankh). So around a shared chest's roll the
// pools are built again: on a client with the host's unlock answers, and once more afterwards with its own; on the host with its own answers as
// they are now (what it publishes to the clients at this node), not as they were at its last load. The registry is the global the AnyUnlocked
// pick reads (AlliesScrambleSpellAfterCast+0x53: mov rbx, [rip+disp]).
typedef void (__fastcall* fn_pools_build)(void* registry);
bool call_pools_build(uintptr_t fn, void* reg) {
    __try { ((fn_pools_build)fn)(reg); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool rebuild_item_pools(const char* how) {
    const uintptr_t fn = addr_of_call(C_ItemPoolsBuild);
    const uint8_t* at = (const uint8_t*)addr_of(T_AlliesScramble);
    uint8_t b[7] = {};
    void* reg = nullptr;
    if (fn && at && mem_read(at + 0x53, b, sizeof(b)) && b[0] == 0x48 && b[1] == 0x8B && b[2] == 0x1D) {
        int32_t d = 0; memcpy(&d, b + 3, 4);
        mem_read(at + 0x5A + d, &reg, sizeof(reg));
    }
    if (!reg) {
        log_line_lvl(LogLevel::Warn, "TREASURE", "!! the item pools could not be rebuilt %s (%s) -- the chest rolls from the pools of this save's last load",
                     how, !fn ? "the build function did not resolve" : "the item registry was not found");
        return false;
    }
    uint64_t* s = rng_global_stream();
    uint64_t saved[4] = {};
    const bool fence = s && mem_read(s, saved, sizeof(saved));
    const ULONGLONG t0 = GetTickCount64();
    const bool ok = call_pools_build(fn, reg);
    if (fence) mem_write(s, saved, sizeof(saved));
    if (ok) log_line("TREASURE", "the item pools rebuilt %s (%llu ms)", how, (unsigned long long)(GetTickCount64() - t0));
    else    log_line_lvl(LogLevel::Error, "TREASURE", "!! rebuilding the item pools %s FAULTED", how);
    return ok;
}

int64_t to_e6(double v) { return (int64_t)(v >= 0 ? v * 1e6 + 0.5 : v * 1e6 - 0.5); }

// --- THE ITEM'S DESCRIPTION (0.1.87) --------------------------------------------------------------------------------------------------------------
// The text the game shows for an item: Shop's slot filler (C_ShopSlotUi, sub_1407988E0) makes it for an item slot (ShopItem kind 1) with
//     lea r8,[r14+38h] / lea rdx,[rsp+60h] / mov rcx,[rip+TEXTS] / call DESC / mov rcx,[rbp+128h] / add rcx,1D8h / mov rdx,rax / call ASSIGN /
//     lea rcx,[rsp+60h] / call TIDY
// DESC(texts, std::wstring* out, const Item*) writes the description into `out` (cleared first; reads rcx, rdx and r8 only -- r9 is written before any
// read, no stack argument) and TIDY (sub_140052290) is std::wstring's _Tidy_deallocate. All three are taken from that call, checked byte for byte.
constexpr uint8_t kDescSite[53] = {
    0x4D, 0x8D, 0x46, 0x38,  0x48, 0x8D, 0x54, 0x24, 0x60,  0x48, 0x8B, 0x0D, 0, 0, 0, 0,  0xE8, 0, 0, 0, 0,
    0x48, 0x8B, 0x8D, 0x28, 0x01, 0x00, 0x00,  0x48, 0x81, 0xC1, 0xD8, 0x01, 0x00, 0x00,  0x48, 0x8B, 0xD0,  0xE8, 0, 0, 0, 0,
    0x48, 0x8D, 0x4C, 0x24, 0x60,  0xE8, 0, 0, 0, 0 };
bool desc_wild(size_t i) { return (i >= 12 && i < 16) || (i >= 17 && i < 21) || (i >= 39 && i < 43) || (i >= 49 && i < 53); }
struct DescFns { bool tried = false; uintptr_t fn = 0, tidy = 0; void* const* texts = nullptr; } g_desc;

void desc_resolve() {
    if (g_desc.tried) return;
    g_desc.tried = true;
    const uintptr_t filler = addr_of_call(C_ShopSlotUi);
    static uint8_t buf[0x1C00];
    if (!filler || !mem_read((const void*)filler, buf, sizeof(buf))) { log_line_lvl(LogLevel::Warn, "TREASURE", "!! item descriptions: the slot filler cannot be read -- none shown"); return; }
    for (size_t at = 0; at + sizeof(kDescSite) <= sizeof(buf); ++at) {
        bool hit = true;
        for (size_t i = 0; i < sizeof(kDescSite) && hit; ++i) hit = desc_wild(i) || buf[at + i] == kDescSite[i];
        if (!hit) continue;
        int32_t d = 0, r1 = 0, r3 = 0;
        memcpy(&d, buf + at + 12, 4); memcpy(&r1, buf + at + 17, 4); memcpy(&r3, buf + at + 49, 4);
        const uintptr_t site = filler + at;
        g_desc.texts = (void* const*)(site + 16 + (intptr_t)d);
        g_desc.fn    = site + 21 + (intptr_t)r1;
        g_desc.tidy  = site + 53 + (intptr_t)r3;
        log_line("TREASURE", "item descriptions: the game's builder found in the slot filler (+0x%zx)", at);
        return;
    }
    log_line_lvl(LogLevel::Warn, "TREASURE", "!! item descriptions: the builder's call is not where this build has it -- none shown");
}

typedef void* (__fastcall* fn_desc)(void* texts, void* out, const void* item);
typedef void  (__fastcall* fn_wtidy)(void* ws);
// A description as UTF-8. Not mem_read_std_wstring_utf8: that one refuses a control character and 256 characters or more, and a description has line
// breaks and can be long (0.1.87 live: the second item of every chest came back "could not be made").
bool desc_utf8(const void* ws, char* out, size_t cap) {
    out[0] = 0;
    uint64_t size = 0, wcap = 0;
    if (!mem_read((const uint8_t*)ws + 16, &size, 8) || !mem_read((const uint8_t*)ws + 24, &wcap, 8) || wcap < 7 || size > wcap) return false;
    if (!size) return true;
    static wchar_t chars[2048];
    if (size >= sizeof(chars) / sizeof(chars[0])) size = sizeof(chars) / sizeof(chars[0]) - 1;
    const void* data = ws;
    if (wcap > 7 && (!mem_read(ws, &data, sizeof(data)) || !data)) return false;
    if (!mem_read(data, chars, (size_t)size * 2)) return false;
    const int n = WideCharToMultiByte(CP_UTF8, 0, chars, (int)size, out, (int)cap - 1, nullptr, nullptr);
    if (n <= 0) { out[0] = 0; return false; }
    out[n] = 0;
    return true;
}
bool call_desc(void* texts, const void* item, char* utf8, size_t cap) {
    __try {
        alignas(16) uint8_t ws[32];
        memset(ws, 0, sizeof(ws));
        ws[0x18] = 7;                                                          // an empty std::wstring (capacity 7, inline)
        ((fn_desc)g_desc.fn)(texts, ws, item);
        const bool ok = desc_utf8(ws, utf8, cap);
        ((fn_wtidy)g_desc.tidy)(ws);
        return ok;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

const char* img_word(const char* n, size_t len) {
    struct W { const char* name; Tx tx; };
    static const W kW[] = { { "str", Tx::IMG_STR }, { "dex", Tx::IMG_DEX }, { "con", Tx::IMG_CON }, { "int", Tx::IMG_INT }, { "cha", Tx::IMG_CHA },
                            { "spd", Tx::IMG_SPD }, { "lck", Tx::IMG_LCK }, { "shield", Tx::IMG_SHIELD }, { "divineshield", Tx::IMG_DIVINESHIELD } };
    for (const W& x : kW) if (strlen(x.name) == len && !_strnicmp(n, x.name, len)) return tr(x.tx);
    return nullptr;
}

// The description of a rolled item slot (kind 1), as plain lines; "" for anything else or when the builder is not there.
void describe_effect(const uint8_t* shop_item, char* out, size_t cap) {
    out[0] = 0;
    uint32_t kind = 0;
    if (!mem_read(shop_item + kSI_Kind, &kind, sizeof(kind)) || kind != 1) return;
    desc_resolve();
    void* texts = nullptr;
    if (!g_desc.fn || !g_desc.tidy || !g_desc.texts || !mem_read(g_desc.texts, &texts, sizeof(texts)) || !texts) return;
    static char raw[2048];
    raw[0] = 0;
    if (!call_desc(texts, shop_item + 0x38, raw, sizeof(raw))) { log_line_lvl(LogLevel::Warn, "TREASURE", "!! an item's description could not be made -- none shown"); return; }
    treasure_desc_plain(raw, out, cap, img_word);
    char head[160];
    _snprintf_s(head, sizeof(head), _TRUNCATE, "%s", raw);
    for (char* q = head; *q; ++q) if (*q == '\n') *q = '|';
    log_line("TREASURE", "    its description: %s%s", head, strlen(raw) >= sizeof(head) - 1 ? " ..." : "");
}

// One rolled ShopItem: its identity (kind, its string, its item's gon name -- the same in every language) and the name the game shows.
char g_last_gon[96] = {};   // the gon name describe() last read (the special-chest check)
bool describe(const uint8_t* item, uint64_t& key, char* shown, size_t cap) {
    uint32_t kind = 0;
    if (!mem_read(item + kSI_Kind, &kind, sizeof(kind))) return false;
    char s[96] = {}, nm[96] = {};
    mem_read_std_string(item + kSI_Str, s, sizeof(s));
    mem_read_std_string(item + kSI_ItemName, nm, sizeof(nm));
    memcpy(g_last_gon, nm, sizeof(g_last_gon));
    uint64_t h = fnv(1469598103934665603ull, &kind, sizeof(kind));
    h = fnv(h, s, strlen(s) + 1);
    h = fnv(h, nm, strlen(nm) + 1);
    key = h ? h : 1;
    shown[0] = 0;
    if (!mem_read_std_wstring_utf8(item + kSI_Shown, shown, cap) || !shown[0])
        _snprintf_s(shown, cap, _TRUNCATE, "%s", nm[0] ? nm : (s[0] ? s : "?"));
    log_line("TREASURE", "  kind %u, '%s' / item '%s', shown as '%s' (key %016llx)", kind, s, nm, shown, (unsigned long long)key);
    return true;
}

bool vec_range(const uint8_t* vec, uint8_t*& first, uint32_t& count) {
    uint8_t* p[2] = {};
    if (!mem_read(vec, p, sizeof(p))) return false;
    first = p[0];
    if (!p[0] || p[1] < p[0] || (uintptr_t)(p[1] - p[0]) % kShopItemSize) { count = 0; return p[0] == p[1]; }
    count = (uint32_t)((p[1] - p[0]) / kShopItemSize);
    return true;
}

void free_candidates() {
    for (uint8_t k = 0; k < g.n; ++k) {
        uint64_t first = 0;
        mem_read(g.c[k].vec, &first, sizeof(first));
        if (first && !call_free(g.c[k].vec))
            log_line_lvl(LogLevel::Warn, "TREASURE", "!! candidate %u could not be freed -- left as it is", k);
        g.c[k] = Cand{};
    }
    g.n = 0;
}

void close_picker(const char* why) {
    if (!g.open) return;
    free_candidates();
    log_line("TREASURE", "picker closed: %s", why);
    vis_reset();
    g.open = false; g.shop = nullptr; g.pending = -1; g.granted = -1; g.skip = false;
}

// The shop is still the one the picker was opened for, and still waiting for its stock.
bool shop_waiting(void* shop) {
    uint64_t vt = 0;
    uint8_t flags[4] = {};
    uint64_t stock[2] = {};
    if (!shop || !mem_read(shop, &vt, sizeof(vt)) || vt != g.shop_vt) return false;
    if (!mem_read((uint8_t*)shop + kShop_Treasure, flags, 2) || !flags[0] || flags[1]) return false;
    if (!mem_read((uint8_t*)shop + kShop_Stock, stock, sizeof(stock))) return false;
    return stock[0] == stock[1];
}

void send_state() {
    if (net_role() != NetRole::Host || !g_ledger.node) return;
    TreasureMsg m;
    m.kind = kTreasureState; m.node = g_ledger.node; m.n = kTreasureMaxSlots;
    for (uint8_t i = 0; i < kTreasureMaxSlots; ++i) {
        m.taker[i] = g_ledger.taker[i];
        m.keys[i] = (g.open && g.node == g_ledger.node && i < g.n) ? g.c[i].key : 0;
    }
    net_send_treasure(m);
}

void check_keys() {
    if (net_role() == NetRole::Host || !g.open) return;
    for (uint8_t i = 0; i < g.n; ++i) {
        if (!g.host_keys[i] || g.host_keys[i] == g.c[i].key || g.mismatch) continue;
        g.mismatch = true;
        log_line_lvl(LogLevel::Warn, "TREASURE", "!! slot %u: the host's item (key %016llx) is not this screen's '%s' (%016llx) -- the players see different items"
                     " (the host's unlocks or the party's luck differ?)", i, (unsigned long long)g.host_keys[i], g.c[i].name, (unsigned long long)g.c[i].key);
    }
}

// The ledger as this peer knows it changed: a pending claim is settled.
void apply_takers(const uint8_t* t) {
    memcpy(g.taker, t, sizeof(g.taker));
    if (!g.open || g.granted >= 0 || g.skip) return;
    const uint8_t me = self_id();
    for (uint8_t i = 0; i < kTreasureMaxSlots; ++i)
        if (g.taker[i] == me && i < g.n) {
            g.granted = i; g.pending = -1;
            log_line("TREASURE", "slot %u ('%s') is this player's -- taking it", i, g.c[i].name);
            return;
        }
    if (g.pending >= 0 && g.taker[g.pending] != kTreasureNobody) {
        log_line("TREASURE", "slot %d went to peer %u first", g.pending, (unsigned)g.taker[g.pending]);
        g.pending = -1;
    }
}

// --- THE CHEST'S OWN SCREEN (2026-10-16, at the user's request: "not a panel -- the items side by side in the treasure room, like Slay the Spire 2") ---
//
// The treasure room (events.swf "TreasureRoom") has ONE item place: the clip "i0" (an unexported symbol holding the ShopItemIcon "icon" at (0,-50)),
// placed at (610,479) from frame 30 -- what Shop::initShop's stock half fills through sub_1407988E0 ("i" + ShopItem+4). For X items this peer makes
// X-1 more clips of i0's own timeline (the pool and the MovieClip constructor DefineSprite's instantiate uses), names them i1..i<X-1>, adds them to
// i0's parent at free depths, lays the X out in a row, and lets sub_1407988E0 fill each with its candidate -- the game's own pictures. The player
// clicks one (mgmp_menu puts an invisible hit area over each); hovering one lifts it and writes its name into the room's banner ("itemname").
// When the choice is made the extra clips are hidden and i0 goes back to its place, and the held-back stock half shows and takes the item in i0.
// Anything unexpected (no i0, a position that is not on the stage, a clip that goes missing) and the panel of the first version is used instead.
constexpr uintptr_t kShop_UI = 0x40, kUI_Renderer = 0x38;          // Shop -> its Flash UI -> the renderer whose +0x80 is the room's root clip
constexpr uintptr_t kDO_Parent = 0x38, kDO_Name = 0x48;            // swf::DisplayObject: the container it is in; its instance name (char*, not owned)
constexpr uintptr_t kDO_Sx = 0x60, kDO_Sy = 0x64, kDO_X = 0x70, kDO_Y = 0x74, kDO_Alpha = 0x84;   // floats (the base constructor 0x1409B08B0: 1,1,0,0,0,0 then the colour 1,1,1,1)
constexpr uintptr_t kMC_Timeline = 0xD0;                            // swf::MovieClip: what the constructor was given
constexpr uintptr_t kCont_Cap = 0xA8, kCont_Count = 0xAC, kCont_Items = 0xB0;   // DisplayObjectContainer: children by DEPTH (inline when cap <= 4)
constexpr int       kCloneDepth = 240;                              // well above what the room's timeline places (its deepest is 21)
// Where an icon's picture sits in a slot clip, and how big it is (both in the clip's own units): the ShopItemIcon "icon" is placed at (0,-50) and
// its art is drawn around a point about 20 below that (measured 2026-10-20 on two items: a stomach and a spiked collar); 85 covers the widest art.
constexpr float     kHitDy = -30.0f, kHitHalf = 85.0f;
// events.swf is a 1280x720 movie that the game draws over the 1920x1080 stage: one of its units is 1.5 stage pixels (measured 2026-10-20 -- the
// first version left this out and put every hit area two thirds of the way to the corner).
constexpr float     kFlashToStage = 1920.0f / 1280.0f;
const char* const   kSlotNames[kTreasureMaxSlots] = { "i0", "i1", "i2", "i3", "i4", "i5", "i6", "i7" };

// THE ROW (2026-10-20, after the first live test): every candidate is in a clip of ITS OWN ("i1".."i<n>"); i0 is not used while the player
// chooses -- the chest's timeline moves i0 (it rises out of the chest), so a position written into it does not stay. i0 is kept invisible, and
// the row is laid out every frame on i0's current place, with only the items nobody has taken: one taken on another screen leaves the row at once.
struct Vis {
    int      state = 0;             // 0 off, 1 being built, 2 up, 3 failed (the panel)
    int      tries = 0;
    void*    i0 = nullptr;
    void*    obj[kTreasureMaxSlots] = {};   // slot k's clip (named i<k+1>)
    void*    parent = nullptr;
    int      depth0 = -1;
    float    sx0 = 1, sy0 = 1;              // i0's scale as the room placed it
    float    wx[kTreasureMaxSlots] = {}, wy[kTreasureMaxSlots] = {}, whalf = 0;   // the hit areas' centres and half size on the stage
    bool     shown[kTreasureMaxSlots] = {};
    int      hover = -1, shown_name = -2;
} vis;
void vis_reset() { vis = Vis{}; }

typedef void* (__fastcall* fn_find)(void* renderer, void* path_by_value);
typedef void* (__fastcall* fn_pool)(void* pool);
typedef void* (__fastcall* fn_mcctor)(void* mem, void* timeline);
typedef void  (__fastcall* fn_addat)(void* container, void* child, int depth);
typedef void  (__fastcall* fn_slotui)(void* shop, void* item, bool);
typedef void* (__fastcall* fn_wcopy)(void* out, const void* src);
typedef void  (__fastcall* fn_settext)(void* tf, void* text_by_value, bool, bool);
typedef void  (__fastcall* fn_vcall)(void* self);

void* swf_find(void* renderer, const char* path) {
    const uintptr_t fn = addr_of_call(C_SwfFind), sctor = addr_of_call(C_StrFromCStr);
    if (!fn || !sctor || !renderer) return nullptr;
    __try {
        alignas(16) uint8_t s[32];
        memset(s, 0, sizeof(s));
        ((fn_str)sctor)(s, path);
        return ((fn_find)fn)(renderer, s);                                   // by value: the callee frees it
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
// The clip pool: DefineSprite's instantiate loads it with `lea rcx, [rip+X]` at +0x11 (48 8D 0D).
void* clip_pool() {
    const uintptr_t fn = addr_of_call(C_SwfSpriteMake);
    uint8_t b[7] = {};
    if (!fn || !mem_read((const void*)(fn + 0x11), b, sizeof(b)) || b[0] != 0x48 || b[1] != 0x8D || b[2] != 0x0D) return nullptr;
    int32_t disp = 0;
    memcpy(&disp, b + 3, 4);
    return (void*)(fn + 0x18 + (intptr_t)disp);
}
void* make_clip(void* timeline) {
    const uintptr_t al = addr_of_call(C_SwfPoolAlloc), ct = addr_of_call(C_MovieClipCtor);
    void* pool = clip_pool();
    if (!al || !ct || !pool || !timeline) return nullptr;
    __try {
        void* mem = ((fn_pool)al)(pool);
        return mem ? ((fn_mcctor)ct)(mem, timeline) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
bool add_child(void* parent, void* child, int depth) {
    const uintptr_t fn = addr_of_call(C_SwfAddChildAt);
    if (!fn) return false;
    __try { ((fn_addat)fn)(parent, child, depth); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool vcall(void* self, int slot) {
    __try { ((fn_vcall)(*(void***)self)[slot])(self); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool slot_ui(void* shop, void* item) {
    const uintptr_t fn = addr_of_call(C_ShopSlotUi);
    if (!fn) return false;
    __try { ((fn_slotui)fn)(shop, item, true); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Text into a TextField from a std::wstring of the game's (copied first: the setter takes it by value).
bool set_text(void* tf, const void* wstr) {
    const uintptr_t cp = addr_of_call(C_WStrCopy), st = addr_of_call(C_SwfSetText);
    if (!cp || !st || !tf || !wstr) return false;
    __try {
        alignas(16) uint8_t t[32];
        memset(t, 0, sizeof(t));
        ((fn_wcopy)cp)(t, wstr);
        ((fn_settext)st)(tf, t, false, true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// ... and from our own UTF-16 text: a std::wstring laid out in place (the long form: the copy reads the pointer).
bool set_text_w(void* tf, const wchar_t* text) {
    struct { const wchar_t* p; uint64_t pad; uint64_t size; uint64_t cap; } src = { text, 0, wcslen(text), 0 };
    src.cap = src.size > 7 ? src.size : 8;
    if (src.size <= 7) {                                                     // the short form keeps the characters inline
        alignas(16) uint8_t inl[32] = {};
        memcpy(inl, text, (src.size + 1) * sizeof(wchar_t));
        *(uint64_t*)(inl + 0x10) = src.size; *(uint64_t*)(inl + 0x18) = 7;
        return set_text(tf, inl);
    }
    return set_text(tf, &src);
}
bool rd_f(const void* o, uintptr_t off, float& v) { return mem_read((const uint8_t*)o + off, &v, sizeof(v)); }
bool wr_f(void* o, uintptr_t off, float v) { return mem_write((uint8_t*)o + off, &v, sizeof(v)); }
void* child_at(void* cont, int depth) {
    uint32_t cap = 0, count = 0;
    if (!mem_read((uint8_t*)cont + kCont_Cap, &cap, 4) || !mem_read((uint8_t*)cont + kCont_Count, &count, 4) || depth < 0 || (uint32_t)depth >= count) return nullptr;
    void* arr = (uint8_t*)cont + kCont_Items;
    if (cap > 4 && !mem_read(arr, &arr, sizeof(arr))) return nullptr;
    void* c = nullptr;
    return mem_read((uint8_t*)arr + depth * 8, &c, sizeof(c)) ? c : nullptr;
}
// Where a point of `obj`'s own space is on the stage: up the parents (translation and scale; the room has no rotation).
bool to_stage(void* obj, float& x, float& y, float& s) {
    s = 1;
    for (int i = 0; obj && i < 24; ++i) {
        float px = 0, py = 0, sx = 1, sy = 1;
        if (!rd_f(obj, kDO_X, px) || !rd_f(obj, kDO_Y, py) || !rd_f(obj, kDO_Sx, sx) || !rd_f(obj, kDO_Sy, sy)) return false;
        x = px + x * sx; y = py + y * sy; s *= sx;
        void* up = nullptr;
        if (!mem_read((uint8_t*)obj + kDO_Parent, &up, sizeof(up))) return false;
        obj = up;
    }
    return true;
}
void* renderer_of(void* shop) {
    void* ui = nullptr; void* r = nullptr;
    if (!mem_read((uint8_t*)shop + kShop_UI, &ui, sizeof(ui)) || !ui || !mem_read((uint8_t*)ui + kUI_Renderer, &r, sizeof(r))) return nullptr;
    return r;
}
uint8_t* first_item(const Cand& c) { uint8_t* f = nullptr; uint32_t n = 0; return vec_range(c.vec, f, n) && n ? f : nullptr; }

void vis_fail(const char* why) {
    vis.state = 3;
    for (uint8_t k = 0; k < kTreasureMaxSlots; ++k) if (vis.obj[k]) { wr_f(vis.obj[k], kDO_Alpha, 0.0f); wr_f(vis.obj[k], kDO_X, -4000.0f); }   // clips already made: out of sight
    if (vis.i0) wr_f(vis.i0, kDO_Alpha, 1.0f);
    log_line_lvl(LogLevel::Warn, "TREASURE", "!! the items cannot be shown in the treasure room itself (%s) -- the panel is used", why);
}

// One try at building the row (the game thread, the lock held). False: try again next frame.
bool vis_build() {
    void* rend = renderer_of(g.shop);
    void* i0 = rend ? swf_find(rend, "i0") : nullptr;
    if (!i0) return false;                                                   // the chest's animation has not placed it yet
    void* parent = nullptr; void* tl = nullptr;
    if (!mem_read((uint8_t*)i0 + kDO_Parent, &parent, sizeof(parent)) || !parent || !mem_read((uint8_t*)i0 + kMC_Timeline, &tl, sizeof(tl)) || !tl) { vis_fail("i0 has no parent or timeline"); return true; }
    int d0 = -1;
    for (int d = 0; d < 64 && d0 < 0; ++d) if (child_at(parent, d) == i0) d0 = d;
    if (d0 < 0) { vis_fail("i0 is not among its parent's children"); return true; }
    float x0 = 0, y0 = 0, sx = 1, sy = 1;
    rd_f(i0, kDO_X, x0); rd_f(i0, kDO_Y, y0); rd_f(i0, kDO_Sx, sx); rd_f(i0, kDO_Sy, sy);
    log_line("TREASURE", "i0 at depth %d: (%.1f, %.1f) scale %.2f/%.2f", d0, x0, y0, sx, sy);
    if (!(x0 > 0 && x0 < 1280 && y0 > 0 && y0 < 720 && sx > 0.2f && sx < 5 && sy > 0.2f && sy < 5)) { vis_fail("i0's position is not where the room puts it"); return true; }
    if (g.n + 1 > kTreasureMaxSlots) { vis_fail("too many items for the slot names"); return true; }
    vis.i0 = i0; vis.parent = parent; vis.depth0 = d0; vis.sx0 = sx; vis.sy0 = sy;
    for (uint8_t k = 0; k < g.n; ++k) {
        void* c = make_clip(tl);
        if (!c) { vis_fail("a clip could not be made"); return true; }
        mem_write((uint8_t*)c + kDO_Name, &kSlotNames[k + 1], sizeof(void*));
        if (!add_child(parent, c, kCloneDepth + k) || !vcall(c, 3)) { vis_fail("a clip could not be added"); return true; }
        vis.obj[k] = c;
        wr_f(c, kDO_X, x0); wr_f(c, kDO_Y, y0); wr_f(c, kDO_Sx, sx); wr_f(c, kDO_Sy, sy);
        uint8_t* item = first_item(g.c[k]);
        if (!item) { vis_fail("a candidate has no item"); return true; }
        const int32_t idx = k + 1;
        mem_write(item + 4, &idx, sizeof(idx));                             // the slot sub_1407988E0 fills: "i" + this
        if (!slot_ui(g.shop, item)) { vis_fail("the game's slot filler faulted"); return true; }
        char shown[96] = {};
        if (mem_read_std_wstring_utf8(item + kSI_Shown, shown, sizeof(shown)) && shown[0]) memcpy(g.c[k].name, shown, sizeof(shown));
        log_line("TREASURE", "  slot %u '%s' in clip %s", k, g.c[k].name, kSlotNames[k + 1]);
    }
    vis.state = 2; vis.shown_name = -2;
    log_line("TREASURE", "the %u items are shown in the treasure room", g.n);
    return true;
}

// Every frame while the row is up: the items nobody has taken, side by side on i0's current place (it rises out of the chest); the taken ones
// out of sight; the hovered one lifted; the banner names the hovered item, or the first one. A clip that went missing turns the row off.
void vis_refresh() {
    for (uint8_t k = 0; k < g.n; ++k)
        if (child_at(vis.parent, kCloneDepth + k) != vis.obj[k]) { for (void*& o : vis.obj) o = nullptr; vis.i0 = nullptr; vis_fail("a clip left the room"); return; }
    if (child_at(vis.parent, vis.depth0) != vis.i0) { for (void*& o : vis.obj) o = nullptr; vis.i0 = nullptr; vis_fail("i0 left the room"); return; }
    wr_f(vis.i0, kDO_Alpha, 0.0f);                                          // empty while choosing; the timeline may set it again, so every frame
    float bx = 0, by = 0;
    rd_f(vis.i0, kDO_X, bx); rd_f(vis.i0, kDO_Y, by);
    const bool busy = g.granted >= 0 || g.skip;
    uint8_t open[kTreasureMaxSlots], m = 0;
    for (uint8_t k = 0; k < g.n; ++k) if (g.taker[k] == kTreasureNobody) open[m++] = k;
    const float spacing = m <= 2 ? 400.0f : (m == 3 ? 340.0f : 290.0f);   // closer and smaller the more there are
    const float f = m <= 2 ? 0.9f : (m == 3 ? 0.8f : 0.68f);
    for (uint8_t k = 0; k < g.n; ++k) { vis.shown[k] = false; wr_f(vis.obj[k], kDO_Alpha, 0.0f); wr_f(vis.obj[k], kDO_X, -4000.0f); }
    for (uint8_t j = 0; j < m; ++j) {
        const uint8_t k = open[j];
        void* o = vis.obj[k];
        const bool up = !busy && k == vis.hover;
        const float sc = f * (up ? 1.1f : 1.0f);
        wr_f(o, kDO_X, bx + (j - (m - 1) * 0.5f) * spacing); wr_f(o, kDO_Y, by);
        wr_f(o, kDO_Sx, vis.sx0 * sc); wr_f(o, kDO_Sy, vis.sy0 * sc); wr_f(o, kDO_Alpha, 1.0f);
        float wx = 0, wy = kHitDy, ws = 1;
        if (!to_stage(o, wx, wy, ws)) { vis_fail("the stage position could not be read"); return; }
        vis.wx[k] = wx * kFlashToStage; vis.wy[k] = wy * kFlashToStage; vis.shown[k] = true;
        vis.whalf = kHitHalf * (ws / (up ? 1.1f : 1.0f)) * kFlashToStage;   // the area does not grow with the lift: no flicker at its edge
    }
    // the banner: the hovered item's name, otherwise the first one's
    int name_slot = vis.hover >= 0 && vis.hover < g.n && vis.shown[vis.hover] ? vis.hover : (m ? open[0] : -1);
    if (name_slot != vis.shown_name) {
        vis.shown_name = name_slot;
        void* tf = swf_find(renderer_of(g.shop), "itemname");
        uint8_t* item = name_slot >= 0 ? first_item(g.c[name_slot]) : nullptr;
        if (tf && item) set_text(tf, item + kSI_Shown);
    }
}

// The choice is made: the row goes, i0 is visible again (the stock half shows the chosen item there as the game always does).
void vis_release() {
    if (vis.state != 2) { vis = Vis{}; return; }
    for (uint8_t k = 0; k < g.n; ++k) if (vis.obj[k]) { wr_f(vis.obj[k], kDO_Alpha, 0.0f); wr_f(vis.obj[k], kDO_X, -4000.0f); }
    if (vis.i0) wr_f(vis.i0, kDO_Alpha, 1.0f);
    vis = Vis{};
}

} // namespace

bool treasure_on_stock(void* shop, void (*original)(void*)) {
    Lock lock;
    g.original = original;
    if (g.releasing || !shop) return false;
    if (!session_exp_treasure() || !net_active() || net_peer_count() < 2) return false;
    uint8_t flags[4] = {};
    uint64_t stock[2] = {};
    if (!mem_read((uint8_t*)shop + kShop_Treasure, flags, 4) || !mem_read((uint8_t*)shop + kShop_Stock, stock, sizeof(stock))) return false;
    if (!flags[0] || stock[0] != stock[1] || flags[kShop_Given - kShop_Treasure]) return false;   // not a treasure room, or its stock is already there
    const uint32_t type = follow_here_type();
    const uint64_t node = follow_here_seed();
    if (type != kNodeTreasure || !node) {
        log_line("TREASURE", "a treasure room outside a treasure node (node type %u) -- left as the game does it", type);
        return false;
    }
    if (g.open && g.shop == shop) return true;                         // the chest's event again: still choosing
    if (g.open) close_picker("another treasure room opened");

    void* def = nullptr;
    double luck = 0;
    uint64_t vt = 0;
    if (!mem_read((uint8_t*)shop + kShop_Def, &def, sizeof(def)) || !def || !mem_read((uint8_t*)shop + kShop_Luck, &luck, sizeof(luck)) ||
        !mem_read(shop, &vt, sizeof(vt))) return false;
    char def_name[64] = {};
    const bool named = mem_read_std_string((const uint8_t*)def + kGon_Name, def_name, sizeof(def_name)) && def_name[0];
    if (named && !shared_chest(def_name)) {
        log_line("TREASURE", "node %016llx: '%s' is a special chest -- left as the game does it", (unsigned long long)node, def_name);
        return false;
    }
    if (!named) log_line_lvl(LogLevel::Warn, "TREASURE", "!! the chest's definition name could not be read -- its items decide whether it is a special chest");
    uint64_t* s = rng_global_stream();
    uint64_t saved[4] = {};
    if (!s || !mem_read(s, saved, sizeof(saved))) return false;

    // THE SAME LUCK ON EVERY SCREEN (proto 101): this chest's luck with this peer's party taken out and the host's put in, in whole millionths
    // (the same double on every peer whichever order the sums were made in).
    {
        const double chest = luck;
        double mine = 0;
        const bool have_mine = party_luck(mine);
        const bool host = net_role() == NetRole::Host;
        int64_t host_e6 = 0;
        bool have_host = false;
        if (g.luck_node == node) { host_e6 = g.luck_e6; have_host = true; }
        else if (host && have_mine) { host_e6 = to_e6(mine); have_host = true; }
        if (have_mine && have_host) {
            luck = (double)(to_e6(chest) - to_e6(mine) + host_e6) / 1e6;
            log_line("TREASURE", "node %016llx: luck %.2f for every player = this chest's %.2f - this party's %.2f + the host's party's %.2f%s", (unsigned long long)node, luck, chest, mine,
                     (double)host_e6 / 1e6, host ? "" : " (sent by the host as it entered the node)");
        } else {
            log_line_lvl(LogLevel::Warn, "TREASURE", "!! node %016llx: %s -- this chest's own luck %.2f is used, the other players may see other items", (unsigned long long)node,
                         !have_mine ? "this party's luck could not be asked of the game" : "the host's party luck for this node has not arrived", chest);
        }
    }
    uint32_t x = net_peer_count();
    if (x > kTreasureMaxSlots) x = kTreasureMaxSlots;
    log_line("TREASURE", "node %016llx: the chest opens -- rolling %u item(s) for %u player(s), luck %.2f%s", (unsigned long long)node, x, (unsigned)net_peer_count(), luck,
             net_role() == NetRole::Client ? ", with the host's unlock answers" : "");
    g.n = 0;
    const bool client = net_role() == NetRole::Client;
    unlocks_force_host_lists(true);
    const bool rebuilt = rebuild_item_pools(client ? "with the host's unlock answers" : "with this save's unlocks as they are now");
    bool ok = true, special = false;
    for (uint8_t k = 0; k < x && ok; ++k) {
        Cand& c = g.c[k];
        g.n = k + 1;                                                    // counted before the roll, so a fault mid-way still frees it
        for (uint32_t attempt = 0; attempt < 6; ++attempt) {
            uint64_t z = node ^ 0x7472656173757265ull ^ ((uint64_t)(k + 1) << 40) ^ ((uint64_t)attempt << 32);
            uint64_t st[4];
            for (uint64_t& w : st) w = mix(z);
            mem_write(s, st, sizeof(st));
            memset(c.vec, 0, sizeof(c.vec));
            if (!call_gen(c.vec, def, luck)) { ok = false; break; }
            uint8_t* first = nullptr;
            if (!vec_range(c.vec, first, c.count)) { ok = false; break; }
            bool dup = false;
            if (c.count) {
                if (!describe(first, c.key, c.name, sizeof(c.name))) { ok = false; break; }
                if (!named && special_item(g_last_gon)) { special = true; break; }
                for (uint8_t j = 0; j < k; ++j) dup = dup || g.c[j].key == c.key;
            }
            if (c.count && (!dup || attempt == 5)) break;
            log_line("TREASURE", "  slot %u, roll %u: %s -- rolled again", k, attempt, c.count ? "the same item as an earlier slot" : "nothing");
            call_free(c.vec);
            c = Cand{};
        }
        if (special) break;
        if (ok && !c.count) { g.n = k; break; }                       // this definition rolls nothing more: fewer slots
    }
    unlocks_force_host_lists(false);
    if (ok && !special)                                                 // 0.1.87: what each item does, for the panel under the hovered one (before the stream is put back:
        for (uint8_t k = 0; k < g.n; ++k)                               // whatever the builder might draw is undone with the rolls')
            if (uint8_t* first = first_item(g.c[k])) describe_effect(first, g.c[k].desc, sizeof(g.c[k].desc));
    mem_write(s, saved, sizeof(saved));
    if (client && rebuilt) rebuild_item_pools("back with this save's own unlocks");
    if (special) {
        log_line("TREASURE", "node %016llx: the chest holds '%s', a special chest's item -- left as the game does it", (unsigned long long)node, g_last_gon);
        free_candidates();
        return false;
    }
    if (!ok || g.n < 2) {
        log_line_lvl(LogLevel::Warn, "TREASURE", "!! the chest's items could not be rolled (%s, %u) -- the game does it the usual way", ok ? "too few" : "a call faulted", g.n);
        free_candidates();
        return false;
    }
    g.open = true; g.shop = shop; g.shop_vt = vt; g.node = node; g.pending = -1; g.granted = -1; g.skip = false; g.mismatch = false;
    if (g.known_node != node) {                                         // nothing heard from the host about this chest yet
        g.known_node = node;
        for (uint8_t& t : g.taker) t = kTreasureNobody;
        memset(g.host_keys, 0, sizeof(g.host_keys));
    }
    check_keys();
    if (net_role() == NetRole::Host) {
        if (g_ledger.node != node) g_ledger.reset(node);
        memcpy(g.taker, g_ledger.taker, sizeof(g.taker));
        send_state();
    }
    vis = Vis{};
    vis.state = 1;
    log_line("TREASURE", "picker open: %u item(s); first come, first served (the host keeps the ledger)", g.n);
    return true;
}

void treasure_publish_luck(uint64_t node) {
    Lock lock;
    if (net_role() != NetRole::Host || !node || !session_exp_treasure() || !net_active() || net_peer_count() < 2) return;
    // A FRESH CHEST AT EVERY ENTRY (0.1.88, room ZGUJ 2026-10-24: "after re-entering the game the chest is empty"). The players reloaded the save from
    // before this node and came back into it; the host's ledger still named both of the last visit's takers, so the chest closed at once with
    // "every item is taken" on both screens -- while the save had neither item any more. The game's own chest is new at every entry, so is the ledger.
    if (!(g.open && g.node == node)) {                                  // (never under a chest of this node that is still being chosen from)
        if (g_ledger.node == node) {
            bool any = false;
            for (uint8_t t : g_ledger.taker) any = any || t != kTreasureNobody;
            if (any) log_line("TREASURE", "node %016llx entered again -- the ledger of the last visit is cleared (the game's chest is a new one)", (unsigned long long)node);
        }
        g_ledger.reset(node);
        if (!g.open) { g.known_node = 0; for (uint8_t& t : g.taker) t = kTreasureNobody; memset(g.host_keys, 0, sizeof(g.host_keys)); }
    }
    double mine = 0;
    if (!party_luck(mine)) { log_line_lvl(LogLevel::Warn, "TREASURE", "!! node %016llx: the host's party luck could not be asked of the game -- not sent", (unsigned long long)node); return; }
    g.luck_node = node; g.luck_e6 = to_e6(mine);
    TreasureMsg m;
    m.kind = kTreasureLuck; m.node = node; m.luck_e6 = g.luck_e6;
    const bool sent = net_send_treasure(m);
    log_line("TREASURE", "node %016llx: a treasure node -- the host's party luck %.2f %s", (unsigned long long)node, (double)g.luck_e6 / 1e6, sent ? "sent to every player" : "!! could NOT be sent");
}

void treasure_on_message(uint8_t from, const TreasureMsg& m) {
    Lock lock;
    if (m.kind == kTreasureLuck) {
        if (net_role() == NetRole::Host || from != kHostPeer) return;
        g.luck_node = m.node; g.luck_e6 = m.luck_e6;
        // 0.1.88: the host has just entered this node -- a new chest; what this peer heard about an earlier visit of it is forgotten
        if (!g.open || g.node != m.node) {
            if (g.known_node == m.node) log_line("TREASURE", "node %016llx entered again -- the takers heard for the last visit are forgotten", (unsigned long long)m.node);
            g.known_node = 0; for (uint8_t& t : g.taker) t = kTreasureNobody; memset(g.host_keys, 0, sizeof(g.host_keys));
        }
        log_line("TREASURE", "node %016llx: the host's party luck is %.2f -- the shared chest is rolled with it", (unsigned long long)m.node, (double)m.luck_e6 / 1e6);
        return;
    }
    const uint64_t here = follow_here_seed();
    if (m.kind == kTreasureClaim) {
        if (net_role() != NetRole::Host) return;
        // the node this host stands in, or the chest whose ledger it still keeps (the host may already have walked on while a player still chooses)
        if (m.node != g_ledger.node) {
            if (!here || m.node != here) { log_line("TREASURE", "peer %u claims slot %u of node %016llx, but this host is at %016llx -- ignored", (unsigned)from, m.slot, (unsigned long long)m.node, (unsigned long long)here); return; }
            g_ledger.reset(here);
        }
        const TreasureLedger::Result r = g_ledger.claim(m.node, from, m.slot);
        static const char* const kR[] = { "granted", "already theirs", "already taken", "they hold another slot", "refused" };
        char who[48] = {};
        room_peer_name(from, who, sizeof(who));
        log_line("TREASURE", "peer %u (%s) claims slot %u: %s", (unsigned)from, who, m.slot, kR[r]);
        if (g.open && g.node == m.node && m.slot < g.n && m.key && m.key != g.c[m.slot].key)
            log_line_lvl(LogLevel::Warn, "TREASURE", "!! peer %u sees another item in slot %u (%016llx, here '%s' %016llx)", (unsigned)from, m.slot, (unsigned long long)m.key, g.c[m.slot].name, (unsigned long long)g.c[m.slot].key);
        if (g.node == m.node) apply_takers(g_ledger.taker);
        send_state();
        return;
    }
    if (m.kind != kTreasureState || net_role() == NetRole::Host || from != kHostPeer) return;
    if (!here || m.node != here) return;
    g.known_node = here;                                               // kept for a chest this peer opens later
    for (uint8_t i = 0; i < kTreasureMaxSlots; ++i) g.host_keys[i] = i < m.n ? m.keys[i] : 0;
    if (g.open && g.node == here) check_keys();
    uint8_t t[kTreasureMaxSlots];
    for (uint8_t i = 0; i < kTreasureMaxSlots; ++i) t[i] = i < m.n ? m.taker[i] : kTreasureNobody;
    apply_takers(t);
}

void treasure_take(uint8_t slot) {
    Lock lock;
    if (!g.open || slot >= g.n || g.granted >= 0 || g.skip || g.pending >= 0 || g.taker[slot] != kTreasureNobody) return;
    if (net_role() == NetRole::Host || !net_active()) {
        if (!net_active()) { g.granted = slot; log_line("TREASURE", "slot %u taken without a session", slot); return; }
        if (g_ledger.node != g.node) g_ledger.reset(g.node);
        const TreasureLedger::Result r = g_ledger.claim(g.node, kHostPeer, slot);
        log_line("TREASURE", "the host takes slot %u ('%s'): %s", slot, g.c[slot].name, r == TreasureLedger::Granted ? "granted" : "refused");
        apply_takers(g_ledger.taker);
        send_state();
        return;
    }
    TreasureMsg m;
    m.kind = kTreasureClaim; m.node = g.node; m.slot = slot; m.key = g.c[slot].key;
    if (!net_send_treasure(m)) { log_line_lvl(LogLevel::Warn, "TREASURE", "!! the claim on slot %u could not be sent", slot); return; }
    g.pending = slot; g.pending_at = GetTickCount64();
    log_line("TREASURE", "asked the host for slot %u ('%s')", slot, g.c[slot].name);
}

void treasure_skip() {
    Lock lock;
    if (!g.open || g.granted >= 0) return;
    g.skip = true;
    log_line("TREASURE", "this player takes nothing from the chest");
}

void treasure_tick() {
    Lock lock;
    if (!g.open) return;
    if (!shop_waiting(g.shop)) { g.shop = nullptr; close_picker("the treasure room is gone"); return; }
    if (g.pending >= 0 && GetTickCount64() - g.pending_at > kClaimTimeoutMs) {
        log_line_lvl(LogLevel::Warn, "TREASURE", "!! no answer from the host for slot %d in %llu ms -- the player may ask again", g.pending, (unsigned long long)kClaimTimeoutMs);
        g.pending = -1;
    }
    if (vis.state == 1 && !vis_build() && ++vis.tries > 180) vis_fail("i0 did not appear within three seconds");
    if (g.granted < 0 && !g.skip && g.pending < 0) {
        bool any = false;
        for (uint8_t k = 0; k < g.n; ++k) any = any || g.taker[k] == kTreasureNobody;
        if (!any) { g.skip = true; log_line("TREASURE", "every item of the chest is taken by the other players -- nothing is left for this one"); }
    }
    if (vis.state == 2) vis_refresh();
    if (g.granted < 0 && !g.skip) return;

    vis_release();
    void* shop = g.shop;
    bool ok = true;
    if (g.granted >= 0) {
        if (uint8_t* item = first_item(g.c[g.granted])) { const int32_t zero = 0; mem_write(item + 4, &zero, sizeof(zero)); }   // "i0": the room's own slot
        ok = call_move((uint8_t*)shop + kShop_Stock, g.c[g.granted].vec);
    }
    else { const uint8_t one = 1; ok = mem_write((uint8_t*)shop + kShop_Given, &one, 1); }
    const int took = g.granted;
    char name[96] = {};
    if (took >= 0) memcpy(name, g.c[took].name, sizeof(name));
    free_candidates();
    g.open = false; g.pending = -1; g.granted = -1; g.skip = false;
    if (!ok || !g.original) {
        log_line_lvl(LogLevel::Error, "TREASURE", "!! the chest could not be handed its stock -- it stays shut");
        return;
    }
    g.releasing = true;
    const bool ran = call_original(g.original, shop);
    g.releasing = false;
    if (took >= 0) log_line("TREASURE", "'%s' is taken from the chest%s", name, ran ? "" : " -- !! the game's stock half FAULTED");
    else           log_line("TREASURE", "the chest closes with nothing taken%s", ran ? "" : " -- !! the game's stock half FAULTED");
}

bool treasure_view(TreasureView& out) {
    Lock lock;
    if (!g.open) return false;
    out = TreasureView{};
    out.n = g.n;
    const uint8_t me = self_id();
    for (uint8_t i = 0; i < g.n; ++i) {
        memcpy(out.name[i], g.c[i].name, sizeof(out.name[i]));
        memcpy(out.desc[i], g.c[i].desc, sizeof(out.desc[i]));
        if (g.taker[i] == kTreasureNobody) continue;
        out.mine[i] = g.taker[i] == me;
        if (!room_peer_name(g.taker[i], out.taker[i], sizeof(out.taker[i])) || !out.taker[i][0])
            _snprintf_s(out.taker[i], sizeof(out.taker[i]), _TRUNCATE, "P%u", (unsigned)g.taker[i] + 1);
    }
    out.pending = g.pending;
    out.busy = g.granted >= 0 || g.skip;
    out.mismatch = g.mismatch;
    out.native = vis.state == 2;
    out.building = vis.state == 1;
    if (out.native) {
        for (uint8_t i = 0; i < g.n; ++i) { out.shown[i] = vis.shown[i]; out.cx[i] = vis.wx[i]; out.cy[i] = vis.wy[i]; }
        out.half = vis.whalf;
    }
    return true;
}

void treasure_hover(int slot) {
    Lock lock;
    vis.hover = slot;
}

} // namespace mgmp
